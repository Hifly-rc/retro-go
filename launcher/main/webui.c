#include "rg_system.h"

#ifdef RG_ENABLE_NETWORKING
#include <esp_http_server.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <cJSON.h>
#include <ctype.h>

// static const char webui_html[];
#include "webui.html.h"

static httpd_handle_t server;

static char *urldecode(const char *str)
{
    char *new_string = strdup(str);
    char *ptr = new_string;
    while (*ptr && *(ptr + 1) && *(ptr + 2))
    {
        if (*ptr == '%' && isxdigit(*(ptr + 1)) && isxdigit(*(ptr + 2)))
        {
            char hex[] = {*(ptr + 1), *(ptr + 2), 0};
            *ptr = strtol(hex, NULL, 16);
            memmove(ptr + 1, ptr + 3, strlen(ptr + 3) + 1);
        }
        ptr++;
    }
    return new_string;
}

static esp_err_t http_api_handler(httpd_req_t *req)
{
    char http_buffer[0x1000] = {0};
    esp_err_t ret = ESP_OK;
    bool success = false;
    FILE *fp;

    if (httpd_req_recv(req, http_buffer, sizeof(http_buffer)) <= 0) {
        return ESP_FAIL;
    }

    cJSON *content = cJSON_Parse(http_buffer);
    cJSON *data = NULL;

    if (!content) {
       return ESP_FAIL;
    }

    const char *cmd  = cJSON_GetStringValue(cJSON_GetObjectItem(content, "cmd")) ?: "-";
    const char *arg1 = cJSON_GetStringValue(cJSON_GetObjectItem(content, "arg1")) ?: "";
    const char *arg2 = cJSON_GetStringValue(cJSON_GetObjectItem(content, "arg2")) ?: "";

    if (strcmp(cmd, "list") == 0)
    {
        data = cJSON_CreateArray();
        rg_scandir_t *files = rg_storage_scandir(arg1, NULL, true);
        for (rg_scandir_t *entry = files; entry && entry->is_valid; ++entry)
        {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", entry->name);
            cJSON_AddNumberToObject(obj, "size", entry->size);
            cJSON_AddBoolToObject(obj, "is_dir", entry->is_dir);
            cJSON_AddItemToArray(data, obj);
        }
        success = data && files;
        free(files);
    }
    else if (strcmp(cmd, "rename") == 0)
    {
        success = rename(arg1, arg2) == 0;
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        success = rg_storage_delete(arg1);
    }
    else if (strcmp(cmd, "mkdir") == 0)
    {
        success = rg_storage_mkdir(arg1);
    }
    else if (strcmp(cmd, "touch") == 0)
    {
        success = (fp = fopen(arg1, "wb")) && fclose(fp) == 0;
    }
    else if (strcmp(cmd, "download") == 0)
    {
        if ((fp = fopen(arg1, "rb")))
        {
            const char *ext = rg_extension(arg1);
            if (ext && (strcmp(ext, "json") == 0 || strcmp(ext, "log") == 0))
                httpd_resp_set_type(req, "text/plain");
            else if (ext && strcmp(ext, "png") == 0)
                httpd_resp_set_type(req, "image/png");
            else
                httpd_resp_set_type(req, "application/binary");

            for (size_t size; (size = fread(http_buffer, 1, sizeof(http_buffer), fp));)
            {
                httpd_resp_send_chunk(req, http_buffer, size);
            }

            httpd_resp_send_chunk(req, NULL, 0);
            fclose(fp);

            RG_LOGI("File transfer complete: %s\n", arg1);
            goto cleanup;
        }

        RG_LOGE("File transfer failed: %s", arg1);
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Send JSON response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "cmd", cmd);
    cJSON_AddItemToObject(response, "data", data);
    cJSON_AddBoolToObject(response, "success", success);
    char *response_text = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_text);
    free(response_text);
    cJSON_free(response);

cleanup:
    cJSON_free(content);
    return ret;
}

static esp_err_t http_upload_handler(httpd_req_t *req)
{
    char http_buffer[0x1000];
    char *filename = urldecode(req->uri);

    RG_LOGI("Receiving file: %s", filename);

    FILE *fp = fopen(filename, "wb");
    if (!fp)
        return ESP_FAIL;

    size_t received = 0;

    while (received < req->content_len)
    {
        int length = httpd_req_recv(req, http_buffer, sizeof(http_buffer));
        if (length <= 0)
            break;
        if (!fwrite(http_buffer, length, 1, fp))
        {
            RG_LOGI("Write failure at %d bytes", received);
            break;
        }
        rg_task_delay(0);
        received += length;
    }

    fclose(fp);
    free(filename);

    if (received < req->content_len)
    {
        RG_LOGE("Received %d/%d bytes", received, req->content_len);
        httpd_resp_sendstr(req, "ERROR");
        return ESP_FAIL;
    }

    RG_LOGI("Received %d/%d bytes", received, req->content_len);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t http_get_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, webui_html);
    return ESP_OK;
}

void webui_stop(void)
{
    if (!server) // Already stopped
        return;

    httpd_stop(server);
    server = NULL;
}

void webui_start(void)
{
    if (server) // Already started
        return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12 * 1024;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = http_get_handler,
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/api",
        .method    = HTTP_POST,
        .handler   = http_api_handler,
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/*",
        .method    = HTTP_PUT,
        .handler   = http_upload_handler,
    });

    RG_LOGI("File server started");
}

#endif
