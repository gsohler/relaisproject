/*
 * serial.c - Reads the serial number slot (see serial.h).
 * Same format and CRC (CRC-16/CCITT-FALSE) as in boot/disk.c and
 * tools/production.py.
 */
#include "jm60.h"
#include "layout.h"
#include "serial.h"

#define FLASH8(a) (*HW_PTR(a))

static uint16_t crc16_byte(uint16_t crc, uint8_t b)
{
    uint8_t i;
    crc ^= (uint16_t)b << 8;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

uint8_t serial_get(char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    static const char none[] = "NOSERIAL";
    uint8_t i, blank = 1;
    uint16_t crc = 0xFFFF;

    for (i = 0; i < SERIAL_LEN; i++) {
        uint8_t b = FLASH8(SERIAL_ADDR + i);
        if (b != 0xFF)
            blank = 0;
        crc = crc16_byte(crc, b);
        buf[2 * i] = hex[b >> 4];
        buf[2 * i + 1] = hex[b & 0x0F];
    }
    buf[2 * SERIAL_LEN] = 0;
    if (!blank && FLASH8(SERIAL_ADDR + SERIAL_LEN) == (uint8_t)(crc >> 8) &&
        FLASH8(SERIAL_ADDR + SERIAL_LEN + 1) == (uint8_t)crc)
        return 1;
    for (i = 0; i < sizeof(none); i++)
        buf[i] = none[i];
    return 0;
}
