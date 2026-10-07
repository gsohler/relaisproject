/*
 * names.c - Relay names in NVS (namespace "names", one blob of text lines
 * "serial<TAB>relay<TAB>name\n").
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "names.h"

static const char *TAG = "names";

static name_entry_t s_names[NAMES_MAX];
static int s_count;
static SemaphoreHandle_t s_lock;

static int ci_cmp(const char *a, const char *b)
{
    unsigned char ca, cb;
    do {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
    } while (ca && ca == cb);
    return ca - cb;
}

static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > NAME_MAX_LEN || n[0] == ' ' || n[l - 1] == ' ') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)n; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

static void save(void)
{
    size_t cap = (size_t)s_count * (32 + 4 + NAME_MAX_LEN + 3) + 1;
    char *buf = malloc(cap);
    if (!buf) {
        return;
    }
    size_t len = 0;
    for (int i = 0; i < s_count; i++) {
        len += snprintf(buf + len, cap - len, "%s\t%d\t%s\n",
                        s_names[i].serial, s_names[i].relay, s_names[i].name);
    }
    nvs_handle_t h;
    if (nvs_open("names", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, "list", buf, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "saving failed: %s", esp_err_to_name(err));
        }
        nvs_close(h);
    }
    free(buf);
}

void names_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open("names", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, "list", NULL, &len) == ESP_OK && len > 0) {
        char *buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, "list", buf, &len) == ESP_OK) {
            buf[len] = 0;
            char *save_line;
            for (char *line = strtok_r(buf, "\n", &save_line); line && s_count < NAMES_MAX;
                 line = strtok_r(NULL, "\n", &save_line)) {
                char *t1 = strchr(line, '\t');
                char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
                if (!t2) {
                    continue;
                }
                *t1 = *t2 = 0;
                name_entry_t *e = &s_names[s_count];
                strlcpy(e->serial, line, sizeof(e->serial));
                e->relay = atoi(t1 + 1);
                strlcpy(e->name, t2 + 1, sizeof(e->name));
                if (e->relay > 0 && valid_name(e->name)) {
                    s_count++;
                }
            }
        }
        free(buf);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d relay name(s) loaded", s_count);
}

bool names_get(const char *serial, int relay, char *out, size_t len)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_names[i].relay == relay && strcmp(s_names[i].serial, serial) == 0) {
            strlcpy(out, s_names[i].name, len);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t names_set(const char *serial, int relay, const char *name)
{
    if (!serial[0] || relay < 1 || (name[0] && !valid_name(name))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_names[i].relay == relay && strcmp(s_names[i].serial, serial) == 0) {
            idx = i;
        } else if (name[0] && ci_cmp(s_names[i].name, name) == 0) {
            err = ESP_ERR_INVALID_STATE;        /* used by another relay */
        }
    }
    if (err == ESP_OK) {
        if (!name[0]) {
            if (idx >= 0) {
                s_names[idx] = s_names[--s_count];
            }
        } else if (idx >= 0) {
            strlcpy(s_names[idx].name, name, sizeof(s_names[idx].name));
        } else if (s_count < NAMES_MAX) {
            name_entry_t *e = &s_names[s_count++];
            strlcpy(e->serial, serial, sizeof(e->serial));
            e->relay = relay;
            strlcpy(e->name, name, sizeof(e->name));
        } else {
            err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK) {
            save();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool names_find(const char *name, char *serial, size_t slen, int *relay)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (ci_cmp(s_names[i].name, name) == 0) {
            strlcpy(serial, s_names[i].serial, slen);
            *relay = s_names[i].relay;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

int names_list(name_entry_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_names, n * sizeof(name_entry_t));
    xSemaphoreGive(s_lock);
    return n;
}
