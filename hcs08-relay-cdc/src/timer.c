/*
 * timer.c - 1 ms time base on TPM1 (overflow flag polled, no interrupt).
 */
#include "jm60.h"
#include "timer.h"
#include "usb_cdc.h"

#define BUS_HZ 24000000UL
#define TICK_MOD ((uint16_t)(BUS_HZ / 1000 - 1))   /* 23999 -> 1 ms */

void timer_init(void)
{
    TPM1SC = 0;                 /* stop */
    TPM1MODH = (uint8_t)(TICK_MOD >> 8);   /* high byte first (latched) */
    TPM1MODL = (uint8_t)TICK_MOD;
    TPM1SC = TPMSC_CLKSA;       /* bus clock, prescaler 1, start */
}

#ifdef HOST_TEST
extern void host_delay_hook(uint16_t ms);
void delay_ms(uint16_t ms)
{
    host_delay_hook(ms);
    usb_poll();
}
#else
void delay_ms(uint16_t ms)
{
    while (ms) {
        usb_poll();
        if (TPM1SC & TPMSC_TOF) {
            TPM1SC &= (uint8_t)~TPMSC_TOF;  /* clear: read with TOF set, write 0 */
            ms--;
        }
    }
}
#endif
