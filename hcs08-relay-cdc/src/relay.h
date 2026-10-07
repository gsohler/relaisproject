#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>

void    relay_init(void);            /* pins inactive; pulses all OFF at boot */
uint8_t relay_map_valid(void);       /* RELAY_MAP_VALID from board.h */

/* Pulse the ON (on=1) or OFF (on=0) input of relay idx (0..RELAY_COUNT-1), always. */
void    relay_switch(uint8_t idx, uint8_t on);

/* bit0 = relay 1 ... bit5 = relay 6; pulses only relays whose state changes */
void    relay_set_mask(uint8_t mask);
uint8_t relay_get_mask(void);        /* state as last switched by firmware */

/* Pin helpers for debugging. pin = (port << 4) | bit, port 0..6 = A..G */
uint8_t pin_usable(uint8_t pin);
void    pin_pulse(uint8_t pin, uint16_t ms, uint8_t active_low, uint8_t restore);

#endif
