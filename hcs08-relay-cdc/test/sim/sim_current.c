/*
 * sim_current.c - Runs the current evaluation (src/current.c, compiled by
 * SDCC with the application flags) in the uCsim HCS08 simulator, see
 * run_sim.py. Checks SDCC's 32-bit arithmetic against a Python model.
 *
 *   0x6000..  samples [100][6], uint16 big endian (loaded by run_sim.py)
 *   0x0C00..  results: per relay 50 Hz mA, RMS mA (uint16 each), valid flag
 */
#include <stdint.h>
#include "jm60.h"
#include "current.h"

#define SAMPLES ((const uint16_t *)0x6000)
#define RESULT  ((volatile uint16_t *)0x0C00)
#define VALID   (*(volatile uint8_t *)0x0C20)

static uint16_t t;

uint16_t host_adc(uint8_t ch)
{
    return SAMPLES[t * 6 + ch];
}

/* stubs for the CDC/timer functions used by the ADC debug report */
void cdc_putc(char c) { (void)c; }
void cdc_puts(const char *s) { (void)s; }
void delay_ms(uint16_t ms) { (void)ms; }

unsigned char _sdcc_external_startup(void) { return 0; }

void sim_done(void)
{
    for (;;)
        ;
}

void main(void)
{
    uint8_t c;
    uint16_t a, b;
    current_init();
    for (t = 0; t < CURRENT_SAMPLES; t++)
        current_isr();
    current_task();
    VALID = 1;
    for (c = 0; c < 6; c++) {
        if (!current_get(c, &a, &b))
            VALID = 0;
        RESULT[2 * c] = a;
        RESULT[2 * c + 1] = b;
    }
    sim_done();
}
