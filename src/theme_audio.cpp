#include "theme_audio.h"

#ifdef ARDUINO
#include "audio.h"
#else
// The simulator builds no audio module, so nothing can be playing and nothing needs releasing
// before it is freed. Compiled out rather than faked, the same way wind_notice does it.
#define audio_release_pcm(p) ((void)0)
#endif
#include "theme_sd.h"
#include "theme_select.h"
#include "theme_style.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <stdio.h>

namespace {

uint8_t *s_wind  = nullptr; size_t s_windLen  = 0;

// The tick set, and where we are in it.
uint8_t *s_tick[theme_audio::TICK_SLOTS_MAX] = { nullptr };
size_t   s_tickLen[theme_audio::TICK_SLOTS_MAX] = { 0 };
int      s_tickN = 0;
int      s_tickAt = 0;

// A ceiling with real headroom over what Studio can produce, which is the only number that
// matters here. It was 64 KB on the arithmetic that this format runs at 32 KB per second. It
// is 16 kHz SIXTEEN BIT STEREO: 64,000 bytes per second, so that ceiling was barely one
// second while Studio would emit half as much again. A click baked to 68,544 bytes, sailed
// past Studio, and was refused here with nothing on screen to say why.
//
// 256 KB now, which is four seconds against a Studio limit of two. Twice what the other end
// can produce rather than a hair above it, because these two numbers have already drifted
// apart once and the cost was an evening of hearing the wrong sound.
constexpr size_t WIND_MAX_BYTES  = 256 * 1024;
// A tick fires every second, so it is held in memory and there are several of it. 64 KB each
// is a full second of this format, which is already far longer than any click: the ceiling is
// here to stop a mistake costing half a megabyte, not to be reached.
constexpr size_t TICK_MAX_BYTES  = 64 * 1024;

uint8_t *load_one(const char *name, size_t maxBytes, size_t &outLen) {
    outLen = 0;
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return nullptr;
    // Only what the theme SAYS it ships. A push never deletes from the card, so a sound from
    // an older push of the same theme would otherwise keep playing after it was removed --
    // the same trap custom_sprite documents for artwork.
    if (!theme_style::hasAsset(name)) return nullptr;

    char path[64];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, maxBytes);
#ifdef ARDUINO
    if (buf) Serial.printf("[theme_audio] %s: %u bytes\n", path, (unsigned)len);
    else     Serial.printf("[theme_audio] %s: not loaded\n", path);
#endif
    // An odd length would put the stream half a sample out and every frame after it would be
    // noise, so the tail is dropped rather than trusted.
    outLen = len & ~(size_t)1;
    return buf;
}

}  // namespace

void theme_audio::load() {
    // Released before freeing, not just dropped: a theme can be applied while its own winding
    // tick is still sounding, and the playback task would then be reading memory this is
    // about to hand back.
    if (s_wind)  { audio_release_pcm(s_wind);  theme_sd::free(s_wind);  s_wind  = nullptr; s_windLen  = 0; }
    s_wind = load_one("wind.pcm", WIND_MAX_BYTES, s_windLen);

    // The tick bank. Numbered from one because that is how somebody recording six takes of a
    // clock names the files, and a gap just ends the bank rather than being an error: five
    // good takes are a perfectly good bank.
    for (int i = 0; i < theme_audio::TICK_SLOTS_MAX; ++i) {
        if (s_tick[i]) { audio_release_pcm(s_tick[i]); theme_sd::free(s_tick[i]); s_tick[i] = nullptr; s_tickLen[i] = 0; }
    }
    s_tickN = 0; s_tickAt = 0;
    for (int i = 0; i < theme_audio::TICK_SLOTS_MAX; ++i) {
        char nm[16]; snprintf(nm, sizeof(nm), "tick%d.pcm", i + 1);
        size_t len = 0;
        uint8_t *buf = load_one(nm, TICK_MAX_BYTES, len);
        if (!buf || !len) break;
        s_tick[s_tickN] = buf; s_tickLen[s_tickN] = len; s_tickN++;
    }
#ifdef ARDUINO
    if (s_tickN) Serial.printf("[theme_audio] tick set: %d take(s), played in order\n", s_tickN);
#endif
    // The chime is NOT loaded here any more. It belongs to the device rather than to the worn
    // theme (chime_library), and it is streamed off the card when it rings rather than held,
    // so a theme with a two minute chime costs this nothing at all.
}

const uint8_t *theme_audio::wind(size_t &bytes)  { bytes = s_windLen;  return s_wind; }

void theme_audio::testBag(int n, uint8_t *out, int draws) {
    const int keepN = s_tickN, keepAt = s_tickAt;
    s_tickN = n; s_tickAt = 0;
    for (int i = 0; i < draws; ++i) { out[i] = (uint8_t)s_tickAt; s_tickAt = (s_tickAt + 1) % s_tickN; }
    s_tickN = keepN; s_tickAt = keepAt;
}

int theme_audio::tickCount() { return s_tickN; }

const uint8_t *theme_audio::nextTick(size_t &bytes) {
    bytes = 0;
    if (s_tickN <= 0) return nullptr;
    const int k = s_tickAt;
    s_tickAt = (s_tickAt + 1) % s_tickN;
    bytes = s_tickLen[k];
    return s_tick[k];
}
