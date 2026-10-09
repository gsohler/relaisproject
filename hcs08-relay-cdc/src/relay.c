/*
 * relay.c - Latching relay driver: every relay has an ON and an OFF input
 * that is pulsed for RELAY_PULSE_MS.
 */
#include "relay.h"
#include "board.h"
#include "timer.h"

#define PIN_PORT(p) ((uint8_t)((p) >> 4))
#define PIN_BIT(p)  ((uint8_t)((p) & 0x07))
#define PIN_MASK(p) ((uint8_t)(1u << PIN_BIT(p)))
#define PIN_DATA(p) HW_PTR(PIN_PORT(p) * 2)      /* PTxD  */
#define PIN_DDR(p)  HW_PTR(PIN_PORT(p) * 2 + 1)  /* PTxDD */

#if RELAY_MAP_VALID
/* Drive pin to its inactive level and make it an output */
static void pin_idle_output(uint8_t pin, uint8_t active_low)
{
    uint8_t m = PIN_MASK(pin);
    if (active_low)
        *PIN_DATA(pin) |= m;
    else
        *PIN_DATA(pin) &= (uint8_t)~m;
    *PIN_DDR(pin) |= m;
}

/* One pulse of ms milliseconds on pin, then back to the inactive level */
static void pin_pulse(uint8_t pin, uint16_t ms, uint8_t active_low)
{
    uint8_t m = PIN_MASK(pin);

    pin_idle_output(pin, active_low);
    *PIN_DATA(pin) ^= m;                    /* active */
    delay_ms(ms);
    *PIN_DATA(pin) ^= m;                    /* inactive */
}

static const uint8_t pins[RELAY_COUNT][2] = RELAY_PINS;   /* {ON, OFF} */
#endif
static uint8_t state;

uint8_t relay_map_valid(void)
{
    return RELAY_MAP_VALID;
}

void relay_init(void)
{
    state = 0;
#if RELAY_MAP_VALID
    {
        uint8_t i;
        for (i = 0; i < RELAY_COUNT; i++) {
            pin_idle_output(pins[i][0], RELAY_ACTIVE_LOW);
            pin_idle_output(pins[i][1], RELAY_ACTIVE_LOW);
        }
#if RELAY_OFF_AT_BOOT
        for (i = 0; i < RELAY_COUNT; i++)
            relay_switch(i, 0);
#endif
    }
#endif
}

void relay_switch(uint8_t idx, uint8_t on)
{
#if RELAY_MAP_VALID
    if (idx >= RELAY_COUNT)
        return;
    pin_pulse(pins[idx][on ? 0 : 1], RELAY_PULSE_MS, RELAY_ACTIVE_LOW);
    delay_ms(RELAY_GAP_MS);
    if (on)
        state |= (uint8_t)(1u << idx);
    else
        state &= (uint8_t)~(1u << idx);
#else
    (void)idx;
    (void)on;
#endif
}

void relay_set_mask(uint8_t mask)
{
    uint8_t i, m;
    for (i = 0; i < RELAY_COUNT; i++) {
        m = (uint8_t)(1u << i);
        if ((state ^ mask) & m)             /* pulse only relays that change */
            relay_switch(i, (mask & m) != 0);
    }
}

uint8_t relay_get_mask(void)
{
    return state;
}
