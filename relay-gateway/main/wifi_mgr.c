/*
 * wifi_mgr.c - Station mode with stored credentials, captive-portal setup AP.
 *
 * Setup flow on a phone:
 *   1. join "RelayGW-XXXX" -> the phone opens the setup page by itself
 *      (DNS answers every name with 192.168.4.1, unknown URLs redirect)
 *   2. pick the home network, enter the password
 *   3. the gateway tries it while the AP stays up and reports the new address
 *   4. credentials are stored; the setup AP closes two minutes later
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "dns_server.h"
#include "wifi_mgr.h"

#if __has_include("mdns.h")
#include "mdns.h"
#define HAVE_MDNS 1
#endif

static const char *TAG = "wifi";

#define NVS_NS          "wifi"
#define STA_RETRIES     5           /* then open the setup AP */
#define TRY_ATTEMPTS    3
#define AP_HOLD_US      (120 * 1000000LL)

static char s_ssid[33], s_pass[65];             /* stored credentials */
static char s_hostname[24], s_ap_ssid[24];
static esp_netif_t *s_sta_netif, *s_ap_netif;
static volatile bool s_setup_active, s_connected;
static int s_retries;
static dns_server_handle_t s_dns;
static esp_timer_handle_t s_reconnect_timer, s_ap_off_timer;

/* connection attempt started from the setup page; s_try_msg holds an
 * error code ("no_ap", "auth", "fail:<reason>") translated by the web page */
static volatile wifi_try_state_t s_try = TRY_IDLE;
static char s_try_ssid[33], s_try_pass[65], s_try_msg[64], s_try_ip[16];
static int s_try_fails;

/* ------------------------------------------------------------------ */

static void set_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static void start_setup_ap(void)
{
    if (s_setup_active) {
        return;
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, CONFIG_GW_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_GW_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);     /* STA side is needed for scan and test */
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    /* DHCP option 114 (RFC 8910): tells newer phones the portal URL directly */
    static const char portal_uri[] = "http://192.168.4.1/setup";
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)portal_uri, strlen(portal_uri));
    esp_netif_dhcps_start(s_ap_netif);

    if (!s_dns) {
        dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&cfg);
    }
    s_setup_active = true;
    ESP_LOGW(TAG, "setup access point \"%s\" active: join it with a phone", s_ap_ssid);
}

static void stop_setup_ap(void)
{
    if (!s_setup_active) {
        return;
    }
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup_active = false;
    ESP_LOGI(TAG, "setup access point closed");
}

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static void ap_off_cb(void *arg)
{
    s_try = TRY_IDLE;
    if (s_connected) {
        stop_setup_ap();
    }
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
}

/* ------------------------------------------------------------------ */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            esp_wifi_connect();
        }

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;

        if (s_try == TRY_CONNECTING) {
            if (ev->reason == WIFI_REASON_ASSOC_LEAVE) {
                return;                 /* our own disconnect before the attempt */
            }
            if (++s_try_fails < TRY_ATTEMPTS) {
                esp_timer_start_once(s_reconnect_timer, 1000000);
                return;
            }
            switch (ev->reason) {
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
                strlcpy(s_try_msg, "no_ap", sizeof(s_try_msg));
                break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                strlcpy(s_try_msg, "auth", sizeof(s_try_msg));
                break;
            default:
                snprintf(s_try_msg, sizeof(s_try_msg), "fail:%d", ev->reason);
                break;
            }
            ESP_LOGW(TAG, "test connection to \"%s\" failed: %s", s_try_ssid, s_try_msg);
            s_try = TRY_FAILED;
            if (s_ssid[0]) {            /* back to the stored network */
                set_sta_config(s_ssid, s_pass);
                esp_timer_start_once(s_reconnect_timer, 1000000);
            }
            return;
        }

        if (!s_ssid[0]) {
            return;
        }
        if (++s_retries == STA_RETRIES) {
            ESP_LOGW(TAG, "cannot reach \"%s\"", s_ssid);
            start_setup_ap();           /* keep trying STA in the background */
        }
        /* never block the event loop: reconnect from a timer */
        esp_timer_start_once(s_reconnect_timer, s_retries < STA_RETRIES ? 1000000 : 15000000);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_retries = 0;
        ESP_LOGI(TAG, "connected, IP " IPSTR ", http://%s.local", IP2STR(&ev->ip_info.ip), s_hostname);

        if (s_try == TRY_CONNECTING) {
            save_credentials(s_try_ssid, s_try_pass);
            snprintf(s_try_ip, sizeof(s_try_ip), IPSTR, IP2STR(&ev->ip_info.ip));
            s_try = TRY_OK;
            /* keep the setup AP a little longer so the phone sees the result */
            esp_timer_stop(s_ap_off_timer);
            esp_timer_start_once(s_ap_off_timer, AP_HOLD_US);
        } else if (s_setup_active && s_try != TRY_OK) {
            stop_setup_ap();
        }
    }
}

void wifi_mgr_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof(s_hostname), "relaygw-%02x%02x", mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RelayGW-%02X%02X", mac[4], mac[5]);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_ssid);
        nvs_get_str(h, "ssid", s_ssid, &l);
        l = sizeof(s_pass);
        nvs_get_str(h, "pass", s_pass, &l);
        nvs_close(h);
    }

    const esp_timer_create_args_t t1 = { .callback = reconnect_cb, .name = "wifi_rc" };
    const esp_timer_create_args_t t2 = { .callback = ap_off_cb, .name = "ap_off" };
    ESP_ERROR_CHECK(esp_timer_create(&t1, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&t2, &s_ap_off_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, s_hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (s_ssid[0]) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        set_sta_config(s_ssid, s_pass);
        ESP_LOGI(TAG, "connecting to \"%s\"", s_ssid);
    } else {
        ESP_LOGW(TAG, "no WLAN configured");
        start_setup_ap();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);      /* fast web response */

#ifdef HAVE_MDNS
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_hostname);
        mdns_instance_name_set("Relay Gateway");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
#endif
}

bool wifi_mgr_has_credentials(void)
{
    return s_ssid[0] != 0;
}

bool wifi_mgr_setup_active(void)
{
    return s_setup_active;
}

const char *wifi_mgr_hostname(void)
{
    return s_hostname;
}

void wifi_mgr_status(char *mode, size_t mlen, char *ssid, size_t slen, char *ip, size_t iplen)
{
    if (!s_ssid[0]) {
        snprintf(mode, mlen, "setup");
    } else {
        snprintf(mode, mlen, "%s%s", s_connected ? "online" : "connecting", s_setup_active ? "+setup" : "");
    }
    strlcpy(ssid, s_ssid, slen);
    esp_netif_ip_info_t info = { 0 };
    esp_netif_get_ip_info(s_connected ? s_sta_netif : s_ap_netif, &info);
    snprintf(ip, iplen, IPSTR, IP2STR(&info.ip));
}

int wifi_mgr_scan(wifi_net_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = esp_wifi_scan_start(&sc, true);       /* blocks ~2-3 s */
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(700));         /* STA busy connecting */
        }
    }
    if (err != ESP_OK) {
        return -1;
    }
    uint16_t num = 24;
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&num, recs);       /* sorted by RSSI */
    int n = 0;
    for (int i = 0; i < num && n < max; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < n; k++) {
            dup |= strcmp(out[k].ssid, (char *)recs[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid));
        out[n].rssi = recs[i].rssi;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(recs);
    return n;
}

bool wifi_mgr_try(const char *ssid, const char *password)
{
    if (!ssid[0] || strlen(ssid) > 32 || strlen(password) > 64 ||
        (password[0] && strlen(password) < 8)) {
        return false;
    }
    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    strlcpy(s_try_pass, password, sizeof(s_try_pass));
    s_try_msg[0] = s_try_ip[0] = 0;
    s_try_fails = 0;
    s_try = TRY_CONNECTING;
    ESP_LOGI(TAG, "testing \"%s\"", ssid);

    esp_timer_stop(s_reconnect_timer);
    esp_wifi_disconnect();
    set_sta_config(ssid, password);
    esp_timer_start_once(s_reconnect_timer, 500000);
    return true;
}

wifi_try_state_t wifi_mgr_try_status(char *ip, size_t iplen, char *msg, size_t mlen)
{
    strlcpy(ip, s_try_ip, iplen);
    strlcpy(msg, s_try_msg, mlen);
    return s_try;
}
