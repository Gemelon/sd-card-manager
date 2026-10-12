#include "sd_card_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "web_interface.h"

extern const unsigned char sd_html_start[] asm("_binary_sd_card_html_start");
extern const unsigned char sd_html_end[] asm("_binary_sd_card_html_end");
extern const unsigned char sd_js_start[] asm("_binary_sd_card_js_start");
extern const unsigned char sd_js_end[] asm("_binary_sd_card_js_end");

#define SD_MOUNT CONFIG_SD_CARD_MANAGER_MOUNT_POINT
#define MAX_PATH 240
#define MAX_REQUEST 2048
#define PAGE_SIZE 48
#define IO_CHUNK 4096
#define CARD_POLL_INTERVAL_MS 250
#define CARD_DETECT_STABLE_SAMPLES 4

static const char *TAG = "sd_card";
static SemaphoreHandle_t storage_lock;
static sdmmc_card_t *card;
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
static bool spi_bus_initialized;
#endif
static sd_card_manager_status_t status;
static bool card_present;
static bool card_detect_candidate;
static unsigned card_detect_samples;

static bool refresh_free_space_locked(void)
{
    uint64_t total_bytes;
    uint64_t free_bytes;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        status.free_space_known = false;
        status.free_bytes = 0;
        ESP_LOGW(TAG, "Cannot query SD free space: %s", esp_err_to_name(err));
        return false;
    }
    status.total_bytes = total_bytes;
    status.free_bytes = free_bytes;
    status.free_space_known = true;
    return true;
}

static bool card_detected(void)
{
    return gpio_get_level(CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT) == 1;
}

static bool update_card_detect_locked(void)
{
    bool detected = card_detected();
    if (detected == card_present) {
        card_detect_candidate = detected;
        card_detect_samples = 0;
    } else if (detected != card_detect_candidate) {
        card_detect_candidate = detected;
        card_detect_samples = 1;
    } else if (++card_detect_samples >= CARD_DETECT_STABLE_SAMPLES) {
        card_present = card_detect_candidate;
        card_detect_samples = 0;
        ESP_LOGI(TAG, "Card detect GPIO %d is now %s",
                 CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT,
                 card_present ? "HIGH (inserted)" : "LOW (removed)");
    }
    return card_present;
}

static void mark_card_unavailable(const char *message)
{
    status.mounted = false;
    status.free_space_known = false;
    status.total_bytes = 0;
    status.free_bytes = 0;
    strlcpy(status.message, message, sizeof(status.message));
}

static bool unmount_card_locked(void)
{
    if (card == NULL) {
        return true;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot unmount SD card: %s", esp_err_to_name(err));
        return false;
    }
    card = NULL;
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
    if (spi_bus_initialized) {
        err = spi_bus_free(SDSPI_DEFAULT_HOST);
        if (err == ESP_OK) {
            spi_bus_initialized = false;
        } else {
            ESP_LOGW(TAG, "Cannot release SD SPI bus: %s", esp_err_to_name(err));
        }
    }
#endif
    return true;
}

static esp_err_t send_error(httpd_req_t *req, const char *http_status, const char *message)
{
    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    ESP_LOGW(TAG, "%s %s: %s", http_status, req->uri, message);
    return httpd_resp_sendstr(req, message);
}

static bool safe_relative_path(const char *path)
{
    if (path == NULL || path[0] != '/' || strlen(path) > MAX_PATH ||
        strchr(path, '\\') != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == ':' || *p == '?' || *p == '*' ||
            *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            return false;
        }
    }
    if (strcmp(path, "/") == 0) {
        return true;
    }
    const char *segment = path + 1;
    for (const char *p = segment;; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t length = (size_t)(p - segment);
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*p == '\0') {
                return true;
            }
            segment = p + 1;
        }
    }
}

static bool full_path(const char *relative, char *out, size_t out_size)
{
    if (!safe_relative_path(relative)) {
        return false;
    }
    int written = snprintf(out, out_size, "%s%s", SD_MOUNT, relative);
    return written > 0 && (size_t)written < out_size;
}

static esp_err_t decode_query_value(httpd_req_t *req, const char *key, char *out, size_t capacity)
{
    size_t query_length = httpd_req_get_url_query_len(req);
    if (query_length == 0 || query_length > MAX_REQUEST) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *query = malloc(query_length + 1);
    if (query == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = httpd_req_get_url_query_str(req, query, query_length + 1);
    if (err == ESP_OK) {
        char encoded[MAX_PATH * 3 + 1];
        err = httpd_query_key_value(query, key, encoded, sizeof(encoded));
        if (err == ESP_OK) {
            size_t written = 0;
            for (size_t i = 0; encoded[i] != '\0'; ++i) {
                unsigned char value = (unsigned char)encoded[i];
                if (value == '+') {
                    value = ' ';
                } else if (value == '%') {
                    if (!isxdigit((unsigned char)encoded[i + 1]) ||
                        !isxdigit((unsigned char)encoded[i + 2])) {
                        err = ESP_ERR_INVALID_ARG;
                        break;
                    }
                    char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
                    value = (unsigned char)strtoul(hex, NULL, 16);
                    i += 2;
                }
                if (value == '\0' || written + 1 >= capacity) {
                    err = ESP_ERR_INVALID_SIZE;
                    break;
                }
                out[written++] = (char)value;
            }
            if (err == ESP_OK) {
                out[written] = '\0';
            }
        }
    }
    free(query);
    return err;
}

static esp_err_t receive_json(httpd_req_t *req, cJSON **json)
{
    if (req->content_len == 0 || req->content_len > MAX_REQUEST) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    *json = cJSON_ParseWithLengthOpts(body, received + 1, NULL, true);
    memset(body, 0, received);
    free(body);
    return cJSON_IsObject(*json) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t mutation_error(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "File or folder not found.");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Destination already exists or folder is not empty.");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, "400 Bad Request", "Invalid path, name or request.");
    }
    ESP_LOGE(TAG, "SD operation failed: %s (errno %d)", esp_err_to_name(err), errno);
    return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static bool lock_storage(httpd_req_t *req)
{
    if (xSemaphoreTake(storage_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        send_error(req, "503 Service Unavailable", "SD card is busy.");
        return false;
    }
    if (!status.mounted) {
        char message[sizeof(status.message)];
        strlcpy(message, card_present ? status.message : "No SD card detected.",
                sizeof(message));
        xSemaphoreGive(storage_lock);
        send_error(req, "503 Service Unavailable", message);
        return false;
    }
    return true;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    if (xSemaphoreTake(storage_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return send_error(req, "503 Service Unavailable", "SD status is busy.");
    }
    sd_card_manager_status_t current = status;
    bool detected = card_present;
    int detect_level = gpio_get_level(CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT);
    xSemaphoreGive(storage_lock);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error(req, "500 Internal Server Error", "Not enough memory.");
    }
    bool ok = cJSON_AddBoolToObject(root, "mounted", current.mounted) &&
              cJSON_AddBoolToObject(root, "cardDetected", detected) &&
              cJSON_AddNumberToObject(root, "cardDetectLevel", detect_level) &&
              cJSON_AddNumberToObject(root, "cardDetectGpio",
                                      CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT) &&
              cJSON_AddBoolToObject(root, "freeSpaceKnown", current.free_space_known) &&
              cJSON_AddNumberToObject(root, "totalBytes", (double)current.total_bytes) &&
              cJSON_AddNumberToObject(root, "freeBytes", (double)current.free_bytes) &&
              cJSON_AddStringToObject(root, "message", current.message);
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (body == NULL) {
        return send_error(req, "500 Internal Server Error", "Could not create SD status.");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static int entry_compare(const void *left, const void *right)
{
    const cJSON *a = *(const cJSON *const *)left;
    const cJSON *b = *(const cJSON *const *)right;
    bool a_dir = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)a, "directory"));
    bool b_dir = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)b, "directory"));
    if (a_dir != b_dir) {
        return a_dir ? -1 : 1;
    }
    const char *a_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)a, "name"));
    const char *b_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)b, "name"));
    return strcasecmp(a_name, b_name);
}

static esp_err_t list_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    char relative[MAX_PATH + 1] = {0};
    esp_err_t err = decode_query_value(req, "path", relative, sizeof(relative));
    if (err != ESP_OK || !safe_relative_path(relative)) {
        return send_error(req, "400 Bad Request", "Invalid or missing directory path.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Path too long.");
    }
    unsigned offset = 0;
    char offset_encoded[16];
    if (httpd_req_get_url_query_len(req) != 0) {
        char query[512];
        size_t length = httpd_req_get_url_query_len(req);
        if (length >= sizeof(query) ||
            httpd_req_get_url_query_str(req, query, length + 1) != ESP_OK) {
            return send_error(req, "400 Bad Request", "Query too long.");
        }
        if (httpd_query_key_value(query, "offset", offset_encoded, sizeof(offset_encoded)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(offset_encoded, &end, 10);
            if (*end != '\0' || parsed > 1000000) {
                return send_error(req, "400 Bad Request", "Invalid offset.");
            }
            offset = (unsigned)parsed;
        }
    }
    if (!lock_storage(req)) return ESP_OK;
    struct stat directory_stat;
    if (stat(path, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode)) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "404 Not Found", "Folder not found.");
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Could not read folder.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        closedir(directory);
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Not enough memory.");
    }
    unsigned skipped = 0;
    unsigned scanned = 0;
    bool has_more = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        if (scanned == PAGE_SIZE) {
            has_more = true;
            break;
        }
        char child[MAX_PATH + sizeof(SD_MOUNT) + 2];
        int size = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat info;
        if (size <= 0 || (size_t)size >= sizeof(child) || stat(child, &info) != 0) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (item == NULL ||
            !cJSON_AddStringToObject(item, "name", entry->d_name) ||
            !cJSON_AddBoolToObject(item, "directory", S_ISDIR(info.st_mode)) ||
            !cJSON_AddNumberToObject(item, "size", S_ISDIR(info.st_mode) ? 0 : (double)info.st_size) ||
            !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(items);
            cJSON_Delete(root);
            closedir(directory);
            xSemaphoreGive(storage_lock);
            return send_error(req, "500 Internal Server Error", "Could not create directory entry.");
        }
        ++scanned;
    }
    closedir(directory);
    xSemaphoreGive(storage_lock);
    if (items->child != NULL) {
        cJSON *sorted[PAGE_SIZE];
        size_t count = 0;
        for (cJSON *item = items->child; item != NULL; item = item->next) {
            sorted[count++] = item;
        }
        qsort(sorted, count, sizeof(sorted[0]), entry_compare);
        cJSON *ordered = cJSON_CreateArray();
        if (ordered == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(root);
            return send_error(req, "500 Internal Server Error", "Not enough memory for sorting.");
        }
        for (size_t i = 0; i < count; ++i) {
            cJSON_DetachItemViaPointer(items, sorted[i]);
            if (!cJSON_AddItemToArray(ordered, sorted[i])) {
                cJSON_Delete(sorted[i]);
                cJSON_Delete(ordered);
                cJSON_Delete(items);
                cJSON_Delete(root);
                return send_error(req, "500 Internal Server Error", "Could not store sorted entries.");
            }
        }
        cJSON_Delete(items);
        items = ordered;
    }
    bool ok = cJSON_AddStringToObject(root, "path", relative) &&
              cJSON_AddBoolToObject(root, "hasMore", has_more) &&
              cJSON_AddNumberToObject(root, "nextOffset", offset + scanned);
    bool items_added = ok && cJSON_AddItemToObject(root, "items", items);
    ok = ok && items_added;
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    if (!items_added) {
        cJSON_Delete(items);
    }
    cJSON_Delete(root);
    if (body == NULL) {
        return send_error(req, "500 Internal Server Error", "Could not serialize directory.");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t mutation_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    cJSON *json = NULL;
    esp_err_t err = receive_json(req, &json);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        if (err == ESP_ERR_NO_MEM) {
            return send_error(req, "500 Internal Server Error", "Not enough memory for JSON request.");
        }
        return send_error(req, err == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large" :
                                                             "400 Bad Request",
                          "JSON request missing, too large or invalid.");
    }
    if (!lock_storage(req)) {
        cJSON_Delete(json);
        return ESP_OK;
    }
    const cJSON *path_item = cJSON_GetObjectItemCaseSensitive(json, "path");
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON *source_item = cJSON_GetObjectItemCaseSensitive(json, "source");
    const cJSON *destination_item = cJSON_GetObjectItemCaseSensitive(json, "destination");
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    char other[MAX_PATH + sizeof(SD_MOUNT) + 2];
    err = ESP_ERR_INVALID_ARG;
    bool valid = false;
    if (strcmp(req->uri, "/api/sd/mkdir") == 0 && cJSON_IsString(path_item)) {
        valid = full_path(path_item->valuestring, path, sizeof(path));
        if (valid) {
            err = mkdir(path, 0775) == 0 ? ESP_OK :
                  (errno == EEXIST ? ESP_ERR_INVALID_STATE :
                   errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL);
        }
    } else if (strcmp(req->uri, "/api/sd/create") == 0 &&
               cJSON_IsString(path_item) && cJSON_IsString(name_item)) {
        char relative[MAX_PATH + 1];
        const char *parent = path_item->valuestring;
        const char *name = name_item->valuestring;
        int written = snprintf(relative, sizeof(relative), "%s%s%s",
                               strcmp(parent, "/") == 0 ? "" : parent,
                               strcmp(parent, "/") == 0 ? "/" : "/", name);
        valid = written > 0 && (size_t)written < sizeof(relative) &&
                full_path(relative, path, sizeof(path));
        if (valid) {
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
            err = fd >= 0 ? ESP_OK :
                  errno == EEXIST ? ESP_ERR_INVALID_STATE :
                  errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            if (fd >= 0 && close(fd) != 0) err = ESP_FAIL;
        }
    } else if (strcmp(req->uri, "/api/sd/delete") == 0 && cJSON_IsString(path_item)) {
        valid = full_path(path_item->valuestring, path, sizeof(path)) &&
                strcmp(path_item->valuestring, "/") != 0;
        if (valid) {
            struct stat info;
            if (stat(path, &info) != 0) {
                err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            } else {
                int result = S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path);
                err = result == 0 ? ESP_OK :
                      (errno == ENOTEMPTY || errno == EEXIST) ? ESP_ERR_INVALID_STATE :
                      errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            }
        }
    } else if (strcmp(req->uri, "/api/sd/move") == 0 &&
               cJSON_IsString(source_item) && cJSON_IsString(destination_item)) {
        valid = full_path(source_item->valuestring, path, sizeof(path)) &&
                full_path(destination_item->valuestring, other, sizeof(other)) &&
                strcmp(source_item->valuestring, "/") != 0;
        if (valid) {
            struct stat info;
            if (stat(path, &info) != 0) {
                err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            } else if (S_ISDIR(info.st_mode) &&
                       strncmp(destination_item->valuestring, source_item->valuestring,
                               strlen(source_item->valuestring)) == 0 &&
                       destination_item->valuestring[strlen(source_item->valuestring)] == '/') {
                err = ESP_ERR_INVALID_ARG;
            } else if (stat(other, &info) == 0) {
                err = ESP_ERR_INVALID_STATE;
            } else if (errno != ENOENT) {
                err = ESP_FAIL;
            } else {
                err = rename(path, other) == 0 ? ESP_OK :
                      errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            }
        }
    }
    xSemaphoreGive(storage_lock);
    cJSON_Delete(json);
    if (!valid || err != ESP_OK) {
        return mutation_error(req, err);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t download_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    char relative[MAX_PATH + 1] = {0};
    if (decode_query_value(req, "path", relative, sizeof(relative)) != ESP_OK ||
        !safe_relative_path(relative) || strcmp(relative, "/") == 0) {
        return send_error(req, "400 Bad Request", "Invalid file path.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Path too long.");
    }
    if (!lock_storage(req)) return ESP_OK;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "404 Not Found", "File not found.");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Could not open file.");
    }
    const char *filename = strrchr(relative, '/') + 1;
    char disposition[MAX_PATH + 64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    unsigned char buffer[IO_CHUNK];
    size_t count;
    esp_err_t err = ESP_OK;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        err = httpd_resp_send_chunk(req, (const char *)buffer, count);
        if (err != ESP_OK) {
            break;
        }
    }
    if (ferror(file) && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    xSemaphoreGive(storage_lock);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    if (req->content_len <= 0) {
        return send_error(req, "400 Bad Request", "Empty file: please create an empty file.");
    }
    char relative[MAX_PATH + 1] = {0};
    if (decode_query_value(req, "path", relative, sizeof(relative)) != ESP_OK ||
        !safe_relative_path(relative) || strcmp(relative, "/") == 0) {
        return send_error(req, "400 Bad Request", "Invalid destination path.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    char temporary[MAX_PATH + sizeof(SD_MOUNT) + 40];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Path too long.");
    }
    snprintf(temporary, sizeof(temporary), "%s.upload-%08" PRIx32, path, esp_random());
    if (!lock_storage(req)) return ESP_OK;
    struct stat target;
    if (stat(path, &target) == 0) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "409 Conflict", "Destination file already exists.");
    }
    int target_errno = errno;
    if (target_errno != ENOENT) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Could not inspect destination file.");
    }
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t space_err = esp_vfs_fat_info(SD_MOUNT, &total_bytes, &free_bytes);
    bool space_available = space_err == ESP_OK;
    if (space_available) {
        status.total_bytes = total_bytes;
        status.free_bytes = free_bytes;
        status.free_space_known = true;
        if ((uint64_t)req->content_len > status.free_bytes) {
            xSemaphoreGive(storage_lock);
            return send_error(req, "507 Insufficient Storage", "Not enough free space.");
        }
    } else {
        status.free_space_known = false;
        status.free_bytes = 0;
        ESP_LOGW(TAG, "Free-space query failed; attempting upload write anyway: %s",
                 esp_err_to_name(space_err));
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0664);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (file == NULL) {
        int open_errno = errno;
        if (fd >= 0) {
            close(fd);
            unlink(temporary);
        }
        xSemaphoreGive(storage_lock);
        if (open_errno == ENOSPC) {
            return send_error(req, "507 Insufficient Storage",
                              "The SD card reports insufficient space.");
        }
        ESP_LOGE(TAG, "Cannot create upload file %s (errno %d)", temporary, open_errno);
        return send_error(req, "500 Internal Server Error", "Could not create temporary upload file.");
    }
    unsigned char buffer[IO_CHUNK];
    size_t received = 0;
    esp_err_t err = ESP_OK;
    int io_errno = 0;
    while (received < (size_t)req->content_len) {
        size_t wanted = (size_t)req->content_len - received;
        if (wanted > sizeof(buffer)) {
            wanted = sizeof(buffer);
        }
        int count = httpd_req_recv(req, (char *)buffer, wanted);
        if (count <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (fwrite(buffer, 1, (size_t)count, file) != (size_t)count) {
            err = ESP_FAIL;
            io_errno = errno;
            break;
        }
        received += (size_t)count;
    }
    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
        io_errno = errno;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
        io_errno = errno;
    }
    if (err == ESP_OK) {
        struct stat target;
        if (stat(path, &target) == 0) {
            err = ESP_ERR_INVALID_STATE;
        } else if (errno != ENOENT || rename(temporary, path) != 0) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        if (unlink(temporary) != 0) {
            ESP_LOGW(TAG, "Cannot remove incomplete upload file %s (errno %d)", temporary, errno);
        }
    } else {
        refresh_free_space_locked();
    }
    xSemaphoreGive(storage_lock);
    if (err != ESP_OK) {
        if (io_errno == ENOSPC) {
            return send_error(req, "507 Insufficient Storage", "The SD card reports insufficient space.");
        }
        return mutation_error(req, err);
    }
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_sendstr(req, "File uploaded.");
}

static esp_err_t mount_card(void)
{
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_SD_CARD_MANAGER_SPI_FREQ_KHZ;
    spi_bus_config_t bus_config = {
        .mosi_io_num = CONFIG_SD_CARD_MANAGER_GPIO_SPI_MOSI,
        .miso_io_num = CONFIG_SD_CARD_MANAGER_GPIO_SPI_MISO,
        .sclk_io_num = CONFIG_SD_CARD_MANAGER_GPIO_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = IO_CHUNK,
    };
    if (!spi_bus_initialized) {
        esp_err_t bus_err = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);
        if (bus_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot initialize SD SPI bus: %s", esp_err_to_name(bus_err));
            return bus_err;
        }
        spi_bus_initialized = true;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_SD_CARD_MANAGER_GPIO_SPI_CS;
    slot.host_id = host.slot;
#else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_SD_CARD_MANAGER_SDMMC_FREQ_KHZ;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = CONFIG_SD_CARD_MANAGER_GPIO_CLK;
    slot.cmd = CONFIG_SD_CARD_MANAGER_GPIO_CMD;
    slot.d0 = CONFIG_SD_CARD_MANAGER_GPIO_D0;
    slot.d1 = CONFIG_SD_CARD_MANAGER_GPIO_D1;
    slot.d2 = CONFIG_SD_CARD_MANAGER_GPIO_D2;
    slot.d3 = CONFIG_SD_CARD_MANAGER_GPIO_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };
    sdmmc_card_t *mounted_card = NULL;
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &config, &mounted_card);
#else
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &config, &mounted_card);
#endif
    if (err != ESP_OK) {
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
        esp_err_t free_err = spi_bus_free(host.slot);
        if (free_err == ESP_OK) {
            spi_bus_initialized = false;
        } else {
            ESP_LOGW(TAG, "Cannot release SD SPI bus after mount failure: %s",
                     esp_err_to_name(free_err));
        }
#endif
        char message[128];
        snprintf(message, sizeof(message), "SD card unavailable: %s. Nothing was formatted.",
                 esp_err_to_name(err));
        status.mounted = false;
        status.free_space_known = false;
        status.total_bytes = 0;
        status.free_bytes = 0;
        strlcpy(status.message, message, sizeof(status.message));
        ESP_LOGW(TAG, "%s", message);
        return err;
    }
    card = mounted_card;
    status.total_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    status.mounted = true;
    status.free_space_known = false;
    refresh_free_space_locked();
    strlcpy(status.message, "SD card ready.", sizeof(status.message));
    sdmmc_card_print_info(stdout, card);
#if CONFIG_SD_CARD_MANAGER_BUS_SPI
    ESP_LOGI(TAG, "Mounted SD card at %s over SPI (MISO=%d, MOSI=%d, CS=%d, CLK=%d, %d kHz)",
             SD_MOUNT, CONFIG_SD_CARD_MANAGER_GPIO_SPI_MISO,
             CONFIG_SD_CARD_MANAGER_GPIO_SPI_MOSI, CONFIG_SD_CARD_MANAGER_GPIO_SPI_CS,
             CONFIG_SD_CARD_MANAGER_GPIO_SPI_CLK, host.max_freq_khz);
#else
    ESP_LOGI(TAG, "Mounted SD card at %s with 4-bit SDMMC at %d kHz",
             SD_MOUNT, host.max_freq_khz);
#endif
    return ESP_OK;
}

static void retry_mount_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CARD_POLL_INTERVAL_MS));
        if (xSemaphoreTake(storage_lock, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Cannot acquire SD storage lock in retry task");
            continue;
        }
        bool present = update_card_detect_locked();
        if (!present) {
            if (card == NULL || unmount_card_locked()) {
                mark_card_unavailable("No SD card detected.");
            } else {
                mark_card_unavailable("SD card removed; retrying release.");
            }
        } else if (card == NULL) {
            mount_card();
        } else if (!status.mounted) {
            if (unmount_card_locked()) {
                mount_card();
            }
        }
        xSemaphoreGive(storage_lock);
    }
}

static const httpd_uri_t routes[] = {
    {.uri = "/api/sd/status", .method = HTTP_GET, .handler = status_handler},
    {.uri = "/api/sd/list", .method = HTTP_GET, .handler = list_handler},
    {.uri = "/api/sd/download", .method = HTTP_GET, .handler = download_handler},
    {.uri = "/api/sd/mkdir", .method = HTTP_POST, .handler = mutation_handler},
    {.uri = "/api/sd/create", .method = HTTP_POST, .handler = mutation_handler},
    {.uri = "/api/sd/delete", .method = HTTP_POST, .handler = mutation_handler},
    {.uri = "/api/sd/move", .method = HTTP_POST, .handler = mutation_handler},
    {.uri = "/api/sd/upload", .method = HTTP_POST, .handler = upload_handler},
};

static web_interface_extension_t extension = {
};

esp_err_t sd_card_manager_init(void)
{
    extension.id = "sd-card";
    extension.label = "SD card";
    extension.content = sd_html_start;
    extension.content_size = (size_t)(sd_html_end - sd_html_start);
    extension.script = sd_js_start;
    extension.script_size = (size_t)(sd_js_end - sd_js_start);
    extension.routes = routes;
    extension.route_count = sizeof(routes) / sizeof(routes[0]);
    storage_lock = xSemaphoreCreateMutex();
    if (storage_lock == NULL) {
        ESP_LOGE(TAG, "Cannot create SD storage mutex");
        return ESP_ERR_NO_MEM;
    }
    const gpio_config_t detect_config = {
        .pin_bit_mask = 1ULL << CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&detect_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot configure SD card-detect GPIO: %s", esp_err_to_name(err));
        vSemaphoreDelete(storage_lock);
        storage_lock = NULL;
        return err;
    }
    strlcpy(status.message, "Waiting for SD card detection.", sizeof(status.message));
    err = web_interface_register_extension(&extension);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register SD UI extension: %s", esp_err_to_name(err));
        vSemaphoreDelete(storage_lock);
        storage_lock = NULL;
        return err;
    }
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    card_present = card_detected();
    card_detect_candidate = card_present;
    if (card_present) {
        mount_card();
    } else {
        mark_card_unavailable("No SD card detected.");
    }
    xSemaphoreGive(storage_lock);
    if (xTaskCreate(retry_mount_task, "sd_mount_retry", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create SD retry task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sd_card_manager_get_status(sd_card_manager_status_t *out)
{
    if (out == NULL) {
        ESP_LOGE(TAG, "SD status output pointer is NULL");
        return;
    }
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(storage_lock);
}

esp_err_t sd_card_manager_storage_acquire(TickType_t timeout_ticks)
{
    if (storage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(storage_lock, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!status.mounted) {
        xSemaphoreGive(storage_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void sd_card_manager_storage_release(void)
{
    if (storage_lock == NULL || xSemaphoreGive(storage_lock) != pdTRUE) {
        ESP_LOGE(TAG, "Cannot release SD storage lock");
    }
}
