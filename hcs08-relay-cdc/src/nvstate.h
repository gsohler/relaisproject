/*
 * nvstate.h - Relay state byte kept in flash across power cycles.
 *
 * The JM60 has no EEPROM. The small flash block 0x10B0-0x17FF lies outside
 * the application area, so firmware updates (bootloader) neither erase it
 * nor include it in their CRC. Two of its pages are used as a log, see
 * nvstate.c. Only a USBDM mass erase clears it.
 */
#ifndef NVSTATE_H
#define NVSTATE_H

#include <stdint.h>

#define NV_PAGE_A 0x1200u
#define NV_PAGE_B 0x1400u

/* Returns the last saved value, or 0 if nothing was saved yet. */
uint8_t nvstate_load(void);

/* Saves value (0x00..0xFE) if it differs from the last saved one.
 * Usually one byte pair is programmed (~0.1 ms); every 255th call also
 * erases a page (~20 ms). Returns 0 on a flash error. */
uint8_t nvstate_save(uint8_t value);

#endif
