#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

void    flash_init(void);                           /* set FCDIV for 24 MHz bus */
uint8_t flash_erase_page(uint16_t addr);            /* 1 = ok */
uint8_t flash_program(uint16_t addr, uint8_t value); /* 1 = ok (also if already equal) */

#endif
