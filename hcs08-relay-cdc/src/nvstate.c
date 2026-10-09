/*
 * nvstate.c - Relay state log in flash (see nvstate.h).
 *
 * Two 512-byte pages, A and B. Page layout:
 *   +0, +1     generation g, ~g     (g = 0..254, the higher modulo 255 wins)
 *   +2, +3     entry 1: value, ~value
 *   ...        up to 255 entries
 * A new value is appended as the next entry (erased flash = 0xFF 0xFF).
 * When the page is full, the other page is erased, gets generation g+1 and
 * the value as first entry, and only then the old page is erased. A power
 * failure at any point leaves either the old or the new value readable:
 * an entry whose two bytes do not match (interrupted programming) is
 * skipped, a page without a valid header is ignored.
 *
 * Endurance: one page erase per 255 changes, so 10,000 erase cycles per
 * page (data sheet minimum) allow about 5 million relay state changes.
 */
#include "jm60.h"
#include "flash.h"
#include "nvstate.h"

#define FLASH8(a)       (*HW_PTR(a))
#define FLASH_PAGE_SIZE 512u

static uint16_t page;           /* active page, 0 = none */
static uint8_t  gen;            /* its generation */
static uint16_t next;           /* offset of the next free entry */
static uint8_t  value;          /* last saved value */

static uint8_t header_ok(uint16_t p)
{
    uint8_t g = FLASH8(p);
    return g != 0xFF && (uint8_t)~g == FLASH8(p + 1);
}

/* Scans the active page: last valid value and next free slot */
static void scan(void)
{
    uint16_t off;
    value = 0;
    for (off = 2; off < FLASH_PAGE_SIZE; off += 2) {
        uint8_t v = FLASH8(page + off), c = FLASH8(page + off + 1);
        if (v == 0xFF && c == 0xFF)
            break;                              /* first free entry */
        if ((uint8_t)~v == c)
            value = v;                          /* valid entry */
    }
    next = off;
}

uint8_t nvstate_load(void)
{
    uint8_t a = header_ok(NV_PAGE_A), b = header_ok(NV_PAGE_B);

    page = 0;
    value = 0;
    if (a && b) {
        /* both valid: power failed while switching pages, the newer wins */
        uint8_t ga = FLASH8(NV_PAGE_A), gb = FLASH8(NV_PAGE_B);
        page = ((uint8_t)(gb == 254 ? 0 : gb + 1) == ga) ? NV_PAGE_A : NV_PAGE_B;
    } else if (a) {
        page = NV_PAGE_A;
    } else if (b) {
        page = NV_PAGE_B;
    }
    if (page) {
        gen = FLASH8(page);
        scan();
    }
    return value;
}

static uint8_t put_entry(uint16_t addr, uint8_t v)
{
    return flash_program(addr, v) && flash_program(addr + 1, (uint8_t)~v);
}

/* Starts a fresh page p with generation g and value v */
static uint8_t start_page(uint16_t p, uint8_t g, uint8_t v)
{
    if (!flash_erase_page(p) || !flash_program(p, g) || !flash_program(p + 1, (uint8_t)~g) ||
        !put_entry(p + 2, v))
        return 0;
    page = p;
    gen = g;
    next = 4;
    return 1;
}

uint8_t nvstate_save(uint8_t v)
{
    uint16_t old;

    if (v == 0xFF)
        return 0;
    if (page && v == value)
        return 1;                               /* nothing changed */
    if (!page) {
        if (!start_page(NV_PAGE_A, 0, v))
            return 0;
    } else if (next < FLASH_PAGE_SIZE) {
        if (!put_entry(page + next, v)) {
            next += 2;                          /* skip the damaged slot */
            return 0;
        }
        next += 2;
    } else {
        old = page;
        if (!start_page(old == NV_PAGE_A ? NV_PAGE_B : NV_PAGE_A,
                        (uint8_t)(gen == 254 ? 0 : gen + 1), v))
            return 0;
        flash_erase_page(old);
    }
    value = v;
    return 1;
}
