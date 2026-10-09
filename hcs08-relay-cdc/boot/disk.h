/*
 * disk.h - Emulated FAT12 drive of the bootloader.
 *
 * Reading returns a small FAT12 volume with INFO.TXT. Writing a .s19 file
 * programs the application: every written sector of the data area is
 * scanned for S-records, all other writes (FAT, directory) are ignored.
 */
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

#define DISK_SECTOR      512u
#define DISK_SECTORS     2048u      /* 1 MiB */

/* Update status, shown in INFO.TXT */
enum {
    UPD_NONE = 0,       /* nothing written yet */
    UPD_RUNNING,        /* records received, end not reached / CRC not yet ok */
    UPD_OK,             /* programmed, verified, info block written */
    UPD_ERR_RANGE,      /* record outside the application area (wrong file) */
    UPD_ERR_FLASH,      /* erase/program failed */
    UPD_ERR_CRC         /* end record seen but CRC does not match */
};

void    disk_init(void);
void    disk_read(uint16_t lba, uint8_t *buf);          /* fills 512 bytes */
void    disk_write(uint16_t lba, uint16_t offset, const uint8_t *data, uint8_t n);
void    disk_write_end(void);       /* after each WRITE command */
uint8_t disk_status(void);

/* Application present and complete (info block valid) */
uint8_t app_valid(void);

/* Serial number as 16 hex digits + NUL; returns 0 if the slot is empty or
 * damaged (then buf = "NOSERIAL") */
uint8_t serial_string(char *buf);

#endif
