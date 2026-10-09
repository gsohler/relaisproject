/*
 * board.h - Board-specific configuration.
 *
 * The relay pin mapping below is the one of the 6-relay board (measured).
 * For another PCB adapt RELAY_PINS. Firmware up to version 1.1 had the
 * debug commands PULSE and SCAN to find such a mapping.
 */
#ifndef BOARD_H
#define BOARD_H

#include "jm60.h"

/* Crystal frequency in MHz. Supported: 4, 8, 12, 16 (PLL ref = 1.5..2 MHz,
 * VCO -> 48 MHz). 12 MHz is what the Freescale JM60 reference designs use. */
#ifndef XTAL_MHZ
#define XTAL_MHZ 12
#endif

/* ---- Relays ----
 * Each relay has an ON input and an OFF input (latching/bistable driver).
 * Switching = one pulse of RELAY_PULSE_MS on the ON or OFF pin.
 *
 * While RELAY_MAP_VALID is 0 the firmware never drives any relay pin by
 * itself; ON/OFF/SET answer "ERR no pin map".
 */
#ifndef RELAY_MAP_VALID
#define RELAY_MAP_VALID 1
#endif

#define RELAY_COUNT     6
#define RELAY_PULSE_MS  100     /* pulse length per ON/OFF signal */
#define RELAY_GAP_MS    20      /* pause between two pulses */
#define RELAY_OFF_AT_BOOT 1     /* pulse all OFF inputs after power-up */

/* Set to 1 if the pulse is a LOW level (pin sinks the driver current). */
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 0
#endif

/* Pin encoding: PIN(port, bit), port PA..PG */
#define PA 0
#define PB 1
#define PC 2
#define PD 3
#define PE 4
#define PF 5
#define PG 6
#define PIN(port, bit) ((uint8_t)(((port) << 4) | (bit)))

/* { ON pin, OFF pin } for relay 1..6, measured on the real board
 * (pulses active high). */
#ifdef HOST_TEST
/* Fixed map for test/host_test.c, independent of the real board */
#define RELAY_PINS {                  \
    { PIN(PE, 0), PIN(PE, 1) },       \
    { PIN(PE, 2), PIN(PE, 3) },       \
    { PIN(PE, 4), PIN(PE, 5) },       \
    { PIN(PE, 6), PIN(PE, 7) },       \
    { PIN(PF, 0), PIN(PF, 1) },       \
    { PIN(PF, 2), PIN(PF, 3) },       \
}
#else
#define RELAY_PINS {                  \
    { PIN(PE, 7), PIN(PE, 6) }, /* R1 */ \
    { PIN(PE, 5), PIN(PE, 4) }, /* R2 */ \
    { PIN(PF, 7), PIN(PF, 6) }, /* R3 */ \
    { PIN(PF, 5), PIN(PF, 4) }, /* R4 */ \
    { PIN(PF, 3), PIN(PF, 2) }, /* R5 */ \
    { PIN(PF, 1), PIN(PF, 0) }, /* R6 */ \
}
#endif

/* USB identity. 0x1209/0x0001 is the pid.codes TEST PID - fine for the lab,
 * request your own PID at https://pid.codes before selling devices. */
#define USB_VID 0x1209
#define USB_PID 0x0001
#define USB_BCD_DEVICE 0x0100
#define USB_MANUFACTURER_STR "Open Relay Project"
#define USB_PRODUCT_STR      "6-Channel Relay Board"
#define USB_SERIAL_STR       "0001"

/* Use the on-chip 3.3 V USB regulator and 1.5k D+ pull-up.
 * WARNING (AN3560): if your board feeds 3.3 V into VUSB33 externally,
 * USB_INTERNAL_VREG MUST be 0, otherwise the chip can be damaged. */
#ifndef USB_INTERNAL_VREG
#define USB_INTERNAL_VREG 1
#endif
#ifndef USB_INTERNAL_PULLUP
#define USB_INTERNAL_PULLUP 1
#endif

#endif
