/*
 * decide.h - Start decision of the bootloader: application or bootloader?
 */
#ifndef DECIDE_H
#define DECIDE_H

#include <stdint.h>

/* Why the bootloader stays active (shown in INFO.TXT) */
enum {
    BOOT_START_APP = 0,     /* start the application */
    BOOT_NO_APP,            /* no complete firmware */
    BOOT_REQUESTED,         /* CDC command BOOTLOADER */
    BOOT_EMERGENCY,         /* SDA tied to GND at power-up */
    BOOT_CRASHED            /* firmware crashed BOOT_CRASH_LIMIT times in a row */
};

/* Request word values (BOOT_MAGIC_ADDR) */
enum { REQ_NONE = 0, REQ_BOOT, REQ_RESET };

/*
 * srs:       reset status register (cause of this reset)
 * request:   REQ_* from the request word
 * count:     crash counter from RAM, already validated (0 if invalid);
 *            updated in place
 * emergency: SDA tied to GND
 * app_ok:    complete application present
 */
uint8_t boot_decide(uint8_t srs, uint8_t request, uint8_t *count,
                    uint8_t emergency, uint8_t app_ok);

#endif
