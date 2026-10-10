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
#include <sys/statvfs.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
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
#define SD_STATUS_FAILURE_LIMIT 3

static const char *TAG = "sd_card";
static SemaphoreHandle_t storage_lock;
static sdmmc_card_t *card;
static sd_card_manager_status_t status;
static unsigned consecutive_status_failures;

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
        return send_error(req, "404 Not Found", "Datei oder Ordner nicht gefunden.");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Ziel existiert bereits oder Ordner ist nicht leer.");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, "400 Bad Request", "Ungueltiger Pfad, Name oder Request.");
    }
    ESP_LOGE(TAG, "SD operation failed: %s (errno %d)", esp_err_to_name(err), errno);
    return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static bool lock_storage(httpd_req_t *req)
{
    if (xSemaphoreTake(storage_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        send_error(req, "503 Service Unavailable", "SD-Karte ist beschaeftigt.");
        return false;
    }
    if (!status.mounted) {
        char message[sizeof(status.message)];
        strlcpy(message, status.message, sizeof(message));
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
    sd_card_manager_status_t current;
    sd_card_manager_get_status(&current);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error(req, "500 Internal Server Error", "Nicht genug Speicher.");
    }
    bool ok = cJSON_AddBoolToObject(root, "mounted", current.mounted) &&
              cJSON_AddNumberToObject(root, "totalBytes", (double)current.total_bytes) &&
              cJSON_AddNumberToObject(root, "freeBytes", (double)current.free_bytes) &&
              cJSON_AddStringToObject(root, "message", current.message);
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (body == NULL) {
        return send_error(req, "500 Internal Server Error", "SD-Status konnte nicht erstellt werden.");
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
        return send_error(req, "400 Bad Request", "Ungueltiger oder fehlender Verzeichnispfad.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Pfad zu lang.");
    }
    unsigned offset = 0;
    char offset_encoded[16];
    if (httpd_req_get_url_query_len(req) != 0) {
        char query[512];
        size_t length = httpd_req_get_url_query_len(req);
        if (length >= sizeof(query) ||
            httpd_req_get_url_query_str(req, query, length + 1) != ESP_OK) {
            return send_error(req, "400 Bad Request", "Query zu lang.");
        }
        if (httpd_query_key_value(query, "offset", offset_encoded, sizeof(offset_encoded)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(offset_encoded, &end, 10);
            if (*end != '\0' || parsed > 1000000) {
                return send_error(req, "400 Bad Request", "Ungueltiger Offset.");
            }
            offset = (unsigned)parsed;
        }
    }
    if (!lock_storage(req)) return ESP_OK;
    struct stat directory_stat;
    if (stat(path, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode)) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "404 Not Found", "Ordner nicht gefunden.");
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Ordner konnte nicht gelesen werden.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        closedir(directory);
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Nicht genug Speicher.");
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
            return send_error(req, "500 Internal Server Error", "Verzeichniseintrag konnte nicht erstellt werden.");
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
            return send_error(req, "500 Internal Server Error", "Sortierspeicher fehlt.");
        }
        for (size_t i = 0; i < count; ++i) {
            cJSON_DetachItemViaPointer(items, sorted[i]);
            if (!cJSON_AddItemToArray(ordered, sorted[i])) {
                cJSON_Delete(sorted[i]);
                cJSON_Delete(ordered);
                cJSON_Delete(items);
                cJSON_Delete(root);
                return send_error(req, "500 Internal Server Error", "Sortierte Eintraege konnten nicht gespeichert werden.");
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
        return send_error(req, "500 Internal Server Error", "Verzeichnis konnte nicht serialisiert werden.");
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
            return send_error(req, "500 Internal Server Error", "Nicht genug Speicher fuer JSON-Request.");
        }
        return send_error(req, err == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large" :
                                                             "400 Bad Request",
                          "JSON-Request fehlt, ist zu gross oder ungueltig.");
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
        return send_error(req, "400 Bad Request", "Ungueltiger Dateipfad.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Pfad zu lang.");
    }
    if (!lock_storage(req)) return ESP_OK;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "404 Not Found", "Datei nicht gefunden.");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Datei konnte nicht geoeffnet werden.");
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
        return send_error(req, "400 Bad Request", "Leere Datei: bitte leere Datei anlegen.");
    }
    char relative[MAX_PATH + 1] = {0};
    if (decode_query_value(req, "path", relative, sizeof(relative)) != ESP_OK ||
        !safe_relative_path(relative) || strcmp(relative, "/") == 0) {
        return send_error(req, "400 Bad Request", "Ungueltiger Zielpfad.");
    }
    char path[MAX_PATH + sizeof(SD_MOUNT) + 2];
    char temporary[MAX_PATH + sizeof(SD_MOUNT) + 40];
    if (!full_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "Pfad zu lang.");
    }
    snprintf(temporary, sizeof(temporary), "%s.upload-%08" PRIx32, path, esp_random());
    if (!lock_storage(req)) return ESP_OK;
    struct statvfs space;
    if (stat(path, &(struct stat){0}) == 0) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "409 Conflict", "Zieldatei existiert bereits.");
    }
    if (errno != ENOENT || statvfs(SD_MOUNT, &space) != 0 ||
        (uint64_t)req->content_len > (uint64_t)space.f_bavail * space.f_frsize) {
        xSemaphoreGive(storage_lock);
        return send_error(req, "507 Insufficient Storage", "Nicht genug freier Speicher oder Karte nicht verfuegbar.");
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0664);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (file == NULL) {
        if (fd >= 0) {
            close(fd);
            unlink(temporary);
        }
        xSemaphoreGive(storage_lock);
        return send_error(req, "500 Internal Server Error", "Temporere Upload-Datei konnte nicht angelegt werden.");
    }
    unsigned char buffer[IO_CHUNK];
    size_t received = 0;
    esp_err_t err = ESP_OK;
    while (received < (size_t)req->content_len) {
        size_t wanted = (size_t)req->content_len - received;
        if (wanted > sizeof(buffer)) {
            wanted = sizeof(buffer);
        }
        int count = httpd_req_recv(req, (char *)buffer, wanted);
        if (count <= 0 || fwrite(buffer, 1, count > 0 ? (size_t)count : 0, file) !=
                          (count > 0 ? (size_t)count : 0)) {
            err = ESP_FAIL;
            break;
        }
        received += (size_t)count;
    }
    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
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
        unlink(temporary);
    }
    xSemaphoreGive(storage_lock);
    if (err != ESP_OK) {
        return mutation_error(req, err);
    }
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_sendstr(req, "Datei hochgeladen.");
}

static esp_err_t mount_card(void)
{
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
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };
    sdmmc_card_t *mounted_card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &config, &mounted_card);
    if (err != ESP_OK) {
        char message[128];
        snprintf(message, sizeof(message), "SD-Karte nicht verfuegbar: %s. Es wurde nichts formatiert.",
                 esp_err_to_name(err));
        status.mounted = false;
        status.total_bytes = 0;
        status.free_bytes = 0;
        strlcpy(status.message, message, sizeof(status.message));
        ESP_LOGW(TAG, "%s", message);
        return err;
    }
    card = mounted_card;
    status.total_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    struct statvfs space;
    if (statvfs(SD_MOUNT, &space) != 0) {
        ESP_LOGW(TAG, "Mounted SD card but cannot query free space (errno %d)", errno);
        status.free_bytes = 0;
        consecutive_status_failures = 1;
        snprintf(status.message, sizeof(status.message),
                 "Karte eingebunden; freier Speicher derzeit nicht lesbar.");
    } else {
        status.free_bytes = (uint64_t)space.f_bavail * space.f_frsize;
        consecutive_status_failures = 0;
        strlcpy(status.message, "SD-Karte bereit.", sizeof(status.message));
    }
    status.mounted = true;
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "Mounted SD card at %s with 4-bit SDMMC at %d kHz",
             SD_MOUNT, host.max_freq_khz);
    return ESP_OK;
}

static void retry_mount_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (xSemaphoreTake(storage_lock, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Cannot acquire SD storage lock in retry task");
            continue;
        }
        if (!status.mounted) {
            mount_card();
        } else {
            struct statvfs space;
            if (statvfs(SD_MOUNT, &space) != 0) {
                int status_errno = errno;
                ++consecutive_status_failures;
                ESP_LOGW(TAG, "SD status check failed (%u/%u, errno %d)",
                         consecutive_status_failures, SD_STATUS_FAILURE_LIMIT, status_errno);
                if (consecutive_status_failures < SD_STATUS_FAILURE_LIMIT) {
                    snprintf(status.message, sizeof(status.message),
                             "SD-Karte prueft Speicher erneut (%u/%u).",
                             consecutive_status_failures, SD_STATUS_FAILURE_LIMIT);
                } else {
                    esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
                    if (unmount_err != ESP_OK) {
                        ESP_LOGE(TAG, "Cannot unmount unavailable SD card: %s",
                                 esp_err_to_name(unmount_err));
                    }
                    card = NULL;
                    status.mounted = false;
                    status.total_bytes = 0;
                    status.free_bytes = 0;
                    consecutive_status_failures = 0;
                    strlcpy(status.message, "SD-Karte getrennt oder nicht lesbar.",
                            sizeof(status.message));
                }
            } else {
                status.free_bytes = (uint64_t)space.f_bavail * space.f_frsize;
                consecutive_status_failures = 0;
                strlcpy(status.message, "SD-Karte bereit.", sizeof(status.message));
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
    extension.label = "SD-Karte";
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
    strlcpy(status.message, "Warte auf SD-Karte.", sizeof(status.message));
    esp_err_t err = web_interface_register_extension(&extension);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register SD UI extension: %s", esp_err_to_name(err));
        vSemaphoreDelete(storage_lock);
        storage_lock = NULL;
        return err;
    }
    /* A missing/unformatted card is a visible storage error, not a boot failure. */
    xSemaphoreTake(storage_lock, portMAX_DELAY);
    mount_card();
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
