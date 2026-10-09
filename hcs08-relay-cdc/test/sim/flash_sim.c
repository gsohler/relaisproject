/* flash_sim.c - flash model for the simulator (no flash controller there).
 * Erase skips 0x8000-0xBFFF, where the harness keeps the file text. */
#include "jm60.h"
#include "layout.h"
#include "flash.h"

static uint8_t flash_cmd(uint16_t addr, uint8_t data, uint8_t cmd)
{
    uint16_t i, base;
    if (addr >= BOOT_START || addr < APP_START)
        return FSTAT_FPVIOL;
    if (cmd == FCMD_PAGE_ERASE) {
        base = addr & (uint16_t)~(FLASH_PAGE - 1);
        if (base >= 0x8000u && base < 0xC000u)
            return 0;
        for (i = 0; i < FLASH_PAGE; i++)
            if ((uint16_t)(base + i) >= APP_START)
                *HW_PTR(base + i) = 0xFF;
    } else {
        *HW_PTR(addr) &= data;
    }
    return 0;
}

void flash_init(void) { }

uint8_t flash_erase_page(uint16_t addr)
{
    return flash_cmd(addr, 0, FCMD_PAGE_ERASE) == 0;
}

uint8_t flash_program(uint16_t addr, uint8_t value)
{
    uint8_t cur = *HW_PTR(addr);
    if (cur == value)
        return 1;
    if (cur != 0xFF)
        return 0;
    flash_cmd(addr, value, FCMD_BYTE_PROG);
    return *HW_PTR(addr) == value;
}
