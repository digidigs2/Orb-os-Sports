#pragma once
// Team logos for the Scores screen.
//
// A logo is a PNG on the SD card at /logos/<league>_<abbr>.png (e.g. /logos/nhl_car.png).
// Missing ones are downloaded once from ESPN's logo server (the dark-background versions,
// which suit a black screen) by the network task and saved there, so each is fetched once
// ever. Any PNG put there by hand is used instead: the Files page can upload to /logos.
//
// get() is UI thread only: it decodes from the card on first use and keeps a few decoded
// logos (fitted into SIZE x SIZE, transparency kept) in PSRAM.
#include <lvgl.h>

namespace logo_store {

constexpr int SIZE = 96;

// The logo, or nullptr if it is not on the card (yet). Asking for one that is missing queues
// its download.
// `id` is ESPN's team number: college logos are filed by it (and saved as
// /logos/<league>_<id>.png), pro ones by code. May be null for pro teams.
const lv_img_dsc_t *get(const char *league, const char *abbr, const char *id = nullptr);

// Ask for a logo to be fetched in the background without decoding it now.
void want(const char *league, const char *abbr, const char *id = nullptr);

// Network task: download at most one queued logo per call. true when one arrived.
bool fetchStep();

// Bumped whenever a download lands, so a screen knows to look again.
unsigned generation();

// Forget decoded logos (the Scores screen calls this when it closes, to give the PSRAM back).
void release_all();

// Exposed for tests: decode any 8-bit-or-less PNG into a SIZE x SIZE RGB565+alpha image.
// `out` must hold SIZE*SIZE*3 bytes.
bool decode_fit(const uint8_t *png, size_t len, uint8_t *out);

}  // namespace logo_store
