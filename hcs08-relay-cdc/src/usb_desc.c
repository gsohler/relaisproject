/*
 * usb_desc.c - USB descriptors for a CDC-ACM device.
 *
 * Interface 0: CDC communication (notification EP1 IN, interrupt)
 * Interface 1: CDC data (EP2 OUT bulk, EP3 IN bulk)
 */
#include "usb_desc.h"
#include "board.h"

#define LO(x) ((uint8_t)((x) & 0xFF))
#define HI(x) ((uint8_t)((x) >> 8))

const uint8_t usb_device_desc[18] = {
    18,                     /* bLength */
    0x01,                   /* bDescriptorType: DEVICE */
    0x00, 0x02,             /* bcdUSB 2.00 */
    0x02,                   /* bDeviceClass: CDC */
    0x00, 0x00,             /* bDeviceSubClass, bDeviceProtocol */
    EP0_SIZE,               /* bMaxPacketSize0 */
    LO(USB_VID), HI(USB_VID),
    LO(USB_PID), HI(USB_PID),
    LO(USB_BCD_DEVICE), HI(USB_BCD_DEVICE),
    1, 2, 3,                /* iManufacturer, iProduct, iSerialNumber */
    1                       /* bNumConfigurations */
};

const uint8_t usb_config_desc[USB_CONFIG_DESC_LEN] = {
    /* Configuration */
    9, 0x02, LO(USB_CONFIG_DESC_LEN), HI(USB_CONFIG_DESC_LEN),
    2,                      /* bNumInterfaces */
    1,                      /* bConfigurationValue */
    0,                      /* iConfiguration */
    0x80,                   /* bmAttributes: bus powered */
    100,                    /* bMaxPower: 200 mA (2 mA units) */

    /* Interface 0: CDC Communication, ACM, no protocol
     * (protocol 0 keeps ModemManager from treating it as a modem) */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x00, 0,

    /* CDC Header functional descriptor, CDC 1.10 */
    5, 0x24, 0x00, 0x10, 0x01,
    /* Call Management: no call management, data interface 1 */
    5, 0x24, 0x01, 0x00, 1,
    /* ACM: supports Set/Get_Line_Coding, Set_Control_Line_State, Send_Break */
    4, 0x24, 0x02, 0x06,
    /* Union: master interface 0, slave interface 1 */
    5, 0x24, 0x06, 0, 1,

    /* EP1 IN interrupt (notifications) */
    7, 0x05, 0x80 | CDC_NOTIF_EP, 0x03, CDC_NOTIF_SIZE, 0, 255,

    /* Interface 1: CDC Data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,

    /* EP2 OUT bulk */
    7, 0x05, CDC_OUT_EP, 0x02, CDC_BULK_SIZE, 0, 0,
    /* EP3 IN bulk */
    7, 0x05, 0x80 | CDC_IN_EP, 0x02, CDC_BULK_SIZE, 0, 0,
};

const char * const usb_strings[USB_NUM_STRINGS] = {
    0,                      /* index 0 is the language ID list */
    USB_MANUFACTURER_STR,
    USB_PRODUCT_STR,
    USB_SERIAL_STR,
};
