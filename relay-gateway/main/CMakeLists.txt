# GW_EXTRA_REQUIRES: only for builds without the component manager (e.g. "mdns")
set(GW_EXTRA_REQUIRES $ENV{GW_EXTRA_REQUIRES})
idf_component_register(
    SRCS "main.c" "usb_boards.c" "wifi_mgr.c" "web.c" "names.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html" "www/setup.html" "www/icon.svg" "www/i18n.js"
    PRIV_REQUIRES usb esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_server json mbedtls esp_app_format dns_server ${GW_EXTRA_REQUIRES}
)
