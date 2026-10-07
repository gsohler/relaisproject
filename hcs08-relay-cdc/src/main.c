/*
 * main.c - 6-channel relay board, controlled over USB CDC-ACM.
 *
 * Text protocol (case-insensitive, one command per line, CR and/or LF):
 *   ON <n|ALL>       pulse ON input of relay n (1..6) or of all relays
 *   OFF <n|ALL>      pulse OFF input of relay n or of all relays
 *   TOGGLE <n>       toggle relay n
 *   SET <hh>         set all relays from a hex bit mask (bit0 = relay 1, 00..3F)
 *   GET              -> "STATE hh"
 *   ID               -> identification string
 *   HELP or ?        -> command list
 * Debug (to find the pin mapping):
 *   PULSE <pin> [ms] [LOW|HIGH]   pulse one MCU pin, e.g. "PULSE PTE3"
 *   SCAN [port|ALL] [ms] [LOW|HIGH]
 *                    pulse every pin in turn, printing "SCAN PTxn" first;
 *                    any input (e.g. "STOP") aborts
 * Every command answers "OK", "STATE hh", "ID ..." or "ERR <reason>".
 */
#include "jm60.h"
#include "board.h"
#include "usb_cdc.h"
#include "relay.h"
#include "timer.h"

#define FW_VERSION "1.1"

/* NV option bytes, programmed together with the code.
 * NVOPT 0xFFBF: KEYEN=0, FNORED=1, SEC=10 (unsecured). NVPROT 0xFFBD: no protection. */
#ifndef HOST_TEST
const uint8_t __at(0xFFBD) nvprot = 0xFF;
const uint8_t __at(0xFFBF) nvopt  = 0x42;
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

/* Returns pointer to next token, or 0 at end of line */
static char *next_token(char *p)
{
    while (*p && *p != ' ' && *p != '\t')
        p++;
    p = skip_ws(p);
    return *p ? p : 0;
}

/* Decimal number up to the next blank; returns 0 on error */
static uint16_t parse_uint(char *p)
{
    uint16_t v = 0;
    uint8_t n = 0;
    while (*p >= '0' && *p <= '9') {
        if (v > 6553)
            return 0;
        v = (uint16_t)(v * 10 + (*p++ - '0'));
        n++;
    }
    if (!n || (*p && *p != ' '))
        return 0;
    return v;
}

/* "PTE3" or "E3" -> pin code (port << 4 | bit), 0xFF on error */
static uint8_t parse_pin(char *p)
{
    uint8_t pin;
    if (p[0] == 'P' && p[1] == 'T')
        p += 2;
    if (p[0] < 'A' || p[0] > 'G' || p[1] < '0' || p[1] > '7' || (p[2] && p[2] != ' '))
        return 0xFF;
    pin = (uint8_t)(((p[0] - 'A') << 4) | (p[1] - '0'));
    return pin_usable(pin) ? pin : 0xFF;
}

static void put_pin(uint8_t pin)
{
    cdc_puts("PT");
    cdc_putc((char)('A' + (pin >> 4)));
    cdc_putc((char)('0' + (pin & 7)));
}

static void ok(void)            { cdc_puts("OK\r\n"); }
static void err(const char *s)  { cdc_puts("ERR "); cdc_puts(s); cdc_puts("\r\n"); }

/* Optional "[ms] [LOW|HIGH]" arguments of PULSE and SCAN; 0 on error */
static uint8_t parse_pulse_opts(char *a, uint16_t *ms, uint8_t *low)
{
    *ms = RELAY_PULSE_MS;
    *low = RELAY_ACTIVE_LOW;
    while (a) {
        if (match(a, "LOW"))
            *low = 1;
        else if (match(a, "HIGH"))
            *low = 0;
        else if ((*ms = parse_uint(a)) == 0 || *ms > 10000)
            return 0;
        a = next_token(a);
    }
    return 1;
}

#define SCAN_PAUSE_MS 1000

/* SCAN [port|ALL] [ms] [LOW|HIGH]: pulse every usable pin, one after another.
 * Any received character aborts the scan. */
static void cmd_scan(char *a)
{
    uint8_t first = 0, last = 6, port, bit, pin, low;
    uint16_t ms;

    if (a && !match(a, "ALL") && a[0] >= 'A' && a[0] <= 'G' && (a[1] == 0 || a[1] == ' ')) {
        first = last = (uint8_t)(a[0] - 'A');
        a = next_token(a);
    } else if (a && match(a, "ALL")) {
        a = next_token(a);
    }
    if (!parse_pulse_opts(a, &ms, &low)) { err("args"); return; }

    for (port = first; port <= last; port++) {
        for (bit = 0; bit < 8; bit++) {
            pin = (uint8_t)((port << 4) | bit);
            if (!pin_usable(pin))
                continue;
            cdc_puts("SCAN ");
            put_pin(pin);
            cdc_puts("\r\n");
            delay_ms(50);               /* let the line reach the host first */
            pin_pulse(pin, ms, low, 1);
            delay_ms(SCAN_PAUSE_MS);
            if (cdc_rx_pending()) {
                cdc_puts("SCAN ABORTED\r\n");
                return;
            }
        }
    }
    ok();
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
        ok();
    } else if ((a = match(p, "TOGGLE")) != 0) {
        if (!relay_map_valid()) { err("no pin map"); return; }
        if (!(m = parse_target(a))) { err("relay"); return; }
        relay_set_mask(relay_get_mask() ^ m);
        ok();
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
        ok();
    } else if (match(p, "GET") || match(p, "STATE")) {
        cdc_puts("STATE ");
        put_hex8(relay_get_mask());
        cdc_puts("\r\n");
    } else if ((a = match(p, "PULSE")) != 0) {
        uint16_t ms;
        uint8_t low, pin;
        if (!*a || (pin = parse_pin(a)) == 0xFF) { err("pin"); return; }
        if (!parse_pulse_opts(next_token(a), &ms, &low)) { err("args"); return; }
        pin_pulse(pin, ms, low, 1);
        ok();
    } else if ((a = match(p, "SCAN")) != 0) {
        cmd_scan(*a ? a : 0);
    } else if (match(p, "STOP")) {
        ok();                           /* only used to abort SCAN */
    } else if (match(p, "ID")) {
        cdc_puts("ID " USB_PRODUCT_STR " FW " FW_VERSION "\r\n");
    } else if (match(p, "HELP") || match(p, "?")) {
        cdc_puts("ON <n|ALL>, OFF <n|ALL>, TOGGLE <n>, SET <hex>, GET, ID\r\n"
                 "PULSE <pin> [ms] [LOW|HIGH], SCAN [port|ALL] [ms] [LOW|HIGH], STOP\r\n");
    } else {
        err("command");
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
    if (line_len < LINE_MAX - 1)
        line[line_len++] = upcase(c);
    else
        line_overflow = 1;
}

/* Called by the SDCC startup code before RAM initialisation. The COP
 * watchdog is enabled out of reset, so switch it off first thing. */
unsigned char _sdcc_external_startup(void)
{
    SOPT1 = SOPT1_STOPE;        /* write-once: COP watchdog off, STOP allowed */
    return 0;                   /* 0 = go on and initialise RAM */
}

void main(void)
{
    uint8_t buf[64];
    uint8_t n, i;

    clock_init();
    timer_init();
    relay_init();               /* pulses all OFF inputs if the pin map is set */
    usb_init();

    for (;;) {
        usb_poll();
        n = cdc_read(buf);
        for (i = 0; i < n; i++)
            feed((char)buf[i]);
    }
}
