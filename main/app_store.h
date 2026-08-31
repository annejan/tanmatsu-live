// Saving and loading sets.
//
// Sets are plain text, the same thing you see in the editor, kept in the
// internal FAT partition so no SD card is needed. They can also be pushed and
// pulled with badgelink's fs commands, which makes a set editable on a host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define APP_STORE_SLOTS 8

// Mounts storage. A microSD card is preferred when one is present, because
// sets on a card travel between badges; the internal FAT partition is the
// fallback and is always there. USB mass storage is a third option that needs
// USB host mode, which conflicts with badge link, so it is not wired up yet.
esp_err_t app_store_init(void);
bool      app_store_available(void);
// "sd" or "int", or "none" when nothing mounted.
char const* app_store_where(void);

bool app_store_save(int slot, char const* text);
bool app_store_load(int slot, char* out, size_t len);
bool app_store_exists(int slot);
// First line of a saved set, so a browser can show what a slot holds rather
// than just its number. Returns false when the slot is empty.
bool app_store_label(int slot, char* out, size_t len);
