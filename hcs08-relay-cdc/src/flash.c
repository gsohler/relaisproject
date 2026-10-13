/*
 * flash.c - Flash erase/program for the MC9S08JM60, used by the bootloader
 * (application area) and by the application (relay state, see nvstate.c).
 *
 * The JM60 has a single flash array, so the CPU must not fetch from flash
 * while a command runs. The whole command sequence (write to the flash
 * address, write FCMD, launch, wait for completion) runs from RAM, like the
 * NXP "do on stack" routines. Address, data and command are patched into
 * the RAM code before each call.
 */
#include "jm60.h"
#include "flash.h"

#ifndef HOST_TEST
static uint8_t ram_cmd[28] = {
    0x45, 0x00, 0x00,       /*  0: ldhx #addr      (patched: 1, 2) */
    0xA6, 0x00,             /*  3: lda  #data      (patched: 4)    */
    0xF7,                   /*  5: sta  ,x         -> latch address and data */
    0xA6, 0x00,             /*  6: lda  #cmd       (patched: 7)    */
    0xC7, 0x18, 0x26,       /*  8: sta  FCMD       */
    0xA6, 0x80,             /* 11: lda  #FCBEF     */
    0xC7, 0x18, 0x25,       /* 13: sta  FSTAT      -> launch */
    0x9D, 0x9D, 0x9D, 0x9D, /* 16: 4 x nop: wait before reading the flags */
    0xC6, 0x18, 0x25,       /* 20: lda  FSTAT      */
    0xA5, 0x40,             /* 23: bit  #FCCF      */
    0x27, 0xF9,             /* 25: beq  20         */
    0x81                    /* 27: rts             */
};

static uint8_t flash_cmd(uint16_t addr, uint8_t data, uint8_t cmd)
{
    FSTAT = FSTAT_FPVIOL | FSTAT_FACCERR;   /* clear old errors */
    ram_cmd[1] = (uint8_t)(addr >> 8);
    ram_cmd[2] = (uint8_t)addr;
    ram_cmd[4] = data;
    ram_cmd[7] = cmd;
    ((void (*)(void))ram_cmd)();
    return (uint8_t)(FSTAT & (FSTAT_FPVIOL | FSTAT_FACCERR));
}
#else
/* Host test: flash model in host_mem. Flash = 0x10B0-0x17FF and
 * APP_START-0xFFFF; the bootloader area is protected. */
#include "layout.h"
int host_flash_ops;
static uint8_t flash_cmd(uint16_t addr, uint8_t data, uint8_t cmd)
{
    uint16_t i, base;
    host_flash_ops++;
    /* the bootloader area is writable only if FPROT says "no protection"
     * (boards programmed with USBDM -unsecure, see nvfix.c) */
    if ((addr >= BOOT_START && !(FPROT & FPROT_FPDIS)) ||
        (addr < APP_START && (addr < 0x10B0u || addr >= 0x1800u)))
        return FSTAT_FPVIOL;
    if (cmd == FCMD_PAGE_ERASE) {
        base = addr & (uint16_t)~(FLASH_PAGE - 1);
        for (i = 0; i < FLASH_PAGE; i++) {
            uint16_t a = (uint16_t)(base + i);
            if (a >= APP_START || (a >= 0x10B0u && a < 0x1800u))
                host_mem[a] = 0xFF;
        }
    } else {
        host_mem[addr] &= data;             /* programming only clears bits */
    }
    return 0;
}
#endif

void flash_init(void)
{
    /* 24 MHz bus / 8 / (14 + 1) = 200 kHz flash clock (150..200 kHz) */
    if (!(FCDIV & FCDIV_FDIVLD))
        FCDIV = FCDIV_PRDIV8 | 14;
}

uint8_t flash_erase_page(uint16_t addr)
{
    return flash_cmd(addr, 0, FCMD_PAGE_ERASE) == 0;
}

uint8_t flash_program(uint16_t addr, uint8_t value)
{
    uint8_t cur = *HW_PTR(addr);
    if (cur == value)
        return 1;                   /* already there (file copied twice) */
    if (cur != 0xFF)
        return 0;                   /* would need an erase */
    if (flash_cmd(addr, value, FCMD_BYTE_PROG) != 0)
        return 0;
    return *HW_PTR(addr) == value;
}
