#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts WLAN: station mode with the stored credentials. If none are stored
 * or the network cannot be reached, the setup access point "RelayGW-XXXX"
 * (192.168.4.1) is opened as a captive portal: phones show the setup page
 * automatically after joining it. */
void wifi_mgr_start(void);

bool wifi_mgr_has_credentials(void);
bool wifi_mgr_setup_active(void);
const char *wifi_mgr_hostname(void);

/* Fills a short status: mode, ssid, ip */
void wifi_mgr_status(char *mode, size_t mlen, char *ssid, size_t slen, char *ip, size_t iplen);

/* Scan for networks. Fills up to max entries, strongest first, without
 * duplicates. Returns the number of entries or -1 on error. */
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} wifi_net_t;
int wifi_mgr_scan(wifi_net_t *out, int max);

/* Tries to connect to a network while the setup AP stays up. On success the
 * credentials are stored and the setup AP is closed two minutes later, so
 * the phone can still show the new address. */
typedef enum { TRY_IDLE, TRY_CONNECTING, TRY_OK, TRY_FAILED } wifi_try_state_t;
bool wifi_mgr_try(const char *ssid, const char *password);
wifi_try_state_t wifi_mgr_try_status(char *ip, size_t iplen, char *msg, size_t mlen);
