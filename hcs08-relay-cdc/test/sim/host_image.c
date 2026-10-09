/* host_image.c - writes the emulated volume (gcc build) to stdout */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "jm60.h"
#include "layout.h"
#include "disk.h"
uint8_t host_mem[0x10000];
int main(void)
{
    uint8_t b[512];
    /* same flash content as in the simulator: app area erased, the rest 0 */
    memset(host_mem + APP_START, 0xFF, BOOT_START - APP_START);
    disk_init();
    for (unsigned lba = 0; lba < DISK_SECTORS; lba++) {
        disk_read((uint16_t)lba, b);
        fwrite(b, 1, 512, stdout);
    }
    return 0;
}
