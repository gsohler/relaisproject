/* util.c - see util.h */
#include "util.h"

void mem_set(void *dst, uint8_t value, uint16_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = value;
}

void mem_cpy(void *dst, const void *src, uint16_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
}
