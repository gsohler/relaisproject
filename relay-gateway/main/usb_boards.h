/*
 * usb_boards.h - USB host side for HCS08 relay boards (USB CDC-ACM).
 *
 * The ESP32-S3 USB host has only 8 channels, and every enumerated device
 * keeps one permanently (its control pipe). Therefore boards are never kept
 * open: each command opens the board, claims its data interface (2 more
 * channels), exchanges one command/reply and releases it again.
 * Practical limit: about 4 boards behind one hub.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define BOARDS_MAX 8
#define RELAYS_PER_BOARD 6

typedef struct {
    uint8_t  addr;              /* USB address, used as id while connected */
    char     serial[32];        /* USB iSerialNumber */
    char     product[40];
    char     location[24];      /* "root" or "hub<addr>.<port>" */
    int      state;             /* relay bit mask, -1 = unknown */
    uint32_t age_ms;            /* time since last successful poll */
    uint32_t errors;
    char     last_error[48];
} board_info_t;

void usb_boards_start(void);

/* Copies the current board list, returns the number of boards. */
int usb_boards_snapshot(board_info_t *out, int max);

/* Sends one command line (without newline) to the board with USB address
 * addr and returns its first reply line. Blocks until done. */
esp_err_t usb_boards_command(uint8_t addr, const char *cmd,
                             char *reply, size_t reply_len, uint32_t timeout_ms);

/* Number of USB devices the host currently sees (boards + hubs + others). */
int usb_boards_device_count(void);
