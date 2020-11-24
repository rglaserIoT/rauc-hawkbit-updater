/**
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2018-2020 Prevas A/S (www.prevas.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * @file hawkbit-client.c
 * @author Lasse Mikkelsen <lkmi@prevas.dk>
 * @date 19 Sep 2018
 * @brief Hawkbit client
 *
 * Implementation of the hawkBit DDI API.
 *
 * @see https://github.com/rauc/rauc-hawkbit
 * @see https://www.eclipse.org/hawkbit/apis/ddi_api/
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <curl/curl.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libgen.h>
#include <bits/types/struct_tm.h>
#include <gio/gio.h>
#include <sys/reboot.h>

#include "json-helper.h"
#ifdef WITH_SYSTEMD
#include "sd-helper.h"
#endif

#include "hawkbit-client.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(CURL, curl_easy_cleanup)

gboolean run_once = FALSE;

/**
 * @brief String representation of HTTP methods.
 */
static const char *HTTPMethod_STRING[] = {
        "GET", "HEAD", "PUT", "POST", "PATCH", "DELETE"
};

static Config *hawkbit_config = NULL;
static GSourceFunc software_ready_cb;
static gchar * volatile action_id = NULL;
static GThread *thread_download = NULL;

GQuark rhu_hawkbit_client_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_error_quark");
}

GQuark rhu_hawkbit_client_curl_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_curl_error_quark");
}

GQuark rhu_hawkbit_client_http_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_http_error_quark");
}

/**
 * @brief Get available free space of a mounted file system.
 *
 * @param[in]  path       Absolute Path of a disk device node containing a mounted file system
 * @param[out] free_space Pointer to goffset containing the free space in bytes
 * @return TRUE if free space calculation succeeded, FALSE otherwise
 */
static gboolean get_available_space(const char *path, goffset *free_space, GError **error)
{
        struct statvfs stat;
        g_autofree gchar *npath = g_strdup(path);
        const char *rpath = dirname(npath);

        g_return_val_if_fail(path, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        if (statvfs(rpath, &stat)) {
                int err = errno;
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                            "Failed to calculate free space for %s: %s", path, g_strerror(err));
                return FALSE;
        }

        // the available free space is f_bsize * f_bavail
        *free_space = (goffset) stat.f_bsize * (goffset) stat.f_bavail;
        return TRUE;
}

/**
 * @brief Curl callback used for writting software bundle file
 *        and calculate hawkbit checksum.
 *
 * @see   https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
static size_t curl_write_to_file_cb(void *ptr, size_t size, size_t nmemb, struct get_binary *data)
{
        size_t written = fwrite(ptr, size, nmemb, data->fp);
        data->written += written;
        if (data->checksum) {
                g_checksum_update(data->checksum, ptr, written);
        }
        //g_debug("Bytes downloaded: %ld, (%3.f %%)", data->written, ((double) data->written / data->size * 100));
        return written;
}

/**
 * @brief Add string to Curl headers, avoiding overwriting an existing
 *        non-empty list on failure.
 *
 * @param[out] headers curl_slist** of already set headers
 * @param[in]  string  Header to add
 * @param[out] error   Error
 * @return TRUE if string was added to headers successfully, FALSE otherwise (error set)
 */
static gboolean add_curl_header(struct curl_slist **headers, const char *string, GError **error)
{
        struct curl_slist *temp = NULL;

        g_return_val_if_fail(headers, FALSE);
        g_return_val_if_fail(string, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        temp = curl_slist_append(*headers, string);
        if (!temp) {
                curl_slist_free_all(*headers);
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Could not add header %s", string);
                return FALSE;
        }

        *headers = temp;
        return TRUE;
}

/**
 * @brief download software bundle to file.
 *
 * @param[in]  download_url   URL to Software bundle
 * @param[in]  file           File the software bundle should be written to.
 * @param[in]  filesize       Expected file size
 * @param[out] sha1sum        Return location for calculated checksum or NULL
 * @param[out] speed          Average download speed
 * @param[out] error          Error
 */
static gboolean get_binary(const gchar* download_url, const gchar* file, gint64 filesize, gchar **sha1sum, curl_off_t *speed, GError **error)
{
        gboolean ret = TRUE;
        glong http_code = 0;
        FILE *fp = fopen(file, "wb");
        if (fp == NULL) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                            "Failed to open file for download: %s", file);
                return FALSE;
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
                fclose(fp);
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Unable to start libcurl easy session");
                return FALSE;
        }

        struct get_binary gb = {
                .fp       = fp,
                .filesize = filesize,
                .written  = 0,
                .checksum = (sha1sum != NULL ? g_checksum_new(G_CHECKSUM_SHA1) : NULL)
        };

        curl_easy_setopt(curl, CURLOPT_URL, download_url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, HAWKBIT_USERAGENT);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, hawkbit_config->connect_timeout);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, DEFAULT_CURL_DOWNLOAD_BUFFER_SIZE);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &gb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, hawkbit_config->ssl_verify ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, hawkbit_config->ssl_verify ? 1L : 0L);
        /* abort if slower than 100 bytes/sec during 60 seconds */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
        // Setup request headers
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Accept: application/octet-stream");
        if (hawkbit_config->auth_token) {
                g_autofree gchar* auth_token = g_strdup_printf("Authorization: TargetToken %s", hawkbit_config->auth_token);
                headers = curl_slist_append(headers, auth_token);
        } else if (hawkbit_config->gateway_token) {
                g_autofree gchar* gateway_token = g_strdup_printf("Authorization: GatewayToken %s", hawkbit_config->gateway_token);
                headers = curl_slist_append(headers, gateway_token);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, speed);

        if (res != CURLE_OK) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, res, "%s",
                            curl_easy_strerror(res));
                ret = FALSE;
        } else if (http_code != 200) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, http_code,
                            "HTTP request failed: %ld", http_code);
                ret = FALSE;
        }

        if (ret && gb.checksum) { // if checksum enabled then return the value
                *sha1sum = g_strdup(g_checksum_get_string(gb.checksum));
                g_checksum_free(gb.checksum);
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        fclose(fp);
        return ret;
}

/**
 * @brief Curl callback writing REST response to RestPayload*->payload buffer.
 *
 * @see   https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
static size_t curl_write_cb(const void *content, size_t size, size_t nmemb, void *data)
{
        RestPayload *p = NULL;
        size_t real_size = size * nmemb;

        g_return_val_if_fail(content, 0);
        g_return_val_if_fail(data, 0);

        p = (RestPayload *) data;
        p->payload = (gchar *) g_realloc(p->payload, p->size + real_size + 1);

        // copy content to buffer
        memcpy(&(p->payload[p->size]), content, real_size);
        p->size += real_size;
        p->payload[p->size] = '\0';

        return real_size;
}

/**
 * @brief Perform REST request with JSON data, expecting response JSON data.
 *
 * @param[in]  method             HTTP Method, e.g. GET
 * @param[in]  url                URL used in HTTP REST request
 * @param[in]  jsonRequestBody    REST request body. If NULL, no body is sent
 * @param[out] jsonResponseParser Return location for a REST response or NULL to skip response
 *                                parsing
 * @param[out] error              Error
 * @return TRUE if request and response parser (if given) suceeded, FALSE otherwise (error set).
 */
static gboolean rest_request(enum HTTPMethod method, const gchar *url,
                             JsonBuilder *jsonRequestBody, JsonParser **jsonResponseParser,
                             GError **error)
{
        g_autofree gchar *postdata = NULL, *token = NULL;
        g_autoptr(RestPayload) fetch_buffer = NULL;
        struct curl_slist *headers = NULL;
        g_autoptr(CURL) curl = NULL;
        glong http_code = 0;
        CURLcode res;

        g_return_val_if_fail(url, FALSE);
        g_return_val_if_fail(jsonResponseParser == NULL || *jsonResponseParser == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        curl = curl_easy_init();
        if (!curl) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Unable to start libcurl easy session");
                return FALSE;
        }

        // init response buffer
        fetch_buffer = g_new0(RestPayload, 1);
        fetch_buffer->size = 0;
        fetch_buffer->payload = g_malloc0(DEFAULT_CURL_REQUEST_BUFFER_SIZE);

        // set up CURL options
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, HAWKBIT_USERAGENT);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, HTTPMethod_STRING[method]);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, hawkbit_config->connect_timeout);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, hawkbit_config->timeout);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fetch_buffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, hawkbit_config->ssl_verify ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, hawkbit_config->ssl_verify ? 1L : 0L);

        if (jsonRequestBody) {
                g_autoptr(JsonGenerator) generator = json_generator_new();
                g_autoptr(JsonNode) req_root = json_builder_get_root(jsonRequestBody);
                g_autofree gchar *json_req_str = NULL;

                json_generator_set_root(generator, req_root);
                postdata = json_generator_to_data(generator, NULL);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);
                json_req_str = json_to_string(req_root, TRUE);
                g_debug("Request body: %s", json_req_str);
        }

        // set up request headers
        if (!add_curl_header(&headers, "Accept: application/json;charset=UTF-8", error))
                return FALSE;

        if (hawkbit_config->auth_token)
                token = g_strdup_printf("Authorization: TargetToken %s",
                                        hawkbit_config->auth_token);
        else if (hawkbit_config->gateway_token)
                token = g_strdup_printf("Authorization: GatewayToken %s",
                                        hawkbit_config->gateway_token);
        if (token && !add_curl_header(&headers, token, error))
                return FALSE;

        if (jsonRequestBody &&
            !add_curl_header(&headers, "Content-Type: application/json;charset=UTF-8", error))
                return FALSE;

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // perform request
        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        if (res != CURLE_OK) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, res, "%s",
                            curl_easy_strerror(res));
                return FALSE;
        }
        if (http_code != 200) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, http_code,
                            "HTTP request failed: %ld; server response: %s", http_code,
                            fetch_buffer->payload);
                return FALSE;
        }

        if (jsonResponseParser && fetch_buffer->size > 0) {
                // process JSON repsonse
                g_autoptr(JsonParser) parser = json_parser_new_immutable();
                JsonNode *resp_root = NULL;
                g_autofree gchar *json_resp_str = NULL;

                if (!json_parser_load_from_data(parser, fetch_buffer->payload, fetch_buffer->size,
                                                error))
                        return FALSE;

                resp_root = json_parser_get_root(parser);
                json_resp_str = json_to_string(resp_root, TRUE);
                g_debug("Response body: %s", json_resp_str);
                *jsonResponseParser = g_steal_pointer(&parser);
        }

        return TRUE;
}

/**
 * @brief Build JSON status request.
 * @see https://www.eclipse.org/hawkbit/rest-api/rootcontroller-api-guide/#_post_tenant_controller_v1_controllerid_deploymentbase_actionid_feedback
 */
static void json_build_status(JsonBuilder *builder, const gchar *id, const gchar *detail, const gchar *result, const gchar *execution, GHashTable *data)
{
        GHashTableIter iter;
        gpointer key, value;

        // Get current time in UTC
        time_t current_time;
        struct tm time_info;
        char timeString[16];
        time(&current_time);
        gmtime_r(&current_time, &time_info);
        strftime(timeString, sizeof(timeString), "%Y%m%dT%H%M%S", &time_info);

        // build json status
        json_builder_begin_object(builder);

        if (id) {
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, id);
        }

        json_builder_set_member_name(builder, "time");
        json_builder_add_string_value(builder, timeString);

        json_builder_set_member_name(builder, "status");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "result");
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "finished");
        json_builder_add_string_value(builder, result);
        json_builder_end_object(builder);

        json_builder_set_member_name(builder, "execution");
        json_builder_add_string_value(builder, execution);

        if (detail) {
                json_builder_set_member_name(builder, "details");
                json_builder_begin_array(builder);
                json_builder_add_string_value(builder, detail);
                json_builder_end_array(builder);
        }
        json_builder_end_object(builder);

        if (data) {
                json_builder_set_member_name(builder, "data");
                json_builder_begin_object(builder);
                g_hash_table_iter_init(&iter, data);
                while (g_hash_table_iter_next(&iter, &key, &value)) {
                        json_builder_set_member_name(builder, key);
                        json_builder_add_string_value(builder, value);
                }
                json_builder_end_object(builder);
        }
        json_builder_end_object(builder);
}

/**
 * @brief Send feedback to hawkBit.
 */
static gboolean feedback(gchar *url, gchar *id, gchar *detail, gchar *finished, gchar *execution, GError **error)
{
        JsonBuilder *builder = json_builder_new();
        gboolean res;

        json_build_status(builder, id, detail, finished, execution, NULL);

        res = rest_request(POST, url, builder, NULL, error);
        g_object_unref(builder);
        return res;
}

/**
 * @brief Send progress feedback to hawkBit.
 */
static gboolean feedback_progress(const gchar *url, const gchar *id, const gchar *detail, GError **error)
{
        JsonBuilder *builder = json_builder_new();
        gboolean res;

        json_build_status(builder, id, detail, "none", "proceeding", NULL);

        res = rest_request(POST, url, builder, NULL, error);
        g_object_unref(builder);
        return res;
}

/**
 * @brief Get polling sleep time from hawkBit JSON response.
 */
static long json_get_sleeptime(JsonNode *root)
{
        gchar *sleeptime_str = NULL;
        struct tm time;
        long poll_sleep_time;

        sleeptime_str = json_get_string(root, "$.config.polling.sleep", NULL);
        if (!sleeptime_str) {
                poll_sleep_time = hawkbit_config->retry_wait;
                goto out;
        }

        strptime(sleeptime_str, "%T", &time);
        poll_sleep_time = (time.tm_sec + (time.tm_min * 60) + (time.tm_hour * 60 * 60));
out:
        g_free(sleeptime_str);
        return poll_sleep_time;
}

/**
 * @brief Build API URL
 *
 * @param path[in] a printf()-like format string describing the API path or NULL for base path
 * @param ... The arguments to be inserted in path
 *
 * @return a newly allocated full API URL
 */
__attribute__((__format__(__printf__, 1, 2)))
static gchar* build_api_url(const gchar *path, ...)
{
        g_autofree gchar *buffer;
        va_list args;

        va_start(args, path);
        buffer = g_strdup_vprintf(path, args);
        va_end(args);

        return g_strdup_printf(
                "%s://%s/%s/controller/v1/%s%s%s",
                hawkbit_config->ssl ? "https" : "http",
                hawkbit_config->hawkbit_server, hawkbit_config->tenant_id,
                hawkbit_config->controller_id,
                buffer ? "/" : "",
                buffer ? buffer : "");
}

gboolean hawkbit_progress(const gchar *msg)
{
        g_autofree gchar *feedback_url = NULL;
        if (action_id) {
                feedback_url = build_api_url("deploymentBase/%s/feedback", action_id);
                feedback_progress(feedback_url, action_id, msg, NULL);
        }
        return G_SOURCE_REMOVE;
}

static gboolean identify(GError **error)
{
        gboolean res;

        g_debug("Identifying ourself to hawkbit server");
        g_autofree gchar *put_config_data_url = build_api_url("configData");

        JsonBuilder *builder = json_builder_new();
        json_build_status(builder, NULL, NULL, "success", "closed", hawkbit_config->device);

        res = rest_request(PUT, put_config_data_url, builder, NULL, error);
        g_object_unref(builder);
        return res;
}

static void process_artifact_cleanup(struct artifact *artifact)
{
        if (artifact == NULL)
                return;
        g_free(artifact->name);
        g_free(artifact->version);
        g_free(artifact->download_url);
        g_free(artifact->feedback_url);
        g_free(artifact->sha1);
        g_free(artifact);
}

static void process_deployment_cleanup()
{
        //g_clear_pointer(action_id, g_free);
        gpointer ptr = action_id;
        action_id = NULL;
        g_free(ptr);

        if (g_file_test(hawkbit_config->bundle_download_location, G_FILE_TEST_EXISTS)) {
                if (g_remove(hawkbit_config->bundle_download_location) != 0) {
                        g_debug("Failed to delete file: %s", hawkbit_config->bundle_download_location);
                }
        }
}

gboolean install_complete_cb(gpointer ptr)
{
        struct on_install_complete_userdata *result = ptr;
        g_autofree gchar *feedback_url = NULL;
        if (action_id) {
                feedback_url = build_api_url("deploymentBase/%s/feedback", action_id);
                if (result->install_success) {
                        g_message("Software bundle installed successful.");
                        feedback(feedback_url, action_id, "Software bundle installed successful.", "success", "closed", NULL);
                        if (hawkbit_config->post_update_reboot) {
                                process_deployment_cleanup();
                                sync();
                                if (reboot(RB_AUTOBOOT) < 0) {
                                        g_critical("Failed to reboot: %s", g_strerror(errno));
                                }
                        }
                } else {
                        g_critical("Failed to install software bundle.");
                        feedback(feedback_url, action_id, "Failed to install software bundle.", "failure", "closed", NULL);
                }

                process_deployment_cleanup();
        }
        return G_SOURCE_REMOVE;
}

static gpointer download_thread(gpointer data)
{
        struct on_new_software_userdata userdata = {
                .install_progress_callback = (GSourceFunc) hawkbit_progress,
                .install_complete_callback = install_complete_cb,
                .file = hawkbit_config->bundle_download_location,
        };

        GError *error = NULL;
        g_autofree gchar *msg = NULL;
        struct artifact *artifact = data;
        curl_off_t speed;
        g_message("Start downloading: %s", artifact->download_url);

        // setup checksum
        g_autofree gchar *sha1sum = NULL;

        // Download software bundle (artifact)
        gboolean res = get_binary(artifact->download_url, hawkbit_config->bundle_download_location,
                                  artifact->size, &sha1sum, &speed, &error);
        if (!res) {
                g_autofree gchar *msg = g_strdup_printf("Download failed: %s", error->message);
                g_clear_error(&error);
                g_critical("%s", msg);
                feedback(artifact->feedback_url, action_id, msg, "failure", "closed", NULL);
                goto down_error;
        }

        // notify hawkbit that download is complete
        msg = g_strdup_printf("Download complete. %.2f MB/s",
                              (double)speed/(1024*1024));
        feedback_progress(artifact->feedback_url, action_id, msg, NULL);
        g_message("%s", msg);

        // validate checksum
        if (g_strcmp0(artifact->sha1, sha1sum)) {
                g_autofree gchar *msg = g_strdup_printf(
                        "Software: %s V%s. Invalid checksum: %s expected %s",
                        artifact->name, artifact->version,
                        sha1sum,
                        artifact->sha1);
                feedback(artifact->feedback_url, action_id, msg, "failure", "closed", NULL);
                g_critical("%s", msg);
                goto down_error;
        }
        g_message("File checksum OK.");
        feedback_progress(artifact->feedback_url, action_id, "File checksum OK.", NULL);
        process_artifact_cleanup(artifact);

        software_ready_cb(&userdata);
        return NULL;
down_error:
        process_artifact_cleanup(artifact);
        process_deployment_cleanup();
        return NULL;
}

static gboolean process_deployment(JsonNode *req_root, GError **error)
{
        GError *ierror = NULL;
        struct artifact *artifact = NULL;
        goffset freespace;
        gboolean res;

        if (action_id) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_ALREADY_IN_PROGRESS,
                            "Deployment %s is already in progress.", action_id);
                return FALSE;
        }

        // get deployment url
        g_autofree gchar *deployment = json_get_string(req_root, "$._links.deploymentBase.href", NULL);
        if (deployment == NULL) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_JSON_RESPONSE_PARSE,
                            "Failed to parse deployment base response.");
                return FALSE;
        }

        // get deployment resource
        JsonParser *json_response_parser = NULL;
        res = rest_request(GET, deployment, NULL, &json_response_parser, error);
        if (!res)
                return FALSE;

        JsonNode *resp_root = json_parser_get_root(json_response_parser);

        action_id = json_get_string(resp_root, "$.id", NULL);
        if (!action_id) {
                g_set_error(error,1,1,"Failed to parse deployment base response.");
                return FALSE;
        }

        gchar *feedback_url = build_api_url("deploymentBase/%s/feedback", action_id);

        JsonArray *json_chunks = json_get_array(resp_root, "$.deployment.chunks", NULL);
        if (json_chunks == NULL || json_array_get_length(json_chunks) == 0) {
                feedback(feedback_url, action_id, "Failed to parse deployment resource.", "failure", "closed", NULL);
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_JSON_RESPONSE_PARSE,
                            "Failed to parse deployment resource.");
                goto proc_error;
        }

        // Downloading multiple chunks not supported. Only first chunk is downloaded (RAUC bundle)
        JsonNode *json_chunk = json_array_get_element(json_chunks, 0);
        JsonArray *json_artifacts = json_get_array(json_chunk, "$.artifacts", NULL);
        if (json_artifacts == NULL || json_array_get_length(json_artifacts) == 0) {
                feedback(feedback_url, action_id, "Failed to parse deployment resource.", "failure", "closed", NULL);
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_JSON_RESPONSE_PARSE,
                            "Failed to parse deployment resource.");
                goto proc_error;
        }
        JsonNode *json_artifact = json_array_get_element(json_artifacts, 0);

        // get artifact information
        artifact = g_new0(struct artifact, 1);
        artifact->version = json_get_string(json_chunk, "$.version", NULL);
        artifact->name = json_get_string(json_chunk, "$.name", NULL);
        artifact->size = json_get_int(json_artifact, "$.size", NULL);
        artifact->sha1 = json_get_string(json_artifact, "$.hashes.sha1", NULL);
        artifact->feedback_url = feedback_url;
        // favour https download
        artifact->download_url = json_get_string(json_artifact, "$._links.download.href", NULL);
        if (artifact->download_url == NULL)
                artifact->download_url = json_get_string(json_artifact, "$._links.download-http.href", NULL);

        if (artifact->download_url == NULL) {
                feedback(feedback_url, action_id, "Failed to parse deployment resource.", "failure", "closed", NULL);
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_JSON_RESPONSE_PARSE,
                            "Failed to parse deployment resource.");
                goto proc_error;
        }

        g_message("New software ready for download. (Name: %s, Version: %s, Size: %" G_GINT64_FORMAT ", URL: %s)",
                  artifact->name, artifact->version, artifact->size, artifact->download_url);

        // Check if there is enough free diskspace
        res = get_available_space(hawkbit_config->bundle_download_location, &freespace, &ierror);
        if (!res) {
                feedback(feedback_url, action_id, ierror->message, "failure", "closed", NULL);
                g_propagate_error(error, ierror);
                goto proc_error;
        }

        if (freespace < artifact->size) {
                // Notify hawkbit that there is not enough free space.
                g_autofree gchar *msg = g_strdup_printf("Not enough free space. File size: %" G_GINT64_FORMAT  ". Free space: %" G_GOFFSET_FORMAT,
                                                        artifact->size, freespace);
                g_debug("%s", msg);
                feedback(feedback_url, action_id, msg, "failure", "closed", NULL);
                g_set_error(error, 1, 23, "%s", msg);
                goto proc_error;
        }

        // unref/free previous download thread by joining it
        if (thread_download)
                g_thread_join(thread_download);

        // start download thread
        thread_download = g_thread_new("downloader", download_thread,
                                       (gpointer) artifact);

        g_object_unref(json_response_parser);
        return TRUE;

proc_error:
        g_object_unref(json_response_parser);
        // Lets cleanup processing deployment failed
        process_artifact_cleanup(artifact);
        process_deployment_cleanup();
        return FALSE;
}


void hawkbit_init(Config *config, GSourceFunc on_install_ready)
{
        hawkbit_config = config;
        software_ready_cb = on_install_ready;
        curl_global_init(CURL_GLOBAL_ALL);
}

typedef struct ClientData_ {
        GMainLoop *loop;
        gboolean res;
        long hawkbit_interval_check_sec;
        long last_run_sec;
} ClientData;

/**
 * @brief Callback for main loop, should run regularly, polls controller base poll resource and
 * triggers appropriate actions.
 *
 * @param[in] user_data ClientData*
 * @return TRUE if polling controller base resource and running appropriate actions succeeded,
 *         FALSE otherwise
 */
static gboolean hawkbit_pull_cb(gpointer user_data)
{
        ClientData *data = user_data;
        gboolean res = FALSE;
        g_autoptr(GError) error = NULL;
        g_autofree gchar *get_tasks_url = NULL;
        g_autoptr(JsonParser) json_response_parser = NULL;
        JsonNode *json_root = NULL;

        g_return_val_if_fail(user_data, FALSE);

        if (++data->last_run_sec < data->hawkbit_interval_check_sec)
                return G_SOURCE_CONTINUE;

        data->last_run_sec = 0;

        // build hawkBit get tasks URL
        get_tasks_url = build_api_url(NULL);

        g_message("Checking for new software...");
        res = rest_request(GET, get_tasks_url, NULL, &json_response_parser, &error);
        if (!res) {
                if (g_error_matches(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, 401)) {
                        if (hawkbit_config->auth_token)
                                g_warning("Failed to authenticate. Check if auth_token is correct?");
                        if (hawkbit_config->gateway_token)
                                g_warning("Failed to authenticate. Check if gateway_token is correct?");
                } else {
                        g_warning("Scheduled check for new software failed: %s (%d)",
                                  error->message, error->code);
                }

                data->hawkbit_interval_check_sec = hawkbit_config->retry_wait;
                goto out;
        }

        // owned by the JsonParser and should never be modified or freed
        json_root = json_parser_get_root(json_response_parser);

        // get hawkbit sleep time (how often should we check for new software)
        data->hawkbit_interval_check_sec = json_get_sleeptime(json_root);

        if (json_contains(json_root, "$._links.configData")) {
                // hawkBit has asked us to identify ourselves
                res = identify(&error);
                if (!res) {
                        g_warning("%s", error->message);
                        g_clear_error(&error);
                }
        }
        if (json_contains(json_root, "$._links.deploymentBase")) {
                // hawkBit has a new deployment for us
                res = process_deployment(json_root, &error);
                if (!res) {
                        if (g_error_matches(error, RHU_HAWKBIT_CLIENT_ERROR,
                                            RHU_HAWKBIT_CLIENT_ERROR_ALREADY_IN_PROGRESS))
                                g_debug("%s", error->message);
                        else
                                g_warning("%s", error->message);
                }
        } else {
                g_message("No new software.");
        }
        if (json_contains(json_root, "$._links.cancelAction")) {
                //TODO: implement me
                g_warning("cancel action not supported");
        }

out:
        if (run_once) {
                data->res = res ? 0 : 1;
                g_main_loop_quit(data->loop);
                return G_SOURCE_REMOVE;
        }

        return G_SOURCE_CONTINUE;
}

int hawkbit_start_service_sync()
{
        GMainContext *ctx;
        ClientData cdata;
        GSource *timeout_source = NULL;
        int res = 0;

        ctx = g_main_context_new();
        cdata.loop = g_main_loop_new(ctx, FALSE);
        cdata.hawkbit_interval_check_sec = hawkbit_config->retry_wait;
        cdata.last_run_sec = hawkbit_config->retry_wait;

        timeout_source = g_timeout_source_new(1000);   // pull every second
        g_source_set_name(timeout_source, "Add timeout");
        g_source_set_callback(timeout_source, (GSourceFunc) hawkbit_pull_cb, &cdata, NULL);
        g_source_attach(timeout_source, ctx);
        g_source_unref(timeout_source);

#ifdef WITH_SYSTEMD
        GSource *event_source = NULL;
        sd_event *event = NULL;
        res = sd_event_default(&event);
        if (res < 0)
                goto finish;
        // Enable automatic service watchdog support
        res = sd_event_set_watchdog(event, TRUE);
        if (res < 0)
                goto finish;

        event_source = sd_source_new(event);
        if (!event_source) {
                res = -ENOMEM;
                goto finish;
        }

        // attach systemd source to glib mainloop
        res = sd_source_attach(event_source, cdata.loop);
        if (res < 0)
                goto finish;

        sd_notify(0, "READY=1\nSTATUS=Init completed, start polling HawkBit for new software.");
#endif

        g_main_loop_run(cdata.loop);

        res = cdata.res;

#ifdef WITH_SYSTEMD
        sd_notify(0, "STOPPING=1\nSTATUS=Stopped polling HawkBit for new software.");
#endif

#ifdef WITH_SYSTEMD
finish:
        g_source_unref(event_source);
        g_source_destroy(event_source);
        sd_event_set_watchdog(event, FALSE);
        event = sd_event_unref(event);
#endif
        g_main_loop_unref(cdata.loop);
        g_main_context_unref(ctx);
        if (res < 0)
                g_warning("%s", strerror(-res));

        return res;
}

void rest_payload_free(RestPayload *payload)
{
        if (!payload)
                return;

        g_free(payload->payload);
        g_free(payload);
}
