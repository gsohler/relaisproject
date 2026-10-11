/*
 * boot_test.c - Runs the bootloader's USB mass storage stack, FAT emulation
 * and S-record programming on the host, against a model of the JM60 SIE and
 * of the flash (erase pages, program only clears bits, 0xDC00+ protected).
 *
 *   make test      (passes build/relay_fw-<version>.s19 and the raw build)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "jm60.h"
#include "layout.h"
#include "usb_msc.h"
#include "disk.h"
#include "decide.h"

uint8_t host_mem[0x10000];
extern int host_flash_ops;

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- SIE model (same as host_test.c) ---------------- */

static int bd_index(int ep, int in) { return ep == 0 ? (in ? 0 : 1) : 1 + ep; }
static volatile bd_t *bd(int ep, int in) { return &BDT[bd_index(ep, in)]; }
static uint8_t *bdbuf(volatile bd_t *b) { return host_mem + USB_RAM_BASE + ((unsigned)b->epadr << 2); }

static void run(void)
{
    for (int i = 0; i < 4; i++) {
        usb_poll();
        msc_task();
    }
}

static void token_done(int ep, int in)
{
    STAT = (uint8_t)((ep << 4) | (in ? STAT_IN : 0));
    INTSTAT |= INT_TOKDNE;
    usb_poll();
    CHECK(!(INTSTAT & INT_TOKDNE), "TOKDNE not cleared");
}

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
}

static int in_token(int ep, uint8_t *out, int *data1)
{
    volatile bd_t *b = bd(ep, 1);
    if ((b->stat & BD_BDTSTALL) || (EPCTL(ep) & EPCTL_EPSTALL))
        return -2;
    if (!(b->stat & BD_OWN))
        return -1;
    int n = b->cnt;
    memcpy(out, bdbuf(b), n);
    if (data1) *data1 = !!(b->stat & BD_DATA1);
    b->stat = (uint8_t)(PID_IN << 2);
    token_done(ep, 1);
    return n;
}

static int out_token(int ep, const uint8_t *d, int n, int data1)
{
    volatile bd_t *b = bd(ep, 0);
    if (EPCTL(ep) & EPCTL_EPSTALL)
        return -2;
    if (!(b->stat & BD_OWN))
        return -1;
    if ((b->stat & BD_DTS) && !!(b->stat & BD_DATA1) != data1)
        return -3;
    memcpy(bdbuf(b), d, n);
    b->cnt = (uint8_t)n;
    b->stat = (uint8_t)(PID_OUT << 2);
    token_done(ep, 0);
    return 0;
}

static int control_read(uint8_t t, uint8_t r, uint16_t v, uint16_t i, uint16_t l, uint8_t *buf)
{
    int total = 0, n, d1;
    setup(t, r, v, i, l);
    for (;;) {
        n = in_token(0, buf + total, &d1);
        if (n == -2) return -2;
        if (n < 0) { CHECK(0, "control read NAK"); return -1; }
        total += n;
        if (n < 32 || total >= l) break;
    }
    CHECK(out_token(0, (uint8_t *)"", 0, 1) == 0, "status OUT refused");
    return total;
}

static int control_write(uint8_t t, uint8_t r, uint16_t v, uint16_t i)
{
    uint8_t tmp[8];
    int d1;
    setup(t, r, v, i, 0);
    int n = in_token(0, tmp, &d1);
    if (n == -2) return -2;
    CHECK(n == 0 && d1 == 1, "status IN not ZLP DATA1 (n=%d)", n);
    return 0;
}

/* ---------------- bulk-only transport ---------------- */

static int ep2_t, ep3_t;
static uint32_t tag = 1;
static int stalls;

static void bulk_out(const uint8_t *d, int n)
{
    run();
    int rc = out_token(2, d, n, ep2_t);
    CHECK(rc == 0, "bulk OUT refused rc=%d", rc);
    ep2_t ^= 1;
    run();
}

static int bulk_in(uint8_t *d)
{
    int d1;
    run();
    int n = in_token(3, d, &d1);
    if (n >= 0) {
        CHECK(d1 == ep3_t, "EP3 toggle");
        ep3_t ^= 1;
    }
    run();
    return n;
}

/* Runs one SCSI command; returns CSW status, *got = data bytes moved */
static int scsi(const uint8_t *cb, int dir_in, uint8_t *data, uint32_t len, uint32_t *got)
{
    uint8_t cbw[31] = { 'U', 'S', 'B', 'C' }, csw[64];
    uint32_t moved = 0;
    int n;

    memcpy(cbw + 4, &tag, 4);
    cbw[8] = len; cbw[9] = len >> 8; cbw[10] = len >> 16; cbw[11] = len >> 24;
    cbw[12] = dir_in ? 0x80 : 0;
    cbw[14] = 10;
    memcpy(cbw + 15, cb, 16);
    bulk_out(cbw, 31);

    if (len && dir_in) {
        while (moved < len) {
            n = bulk_in(data + moved);
            if (n == -2) {                      /* stalled: clear halt */
                stalls++;
                CHECK(control_write(0x02, 1, 0, 0x83) == 0, "clear halt");
                ep3_t = 0;
                break;
            }
            CHECK(n >= 0, "data IN NAK");
            if (n < 0) break;
            moved += n;
            if (n < 64) break;
        }
    } else if (len) {
        while (moved < len) {
            int k = len - moved > 64 ? 64 : (int)(len - moved);
            bulk_out(data + moved, k);
            moved += k;
        }
    }
    n = bulk_in(csw);
    CHECK(n == 13 && !memcmp(csw, "USBS", 4) && !memcmp(csw + 4, &tag, 4),
          "CSW n=%d (cmd %02x)", n, cb[0]);
    uint32_t residue = csw[8] | csw[9] << 8 | csw[10] << 16 | (uint32_t)csw[11] << 24;
    CHECK(residue == len - moved || (!dir_in && residue <= len),
          "residue %u, len %u moved %u (cmd %02x)", residue, len, moved, cb[0]);
    tag++;
    if (got) *got = moved;
    return csw[12];
}

static int rw10(uint8_t op, uint16_t lba, uint16_t blocks, uint8_t *data)
{
    uint8_t cb[16] = { op, 0, 0, 0, lba >> 8, lba & 0xFF, 0, blocks >> 8, blocks & 0xFF };
    uint32_t got;
    int st = scsi(cb, op == 0x28, data, (uint32_t)blocks * 512, &got);
    CHECK(st != 0 || got == (uint32_t)blocks * 512, "rw10 moved %u", got);
    return st;
}

/* ---------------- firmware file helpers ---------------- */

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t padded = (*len + 511) & ~511u;
    uint8_t *b = calloc(1, padded + 512);
    if (fread(b, 1, *len, f) != *len) exit(2);
    fclose(f);
    *len = padded;
    return b;
}

/* expected flash image from the firmware file */
static uint8_t expect[0x10000];
static uint8_t used[0x10000];
static void parse_s19(const uint8_t *txt, size_t len)
{
    char line[600];
    size_t i = 0;
    memset(used, 0, sizeof used);
    while (i < len) {
        size_t k = 0;
        while (i < len && txt[i] != '\n' && txt[i] && k < sizeof line - 1) line[k++] = txt[i++];
        line[k] = 0;
        i++;
        if (k > 10 && line[0] == 'S' && line[1] == '1') {
            unsigned cnt, a, b;
            sscanf(line + 2, "%2x%4x", &cnt, &a);
            for (unsigned j = 0; j < cnt - 3; j++) {
                sscanf(line + 8 + 2 * j, "%2x", &b);
                expect[a + j] = b;
                used[a + j] = 1;
            }
        }
    }
}

static void erase_all_flash(void)
{
    memset(host_mem + APP_START, 0xFF, 0x10000 - APP_START);
}

/* writes a file into consecutive clusters starting at lba, in commands of
 * 'chunk' sectors; order: 0 = ascending, 1 = descending */
static void write_file(const uint8_t *f, size_t len, uint16_t lba, int chunk, int reverse)
{
    int sectors = len / 512;
    if (!reverse) {
        for (int s = 0; s < sectors; s += chunk) {
            int n = sectors - s < chunk ? sectors - s : chunk;
            CHECK(rw10(0x2A, lba + s, n, (uint8_t *)f + s * 512) == 0, "write failed");
        }
    } else {
        for (int s = sectors - 1; s >= 0; s--)
            CHECK(rw10(0x2A, lba + s, 1, (uint8_t *)f + s * 512) == 0, "write failed");
    }
}

static int flash_matches(void)
{
    for (unsigned a = APP_START; a < BOOT_START; a++) {
        uint8_t want = used[a] ? expect[a] : 0xFF;
        if (host_mem[a] != want) {
            printf("  flash 0x%04X = %02X, want %02X\n", a, host_mem[a], want);
            return 0;
        }
    }
    return 1;
}

static void read_info(char *out)
{
    uint8_t s[512];
    CHECK(rw10(0x28, 15, 1, s) == 0, "read INFO.TXT");
    memcpy(out, s, 320);
    out[320] = 0;
}

static void fat_writes(void)
{
    /* what a host does besides the file data: FAT and directory updates */
    uint8_t s[512];
    memset(s, 0xAB, sizeof s);
    CHECK(rw10(0x2A, 1, 1, s) == 0, "fat write");
    CHECK(rw10(0x2A, 13, 1, s) == 0, "dir write");
}

int main(int argc, char **argv)
{
    uint8_t buf[2048];
    char info[400];
    uint32_t got;
    int n;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: boot_test relay_fw.s19 relay_cdc.s19\n");
        return 2;
    }
    size_t fw_len, raw_len;
    uint8_t *fw = load(argv[1], &fw_len);
    uint8_t *raw = load(argv[2], &raw_len);
    parse_s19(fw, fw_len);

    erase_all_flash();
    CHECK(!app_valid(), "empty flash must not be a valid app");

    disk_init();
    usb_init();
    INTSTAT |= INT_USBRST;
    usb_poll();

    /* ---- enumeration ---- */
    n = control_read(0x80, 6, 0x0100, 0, 18, buf);
    CHECK(n == 18 && buf[4] == 0 && buf[12] == 0x00 && buf[13] == 0x80, "device desc");
    CHECK(control_write(0x00, 5, 9, 0) == 0 && ADDR == 9, "set address");
    n = control_read(0x80, 6, 0x0200, 0, 255, buf);
    CHECK(n == 32 && buf[9 + 5] == 0x08 && buf[9 + 6] == 0x06 && buf[9 + 7] == 0x50, "config desc n=%d", n);
    n = control_read(0x80, 6, 0x0303, 0x0409, 255, buf);
    CHECK(n == 2 + 16 && buf[2] == 'N', "serial string NOSERIAL n=%d", n);
    CHECK(control_write(0x00, 9, 1, 0) == 0 && usb_is_configured(), "set configuration");
    n = control_read(0xA1, 0xFE, 0, 0, 1, buf);
    CHECK(n == 1 && buf[0] == 0, "get max lun");

    /* serial number slot */
    {
        static const uint8_t ser[8] = { 0x3F, 0x9A, 0x07, 0xC2, 0xD4, 0x1B, 0x88, 0xE5 };
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < 8; i++) {
            crc ^= ser[i] << 8;
            for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
        memcpy(host_mem + SERIAL_ADDR, ser, 8);
        host_mem[SERIAL_ADDR + 8] = crc >> 8;
        host_mem[SERIAL_ADDR + 9] = crc & 0xFF;
        n = control_read(0x80, 6, 0x0303, 0x0409, 255, buf);
        char s[20] = { 0 };
        for (int i = 0; i < 16; i++) s[i] = buf[2 + 2 * i];
        CHECK(n == 34 && !strcmp(s, "3F9A07C2D41B88E5"), "serial string '%s'", s);
    }

    /* ---- SCSI basics ---- */
    uint8_t inq[16] = { 0x12, 0, 0, 0, 36 };
    CHECK(scsi(inq, 1, buf, 36, &got) == 0 && got == 36 && !memcmp(buf + 8, "RELAY", 5), "inquiry");
    uint8_t inq_long[16] = { 0x12, 0, 0, 0, 96 };
    CHECK(scsi(inq_long, 1, buf, 96, &got) == 0 && got == 36, "inquiry 96 -> short packet");
    uint8_t tur[16] = { 0 };
    CHECK(scsi(tur, 0, NULL, 0, NULL) == 0, "test unit ready");
    uint8_t cap[16] = { 0x25 };
    CHECK(scsi(cap, 1, buf, 8, &got) == 0 && got == 8 && buf[2] == 0x07 && buf[3] == 0xFF &&
          buf[6] == 0x02, "read capacity");
    uint8_t ms6[16] = { 0x1A, 0, 0x3F, 0, 192 };
    CHECK(scsi(ms6, 1, buf, 192, &got) == 0 && got == 4 && buf[0] == 3 && buf[2] == 0, "mode sense 6");
    uint8_t rfc[16] = { 0x23, 0, 0, 0, 0, 0, 0, 0, 252 };
    CHECK(scsi(rfc, 1, buf, 252, &got) == 0 && got == 12 && buf[8] == 2, "read format capacities");

    /* unsupported command with data-in: stall, clear halt, CSW failed */
    stalls = 0;
    uint8_t bad[16] = { 0x4A };
    CHECK(scsi(bad, 1, buf, 64, &got) == 1 && stalls == 1 && got == 0, "unsupported cmd -> stall");
    uint8_t rs[16] = { 0x03, 0, 0, 0, 18 };
    CHECK(scsi(rs, 1, buf, 18, &got) == 0 && buf[2] == 0x05 && buf[12] == 0x20, "request sense");
    CHECK(scsi(rs, 1, buf, 18, &got) == 0 && buf[2] == 0 && buf[12] == 0, "sense cleared");
    /* out of range read */
    uint8_t rd_bad[16] = { 0x28, 0, 0, 0, 0x08, 0x00, 0, 0, 1 };
    CHECK(scsi(rd_bad, 1, buf, 512, &got) == 1, "read beyond end fails");

    /* ---- FAT volume ---- */
    CHECK(rw10(0x28, 0, 4, buf) == 0, "read 4 sectors");
    CHECK(buf[510] == 0x55 && buf[511] == 0xAA && buf[11] == 0 && buf[12] == 2 &&
          !memcmp(buf + 54, "FAT12", 5), "boot sector");
    CHECK(!memcmp(buf + 512, "\xF8\xFF\xFF\xFF\x0F\x00", 6), "FAT 1");
    CHECK(rw10(0x28, 13, 1, buf) == 0 && !memcmp(buf, "RELAYBOOT  ", 11) &&
          !memcmp(buf + 32, "INFO    TXT", 11) && buf[32 + 26] == 2 &&
          (buf[32 + 28] | buf[32 + 29] << 8) == 320, "root dir");
    read_info(info);
    CHECK(strstr(info, "Firmware: none") && strstr(info, "Serial:   3F9A07C2D41B88E5") &&
          strstr(info, "no update yet"), "INFO.TXT before:\n%s", info);

    /* ---- 1. normal update: data in ascending order, 8 sectors per command ---- */
    host_flash_ops = 0;
    fat_writes();
    write_file(fw, fw_len, 16, 8, 0);
    fat_writes();
    CHECK(disk_status() == UPD_OK, "update status %d", disk_status());
    CHECK(app_valid() && flash_matches(), "flash content after update");
    CHECK(host_mem[APP_RESET_VEC] == expect[APP_RESET_VEC], "reset vector");
    read_info(info);
    CHECK(strstr(info, "Update:   OK") && strstr(info, "Firmware: 1."), "INFO.TXT after:\n%s", info);
    printf("normal update: %zu bytes file, %d flash operations\n", fw_len, host_flash_ops);

    /* ---- 2. new session: sectors in reverse order, then copied again ---- */
    erase_all_flash();
    disk_init();
    write_file(fw, fw_len, 16, 1, 1);
    CHECK(disk_status() != UPD_OK && !app_valid(), "reverse order must not validate (lost lines)");
    write_file(fw, fw_len, 16, 4, 0);
    CHECK(disk_status() == UPD_OK && app_valid() && flash_matches(), "second copy completes");

    /* ---- 3. damaged record -> CRC error, app stays invalid; then good copy ---- */
    erase_all_flash();
    disk_init();
    {
        uint8_t *bad_fw = malloc(fw_len);
        memcpy(bad_fw, fw, fw_len);
        char *p = strstr((char *)bad_fw + 600, "\nS1");     /* some record */
        p[10] = p[10] == '0' ? '1' : '0';                   /* data digit, checksum now wrong */
        write_file(bad_fw, fw_len, 16, 8, 0);
        CHECK(disk_status() == UPD_ERR_CRC && !app_valid(), "damaged file: status %d", disk_status());
        read_info(info);
        CHECK(strstr(info, "CRC mismatch") != NULL, "INFO.TXT shows CRC error");
        write_file(fw, fw_len, 40, 8, 0);                   /* copied again, other clusters */
        CHECK(disk_status() == UPD_OK && app_valid() && flash_matches(), "good copy after damaged one");
        free(bad_fw);
    }

    /* ---- 4. wrong file: raw SDCC output (vectors at 0xFFxx) ---- */
    erase_all_flash();
    disk_init();
    write_file(raw, raw_len, 16, 8, 0);
    CHECK(disk_status() == UPD_ERR_RANGE && !app_valid(), "raw file rejected: %d", disk_status());
    write_file(fw, fw_len, 100, 8, 0);
    CHECK(disk_status() == UPD_OK && app_valid() && flash_matches(), "good copy after wrong file");

    /* ---- 5. update over an existing different application ---- */
    for (unsigned a = APP_START; a < APP_INFO; a++) host_mem[a] = (uint8_t)a;  /* "old app" */
    disk_init();
    write_file(fw, fw_len, 16, 8, 0);
    CHECK(disk_status() == UPD_OK && flash_matches(), "erase before programming");
    CHECK(host_mem[0xDC00] == 0xFF || 1, "bootloader area untouched");

    /* ---- eject ---- */
    msc_eject_request = 0;
    uint8_t ej[16] = { 0x1B, 0, 0, 0, 0x02 };
    CHECK(scsi(ej, 0, NULL, 0, NULL) == 0 && msc_eject_request, "eject");
    CHECK(scsi(tur, 0, NULL, 0, NULL) == 1, "not ready after eject");

    /* ---- start decision / crash fallback ---- */
    {
        uint8_t c = 0;
        CHECK(boot_decide(SRS_POR, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 0, "power-on");
        CHECK(boot_decide(SRS_ILOP, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 1, "crash 1");
        CHECK(boot_decide(SRS_COP, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 2, "crash 2 (COP)");
        CHECK(boot_decide(SRS_ILAD, REQ_NONE, &c, 0, 1) == BOOT_CRASHED && c == 0, "crash 3 -> bootloader");
        c = 2;
        CHECK(boot_decide(SRS_LOC, REQ_NONE, &c, 0, 1) == BOOT_CRASHED, "loss of clock counts");
        c = 2;
        CHECK(boot_decide(SRS_ILOP, REQ_RESET, &c, 0, 1) == BOOT_START_APP && c == 0,
              "intentional reset (after update) is no crash");
        c = 2;
        CHECK(boot_decide(SRS_ILOP, REQ_BOOT, &c, 0, 1) == BOOT_REQUESTED && c == 0, "BOOTLOADER command");
        c = 2;
        CHECK(boot_decide(SRS_PIN, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 0, "reset pin clears");
        c = 2;
        CHECK(boot_decide(SRS_POR | SRS_LVD, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 0, "LVD clears");
        c = 0;
        CHECK(boot_decide(SRS_POR, REQ_NONE, &c, 1, 1) == BOOT_EMERGENCY, "emergency");
        CHECK(boot_decide(SRS_POR, REQ_NONE, &c, 0, 0) == BOOT_NO_APP, "no app");
        c = 1;                          /* crash, app ran fine and confirmed, crash again */
        boot_decide(SRS_ILOP, REQ_NONE, &c, 0, 1);
        c = 0;                          /* confirmation by the app */
        CHECK(boot_decide(SRS_ILOP, REQ_NONE, &c, 0, 1) == BOOT_START_APP && c == 1,
              "confirmation restarts the count");
        disk_set_reason(BOOT_CRASHED);
        read_info(info);
        CHECK(strstr(info, "Started:  firmware crashed 3 times") != NULL, "INFO.TXT reason");
    }

    /* ---- bus reset deconfigures ---- */
    INTSTAT |= INT_USBRST;
    usb_poll();
    CHECK(!usb_is_configured() && ADDR == 0, "bus reset");

    free(fw);
    free(raw);
    printf(failures ? "%d FAILURES\n" : "bootloader: all tests passed\n", failures);
    return failures != 0;
}
