#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

#define EP0_SIZE        32

#define CDC_NOTIF_EP    1
#define CDC_OUT_EP      2
#define CDC_IN_EP       3
#define CDC_NOTIF_SIZE  8
#define CDC_BULK_SIZE   64

/* 9 + 9 + 5+5+4+5 + 7 + 9 + 7 + 7 */
#define USB_CONFIG_DESC_LEN 67
#define USB_NUM_STRINGS     4

extern const uint8_t usb_device_desc[18];
extern const uint8_t usb_config_desc[USB_CONFIG_DESC_LEN];
extern const char * const usb_strings[USB_NUM_STRINGS];

#endif
