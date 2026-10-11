/*
 * layout.h - Flash/RAM layout shared by the bootloader, the application,
 * the host tests and tools/mkfw.py (keep the numbers there in sync!).
 *
 *   0x1200 .. 0x15FF  relay state log (application, nvstate.c), kept by updates
 *   0x1960 .. 0xDBAF  application code and constants
 *   0xDBB0 .. 0xDBBF  application info block (written last by the bootloader)
 *   0xDBC0 .. 0xDBFF  application vector table (hardware vector redirection)
 *   0xDC00 .. 0xFFFF  bootloader (9 KiB), write-protected via NVPROT
 *       0xFFA0 .. 0xFFA9  serial number slot (8 bytes + CRC16), production
 *       0xFFB0 .. 0xFFBF  NV registers (NVPROT, NVOPT, ...)
 *       0xFFC0 .. 0xFFFF  real vector table, only the reset vector is used
 *
 * With NVPROT protecting 0xDC00-0xFFFF and FNORED = 0 in NVOPT, the CPU
 * fetches all interrupt vectors except reset from 0xDBC0-0xDBFD. Reset always
 * starts the bootloader, which then jumps to the address stored at 0xDBFE.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#define APP_START        0x1960u
#define APP_INFO         0xDBB0u    /* info block, 16 bytes */
#define APP_VECTORS      0xDBC0u    /* redirected vectors */
#define APP_RESET_VEC    0xDBFEu    /* application entry address */
#define BOOT_START       0xDC00u
#define VECTOR_OFFSET    0x2400u    /* 0xFFC0 - APP_VECTORS */

#define FLASH_PAGE       512u

/* Info block (APP_INFO):
 *   +0  'R' 'L' 'F' 'W'         magic
 *   +4  CRC16 high, low         CRC-16/CCITT-FALSE over APP_START..APP_INFO-1
 *                               and APP_VECTORS..BOOT_START-1 (erased = 0xFF)
 *   +6  version, 8 bytes ASCII, NUL padded
 *   +14 flags: bit 0 = 0 -> the application services the COP watchdog,
 *       so the bootloader enables it (1 s). Older firmware has 0xFF.
 *   +15 reserved (0xFF)
 */
#define APP_INFO_SIZE    16u
#define APP_INFO_VERSION 6u
#define APP_INFO_FLAGS   14u        /* bit 0 = 0: application services the COP */
#define APP_FLAG_NO_COP  0x01

/* Serial number slot inside the protected bootloader area:
 * 8 bytes serial (big endian) + CRC16 (high, low) of those 8 bytes. */
#define SERIAL_ADDR      0xFFA0u
#define SERIAL_LEN       8u

/* NVPROT 0xDA: FPS = 0x6D -> 0xDC00-0xFFFF protected, FPDIS = 0.
 * NVOPT  0x3E: bit 7 KEYEN = 0 (backdoor off), bit 6 FNORED = 0 (vector
 *              redirection ON), bits 5..2 unused, SEC = 10 (unsecured).
 *              Bootloader 1.0 had 0x42 = FNORED 1: no redirection, so
 *              application interrupts went to the erased real vectors.
 *              The application checks this at runtime (current.c). */
#define BOOT_NVPROT      0xDA
#define BOOT_NVOPT       0x3E

/* RAM that survives a reset (not power-on). Both stacks start below it
 * (--stack-loc 0x10A9).
 *   0x10AA, 0x10AB  crash counter: count, ~count (see boot/decide.c)
 *   0x10AC..0x10AF  request word:
 *                   "BOOT" = stay in the bootloader (CDC command BOOTLOADER)
 *                   "RSET" = intentional reset by the bootloader, not a crash */
#define BOOT_COUNT_ADDR  0x10AAu
#define BOOT_CRASH_LIMIT 3          /* abnormal resets in a row -> bootloader */
#define APP_CONFIRM_MS   5000u      /* app runs this long -> counter cleared */

#define BOOT_MAGIC_ADDR  0x10ACu
#define BOOT_MAGIC_0     'B'
#define BOOT_MAGIC_1     'O'
#define BOOT_MAGIC_2     'O'
#define BOOT_MAGIC_3     'T'

#endif
