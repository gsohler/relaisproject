/*
 * usb_cdc.h - Minimal USB CDC-ACM (virtual COM port) device stack for the
 * MC9S08JM60/JM32 USB module. Polled, no interrupts required.
 */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

void usb_init(void);          /* reset module and attach (pull-up on) */
void usb_poll(void);          /* call as often as possible from the main loop */

uint8_t cdc_is_configured(void);
uint8_t cdc_dtr(void);        /* host has the port open (DTR asserted) */

/* Data from the host is waiting to be read */
uint8_t cdc_rx_pending(void);

/* Returns number of received bytes copied to buf (0 if none). buf must hold 64 bytes. */
uint8_t cdc_read(uint8_t *buf);

/* Queue bytes for sending. Blocks (while servicing USB) if the TX buffer is
 * full; bytes are dropped if the port is not open or the host stops reading. */
void cdc_putc(char c);
void cdc_puts(const char *s);

#endif
