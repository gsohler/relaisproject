/*
 * web.c - HTTP server: web page and JSON API.
 *
 *   GET  /                 web interface
 *   GET  /api/boards       gateway status and all boards
 *   POST /api/cmd          {"addr": 3, "cmd": "ON 2"}  -> {"ok": true, "reply": "OK"}
 *   GET  /setup            WLAN setup page (captive portal target)
 *   GET  /api/scan         visible networks
 *   POST /api/wifi         {"ssid": "...", "password": "..."} -> test connection
 *   GET  /api/wifi/status  result of the test: connecting / ok + new IP / failed
 *   GET  /manifest.json, /icon.svg   "add to home screen" support
 *   GET  /i18n.js          translations (de, en, fr), chosen in the browser
 *   GET  /api/names        all relay names
 *   POST /api/name         {"serial": "...", "relay": 3, "name": "Pumpe"}  ("" deletes)
 *
 * Simple switching protocol (plain text answers, for curl, scripts, Home Assistant):
 *   GET /switch?name=Pumpe&state=on        -> "OK"       (on | off | toggle)
 *   GET /switch?name=Pumpe                 -> "on" / "off"
 *   GET /switch?serial=0001&relay=3&state=off
 * Errors answer "ERR <reason>" with an HTTP error status.
 *
 * While the setup access point is active, every unknown URL is redirected to
 * /setup. Phones probe such URLs after joining a WLAN (generate_204,
 * hotspot-detect.html, ...) and then open the setup page by themselves.
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "usb_boards.h"
#include "wifi_mgr.h"
#include "names.h"
#include "web.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");
extern const char icon_svg_start[]   asm("_binary_icon_svg_start");
extern const char i18n_js_start[]    asm("_binary_i18n_js_start");
extern const char i18n_js_end[]      asm("_binary_i18n_js_end");
extern const char icon_svg_end[]     asm("_binary_icon_svg_end");

static const char manifest_json[] =
    "{\"name\":\"Relay Gateway\",\"short_name\":\"Relais\",\"start_url\":\"/\","
    "\"display\":\"standalone\",\"background_color\":\"#f4f5f7\",\"theme_color\":\"#2563eb\","
    "\"icons\":[{\"src\":\"/icon.svg\",\"sizes\":\"any\",\"type\":\"image/svg+xml\"}]}";

static char s_auth_expected[128];   /* "Basic base64(admin:pw)" or empty */

static bool check_auth(httpd_req_t *req)
{
    if (!s_auth_expected[0]) {
        return true;
    }
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strcmp(hdr, s_auth_expected) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Relay Gateway\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return err;
}

static cJSON *read_json_body(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(buf)) {
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", to);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* iOS needs a body to recognise the captive portal */
    return httpd_resp_sendstr(req, "<a href=\"/setup\">Relay Gateway setup</a>");
}

static esp_err_t send_static(httpd_req_t *req, const char *type, const char *start,
                             const char *end, bool text)
{
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, start, end - start - (text ? 1 : 0));
}

static esp_err_t index_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (!wifi_mgr_has_credentials()) {
        return redirect(req, "/setup");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t boards_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    static board_info_t list[BOARDS_MAX];   /* httpd runs one handler at a time */
    int n = usb_boards_snapshot(list, BOARDS_MAX);

    char mode[24], ssid[33], ip[16];
    wifi_mgr_status(mode, sizeof(mode), ssid, sizeof(ssid), ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON *gw = cJSON_AddObjectToObject(root, "gateway");
    cJSON_AddStringToObject(gw, "host", wifi_mgr_hostname());
    cJSON_AddStringToObject(gw, "mode", mode);
    cJSON_AddStringToObject(gw, "ssid", ssid);
    cJSON_AddStringToObject(gw, "ip", ip);
    cJSON_AddBoolToObject(gw, "setup", wifi_mgr_setup_active());
    cJSON_AddStringToObject(gw, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(gw, "usb_devices", usb_boards_device_count());

    cJSON *arr = cJSON_AddArrayToObject(root, "boards");
    for (int i = 0; i < n; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "addr", list[i].addr);
        cJSON_AddNumberToObject(b, "relays", RELAYS_PER_BOARD);
        cJSON_AddStringToObject(b, "serial", list[i].serial);
        cJSON_AddStringToObject(b, "product", list[i].product);
        cJSON_AddStringToObject(b, "location", list[i].location);
        cJSON_AddNumberToObject(b, "state", list[i].state);
        if (list[i].age_ms == UINT32_MAX) {
            cJSON_AddNullToObject(b, "age_ms");
        } else {
            cJSON_AddNumberToObject(b, "age_ms", list[i].age_ms);
        }
        cJSON_AddNumberToObject(b, "errors", list[i].errors);
        cJSON_AddStringToObject(b, "last_error", list[i].last_error);
        cJSON *jn = cJSON_AddArrayToObject(b, "names");
        for (int r = 1; r <= RELAYS_PER_BOARD; r++) {
            char nm[NAME_MAX_LEN + 1] = "";
            names_get(list[i].serial, r, nm, sizeof(nm));
            cJSON_AddItemToArray(jn, cJSON_CreateString(nm));
        }
        cJSON_AddItemToArray(arr, b);
    }
    return send_json(req, root);
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *in = read_json_body(req);
    cJSON *jaddr = in ? cJSON_GetObjectItem(in, "addr") : NULL;
    cJSON *jcmd = in ? cJSON_GetObjectItem(in, "cmd") : NULL;
    cJSON *out = cJSON_CreateObject();

    if (!cJSON_IsNumber(jaddr) || !cJSON_IsString(jcmd) ||
        strlen(jcmd->valuestring) == 0 || strlen(jcmd->valuestring) > 60 ||
        strpbrk(jcmd->valuestring, "\r\n")) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "reply", "bad request");
    } else {
        char cmd[64];
        strlcpy(cmd, jcmd->valuestring, sizeof(cmd));
        for (char *p = cmd; *p; p++) {
            *p = toupper((unsigned char)*p);
        }
        if (strncmp(cmd, "SCAN", 4) == 0) {
            /* runs for up to a minute on the board - use USB directly */
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "reply", "SCAN only via direct USB connection");
        } else {
            /* ON ALL / SET pulse up to 8 relays (~1 s), PULSE up to 10 s */
            uint32_t timeout = strncmp(cmd, "PULSE", 5) == 0 ? 11000 : 3000;
            char reply[80];
            esp_err_t err = usb_boards_command((uint8_t)jaddr->valueint, cmd,
                                               reply, sizeof(reply), timeout);
            bool ok = err == ESP_OK && strncmp(reply, "ERR", 3) != 0;
            if (err == ESP_OK && (strncmp(cmd, "ON", 2) == 0 || strncmp(cmd, "OFF", 3) == 0 ||
                                  strncmp(cmd, "SET", 3) == 0 || strncmp(cmd, "TOGGLE", 6) == 0)) {
                char st[32];   /* refresh cached state right away */
                usb_boards_command((uint8_t)jaddr->valueint, "GET", st, sizeof(st), 1500);
            }
            cJSON_AddBoolToObject(out, "ok", ok);
            cJSON_AddStringToObject(out, "reply", err == ESP_OK ? reply : esp_err_to_name(err));
        }
    }
    cJSON_Delete(in);
    return send_json(req, out);
}

/* ------------------------------------------------------------------ */
/* Relay names and the /switch protocol                               */
/* ------------------------------------------------------------------ */

static esp_err_t names_get_h(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    static name_entry_t list[NAMES_MAX];
    int n = names_list(list, NAMES_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "serial", list[i].serial);
        cJSON_AddNumberToObject(o, "relay", list[i].relay);
        cJSON_AddStringToObject(o, "name", list[i].name);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t name_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *in = read_json_body(req);
    cJSON *js = in ? cJSON_GetObjectItem(in, "serial") : NULL;
    cJSON *jr = in ? cJSON_GetObjectItem(in, "relay") : NULL;
    cJSON *jn = in ? cJSON_GetObjectItem(in, "name") : NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(js) && cJSON_IsNumber(jr) && cJSON_IsString(jn) &&
        jr->valueint >= 1 && jr->valueint <= RELAYS_PER_BOARD) {
        char name[NAME_MAX_LEN + 8];
        strlcpy(name, jn->valuestring, sizeof(name));
        /* trim surrounding spaces typed by accident */
        char *p = name;
        while (*p == ' ') p++;
        size_t l = strlen(p);
        while (l && p[l - 1] == ' ') p[--l] = 0;
        err = strlen(jn->valuestring) > NAME_MAX_LEN + 4 ? ESP_ERR_INVALID_ARG
              : names_set(js->valuestring, jr->valueint, p);
    }
    cJSON_Delete(in);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddStringToObject(out, "reply",
        err == ESP_OK ? "ok" : err == ESP_ERR_INVALID_STATE ? "name_dup" :
        err == ESP_ERR_NO_MEM ? "name_full" : "name_invalid");
    return send_json(req, out);
}

/* In-place URL decoding: %XX and '+' */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, text);
    return ESP_OK;
}

static esp_err_t switch_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char q[256], val[128], serial[32] = "", state[12] = "";
    int relay = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return send_text(req, "400 Bad Request",
                         "ERR usage: /switch?name=<name>&state=on|off|toggle\n");
    }
    if (httpd_query_key_value(q, "name", val, sizeof(val)) == ESP_OK) {
        url_decode(val);
        if (!names_find(val, serial, sizeof(serial), &relay)) {
            return send_text(req, "404 Not Found", "ERR unknown name\n");
        }
    } else if (httpd_query_key_value(q, "serial", val, sizeof(val)) == ESP_OK) {
        url_decode(val);
        strlcpy(serial, val, sizeof(serial));
        if (httpd_query_key_value(q, "relay", val, sizeof(val)) == ESP_OK) {
            relay = atoi(val);
        }
    }
    if (!serial[0] || relay < 1 || relay > RELAYS_PER_BOARD) {
        return send_text(req, "400 Bad Request",
                         "ERR usage: /switch?name=<name>&state=on|off|toggle\n");
    }

    /* find the board with this serial number */
    static board_info_t list[BOARDS_MAX];
    int n = usb_boards_snapshot(list, BOARDS_MAX), found = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].serial, serial) == 0) {
            found = i;
            matches++;
        }
    }
    if (matches == 0) {
        return send_text(req, "503 Service Unavailable", "ERR board not connected\n");
    }
    if (matches > 1) {
        return send_text(req, "409 Conflict", "ERR serial number not unique\n");
    }
    uint8_t addr = list[found].addr;
    int mask = list[found].state;
    char reply[48];

    httpd_query_key_value(q, "state", state, sizeof(state));
    for (char *p = state; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    if ((!state[0] || strcmp(state, "toggle") == 0) && mask < 0) {
        unsigned v;     /* state unknown yet: ask the board */
        if (usb_boards_command(addr, "GET", reply, sizeof(reply), 1500) == ESP_OK &&
            sscanf(reply, "STATE %x", &v) == 1) {
            mask = (int)v;
        }
    }
    bool is_on = mask >= 0 && (mask >> (relay - 1)) & 1;

    if (!state[0]) {
        return send_text(req, "200 OK", mask < 0 ? "unknown\n" : is_on ? "on\n" : "off\n");
    }
    const char *verb;
    if (strcmp(state, "on") == 0 || strcmp(state, "1") == 0) {
        verb = "ON";
    } else if (strcmp(state, "off") == 0 || strcmp(state, "0") == 0) {
        verb = "OFF";
    } else if (strcmp(state, "toggle") == 0) {
        if (mask < 0) {
            return send_text(req, "503 Service Unavailable", "ERR state unknown\n");
        }
        verb = is_on ? "OFF" : "ON";
    } else {
        return send_text(req, "400 Bad Request", "ERR state must be on, off or toggle\n");
    }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%s %d", verb, relay);
    esp_err_t err = usb_boards_command(addr, cmd, reply, sizeof(reply), 3000);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ERR %s\n", esp_err_to_name(err));
        return send_text(req, "504 Gateway Timeout", msg);
    }
    char st[32];
    usb_boards_command(addr, "GET", st, sizeof(st), 1500);   /* refresh cached state */
    if (strcmp(reply, "OK") != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s\n", reply);
        return send_text(req, "502 Bad Gateway", msg);
    }
    return send_text(req, "200 OK", "OK\n");
}

static esp_err_t setup_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_static(req, "text/html; charset=utf-8", setup_html_start, setup_html_end, true);
}

static esp_err_t manifest_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/manifest+json");
    return httpd_resp_sendstr(req, manifest_json);
}

static esp_err_t icon_get(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return send_static(req, "image/svg+xml", icon_svg_start, icon_svg_end, true);
}

static esp_err_t i18n_get(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_static(req, "application/javascript; charset=utf-8", i18n_js_start, i18n_js_end, true);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    static wifi_net_t nets[20];
    int n = wifi_mgr_scan(nets, 20);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", n >= 0);
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(o, "secure", nets[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *in = read_json_body(req);
    cJSON *jssid = in ? cJSON_GetObjectItem(in, "ssid") : NULL;
    cJSON *jpass = in ? cJSON_GetObjectItem(in, "password") : NULL;
    bool ok = cJSON_IsString(jssid) && cJSON_IsString(jpass) &&
              wifi_mgr_try(jssid->valuestring, jpass->valuestring);
    cJSON_Delete(in);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "reply", ok ? "connecting" : "invalid");
    return send_json(req, out);
}

static esp_err_t wifi_status_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char ip[16], msg[64];
    static const char *names[] = { "idle", "connecting", "ok", "failed" };
    wifi_try_state_t st = wifi_mgr_try_status(ip, sizeof(ip), msg, sizeof(msg));
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "state", names[st]);
    cJSON_AddStringToObject(out, "ip", ip);
    cJSON_AddStringToObject(out, "message", msg);
    cJSON_AddStringToObject(out, "host", wifi_mgr_hostname());
    return send_json(req, out);
}

/* Captive portal: unknown URLs -> setup page while the setup AP is active */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    if (wifi_mgr_setup_active()) {
        return redirect(req, "http://192.168.4.1/setup");
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

void web_start(void)
{
    const char *pw = CONFIG_GW_WEB_PASSWORD;
    if (pw[0]) {
        char userpass[80];
        size_t olen = 0;
        int l = snprintf(userpass, sizeof(userpass), "admin:%s", pw);
        strcpy(s_auth_expected, "Basic ");
        mbedtls_base64_encode((unsigned char *)s_auth_expected + 6,
                              sizeof(s_auth_expected) - 6, &olen,
                              (unsigned char *)userpass, l);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 7;       /* phones open several connections */
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start HTTP server");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/boards", .method = HTTP_GET,  .handler = boards_get },
        { .uri = "/api/cmd",    .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/api/wifi",   .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = wifi_status_get },
        { .uri = "/api/scan",   .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/setup",      .method = HTTP_GET,  .handler = setup_get },
        { .uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_get },
        { .uri = "/icon.svg",   .method = HTTP_GET,  .handler = icon_get },
        { .uri = "/i18n.js",    .method = HTTP_GET,  .handler = i18n_get },
        { .uri = "/api/names",  .method = HTTP_GET,  .handler = names_get_h },
        { .uri = "/api/name",   .method = HTTP_POST, .handler = name_post },
        { .uri = "/switch",     .method = HTTP_GET,  .handler = switch_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web server running%s", pw[0] ? " (password protected)" : "");
}
