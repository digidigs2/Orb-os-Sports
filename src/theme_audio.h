#pragma once

#include <stddef.h>
#include <stdint.h>

// Sounds a theme brings with it.
//
// Two of them, and they are not the same kind of thing. `wind.pcm` is a tick, fired once per
// detent while somebody is turning the knob to wind the clock, so it has to be short and it
// has to be cheap. `chime.pcm` is the hour, fired once every sixty minutes, so it can be a
// few seconds of something proper.
//
// WHY THE THEME AND NOT THE DEVICE. Zion's vintage radio is the whole argument: the detail
// people talked about was the AM static between stations, which was a SOUND, and it belonged
// to that object rather than to a settings menu. A Steam Punk clock and an Aviator chronometer
// should not click the same way any more than they should share a typeface.
//
// FORMAT. Raw PCM, 16 kHz, 16-bit signed, stereo interleaved, no container — exactly what
// audio.cpp already streams for the built-in chimes, so playback is the code that was already
// there. Studio does the decoding and resampling in the browser, where there is a real audio
// stack, rather than asking an ESP32 to parse an MP3.
//
// Both are OPTIONAL. Absent, the Orb uses its built-in tick and its built-in chime, which is
// what every theme written before this does.

namespace theme_audio {

// Load whatever the active theme ships, freeing whatever the last one did. Call when a theme
// becomes active, on the same path that applies the rest of its settings.
void load();

// The clock's own tick, as a set of recordings played IN ORDER.
//
// A theme ships several takes of one real click and the Orb plays them in the order they were
// given, looping. Six takes is a six second loop.
//
// This was a shuffle bag first, on the argument that any fixed cycle is a loop and the ear
// finds a six second one. Zion recorded six consecutive seconds off a real clock, listened to
// them, and chose to keep that exact sequence: the takes are not interchangeable samples, they
// are one continuous passage of a real mechanism, and shuffling them threw away the ordering
// the mechanism itself produced. His call, and the recordings are the argument for it.
//
// Same format as every other sound here: raw PCM, 16 kHz, 16-bit signed, stereo interleaved.
// At 64,000 bytes a second a 300 ms click is 19.2 KB, so a set of six costs about 115 KB and
// is held in memory like the winding tick rather than streamed like a chime.
constexpr int TICK_SLOTS_MAX = 8;

// How many takes the worn theme actually shipped. Zero means it does not tick, which is every
// theme written before this one.
int tickCount();

// The next click to play, taking them in order and looping. Null when the theme ships none.
// Advances the cursor, so call it once per tick and not for a preview.
const uint8_t *nextTick(size_t &bytes);

// Simulator only: the sequence that WOULD be played for n clips, without needing any audio
// loaded. Checked rather than asserted in a comment.
void testBag(int n, uint8_t *out, int draws);

// Null when this theme ships no winding sound, which is the normal case.
//
// The chime is deliberately NOT here. It belongs to the device rather than to the worn theme
// (see chime_library) and it is streamed off the card as it rings rather than held, so a two
// minute chime costs this nothing.
const uint8_t *wind(size_t &bytes);

}  // namespace theme_audio
