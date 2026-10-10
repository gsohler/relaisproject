/*
 * serial.h - Board serial number, stored at production in the protected
 * bootloader area (SERIAL_ADDR, see layout.h): 8 bytes + CRC16.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define SERIAL_STR_LEN 16       /* hex digits */

/* Writes the serial number as 16 hex digits + NUL to buf (17 bytes).
 * Returns 0 and writes "NOSERIAL" if the slot is empty or damaged. */
uint8_t serial_get(char *buf);

#endif
