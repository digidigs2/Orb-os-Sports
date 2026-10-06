// Desktop encoder backend: implements the knob:: API from knob.h against injected
// SDL events. Mirrors knob.cpp's semantics so the sim and the device agree:
//   - a push registers on the DOWN edge (knob_sw_isr sets s_pendingPress there)
//   - a long-press fires once after LONG_PRESS_MS of continuous hold, in addition
//     to the down-edge press, exactly as the hardware does
#include "knob.h"
#include "sim_knob.h"

namespace {
    constexpr uint32_t LONG_PRESS_MS = 8000;   // matches knob.cpp

    int32_t  s_pendingDelta = 0;
    bool     s_pendingPress = false;
    bool     s_pendingLong  = false;

    bool     s_down       = false;
    uint32_t s_downAt      = 0;
    uint32_t s_heldMs      = 0;
    bool     s_longFired   = false;
}

// ---- injection from the SDL event loop -------------------------------------
// Mirrors knob.cpp exactly, including detecting the reversal in sequence rather than by
// comparing timestamps, so the Rock behaves identically in the simulator.
// The same rule as knob.cpp's on_detent, constant for constant: a reversal after a run of
// at most ROCK_MAX_RUN detents, arriving between ROCK_MIN_GAP_MS and ROCK_QUICK_MS after
// the last detent the other way, in either direction. Anything the sim decides differently
// from the device is a bug in one of them.
static constexpr int      ROCK_MAX_RUN    = 2;
static constexpr uint32_t ROCK_MIN_GAP_MS = 45;
static constexpr uint32_t ROCK_QUICK_MS   = 250;
static constexpr uint32_t ROCK_MAX_GAP_MS = 900;
static int      s_lastDir    = 0;
static uint32_t s_lastDirMs  = 0;
static int      s_runLen     = 0;
static int32_t  s_detent     = 0;
static uint32_t s_rockMs     = 0;
static uint32_t s_rockGapMs  = 0;
static int32_t  s_rockDetent = 0;
static uint32_t sim_now_ms();
void simknob::injectTurn(int detents) {
    if (detents == 0) return;
    s_pendingDelta += detents;
    const int dir = detents > 0 ? 1 : -1;
    const uint32_t now = sim_now_ms();
    for (int k = 0; k < (detents > 0 ? detents : -detents); ++k) {
        s_detent += dir;
        const bool fresh = (now - s_lastDirMs) > ROCK_MAX_GAP_MS;
        if (!fresh && s_lastDir != 0 && dir != s_lastDir && s_runLen <= ROCK_MAX_RUN) {
            const uint32_t gap = now - s_lastDirMs;
            if (gap >= ROCK_MIN_GAP_MS && gap <= ROCK_QUICK_MS) {
                s_rockGapMs  = gap;
                s_rockDetent = s_detent;
                s_rockMs     = now ? now : 1;
            }
        }
        s_runLen    = (!fresh && dir == s_lastDir) ? s_runLen + 1 : 1;
        s_lastDir   = dir;
        s_lastDirMs = now;
    }
}

void simknob::injectPress(bool down, uint32_t now_ms) {
    if (down && !s_down) {                 // falling edge: accept the press immediately
        s_down       = true;
        s_downAt     = now_ms;
        s_heldMs     = 0;
        s_longFired  = false;
        s_pendingPress = true;
    } else if (!down && s_down) {          // rising edge: just release; never "stuck"
        s_down   = false;
        s_heldMs = 0;
    }
}

void simknob::tick(uint32_t now_ms) {
    if (s_down) {
        s_heldMs = now_ms - s_downAt;
        if (!s_longFired && s_heldMs >= LONG_PRESS_MS) {
            s_longFired  = true;
            s_pendingLong = true;
        }
    }
}

// ---- knob:: API (consumed by app_shell via input_router) --------------------
void knob::begin() {}
void knob::poll()  {}

int32_t knob::takeDelta() { int32_t d = s_pendingDelta; s_pendingDelta = 0; return d; }
bool    knob::takePress() { bool p = s_pendingPress;   s_pendingPress = false; return p; }
bool    knob::takeLongPress() { bool p = s_pendingLong; s_pendingLong = false; return p; }

int32_t  knob::rawPosition()  { return 0; }
uint32_t knob::lastRockMs()    { return s_rockMs; }
uint32_t knob::lastRockGapMs() { return s_rockGapMs; }
int32_t  knob::lastRockDetent() { return s_rockDetent; }
int32_t  knob::detentCount()    { return s_detent; }
bool     knob::pendingPress() { return s_pendingPress; }
uint32_t knob::heldMs()       { return s_down ? s_heldMs : 0; }
uint32_t knob::longPressMs()  { return LONG_PRESS_MS; }

// A steady millisecond clock for the injection stamps above. std::chrono rather than SDL so
// this file keeps no dependency on the windowing layer.
#include <chrono>
static uint32_t sim_now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
