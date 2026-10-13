/*
 * host_test.c - Runs the firmware's USB stack and command parser on the host
 * against a simple model of the JM60 SIE (buffer descriptors + STAT FIFO of 1).
 *
 *   make test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

uint8_t host_mem[0x10000];

#define main firmware_main
#include "../src/main.c"
#include "../src/nvstate.h"
#include "../src/current.h"
#include <math.h>

/* ---- ADC model: per channel a 50 Hz current (A rms), phase, harmonic,
 * noise; sampled at the simulated time in ms ---- */
static double sim_ms;
static double cur_a[16], cur_ph[16], cur_h3[16], cur_noise[16], cur_hz[16];
static uint16_t adc_fixed[32];
static const uint8_t sensor_ch[6] = CURRENT_ADC_CHANNELS;
uint16_t host_adc(uint8_t adc_ch)
{
    int ch = -1;                        /* model arrays are indexed by relay */
    if (adc_ch >= 16)
        return adc_fixed[adc_ch & 31];
    for (int i = 0; i < 6; i++)
        if (sensor_ch[i] == adc_ch)
            ch = i;
    if (ch < 0)
        return 2040;                    /* not a sensor: idle level */
    double lsb_a = CURRENT_UA_PER_LSB / 1e6;            /* A per step */
    double f = cur_hz[ch] ? cur_hz[ch] : 50.0, w = 2 * M_PI * f * sim_ms / 1000.0;
    double i = cur_a[ch] * sqrt(2) * (sin(w + cur_ph[ch]) + cur_h3[ch] * sin(3 * w));
    double v = 2040 + i / lsb_a + cur_noise[ch] * ((rand() % 2001) / 1000.0 - 1.0);
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    return (uint16_t)lround(v);
}

static void run_window(void)
{
    for (int k = 0; k < CURRENT_SAMPLES; k++) {
        current_isr();
        sim_ms += 1.0;
    }
    current_task();
}
#include "../src/usb_desc.h"
#undef main

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int bd_index(int ep, int in) { return ep == 0 ? (in ? 0 : 1) : 1 + ep; }
static volatile bd_t *bd(int ep, int in) { return &BDT[bd_index(ep, in)]; }
static uint8_t *bdbuf(volatile bd_t *b) { return host_mem + USB_RAM_BASE + ((unsigned)b->epadr << 2); }

static void token_done(int ep, int in)
{
    STAT = (uint8_t)((ep << 4) | (in ? STAT_IN : 0));
    INTSTAT |= INT_TOKDNE;
    usb_poll();
    CHECK(!(INTSTAT & INT_TOKDNE), "TOKDNE not cleared");
}

static void bus_reset(void)
{
    INTSTAT |= INT_USBRST;
    usb_poll();
}

/* host sends SETUP */
static void setup(uint8_t t, uint8_t r, uint16_t v, uint16_t i, uint16_t l)
{
    volatile bd_t *b = bd(0, 0);
    uint8_t pkt[8] = { t, r, v & 0xFF, v >> 8, i & 0xFF, i >> 8, l & 0xFF, l >> 8 };
    CHECK(b->stat & BD_OWN, "EP0 OUT not armed for SETUP (req %02x)", r);
    memcpy(bdbuf(b), pkt, 8);
    b->cnt = 8;
    b->stat = (uint8_t)(PID_SETUP << 2);
    CTL |= CTL_TSUSPEND;
    token_done(0, 0);
    CHECK(!(CTL & CTL_TSUSPEND), "TSUSPEND not cleared");
}

/* host sends IN token; returns length, -1 = NAK, -2 = STALL */
static int in_token(int ep, uint8_t *out, int *data1)
{
    volatile bd_t *b = bd(ep, 1);
    if (!(b->stat & BD_OWN))
        return -1;
    if ((b->stat & BD_BDTSTALL) || (EPCTL(ep) & EPCTL_EPSTALL))
        return -2;
    int n = b->cnt;
    memcpy(out, bdbuf(b), n);
    if (data1) *data1 = !!(b->stat & BD_DATA1);
    b->stat = (uint8_t)(PID_IN << 2);
    token_done(ep, 1);
    return n;
}

/* host sends OUT; returns 0 ok, -1 NAK */
static int out_token(int ep, const uint8_t *d, int n, int data1)
{
    volatile bd_t *b = bd(ep, 0);
    if (!(b->stat & BD_OWN))
        return -1;
    if ((b->stat & BD_DTS) && !!(b->stat & BD_DATA1) != data1)
        return -3;      /* toggle mismatch: data ignored */
    memcpy(bdbuf(b), d, n);
    b->cnt = (uint8_t)n;
    b->stat = (uint8_t)(PID_OUT << 2);
    token_done(ep, 0);
    return 0;
}

/* complete control read: collects data, then does status OUT */
static int control_read(uint8_t t, uint8_t r, uint16_t v, uint16_t i, uint16_t l, uint8_t *buf)
{
    int total = 0, n, d1, expect = 1;
    setup(t, r, v, i, l);
    for (;;) {
        n = in_token(0, buf + total, &d1);
        if (n == -2) return -2;
        CHECK(n >= 0, "control read NAK");
        if (n < 0) return -1;
        CHECK(d1 == expect, "EP0 IN toggle wrong");
        expect ^= 1;
        total += n;
        if (n < EP0_SIZE || total >= l) break;
    }
    CHECK(out_token(0, (uint8_t *)"", 0, 1) == 0, "status OUT refused");
    return total;
}

static int control_write(uint8_t t, uint8_t r, uint16_t v, uint16_t i, const uint8_t *data, uint16_t l)
{
    uint8_t tmp[8];
    int d1;
    setup(t, r, v, i, l);
    if (l) {
        int rc = out_token(0, data, l, 1);
        CHECK(rc == 0, "data OUT refused rc=%d", rc);
    }
    int n = in_token(0, tmp, &d1);
    if (n == -2) return -2;
    CHECK(n == 0 && d1 == 1, "status IN not ZLP DATA1 (n=%d)", n);
    return 0;
}

static int ep3_toggle_host = 0;
static int bulk_rx(char *out)
{
    int total = 0, n, d1;
    for (int k = 0; k < 20; k++) {
        usb_poll();
        n = in_token(3, (uint8_t *)out + total, &d1);
        if (n < 0) break;
        CHECK(d1 == ep3_toggle_host, "EP3 toggle");
        ep3_toggle_host ^= 1;
        total += n;
    }
    out[total] = 0;
    return total;
}

/* ---- pulse log: records every pin at its active (high) level during a delay ---- */
static char pulse_log[1024];
static const char *inject_on_pause;     /* sent as bulk OUT during a >=1000 ms pause */
static int ep2_toggle_host;

static char capture[4096];
static int capture_len, capturing;

void host_delay_hook(uint16_t ms)
{
    sim_ms += ms;
    if (capturing) {                    /* host keeps reading during long commands */
        int d1, n;
        usb_poll();
        n = in_token(3, (uint8_t *)capture + capture_len, &d1);
        if (n > 0 && capture_len + n < (int)sizeof(capture) - 64) {
            capture_len += n;
            ep3_toggle_host ^= 1;
        } else if (n == 0) {
            ep3_toggle_host ^= 1;
        }
    }
    for (int port = 0; port < 7; port++) {
        uint8_t act = host_mem[port * 2] & host_mem[port * 2 + 1];
        for (int bit = 0; bit < 8; bit++) {
            if (act & (1 << bit)) {
                char t[16];
                if (ms == RELAY_PULSE_MS) snprintf(t, sizeof t, "P%c%d ", 'A' + port, bit);
                else snprintf(t, sizeof t, "P%c%d:%d ", 'A' + port, bit, ms);
                strcat(pulse_log, t);
            }
        }
    }
    if (inject_on_pause && ms >= 1000) {
        const char *c = inject_on_pause;
        inject_on_pause = 0;
        volatile bd_t *b = bd(2, 0);
        memcpy(bdbuf(b), c, strlen(c));
        b->cnt = (uint8_t)strlen(c);
        b->stat = (uint8_t)(PID_OUT << 2);
        ep2_toggle_host ^= 1;
        token_done(2, 0);
    }
}

static uint8_t header_ok_test(uint16_t p)
{
    return host_mem[p] != 0xFF && (uint8_t)~host_mem[p] == host_mem[p + 1];
}

static int reset_requested;
void host_reset_hook(void) { reset_requested = 1; }

static void expect_pulses(const char *want)
{
    size_t l = strlen(pulse_log);
    if (l && pulse_log[l - 1] == ' ') pulse_log[l - 1] = 0;
    CHECK(strcmp(pulse_log, want) == 0, "pulses: got '%s' want '%s'", pulse_log, want);
    pulse_log[0] = 0;
}

static void send_cmd(const char *s)
{
    uint8_t buf[64];
    int rc = out_token(2, (const uint8_t *)s, (int)strlen(s), ep2_toggle_host);
    CHECK(rc == 0, "bulk OUT refused rc=%d", rc);
    ep2_toggle_host ^= 1;
    /* firmware main loop body */
    usb_poll();
    uint8_t n;
    while ((n = cdc_read(buf)) != 0) {   /* main loop until nothing is pending */
        for (uint8_t i = 0; i < n; i++) feed((char)buf[i]);
        usb_poll();
    }
}

static void expect_reply(const char *cmd, const char *reply)
{
    char rx[512];
    send_cmd(cmd);
    bulk_rx(rx);
    CHECK(strcmp(rx, reply) == 0, "cmd '%s': got '%s' want '%s'", cmd, rx, reply);
}

static void reconnect(void)
{
    usb_init();
    bus_reset();
    CHECK(control_write(0x00, 5, 7, 0, 0, 0) == 0, "set address again");
    CHECK(control_write(0x00, 9, 1, 0, 0, 0) == 0, "set configuration again");
    CHECK(control_write(0x21, 0x22, 3, 0, 0, 0) == 0, "DTR again");
    ep2_toggle_host = ep3_toggle_host = 0;
}

/* NVFIX: boot page 0xFE00-0xFFFF as left by USBDM -unsecure */
extern int host_flash_ops;
static void nv_page(uint8_t nvprot, uint8_t nvopt, uint8_t fprot, uint8_t fopt, uint16_t reset_vec)
{
    int i;
    for (i = 0; i < 0x200; i++)
        host_mem[0xFE00 + i] = 0xFF;
    for (i = 0; i < 0x80; i++)               /* end of the bootloader code */
        host_mem[0xFE00 + i] = (uint8_t)(i * 7 + 1);
    memcpy(host_mem + 0xFFA0, "\x12\x34\x56\x78\x9A\xBC\xDE\xF0\x55\xAA", 10);
    for (i = 0xFFC0; i < 0xFFFE; i += 2) {   /* bootloader vectors */
        host_mem[i] = 0xF0; host_mem[i + 1] = (uint8_t)i;
    }
    host_mem[0xFFFE] = (uint8_t)(reset_vec >> 8);
    host_mem[0xFFFF] = (uint8_t)reset_vec;
    host_mem[0xFFBD] = nvprot;
    host_mem[0xFFBF] = nvopt;
    host_mem[0x1824] = fprot;                /* FPROT, FOPT: copies made by the reset */
    host_mem[0x1821] = fopt;
    reset_requested = 0;
    memset(host_mem + BOOT_MAGIC_ADDR, 0, 4);
}

static void test_nvfix(void)
{
    uint8_t save[0x200], want[0x200], fprot = host_mem[0x1824], fopt = host_mem[0x1821];
    int ops;
    memcpy(save, host_mem + 0xFE00, 0x200);

    /* correct and active */
    nv_page(0xDA, 0x3E, 0xDA, 0x02, 0xDC00);
    expect_reply("NVFIX\n", "OK nothing to do\r\n");
    CHECK(!reset_requested, "nvfix: no reset when ok");

    /* USBDM -unsecure: NVPROT erased, NVOPT 0xFE -> page erase + rewrite */
    nv_page(0xFF, 0xFE, 0xFF, 0xC2, 0xDC00);
    memcpy(want, host_mem + 0xFE00, 0x200);
    want[0x1BD] = 0xDA;
    want[0x1BF] = 0x3E;
    expect_reply("CURRENT\n", "ERR no interrupt vector - FOPT C2 FPROT FF - send NVFIX to repair\r\n");
    expect_reply("NVFIX\n", "OK fixed, restarting\r\n");
    CHECK(memcmp(host_mem + 0xFE00, want, 0x200) == 0, "nvfix: page rewritten");
    CHECK(reset_requested && !memcmp(host_mem + BOOT_MAGIC_ADDR, "RSET", 4) && USBCTL0 == 0,
          "nvfix: reset request");
    reconnect();

    /* both erased -> only two bytes programmed, no erase */
    nv_page(0xFF, 0xFF, 0xFF, 0xC2, 0xDC00);
    memcpy(want, host_mem + 0xFE00, 0x200);
    want[0x1BD] = 0xDA;
    want[0x1BF] = 0x3E;
    ops = host_flash_ops;
    expect_reply("NVFIX\n", "OK fixed, restarting\r\n");
    CHECK(host_flash_ops - ops == 2, "nvfix: %d flash ops, want 2", host_flash_ops - ops);
    CHECK(memcmp(host_mem + 0xFE00, want, 0x200) == 0, "nvfix: two bytes");
    reconnect();

    /* wrong values, but the page is protected */
    nv_page(0xC0, 0xFE, 0xC0, 0xC2, 0xDC00);
    memcpy(want, host_mem + 0xFE00, 0x200);
    expect_reply("NVFIX\n", "ERR nvfix 03 FOPT C2 FPROT C0\r\n");
    CHECK(memcmp(host_mem + 0xFE00, want, 0x200) == 0 && !reset_requested, "nvfix: protected untouched");

    /* reset vector not in the bootloader -> refuse */
    nv_page(0xFF, 0xFE, 0xFF, 0xC2, 0x1960);
    memcpy(want, host_mem + 0xFE00, 0x200);
    expect_reply("NVFIX\n", "ERR nvfix 04 FOPT C2 FPROT FF\r\n");
    CHECK(memcmp(host_mem + 0xFE00, want, 0x200) == 0 && !reset_requested, "nvfix: no bootloader");

    memcpy(host_mem + 0xFE00, save, 0x200);
    host_mem[0x1824] = fprot;
    host_mem[0x1821] = fopt;
}

int main(void)
{
    uint8_t buf[256];
    int n;

    memset(host_mem + 0x10B0, 0xFF, 0x1800 - 0x10B0);   /* erased flash block */
    relay_init();                        /* nothing saved: all OFF */
    expect_pulses("PE1 PE3 PE5 PE7 PF1 PF3");
    CHECK(PTEDD == 0xFF && PTED == 0x00 && PTFDD == 0x0F, "relay init");
    usb_init();
    CHECK((USBCTL0 & 0x45) == 0x45 && (CTL & CTL_USBEN), "usb_init regs");
    bus_reset();

    n = control_read(0x80, 6, 0x0100, 0, 64, buf);
    CHECK(n == 18 && buf[0] == 18 && buf[1] == 1 && buf[7] == EP0_SIZE, "device desc n=%d", n);

    CHECK(control_write(0x00, 5, 7, 0, 0, 0) == 0, "set address");
    CHECK(ADDR == 7, "address not applied (ADDR=%d)", ADDR);

    n = control_read(0x80, 6, 0x0200, 0, 9, buf);
    CHECK(n == 9 && buf[2] == USB_CONFIG_DESC_LEN, "config desc header");
    n = control_read(0x80, 6, 0x0200, 0, 255, buf);
    CHECK(n == USB_CONFIG_DESC_LEN, "config desc full n=%d", n);
    /* walk descriptors: lengths must add up exactly */
    { int p = 0, cnt = 0; while (p < n) { CHECK(buf[p] > 0, "zero length desc"); p += buf[p]; cnt++; }
      CHECK(p == n && cnt == 10, "descriptor chain p=%d cnt=%d", p, cnt); }
    /* exactly 64 requested -> 2 full packets, no ZLP needed */
    n = control_read(0x80, 6, 0x0200, 0, 64, buf);
    CHECK(n == 64, "config 64 n=%d", n);

    n = control_read(0x80, 6, 0x0300, 0, 255, buf);
    CHECK(n == 4 && buf[2] == 0x09 && buf[3] == 0x04, "lang id");
    n = control_read(0x80, 6, 0x0302, 0x0409, 255, buf);
    CHECK(n == 2 + 2 * (int)strlen(USB_PRODUCT_STR), "product string n=%d", n);
    CHECK(control_read(0x80, 6, 0x0600, 0, 10, buf) == -2, "device qualifier must stall");
    /* after a stall the next SETUP must work */
    n = control_read(0x80, 6, 0x0100, 0, 18, buf);
    CHECK(n == 18, "request after stall");

    CHECK(control_write(0x00, 9, 1, 0, 0, 0) == 0, "set configuration");
    n = control_read(0x80, 8, 0, 0, 1, buf);
    CHECK(n == 1 && buf[0] == 1, "get configuration");

    uint8_t lc[7] = { 0x80, 0x25, 0, 0, 0, 0, 8 };     /* 9600 8N1 */
    CHECK(control_write(0x21, 0x20, 0, 0, lc, 7) == 0, "set line coding");
    n = control_read(0xA1, 0x21, 0, 0, 7, buf);
    CHECK(n == 7 && memcmp(buf, lc, 7) == 0, "get line coding");

    /* port closed (DTR low): output is dropped, relays still switch */
    send_cmd("ON 1\n");
    expect_pulses("PE0");
    char rx[512];
    CHECK(bulk_rx(rx) == 0, "no output without DTR");

    CHECK(control_write(0x21, 0x22, 3, 0, 0, 0) == 0, "set control line state");

    expect_reply("on 3\r\n", "OK\r\n");
    expect_pulses("PE4");
    CHECK(PTED == 0 && PTFD == 0, "pins back to idle");
    expect_reply("GET\n", "STATE 05\r\n");
    expect_reply("ON 3\n", "OK\r\n");      /* ON always pulses */
    expect_pulses("PE4");
    expect_reply("SET 25\n", "OK\r\n");    /* 05 -> 25: only R6 changes */
    expect_pulses("PF2");
    expect_reply("GET\n", "STATE 25\r\n");
    expect_reply("SET 7F\n", "ERR mask\r\n");  /* only 6 relays */
    expect_reply("OFF ALL\n", "OK\r\n");
    expect_pulses("PE1 PE3 PE5 PE7 PF1 PF3");
    expect_reply("TOGGLE 6\n", "OK\r\n");
    expect_pulses("PF2");
    expect_reply("ON 7\n", "ERR relay\r\n");
    expect_reply("ON 9\n", "ERR relay\r\n");
    expect_reply("FOO\n", "ERR command 'FOO'\r\n");
    expect_reply("\x1b[AX\n", "ERR command '<1B>[AX'\r\n");
    expect_reply("IDX\x7f\n", "ID 6-Channel Relay Board FW 1.11 SN NOSERIAL\r\n");  /* backspace */
    expect_reply("ON 2\nON 4\nGET\n", "OK\r\nOK\r\nSTATE 2A\r\n");
    expect_pulses("PE2 PE6");
    /* command split across two packets */
    send_cmd("ON ");
    expect_reply("5\n", "OK\r\n");
    expect_pulses("PF0");

    /* debug commands are gone */
    expect_reply("PULSE PTA3\n", "ERR command 'PULSE PTA3'\r\n");
    expect_reply("SCAN\n", "ERR command 'SCAN'\r\n");
    expect_pulses("");

    /* BOOTLOADER: OK, magic word in RAM, USB detached, reset */
    expect_reply("BOOTLOADER\n", "OK\r\n");
    CHECK(reset_requested && !memcmp(host_mem + BOOT_MAGIC_ADDR, "BOOT", 4) && USBCTL0 == 0,
          "bootloader request");
    memset(host_mem + BOOT_MAGIC_ADDR, 0, 4);
    reconnect();                            /* back to normal for the rest */

    test_nvfix();

    /* long output (>64 bytes) spans packets */
    send_cmd("HELP\n");
    n = bulk_rx(rx);
    CHECK(strstr(rx, "TOGGLE") != NULL, "help n=%d", n);
    /* 16 commands in one 64-byte packet -> 160 bytes reply over 3 packets */
    send_cmd("GET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\nGET\n");
    n = bulk_rx(rx);
    CHECK(n == 160, "multi-packet reply n=%d", n);
    /* 64-byte reply ends on packet boundary -> must be followed by a ZLP */
    {
        uint8_t tmp[64]; int d1, n1, n2, n3;
        send_cmd("GET\nGET\nON 9\nON 9\nON 9\nON 9\n");  /* 2*10 + 4*11 = 64 */
        usb_poll();
        n1 = in_token(3, tmp, &d1); ep3_toggle_host ^= 1; usb_poll();
        n2 = in_token(3, tmp, &d1); ep3_toggle_host ^= 1; usb_poll();
        n3 = in_token(3, tmp, &d1);
        CHECK(n1 == 64 && n2 == 0 && n3 == -1, "ZLP n1=%d n2=%d n3=%d", n1, n2, n3);
    }

    /* endpoint halt on EP3 + clear */
    CHECK(control_write(0x02, 3, 0, 0x83, 0, 0) == 0, "set halt");
    n = control_read(0x82, 0, 0, 0x83, 2, buf);
    CHECK(n == 2 && buf[0] == 1, "ep halted status");
    CHECK(control_write(0x02, 1, 0, 0x83, 0, 0) == 0, "clear halt");
    ep3_toggle_host = 0;
    expect_reply("GET\n", "STATE 3A\r\n");

    /* unknown class request stalls, recovers */
    CHECK(control_read(0xA1, 0x55, 0, 0, 4, buf) == -2, "unknown class req stall");
    n = control_read(0x80, 0, 0, 0, 2, buf);
    CHECK(n == 2, "get status after stall");

    /* ---- serial number (slot in the bootloader area) ---- */
    memset(host_mem + SERIAL_ADDR, 0xFF, 10);
    expect_reply("SERIAL\n", "ERR no serial\r\n");
    n = control_read(0x80, 6, 0x0303, 0x0409, 255, buf);
    CHECK(n == 2 + 2 * 8 && buf[2] == 'N', "USB serial NOSERIAL n=%d", n);
    {
        /* CRC 0x904C computed by tools/mkfw.py crc16() - cross-checks C and Python */
        static const uint8_t slot[10] = { 0x3F, 0x9A, 0x07, 0xC2, 0xD4, 0x1B, 0x88, 0xE5, 0x90, 0x4C };
        memcpy(host_mem + SERIAL_ADDR, slot, 10);
        expect_reply("SERIAL\n", "SERIAL 3F9A07C2D41B88E5\r\n");
        expect_reply("ID\n", "ID 6-Channel Relay Board FW 1.11 SN 3F9A07C2D41B88E5\r\n");
        n = control_read(0x80, 6, 0x0303, 0x0409, 255, buf);
        char sn[17] = { 0 };
        for (int k = 0; k < 16; k++) sn[k] = (char)buf[2 + 2 * k];
        CHECK(n == 34 && !strcmp(sn, "3F9A07C2D41B88E5"), "USB serial '%s'", sn);
        host_mem[SERIAL_ADDR + 9] ^= 1;            /* damaged CRC */
        expect_reply("SERIAL\n", "ERR no serial\r\n");
        memset(host_mem + SERIAL_ADDR, 0, 10);
    }

    /* ---- relay state in flash ---- */
    expect_reply("SET 25\n", "OK\r\n");
    pulse_log[0] = 0;
    CHECK(nvstate_load() == 0x25, "saved state %02x", nvstate_load());
    relay_init();                       /* "power cycle": restore 25 = R1, R3, R6 on */
    expect_pulses("PE0 PE3 PE4 PE7 PF1 PF2");
    CHECK(relay_get_mask() == 0x25, "restored mask");
    expect_reply("GET\n", "STATE 25\r\n");
    expect_reply("OFF 1\n", "OK\r\n");
    pulse_log[0] = 0;
    CHECK(nvstate_load() == 0x24, "OFF saved");
    {
        /* many changes: page switches, old page erased, last value wins */
        int i, pages_ok = 1;
        for (i = 0; i < 700; i++) {
            uint8_t v = (uint8_t)(i % 64);
            if (!nvstate_save(v) || nvstate_load() != v) pages_ok = 0;
        }
        CHECK(pages_ok, "700 saves");
        uint8_t a = header_ok_test(NV_PAGE_A), b = header_ok_test(NV_PAGE_B);
        CHECK(a + b == 1, "exactly one page active (a=%d b=%d)", a, b);
        /* interrupted programming of the next entry: previous value stays */
        uint8_t last = nvstate_load();
        uint16_t pg = a ? NV_PAGE_A : NV_PAGE_B, off = 2;
        while (host_mem[pg + off] != 0xFF || host_mem[pg + off + 1] != 0xFF) off += 2;
        host_mem[pg + off] = 0x12;          /* value written, complement not */
        CHECK(nvstate_load() == last, "damaged entry ignored");
        CHECK(nvstate_save(0x07) && nvstate_load() == 0x07, "save after damaged entry");
        /* both pages valid (power failed before the old page was erased) */
        uint16_t other = pg == NV_PAGE_A ? NV_PAGE_B : NV_PAGE_A;
        memset(host_mem + other, 0xFF, 512);
        host_mem[other] = (uint8_t)(host_mem[pg] == 0 ? 254 : host_mem[pg] - 1);  /* older */
        host_mem[other + 1] = (uint8_t)~host_mem[other];
        host_mem[other + 2] = 0x3F; host_mem[other + 3] = 0xC0;
        CHECK(nvstate_load() == 0x07, "newer page wins");
        /* nothing saved after mass erase */
        memset(host_mem + 0x10B0, 0xFF, 0x1800 - 0x10B0);
        CHECK(nvstate_load() == 0, "erased -> 0");
    }

    /* ---- current measurement ---- */
    {
        uint16_t i50, irms;
        /* constant used in current.c */
        {
            double P = 0;
            for (int k = 0; k < 20; k++)
                P += round(127 * cos(2 * M_PI * k / 20)) * cos(2 * M_PI * k / 20);
            CHECK(fabs(P * CURRENT_SAMPLES / 20 * sqrt(2) * 2032 / 512 - 35701) < 1, "RMS50_DIV constant");
        }

        current_init();
        CHECK(!current_get(0, &i50, &irms), "no data before the first window");
        expect_reply("CURRENT\n", "ERR no data yet\r\n");
        cur_a[0] = 1.0;                                 /* 1 A */
        cur_a[1] = 5.0; cur_h3[1] = 0.3;                /* 5 A + 30 % 3rd harmonic */
        cur_noise[2] = 3;                               /* no load, noise */
        cur_a[3] = 10.0; cur_ph[3] = 1.0;               /* 10 A, other phase */
        cur_a[4] = 2.0; cur_hz[4] = 49.8;               /* mains frequency off */
        cur_a[5] = 14.0;                                /* near full scale */
        run_window();
        static const double want50[6] = { 1.0, 5.0, 0.0, 10.0, 2.0, 14.0 };
        for (int c = 0; c < 6; c++) {
            CHECK(current_get(c, &i50, &irms), "data ch %d", c);
            double tol = want50[c] * 0.01 + 0.03;
            CHECK(fabs(i50 / 1000.0 - want50[c]) <= tol, "ch %d: 50 Hz %.3f A, want %.3f",
                  c, i50 / 1000.0, want50[c]);
        }
        current_get(1, &i50, &irms);
        CHECK(fabs(irms / 1000.0 - 5.0 * sqrt(1.09)) < 0.06, "true RMS with harmonic %.3f", irms / 1000.0);
        current_get(2, &i50, &irms);
        CHECK(i50 < 30 && irms < 40, "noise only: 50 Hz %u mA, rms %u mA", i50, irms);
        expect_reply("CURRENT\n", "CURRENT 1:1.00 2:5.00 3:0.00 4:10.00 5:2.00 6:14.00\r\n");
        {
            char rx[256];
            send_cmd("CURRENT RMS\n");
            bulk_rx(rx);
            CHECK(!strncmp(rx, "CURRENT 1:1.00 2:5.2", 20), "CURRENT RMS '%s'", rx);
        }
        expect_reply("CURRENT X\n", "ERR args\r\n");

        /* a flash write in the middle restarts the window */
        for (int k = 0; k < 50; k++) { current_isr(); sim_ms += 1; }
        current_pause();
        current_resume();
        current_task();                                 /* not complete: nothing */
        cur_a[0] = 3.0;
        run_window();
        current_get(0, &i50, &irms);
        CHECK(fabs(i50 / 1000.0 - 3.0) < 0.05, "after restart %.3f", i50 / 1000.0);

        /* ADC debug report */
        adc_fixed[ADC_CH_BANDGAP] = 983;                /* 1.2 V at VDD 5 V */
        {
            char rx[1024];
            capture_len = 0;
            capturing = 1;
            send_cmd("ADC\n");
            capturing = 0;
            bulk_rx(rx);
            strcat(capture, rx);
            CHECK(strstr(capture, "ADC 0 mean 20") && strstr(capture, "ADC 11 mean") &&
                  strstr(capture, "ADC VDD 5000 mV") && strstr(capture, "OK\r\n"),
                  "ADC report '%s'", capture);
        }
        pulse_log[0] = 0;
    }

    /* bus reset deconfigures */
    bus_reset();
    CHECK(!cdc_is_configured() && ADDR == 0, "reset");

    printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures != 0;
}
