/*
 * main.c - 6-channel relay board, controlled over USB CDC-ACM.
 *
 * Text protocol (case-insensitive, one command per line, CR and/or LF):
 *   ON <n|ALL>       pulse ON input of relay n (1..6) or of all relays
 *   OFF <n|ALL>      pulse OFF input of relay n or of all relays
 *   TOGGLE <n>       toggle relay n
 *   SET <hh>         set all relays from a hex bit mask (bit0 = relay 1, 00..3F)
 *   GET              -> "STATE hh"
 *   ID               -> identification string, incl. serial number
 *   SERIAL           -> "SERIAL <16 hex digits>" or "ERR no serial"
 *   CURRENT          -> "CURRENT 1:0.42 2:0.00 ... 6:0.00", 50 Hz RMS in A
 *   CURRENT RMS      -> same format, true RMS (incl. harmonics and noise)
 *   ADC              -> debug: mean and peak-peak of all ADC inputs, VDD
 *   HELP or ?        -> command list
 *   BOOTLOADER       -> "OK", then the board restarts as USB drive (bootloader)
 * Every command answers "OK", "STATE hh", "ID ..." or "ERR <reason>".
 */
#include "jm60.h"
#include "board.h"
#include "usb_cdc.h"
#include "relay.h"
#include "timer.h"
#include "layout.h"
#include "flash.h"
#include "serial.h"
#include "current.h"

#define FW_VERSION "1.8"

/* NV option bytes, programmed together with the code.
 * NVOPT 0xFFBF: KEYEN=0, FNORED=1, SEC=10 (unsecured). NVPROT 0xFFBD: no protection. */
#ifndef HOST_TEST
const uint8_t __at(0xFFBD) nvprot = 0xFF;
const uint8_t __at(0xFFBF) nvopt  = 0x42;  /* standalone: FNORED = 1, real vectors */
#endif

static void clock_init(void)
{
    /* PEE mode: crystal -> PLL -> 48 MHz MCGOUT, 24 MHz bus (AN3560) */
#if XTAL_MHZ == 12
#define RDIV_BITS 3     /* /8  -> 1.5 MHz */
#define VDIV_BITS 8     /* x32 -> 48 MHz  */
#elif XTAL_MHZ == 4
#define RDIV_BITS 1     /* /2  -> 2 MHz   */
#define VDIV_BITS 6     /* x24 -> 48 MHz  */
#elif XTAL_MHZ == 8
#define RDIV_BITS 2     /* /4  -> 2 MHz   */
#define VDIV_BITS 6
#elif XTAL_MHZ == 16
#define RDIV_BITS 3     /* /8  -> 2 MHz   */
#define VDIV_BITS 6
#else
#error "Unsupported XTAL_MHZ"
#endif

    MCGC2 = 0x36;                       /* RANGE=high, HGO, EREFS=xtal, ERCLKEN */
    while (!(MCGSC & MCGSC_OSCINIT))
        ;
    MCGC1 = 0x80 | (RDIV_BITS << 3) | 0x03;   /* CLKS=ext (FBE), IRCLKEN, IREFSTEN */
    while ((MCGSC & (MCGSC_IREFST | MCGSC_CLKST)) != 0x08)
        ;
    MCGC3 = 0x40 | VDIV_BITS;           /* PLLS -> PBE */
    while ((MCGSC & (MCGSC_PLLST | MCGSC_LOCK)) != (MCGSC_PLLST | MCGSC_LOCK))
        ;
    MCGC1 &= 0x3F;                      /* CLKS=PLL -> PEE */
    while ((MCGSC & MCGSC_CLKST) != 0x0C)
        ;
}

/* ------------------------------------------------------------------ */
/* Command parser                                                     */
/* ------------------------------------------------------------------ */

#define LINE_MAX 32
static char line[LINE_MAX];
static uint8_t line_len;
static uint8_t line_overflow;

static const char hexdig[] = "0123456789ABCDEF";

#define RELAY_ALL ((uint8_t)((1u << RELAY_COUNT) - 1))

static void put_hex8(uint8_t v)
{
    cdc_putc(hexdig[v >> 4]);
    cdc_putc(hexdig[v & 0x0F]);
}

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Compare keyword at p; returns pointer after it (and after whitespace) or 0 */
static char *match(char *p, const char *kw)
{
    while (*kw) {
        if (*p++ != *kw++)
            return 0;
    }
    if (*p != 0 && *p != ' ' && *p != '\t')
        return 0;
    return skip_ws(p);
}

static int8_t hexval(char c)
{
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
    return -1;
}

/* Parses "1".."RELAY_COUNT" or "ALL"; returns bit mask, 0 on error */
static uint8_t parse_target(char *p)
{
    if (match(p, "ALL"))
        return RELAY_ALL;
    if (p[0] >= '1' && p[0] <= '0' + RELAY_COUNT && (p[1] == 0 || p[1] == ' '))
        return (uint8_t)(1u << (p[0] - '1'));
    return 0;
}

static void ok(void)            { cdc_puts("OK\r\n"); }

/* mA -> "12.34" (A, two decimals, rounded) */
static void put_amps(uint16_t ma)
{
    uint16_t a = ma / 1000, frac = (uint16_t)((ma % 1000 + 5) / 10);
    if (frac == 100) {
        a++;
        frac = 0;
    }
    if (a >= 10)
        cdc_putc((char)('0' + a / 10));
    cdc_putc((char)('0' + a % 10));
    cdc_putc('.');
    cdc_putc((char)('0' + frac / 10));
    cdc_putc((char)('0' + frac % 10));
}

/* After switching: store the new state in flash, then answer.
 * No interrupts while the flash is busy, so sampling pauses. */
static void ok_saved(void)
{
    uint8_t saved;
    current_pause();
    saved = relay_save();
    current_resume();
    if (saved)
        cdc_puts("OK\r\n");
    else
        cdc_puts("ERR state not saved\r\n");
}
static void err(const char *s)  { cdc_puts("ERR "); cdc_puts(s); cdc_puts("\r\n"); }

#ifdef HOST_TEST
extern void host_reset_hook(void);
#endif

/* Restart into the bootloader: magic word in RAM (survives the reset),
 * USB detach, then an illegal opcode resets the MCU. */
static void enter_bootloader(void)
{
    volatile uint8_t *magic = HW_PTR(BOOT_MAGIC_ADDR);

    ok();
    delay_ms(20);                   /* let "OK" reach the host */
    DisableInterrupts();
    magic[0] = BOOT_MAGIC_0;
    magic[1] = BOOT_MAGIC_1;
    magic[2] = BOOT_MAGIC_2;
    magic[3] = BOOT_MAGIC_3;
    USBCTL0 = 0;                    /* pull-up off: host sees a disconnect */
#ifdef HOST_TEST
    host_reset_hook();
#else
    __asm__(".db 0x8D");            /* illegal opcode -> reset */
#endif
}

static void execute(char *p)
{
    char *a;
    uint8_t m, i;

    p = skip_ws(p);
    if (*p == 0)
        return;

    if ((a = match(p, "ON")) != 0 || (a = match(p, "OFF")) != 0) {
        uint8_t on = (p[1] == 'N');
        if (!relay_map_valid()) { err("no pin map"); return; }
        if (!(m = parse_target(a))) { err("relay"); return; }
        for (i = 0; i < RELAY_COUNT; i++)
            if (m & (1u << i))
                relay_switch(i, on);    /* always pulse, even if state says so */
        ok_saved();
    } else if ((a = match(p, "TOGGLE")) != 0) {
        if (!relay_map_valid()) { err("no pin map"); return; }
        if (!(m = parse_target(a))) { err("relay"); return; }
        relay_set_mask(relay_get_mask() ^ m);
        ok_saved();
    } else if ((a = match(p, "SET")) != 0) {
        int8_t h = hexval(a[0]), l = hexval(a[1]);
        if (!relay_map_valid()) { err("no pin map"); return; }
        if (h < 0) { err("mask"); return; }
        if (l < 0) { m = (uint8_t)h; if (a[1] && a[1] != ' ') { err("mask"); return; } }
        else {
            m = (uint8_t)((h << 4) | l);
            if (a[2] && a[2] != ' ') { err("mask"); return; }
        }
        if (m & (uint8_t)~RELAY_ALL) { err("mask"); return; }
        relay_set_mask(m);
        ok_saved();
    } else if (match(p, "GET") || match(p, "STATE")) {
        cdc_puts("STATE ");
        put_hex8(relay_get_mask());
        cdc_puts("\r\n");
    } else if (match(p, "ID")) {
        static __xdata char ser[SERIAL_STR_LEN + 1];
        serial_get(ser);
        cdc_puts("ID " USB_PRODUCT_STR " FW " FW_VERSION " SN ");
        cdc_puts(ser);
        cdc_puts("\r\n");
    } else if (match(p, "SERIAL")) {
        static __xdata char ser[SERIAL_STR_LEN + 1];
        if (serial_get(ser)) {
            cdc_puts("SERIAL ");
            cdc_puts(ser);
            cdc_puts("\r\n");
        } else {
            err("no serial");
        }
    } else if ((a = match(p, "CURRENT")) != 0) {
        uint8_t rms = match(a, "RMS") != 0;
        uint16_t i50, irms;
        if (*a && !rms) { err("args"); return; }
        if (!current_available()) { err("no interrupt vector - update bootloader"); return; }
        if (!current_get(0, &i50, &irms)) { err("no data yet"); return; }
        cdc_puts("CURRENT");
        for (i = 0; i < RELAY_COUNT; i++) {
            current_get(i, &i50, &irms);
            cdc_putc(' ');
            cdc_putc((char)('1' + i));
            cdc_putc(':');
            put_amps(rms ? irms : i50);
        }
        cdc_puts("\r\n");
    } else if (match(p, "ADC")) {
        current_adc_report();
        ok();
    } else if (match(p, "BOOTLOADER")) {
        enter_bootloader();
    } else if (match(p, "HELP") || match(p, "?")) {
        cdc_puts("ON <n|ALL>, OFF <n|ALL>, TOGGLE <n>, SET <hex>, GET, ID, SERIAL,\r\n"
                 "CURRENT [RMS], ADC, BOOTLOADER\r\n");
    } else {
        /* Echo what was received, so wrong line endings or stray bytes
         * (backspace, escape sequences) can be seen. Non-printable bytes
         * are shown as <hh>. */
        cdc_puts("ERR command '");
        for (; *p; p++) {
            if (*p >= 0x20 && *p < 0x7F) {
                cdc_putc(*p);
            } else {
                cdc_putc('<');
                put_hex8((uint8_t)*p);
                cdc_putc('>');
            }
        }
        cdc_puts("'\r\n");
    }
}

static void feed(char c)
{
    if (c == '\r' || c == '\n') {
        if (line_overflow) {
            err("too long");
        } else if (line_len) {
            line[line_len] = 0;
            execute(line);
        }
        line_len = 0;
        line_overflow = 0;
        return;
    }
    if (c == 0x08 || c == 0x7F) {       /* backspace / delete from a terminal */
        if (line_len)
            line_len--;
        return;
    }
    if (line_len < LINE_MAX - 1)
        line[line_len++] = upcase(c);
    else
        line_overflow = 1;
}

/* Called by the SDCC startup code before RAM initialisation. The COP
 * watchdog is enabled out of reset, so switch it off first thing. */
unsigned char _sdcc_external_startup(void)
{
    volatile uint8_t *p;
    SOPT1 = SOPT1_STOPE;        /* write-once: COP watchdog off, STOP allowed */
    /* The SDCC hc08 startup code does not clear uninitialised variables:
     * zero 0x00B0-0x0BFF (all variables; stack and request word untouched) */
    for (p = HW_PTR(0x00B0); p != HW_PTR(0x0C00); p++)
        *p = 0;
    return 0;                   /* 0 = go on and initialise RAM */
}

/* Tells the bootloader that this firmware starts fine (crash counter = 0,
 * see boot/decide.c). Missing TPM1 ticks consumed by delay_ms() only make
 * the confirmation come a bit later. */
static uint16_t uptime_ms;

static void boot_confirm(void)
{
    volatile uint8_t *cnt = HW_PTR(BOOT_COUNT_ADDR);
    cnt[0] = 0;
    cnt[1] = 0xFF;
}

void main(void)
{
    uint8_t buf[64];
    uint8_t n, i;

    clock_init();
    timer_init();
    flash_init();               /* flash clock, for the saved relay state */
    relay_init();               /* relays to their saved state */
    usb_init();
    current_init();
    EnableInterrupts();         /* only the TPM2 sampling interrupt */

    for (;;) {
        usb_poll();
        current_task();
        if (uptime_ms < APP_CONFIRM_MS && (TPM1SC & TPMSC_TOF)) {
            TPM1SC &= (uint8_t)~TPMSC_TOF;
            if (++uptime_ms == APP_CONFIRM_MS)
                boot_confirm();         /* running fine: clear crash counter */
        }
        n = cdc_read(buf);
        for (i = 0; i < n; i++)
            feed((char)buf[i]);
    }
}
