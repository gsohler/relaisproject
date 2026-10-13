#ifndef NVFIX_H
#define NVFIX_H

#include <stdint.h>
#include "jm60.h"            /* __reentrant on the host */

#define NVFIX_OK         0  /* NV bytes correct and active */
#define NVFIX_RESET      1  /* NV bytes correct in flash, active after a reset */
#define NVFIX_NEEDED     2  /* wrong and repairable */
#define NVFIX_PROTECTED  3  /* wrong, but the page is write-protected */
#define NVFIX_NO_BOOT    4  /* reset vector does not point to the bootloader */
#define NVFIX_FAILED     5  /* flash command or verify failed */

uint8_t nvfix_state(void) __reentrant;
uint8_t nvfix_run(void) __reentrant;

#endif
