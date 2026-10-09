/*
 * util.h - Tiny replacements for memset/memcpy/strcpy.
 *
 * The bootloader is compiled with --stack-auto (function arguments and
 * locals on the stack; the direct page is too small otherwise). SDCC ships
 * no stack-auto library for the s08 port: the library functions expect their
 * arguments in fixed RAM cells and would get garbage. So the bootloader must
 * not call memset, memcpy, strcpy & co.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

void mem_set(void *dst, uint8_t value, uint16_t n);
void mem_cpy(void *dst, const void *src, uint16_t n);

#endif
