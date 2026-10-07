#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);          /* TPM1: 1 ms overflow at 24 MHz bus clock */

/* Waits ms milliseconds while keeping USB serviced. */
void delay_ms(uint16_t ms);

#endif
