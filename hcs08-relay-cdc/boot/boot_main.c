/*
 * boot_main.c - USB mass storage bootloader for the relay board.
 *
 * Runs after every reset (it owns the reset vector) and decides:
 *   - stay in the bootloader if
 *       * the application asked for it (magic word in RAM, CDC command
 *         BOOTLOADER), or
 *       * SDA (PTC1, white wire) is tied to GND at power-up (emergency), or
 *       * there is no complete application (info block missing)
 *   - otherwise jump to the application at once, before touching any
 *     hardware (clock, USB), so the application starts as after a reset.
 *
 * In the bootloader the board appears as a USB drive "RELAYBOOT". Copying a
 * firmware file (make firmware -> build/relay_fw-<version>.s19) onto it
 * programs the application. After success (1 s without further writes)
 * or when the drive is ejected with a valid application, the board resets
 * and starts the new firmware.
 */
#include "jm60.h"
#include "board.h"
#include "layout.h"
#include "flash.h"
#include "disk.h"
#include "usb_msc.h"

/* NV option bytes: protect 0xDC00-0xFFFF, vector redirection on, unsecured */
const uint8_t __at(0xFFBD) nvprot = BOOT_NVPROT;
const uint8_t __at(0xFFBF) nvopt  = BOOT_NVOPT;

#define FLASH8(a) (*HW_PTR(a))

/* COP watchdog off before RAM initialisation (SOPT1 is write-once; the
 * application writes the same value, which is then ignored).
 * The SDCC hc08 startup code only copies initialised variables, it does not
 * clear the others, so all variables (0x00B0-0x0BFF) are zeroed here. The
 * stack (below 0x10AC) and the request word at 0x10AC are not touched. */
unsigned char _sdcc_external_startup(void)
{
    volatile uint8_t *p;
    SOPT1 = SOPT1_STOPE;
    for (p = HW_PTR(0x00B0); p != HW_PTR(0x0C00); p++)
        *p = 0;
    return 0;
}

static void clock_init(void)
{
    /* PEE: 12 MHz crystal / 8 * 32 = 48 MHz, 24 MHz bus (same as the app) */
#if XTAL_MHZ == 12
#define RDIV_BITS 3
#define VDIV_BITS 8
#elif XTAL_MHZ == 4
#define RDIV_BITS 1
#define VDIV_BITS 6
#elif XTAL_MHZ == 8
#define RDIV_BITS 2
#define VDIV_BITS 6
#elif XTAL_MHZ == 16
#define RDIV_BITS 3
#define VDIV_BITS 6
#else
#error "Unsupported XTAL_MHZ"
#endif
    MCGC2 = 0x36;
    while (!(MCGSC & MCGSC_OSCINIT))
        ;
    MCGC1 = 0x80 | (RDIV_BITS << 3) | 0x03;
    while ((MCGSC & (MCGSC_IREFST | MCGSC_CLKST)) != 0x08)
        ;
    MCGC3 = 0x40 | VDIV_BITS;
    while ((MCGSC & (MCGSC_PLLST | MCGSC_LOCK)) != (MCGSC_PLLST | MCGSC_LOCK))
        ;
    MCGC1 &= 0x3F;
    while ((MCGSC & MCGSC_CLKST) != 0x0C)
        ;
}

/* 1 ms tick on TPM1, overflow flag polled */
static void timer_init(void)
{
    TPM1SC = 0;
    TPM1MODH = (uint8_t)(23999u >> 8);
    TPM1MODL = (uint8_t)23999u;
    TPM1SC = TPMSC_CLKSA;
}

static uint8_t tick(void)
{
    if (TPM1SC & TPMSC_TOF) {
        TPM1SC &= (uint8_t)~TPMSC_TOF;
        return 1;
    }
    return 0;
}

static void reset_board(void)
{
    uint8_t n = 50;
    usb_detach();
    while (n)                       /* keep D+ released for 50 ms */
        n -= tick();
    __asm__(".db 0x8D");            /* illegal opcode -> reset */
}

static void start_app(void)
{
    uint16_t entry = ((uint16_t)FLASH8(APP_RESET_VEC) << 8) | FLASH8(APP_RESET_VEC + 1);
    ((void (*)(void))entry)();      /* application startup sets its own SP */
}

void main(void)
{
    volatile uint8_t *magic = HW_PTR(BOOT_MAGIC_ADDR);
    uint8_t stay = 0, n;
    uint16_t idle = 0;

    if (magic[0] == BOOT_MAGIC_0 && magic[1] == BOOT_MAGIC_1 &&
        magic[2] == BOOT_MAGIC_2 && magic[3] == BOOT_MAGIC_3)
        stay = 1;
    magic[0] = 0;

    /* Emergency entry: SDA (PTC1) bridged to GND at power-up */
    PTCPE |= 0x02;
    for (n = 200; n; n--)           /* let the pull-up charge the line */
        __asm__("nop");
    if (!(PTCD & 0x02))
        stay = 1;
    PTCPE &= (uint8_t)~0x02;

    if (!stay && app_valid())
        start_app();

    /* Bootloader: relay driver inputs to inactive level */
    PTED &= (uint8_t)~BOOT_IDLE_PTE;
    PTEDD |= BOOT_IDLE_PTE;
    PTFD &= (uint8_t)~BOOT_IDLE_PTF;
    PTFDD |= BOOT_IDLE_PTF;

    clock_init();
    timer_init();
    flash_init();
    disk_init();

    n = 100;                        /* stay detached for 100 ms */
    while (n)
        n -= tick();
    usb_init();

    for (;;) {
        usb_poll();
        msc_task();
        if (!tick())
            continue;
        if (msc_activity) {
            msc_activity = 0;
            idle = 0;
        } else if (idle < 60000u) {
            idle++;
        }
        /* After a successful update, or after "eject" with a valid
         * application: reset once the host has been quiet for 1 s. */
        if (idle >= 1000 &&
            (disk_status() == UPD_OK || (msc_eject_request && app_valid())))
            reset_board();
    }
}
