/*
 * usb_msc.h - Polled USB mass storage device for the bootloader.
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

void    usb_init(void);         /* reset module and attach (pull-up on) */
void    usb_poll(void);         /* EP0 + bulk token handling */
void    msc_task(void);         /* bulk-only transport state machine */
uint8_t usb_is_configured(void);
void    usb_detach(void);       /* pull-up off */

extern uint8_t msc_eject_request;   /* host ejected the drive */
extern uint8_t msc_activity;        /* set on every READ/WRITE */

#endif
