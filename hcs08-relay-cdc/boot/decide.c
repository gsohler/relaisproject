/*
 * decide.c - Start decision (see decide.h).
 *
 * Crash fallback: a reset caused by the COP watchdog, an illegal opcode,
 * an illegal address or a loss of clock counts as a crash, unless the
 * request word says it was intentional (BOOTLOADER command, reset after an
 * update). After BOOT_CRASH_LIMIT crashes in a row the bootloader stays
 * active, so a broken firmware can be replaced without USBDM. The
 * application clears the counter after running APP_CONFIRM_MS. Power-on,
 * low-voltage and reset-pin resets clear it too.
 */
#include "jm60.h"
#include "layout.h"
#include "decide.h"

uint8_t boot_decide(uint8_t srs, uint8_t request, uint8_t *count,
                    uint8_t emergency, uint8_t app_ok)
{
    uint8_t crash = (srs & (SRS_COP | SRS_ILOP | SRS_ILAD | SRS_LOC)) != 0;

    if ((srs & (SRS_POR | SRS_LVD | SRS_PIN)) || request != REQ_NONE || !crash)
        *count = 0;
    else if (*count < 255)
        (*count)++;

    if (request == REQ_BOOT)
        return BOOT_REQUESTED;
    if (emergency)
        return BOOT_EMERGENCY;
    if (*count >= BOOT_CRASH_LIMIT) {
        *count = 0;                     /* eject = try the firmware again */
        return BOOT_CRASHED;
    }
    if (!app_ok)
        return BOOT_NO_APP;
    return BOOT_START_APP;
}
