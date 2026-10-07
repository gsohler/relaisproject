/*
 * names.h - User-defined relay names, stored in NVS.
 *
 * A name belongs to (board serial number, relay number 1..6). Names are
 * unique (case-insensitive for ASCII letters), 1..32 bytes UTF-8, without
 * control characters and without leading/trailing spaces.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NAME_MAX_LEN   32
#define NAMES_MAX      64

typedef struct {
    char serial[32];
    int  relay;
    char name[NAME_MAX_LEN + 1];
} name_entry_t;

void names_init(void);

/* Copies the name of (serial, relay) to out; returns false if none. */
bool names_get(const char *serial, int relay, char *out, size_t len);

/* Sets or (with an empty name) deletes a name and saves to NVS.
 * ESP_ERR_INVALID_ARG: bad name, ESP_ERR_INVALID_STATE: name already used,
 * ESP_ERR_NO_MEM: table full. */
esp_err_t names_set(const char *serial, int relay, const char *name);

/* Looks up a name; returns false if unknown. */
bool names_find(const char *name, char *serial, size_t slen, int *relay);

/* Copies all entries, returns their number. */
int names_list(name_entry_t *out, int max);
