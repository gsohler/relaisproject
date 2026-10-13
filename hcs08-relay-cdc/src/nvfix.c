/*
 * nvfix.c - repairs NVPROT/NVOPT on boards that were programmed with
 * "UsbdmFlashProgrammer -unsecure" (Makefile up to FW 1.9).
 *
 * That option replaces the NV bytes of the image by default values: no
 * flash protection, FNORED = 1. The bootloader still works, but the
 * hardware vector redirection is off, so the application's TPM2 interrupt
 * points into the bootloader and CURRENT is disabled (see vector_ok() in
 * current.c).
 *
 * Because the protection is off as well, the application can rewrite the
 * last flash page 0xFE00-0xFFFF (bootloader end, serial number, NV bytes,
 * vectors) with the correct NV bytes:
 *   - both NV bytes still erased (0xFF): program the two bytes,
 *   - otherwise: copy the page to RAM, erase it, program it back.
 * The second case has a window of about 30 ms in which a power loss leaves
 * the page empty (no reset vector) -> board needs USBDM again. Therefore
 * this only runs on an explicit command, never automatically.
 *
 * The new values take effect at the next reset (NVPROT/NVOPT are copied
 * to FPROT/FOPT by the reset).
 */
#include "jm60.h"
#include "layout.h"
#include "flash.h"
#include "nvfix.h"

#define NV_PAGE      0xFE00u
#define NVPROT_ADDR  0xFFBDu
#define NVOPT_ADDR   0xFFBFu
#define RESET_VEC    0xFFFEu

static __xdata uint8_t page[FLASH_PAGE];

uint8_t nvfix_state(void) __reentrant
{
    uint8_t prot = *HW_PTR(NVPROT_ADDR), opt = *HW_PTR(NVOPT_ADDR);
    uint16_t rv;

    if (prot == BOOT_NVPROT && opt == BOOT_NVOPT)
        return (FPROT == BOOT_NVPROT && !(FOPT & FOPT_FNORED)) ? NVFIX_OK : NVFIX_RESET;
    if (!(FPROT & FPROT_FPDIS))
        return NVFIX_PROTECTED;         /* top page write-protected */
    rv = ((uint16_t)*HW_PTR(RESET_VEC) << 8) | *HW_PTR(RESET_VEC + 1);
    if (rv < BOOT_START)
        return NVFIX_NO_BOOT;           /* no bootloader of ours on this board */
    return NVFIX_NEEDED;
}

static uint8_t program_page(void) __reentrant
{
    uint16_t i;
    uint8_t retry;

    for (i = 0; i < FLASH_PAGE; i++) {
        if (page[i] == 0xFF)
            continue;
        for (retry = 3; retry; retry--)
            if (flash_program((uint16_t)(NV_PAGE + i), page[i]))
                break;
        if (!retry)
            return 0;
        if ((i & 0x3F) == 0)
            COP_SERVICE();
    }
    return 1;
}

uint8_t nvfix_run(void) __reentrant
{
    uint16_t i;
    uint8_t ok, st = nvfix_state();

    if (st != NVFIX_NEEDED)
        return st;

    if (*HW_PTR(NVPROT_ADDR) == 0xFF && *HW_PTR(NVOPT_ADDR) == 0xFF) {
        /* Erased: plain byte programming, no erase needed */
        DisableInterrupts();
        ok = flash_program(NVPROT_ADDR, BOOT_NVPROT) && flash_program(NVOPT_ADDR, BOOT_NVOPT);
        EnableInterrupts();
        return ok ? NVFIX_RESET : NVFIX_FAILED;
    }

    for (i = 0; i < FLASH_PAGE; i++)
        page[i] = *HW_PTR(NV_PAGE + i);
    page[NVPROT_ADDR - NV_PAGE] = BOOT_NVPROT;
    page[NVOPT_ADDR - NV_PAGE] = BOOT_NVOPT;

    /* No interrupts while the vectors are gone (FNORED = 1 here, so the
     * CPU uses 0xFFC0-0xFFFF of this very page) */
    DisableInterrupts();
    COP_SERVICE();
    ok = flash_erase_page(NV_PAGE);
    if (!ok)                            /* page unchanged? try to continue */
        ok = flash_erase_page(NV_PAGE);
    ok = ok && program_page();
    COP_SERVICE();
    EnableInterrupts();

    for (i = 0; ok && i < FLASH_PAGE; i++)
        if (*HW_PTR(NV_PAGE + i) != page[i])
            ok = 0;
    return ok ? NVFIX_RESET : NVFIX_FAILED;
}
