/*
 * disk.c - Emulated FAT12 drive + S-record programming (see disk.h).
 *
 * Volume (1 MiB, 512-byte sectors, 1 sector per cluster):
 *   LBA 0        boot sector
 *   LBA 1..6     FAT 1          LBA 7..12  FAT 2
 *   LBA 13..14   root directory (32 entries)
 *   LBA 15..     data, cluster 2 = INFO.TXT
 *
 * Writing: the host writes the file data into free clusters, normally in
 * ascending order. Each written chunk of the data area is fed into an
 * S-record line parser. A line split across two sectors is joined when the
 * second chunk directly follows the first; after a jump the parser restarts
 * at the next line. Every record carries its own address and checksum, so
 * the order of the sectors does not matter otherwise. Completeness is
 * checked with the CRC from the info block: only when the end record has
 * been seen and the CRC over the application area matches, the info block
 * is programmed (last) and the application becomes valid.
 */
#include "jm60.h"
#include "util.h"
#include "layout.h"
#include "flash.h"
#include "disk.h"

#define BOOT_VERSION "1.0"

#define FAT_SECTORS   6u
#define FAT1_LBA      1u
#define FAT2_LBA      (FAT1_LBA + FAT_SECTORS)
#define ROOT_LBA      (FAT2_LBA + FAT_SECTORS)
#define ROOT_SECTORS  2u
#define DATA_LBA      (ROOT_LBA + ROOT_SECTORS)
#define INFO_SIZE     320u

#define FLASH8(a) (*HW_PTR(a))

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), nibble table        */
/* ------------------------------------------------------------------ */

static const uint16_t crc_tab[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
};

static uint16_t crc_byte(uint16_t crc, uint8_t b)
{
    crc = (uint16_t)(crc << 4) ^ crc_tab[(uint8_t)(crc >> 12) ^ (b >> 4)];
    crc = (uint16_t)(crc << 4) ^ crc_tab[(uint8_t)(crc >> 12) ^ (b & 0x0F)];
    return crc;
}

static uint16_t crc_range(uint16_t crc, uint16_t from, uint16_t to)
{
    while (from != to) {
        crc = crc_byte(crc, FLASH8(from));
        from++;
    }
    return crc;
}

static uint16_t app_crc(void)
{
    uint16_t crc = crc_range(0xFFFF, APP_START, APP_INFO);
    return crc_range(crc, APP_VECTORS, BOOT_START);
}

/* ------------------------------------------------------------------ */
/* Application / serial number                                        */
/* ------------------------------------------------------------------ */

uint8_t app_valid(void)
{
    uint16_t entry;
    if (FLASH8(APP_INFO) != 'R' || FLASH8(APP_INFO + 1) != 'L' ||
        FLASH8(APP_INFO + 2) != 'F' || FLASH8(APP_INFO + 3) != 'W')
        return 0;
    entry = ((uint16_t)FLASH8(APP_RESET_VEC) << 8) | FLASH8(APP_RESET_VEC + 1);
    return entry >= APP_START && entry < APP_INFO;
}

static const char hexdig[] = "0123456789ABCDEF";

uint8_t serial_string(char *buf)
{
    uint8_t i, blank = 1;
    uint16_t crc = 0xFFFF;
    for (i = 0; i < SERIAL_LEN; i++) {
        uint8_t b = FLASH8(SERIAL_ADDR + i);
        if (b != 0xFF)
            blank = 0;
        crc = crc_byte(crc, b);
        buf[2 * i] = hexdig[b >> 4];
        buf[2 * i + 1] = hexdig[b & 0x0F];
    }
    buf[2 * SERIAL_LEN] = 0;
    if (blank || FLASH8(SERIAL_ADDR + SERIAL_LEN) != (uint8_t)(crc >> 8) ||
        FLASH8(SERIAL_ADDR + SERIAL_LEN + 1) != (uint8_t)crc) {
        mem_cpy(buf, "NOSERIAL", 9);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Update session                                                     */
/* ------------------------------------------------------------------ */

static uint8_t status;
static uint8_t erased;
static uint8_t got_end;
static __xdata uint8_t info_buf[APP_INFO_SIZE];
static uint16_t info_have;          /* bit n = info byte n received */

static void new_session(void)
{
    status = UPD_NONE;
    erased = 0;
    got_end = 0;
    info_have = 0;
    mem_set(info_buf, 0xFF, sizeof(info_buf));
}

static uint8_t accepting(void)
{
    return status == UPD_NONE || status == UPD_RUNNING || status == UPD_ERR_CRC;
}

static uint8_t erase_app(void)
{
    uint16_t a;
    if (!flash_erase_page(APP_START))
        return 0;
    for (a = (APP_START | (FLASH_PAGE - 1)) + 1; a != BOOT_START; a += FLASH_PAGE) {
        if (!flash_erase_page(a))
            return 0;
    }
    return 1;
}

static void program_record(uint16_t addr, const uint8_t *data, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++, addr++) {
        if (addr >= APP_INFO && addr < APP_INFO + APP_INFO_SIZE) {
            info_buf[addr - APP_INFO] = data[i];         /* programmed last */
            info_have |= (uint16_t)1 << (addr - APP_INFO);
            continue;
        }
        if (!((addr >= APP_START && addr < APP_INFO) ||
              (addr >= APP_VECTORS && addr < BOOT_START))) {
            status = UPD_ERR_RANGE;
            return;
        }
        if (!erased) {
            erased = 1;
            if (!erase_app()) {
                status = UPD_ERR_FLASH;
                return;
            }
        }
        if (!flash_program(addr, data[i])) {
            status = UPD_ERR_FLASH;
            return;
        }
    }
}

static void try_finish(void)
{
    uint8_t i;
    if (!got_end || !accepting())
        return;
    if (info_have != 0xFFFF || info_buf[0] != 'R' || info_buf[1] != 'L' ||
        info_buf[2] != 'F' || info_buf[3] != 'W' || !erased ||
        app_crc() != (((uint16_t)info_buf[4] << 8) | info_buf[5])) {
        status = UPD_ERR_CRC;       /* may still complete with later sectors */
        return;
    }
    for (i = 0; i < APP_INFO_SIZE; i++) {
        if (!flash_program(APP_INFO + i, info_buf[i])) {
            status = UPD_ERR_FLASH;
            return;
        }
    }
    status = app_valid() ? UPD_OK : UPD_ERR_FLASH;
}

/* ------------------------------------------------------------------ */
/* S-record parser                                                    */
/* ------------------------------------------------------------------ */

#define LINE_MAX 80                 /* S1 with 32 data bytes = 74 chars */
static __xdata char line[LINE_MAX];
static uint8_t line_len;
static uint8_t line_skip;           /* line too long: ignore up to newline */
static uint16_t expect_lba;
static uint16_t expect_off;

static int8_t hexval(char c)
{
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
    return -1;
}

static void process_line(void)
{
    static __xdata uint8_t rec[(LINE_MAX - 2) / 2];
    uint8_t i, n, sum;

    if (line_len < 10 || line[0] != 'S' || (line_len & 1))
        return;
    n = (uint8_t)((line_len - 2) / 2);              /* count + payload */
    for (i = 0; i < n; i++) {
        int8_t h = hexval(line[2 + 2 * i]), l = hexval(line[3 + 2 * i]);
        if (h < 0 || l < 0)
            return;
        rec[i] = (uint8_t)((h << 4) | l);
    }
    if (rec[0] != n - 1)
        return;                                     /* length mismatch */
    sum = 0;
    for (i = 0; i < n; i++)
        sum += rec[i];
    if (sum != 0xFF)
        return;                                     /* checksum error */

    switch (line[1]) {
    case '0':                                       /* header = start of a file */
        if (status != UPD_OK && (got_end || !accepting()))
            new_session();                          /* new copy after a failed one */
        break;
    case '1':
        if (!accepting())
            break;
        status = UPD_RUNNING;
        program_record(((uint16_t)rec[1] << 8) | rec[2], rec + 3, (uint8_t)(n - 4));
        if (status == UPD_RUNNING && got_end)
            status = UPD_ERR_CRC;                   /* still waiting for the rest */
        break;
    case '9':
        if (accepting())
            got_end = 1;
        break;
    case '5':
        break;
    default:                                        /* S2/S3/S7/S8: not for us */
        if (accepting())
            status = UPD_ERR_RANGE;
        break;
    }
}

void disk_write(uint16_t lba, uint16_t offset, const uint8_t *data, uint8_t n)
{
    uint8_t i;
    if (lba < DATA_LBA)
        return;                                     /* FAT, directory: ignore */
    if (lba != expect_lba || offset != expect_off) {
        line_len = 0;                               /* not a continuation */
        line_skip = 0;
    }
    for (i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r' || c == 0) {
            if (!line_skip && line_len)
                process_line();
            line_len = 0;
            line_skip = 0;
        } else if (line_len < LINE_MAX) {
            line[line_len++] = c;
        } else {
            line_skip = 1;
        }
    }
    offset += n;
    if (offset >= DISK_SECTOR) {
        expect_lba = lba + 1;
        expect_off = 0;
    } else {
        expect_lba = lba;
        expect_off = offset;
    }
}

void disk_write_end(void)
{
    try_finish();
}

uint8_t disk_status(void)
{
    return status;
}

void disk_init(void)
{
    new_session();
    line_len = 0;
    line_skip = 0;
    expect_lba = 0xFFFF;
    expect_off = 0;
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

static const uint8_t boot_sector[62] = {
    0xEB, 0x3C, 0x90,                               /* jump */
    'M', 'S', 'W', 'I', 'N', '4', '.', '1',         /* OEM name */
    0x00, 0x02,                                     /* 512 bytes per sector */
    1,                                              /* sectors per cluster */
    1, 0,                                           /* reserved sectors */
    2,                                              /* number of FATs */
    32, 0,                                          /* root entries */
    (uint8_t)DISK_SECTORS, (uint8_t)(DISK_SECTORS >> 8),
    0xF8,                                           /* media: fixed disk */
    FAT_SECTORS, 0,                                 /* sectors per FAT */
    32, 0, 2, 0,                                    /* sectors/track, heads */
    0, 0, 0, 0,                                     /* hidden sectors */
    0, 0, 0, 0,                                     /* total sectors (32 bit) */
    0x80, 0, 0x29,                                  /* drive, reserved, ext. sig */
    0x52, 0x4C, 0x42, 0x31,                         /* volume ID */
    'R', 'E', 'L', 'A', 'Y', 'B', 'O', 'O', 'T', ' ', ' ',
    'F', 'A', 'T', '1', '2', ' ', ' ', ' '
};

/* Directory entry: name(11) attr ... start cluster @26, size @28 */
static void dir_entry(uint8_t *e, const char *name, uint8_t attr,
                      uint8_t cluster, uint16_t size)
{
    mem_cpy(e, name, 11);
    e[11] = attr;
    e[24] = 0x21; e[25] = 0x5C;                     /* date 2026-01-01 */
    e[18] = 0x21; e[19] = 0x5C;                     /* access date */
    e[26] = cluster;
    e[28] = (uint8_t)size;
    e[29] = (uint8_t)(size >> 8);
}

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static void info_text(char *p)
{
    char *start = p;
    char ser[2 * SERIAL_LEN + 1];
    uint8_t i;
    static const char * const st[] = {
        "no update yet",
        "INCOMPLETE - copy the file again",
        "OK",
        "ERROR - wrong file (address outside the application area)",
        "ERROR - flash erase/program failed",
        "ERROR - file incomplete (CRC mismatch), copy it again",
    };

    p = put(p, "Relay board bootloader " BOOT_VERSION "\r\nFirmware: ");
    if (app_valid()) {
        for (i = 0; i < 8; i++) {
            char c = (char)FLASH8(APP_INFO + APP_INFO_VERSION + i);
            if (c < 0x20 || c > 0x7E)
                break;
            *p++ = c;
        }
    } else {
        p = put(p, "none");
    }
    serial_string(ser);
    p = put(p, "\r\nSerial:   ");
    p = put(p, ser);
    p = put(p, "\r\nUpdate:   ");
    p = put(p, st[status]);
    p = put(p, "\r\n\r\nCopy relay_fw-*.s19 onto this drive to update.\r\n"
               "Eject the drive to start the firmware.\r\n");
    while (p < start + INFO_SIZE - 2)
        *p++ = ' ';
    p[0] = '\r';
    p[1] = '\n';
}

void disk_read(uint16_t lba, uint8_t *buf)
{
    mem_set(buf, 0, DISK_SECTOR);
    if (lba == 0) {
        mem_cpy(buf, boot_sector, sizeof(boot_sector));
        buf[510] = 0x55;
        buf[511] = 0xAA;
    } else if (lba == FAT1_LBA || lba == FAT2_LBA) {
        buf[0] = 0xF8;                              /* clusters 0, 1 */
        buf[1] = 0xFF;
        buf[2] = 0xFF;
        buf[3] = 0xFF;                              /* cluster 2: end of chain */
        buf[4] = 0x0F;
    } else if (lba == ROOT_LBA) {
        dir_entry(buf, "RELAYBOOT  ", 0x08, 0, 0);  /* volume label */
        dir_entry(buf + 32, "INFO    TXT", 0x01, 2, INFO_SIZE);
    } else if (lba == DATA_LBA) {
        info_text((char *)buf);
    }
}
