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

void host_delay_hook(uint16_t ms)
{
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

int main(void)
{
    uint8_t buf[256];
    int n;

    relay_init();                        /* pulses all OFF inputs */
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
    expect_reply("IDX\x7f\n", "ID 6-Channel Relay Board FW 1.2\r\n");  /* backspace */
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

    /* bus reset deconfigures */
    bus_reset();
    CHECK(!cdc_is_configured() && ADDR == 0, "reset");

    printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures != 0;
}
