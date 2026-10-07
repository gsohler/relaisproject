/*
 * Relay Gateway - ESP32-S3: WLAN + web interface for HCS08 USB relay boards
 * connected to the USB-OTG port (directly or through a powered hub).
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "usb_boards.h"
#include "wifi_mgr.h"
#include "web.h"
#include "names.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    names_init();
    usb_boards_start();
    wifi_mgr_start();
    web_start();
}
