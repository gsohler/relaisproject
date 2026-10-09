/*
 * sim_main.c - Runs the bootloader's disk code (FAT emulation, S-record
 * parser) compiled by SDCC in the uCsim HCS08 simulator, see run_sim.py.
 * This catches code generation and calling convention problems that the
 * gcc host test cannot see.
 *
 * Memory used by the harness:
 *   0x8000..   firmware file text (loaded by run_sim.py), NUL terminated,
 *              max. 16 KiB; set to 0xFF after feeding, before the CRC check
 *   code at 0xE800, stack below it (outside the application area)
 *   0x0C00     LBA to read (16 bit)        0x0C02  result: disk_status()
 *   0x0C10..   512-byte sector buffer for disk_read()
 *   0x0C04     mode: 0 = read sector, 1 = write the firmware file
 */
#include <stdint.h>
#include "jm60.h"
#include "disk.h"

#define SIM_LBA    (*(volatile uint16_t *)0x0C00)
#define SIM_STATUS (*(volatile uint8_t *)0x0C02)
#define SIM_MODE   (*(volatile uint8_t *)0x0C04)
#define SIM_BUF    ((uint8_t *)0x0C10)
#define FILE_TEXT  ((const uint8_t *)0x8000)

unsigned char _sdcc_external_startup(void) { return 0; }

void sim_done(void)
{
    for (;;)
        ;
}

void main(void)
{
    uint16_t pos = 0, lba = 16, off = 0;
    disk_init();
    if (SIM_MODE == 0) {
        disk_read(SIM_LBA, SIM_BUF);
        sim_done();
    }
    /* write the file in 64-byte packets, 8 per sector, ascending clusters */
    for (;;) {
        uint8_t n = 0;
        while (n < 64 && FILE_TEXT[pos + n])
            n++;
        if (!n)
            break;
        disk_write(lba, off, FILE_TEXT + pos, 64);   /* tail is NUL padding */
        pos += 64;
        off += 64;
        if (off == 512) {
            off = 0;
            lba++;
        }
    }
    for (pos = 0x8000u; pos != 0xC000u; pos++)     /* "erase" the text */
        *HW_PTR(pos) = 0xFF;
    disk_write_end();
    SIM_STATUS = disk_status();
    sim_done();
}
