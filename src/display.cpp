// M0 bring-up: CO5300 466x466 AMOLED via Arduino_GFX (QSPI) + LVGL.
// Pins come from config.h (confirmed against the Waveshare board definition and a
// working Arduino_GFX port for this exact panel). The panel runs off the always-on
// DC1 rail, so it lights up without configuring the AXP2101 PMIC.
// The actual UI is built by ui_boot_create() (shared with the native SDL sim).
#include "display.h"
#include "config.h"
#include "radar_view.h"
#include "ui.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// --- Arduino_GFX panel -------------------------------------------------------
// Typed as Arduino_CO5300* (not Arduino_GFX*) so setBrightness() — declared on
// Arduino_OLED, not the GFX base — is reachable.
static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

// --- LVGL plumbing -----------------------------------------------------------
#define LVGL_BUF_LINES 40    // partial draw-buffer height (lines); kept in fast internal RAM
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_color_t        *s_buf1 = nullptr;
static lv_color_t        *s_buf2 = nullptr;

static volatile uint32_t s_frameCount = 0;   // rendered frames (last-flush), for FPS measurement
uint32_t display_frames() { return s_frameCount; }

// Render-time accounting. Flight Tracker measured 5 fps against a 33 fps target, while
// [perf] showed the aircraft drawing costs only 0.2 ms — so the time goes somewhere else
// in the frame. Splitting "total time inside LVGL" from "time spent pushing pixels to the
// panel" distinguishes compositing cost (CPU, layer blending) from QSPI transfer cost.
static volatile uint32_t s_lvglUs  = 0;   // cumulative us inside lv_timer_handler()
static volatile uint32_t s_flushUs = 0;   // cumulative us inside flush_cb (a subset)
static volatile uint32_t s_inputAtMs = 0;  // see display::markInput
static volatile uint32_t s_inputPx0  = 0;  // pixels flushed when that input arrived
static volatile uint32_t s_flushedPx = 0; // cumulative pixels pushed (dirty-area size)
uint32_t display_flushed_px() { return s_flushedPx; }
// What the PANEL is actually sent, which since the single-transaction flush is no longer the
// same number as above: s_flushedPx counts the strips LVGL rendered, this counts the one box
// per frame that reaches the glass. Keeping both is what makes "is the box bigger than the
// dirty area" answerable, which is exactly the question the full-width band got wrong.
static volatile uint32_t s_pushedPx = 0;
uint32_t display_pushed_px() { return s_pushedPx; }
static bool s_logQuiet = false;
void orb_log_set_quiet(bool quiet) { s_logQuiet = quiet; }
bool orb_log_quiet() { return s_logQuiet; }

// Defined at file scope, matching display_frames() above: display.h declares these
// globally, not inside namespace display.
uint32_t display_lvgl_us()  { return s_lvglUs; }
uint32_t display_flush_us() { return s_flushUs; }

static volatile uint16_t s_rot = 0;          // clockwise display rotation, 0..359 degrees
static float      s_rotCos = 1.0f;
static float      s_rotSin = 0.0f;
static int32_t    s_rotCosQ16 = 65536;       // fixed-point values used in the per-pixel hot path
static int32_t    s_rotSinQ16 = 0;
static lv_color_t *s_rotBuf = nullptr;       // PSRAM scratch for rotated output (see begin())
static lv_color_t *s_frameBuf = nullptr;     // full logical framebuffer for arbitrary-angle sampling

static void draw_block(int16_t x, int16_t y, lv_color_t *pixels, uint16_t w, uint16_t h) {
#if (LV_COLOR_16_SWAP != 0)
    s_gfx->draw16bitBeRGBBitmap(x, y, (uint16_t *)pixels, w, h);
#else
    s_gfx->draw16bitRGBBitmap(x, y, (uint16_t *)pixels, w, h);
#endif
}

// Render the physical bounding box affected by a logical dirty rectangle. Sampling
// from the full logical framebuffer avoids gaps/overdraw artifacts that forward-mapping
// individual source pixels would create at non-cardinal angles.
static void flush_arbitrary(const lv_area_t *area) {
    const float cx = (SCREEN_W - 1) * 0.5f;
    const float cy = (SCREEN_H - 1) * 0.5f;
    const float xs[4] = {(float)area->x1, (float)area->x2, (float)area->x2, (float)area->x1};
    const float ys[4] = {(float)area->y1, (float)area->y1, (float)area->y2, (float)area->y2};
    float minX = (float)SCREEN_W, minY = (float)SCREEN_H, maxX = -1.0f, maxY = -1.0f;
    for (int i = 0; i < 4; ++i) {
        const float rx = xs[i] - cx;
        const float ry = ys[i] - cy;
        const float dx = cx + s_rotCos * rx - s_rotSin * ry;
        const float dy = cy + s_rotSin * rx + s_rotCos * ry;
        if (dx < minX) minX = dx;
        if (dx > maxX) maxX = dx;
        if (dy < minY) minY = dy;
        if (dy > maxY) maxY = dy;
    }

    // Include nearest-neighbour edge pixels and preserve the panel's required 2-pixel
    // alignment (even start, odd end) in both axes.
    int x1 = (int)floorf(minX) - 1;
    int y1 = (int)floorf(minY) - 1;
    int x2 = (int)ceilf(maxX) + 1;
    int y2 = (int)ceilf(maxY) + 1;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    x1 &= ~1;
    y1 &= ~1;
    x2 |= 1;
    y2 |= 1;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if (y2 >= SCREEN_H) y2 = SCREEN_H - 1;
    if (x1 > x2 || y1 > y2) return;

    const int outW = x2 - x1 + 1;
    const lv_color_t black = lv_color_black();
    const int64_t centerX2Q16 = (int64_t)(SCREEN_W - 1) * 65536;
    const int64_t centerY2Q16 = (int64_t)(SCREEN_H - 1) * 65536;
    const int32_t stepX = s_rotCosQ16 * 2;
    const int32_t stepY = -s_rotSinQ16 * 2;

    // Batch scanlines while keeping every physical write 2-pixel aligned. Fewer QSPI
    // transactions matter here because an arbitrary-angle dirty box can be fairly large.
    for (int dy = y1; dy <= y2; ) {
        int rows = y2 - dy + 1;
        if (rows > LVGL_BUF_LINES) rows = LVGL_BUF_LINES;
        rows &= ~1;
        for (int row = 0; row < rows; ++row) {
            const int py = dy + row;
            const int relX2 = 2 * x1 - (SCREEN_W - 1);
            const int relY2 = 2 * py - (SCREEN_H - 1);
            int64_t srcX2Q16 = centerX2Q16 + (int64_t)s_rotCosQ16 * relX2
                                                + (int64_t)s_rotSinQ16 * relY2;
            int64_t srcY2Q16 = centerY2Q16 - (int64_t)s_rotSinQ16 * relX2
                                                + (int64_t)s_rotCosQ16 * relY2;
            lv_color_t *out = s_rotBuf + row * outW;
            for (int dx = 0; dx < outW; ++dx) {
                const int sx = (int)((srcX2Q16 + 65536) >> 17);
                const int sy = (int)((srcY2Q16 + 65536) >> 17);
                out[dx] = (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H)
                            ? s_frameBuf[sy * SCREEN_W + sx]
                            : black;
                srcX2Q16 += stepX;
                srcY2Q16 += stepY;
            }
        }
        draw_block((int16_t)x1, (int16_t)dy, s_rotBuf, (uint16_t)outW, (uint16_t)rows);
        dy += rows;
    }
}

// ---- tear-free single-transaction flush (rotation 0) -------------------------
// WHY THIS EXISTS. LVGL renders a dirty region in LVGL_BUF_LINES-tall strips and the old
// flush_cb pushed each strip to the panel the instant it was rendered. For the Flight
// Tracker's sweep that is ~13 separate QSPI writes spread across the WHOLE frame, and the
// frame is 54-88 ms measured. The panel is scanning its GRAM out the entire time, so for
// those tens of milliseconds the glass holds a mixture of the new frame's upper strips and
// the old frame's lower ones. A sweep hand crosses every strip, so it appears as a line
// snapped into offset segments -- the fault Greg could see and which no amount of making
// the frame CHEAPER was ever going to fix, because the tear window is the RENDER time, not
// the transfer time (flush was only 67 of 640 ms per second).
//
// So: render every strip into a full-screen staging buffer, remember the rows touched, and
// push them ONCE when LVGL says the frame is done. The tear window collapses from the whole
// render to a single transfer.
//
// The staging buffer is free. s_frameBuf is a 434 KB full-screen framebuffer that was
// already being allocated at boot and, at every cardinal rotation, never read or written --
// both of its uses are gated on `arbitrary`. It was dead weight until now.
//
// THE TIGHT BOUNDING BOX, streamed with a stride. The first version of this pushed
// full-width BANDS, because draw16bitRGBBitmap wants a tightly packed w*h block and the rows
// of a sub-rectangle are not contiguous in a 466-wide framebuffer. That worked and it halved
// the artifact, but it pushed about twice the pixels needed (466 wide against a ~240 px dirty
// box) and measured 17.8 ms of transfer against a panel refresh period of roughly 16.7 ms --
// so the write still RACED the scan-out instead of beating it, and the tear was reduced
// rather than removed. Confirmed on the glass, not inferred.
//
// One address window, then one writePixels per row at the framebuffer's stride, is the way
// out. The panel's GRAM pointer auto-increments inside a CASET/PASET window, so N strided row
// writes inside ONE window are a single logical transfer and reintroduce no tearing between
// rows -- this is the same thing Arduino_TFT::draw16bitRGBBitmap already does on its clipped
// path, just with the stride we need. ~120 KB instead of ~233 KB, which fits inside one
// refresh period rather than straddling it.
//
// 2-pixel alignment is still honoured: rounder_cb gives LVGL areas an even x1 and an odd x2,
// and a union of such boxes keeps both, which is what the CO5300 requires of a window.
//
// Rotation 0 only. 90/180/270 and the arbitrary-angle path are untouched below: they each
// transform the strip on its way past, which this staging path would have to redo, and
// nobody is looking at a tear on a rotated Orb today. Said plainly rather than left to be
// discovered: if those angles ever need it too, they need their own version of this.
// TWO THINGS TRIED HERE AND REVERTED, recorded so they are not tried again:
//
// 1. Staging the pixels ALREADY BYTE-SWAPPED and pushing with writeBytes (which hands the
//    caller's buffer straight to the DMA) instead of writePixels (which swaps each pixel into
//    its own internal buffer first). Measured on 2026-10-06: 4.8 MB/s raw against 4.9 MB/s
//    swapped. Identical. The per-pixel swap was never the bottleneck.
//
// 2. Packing the box into a contiguous buffer so it could go as ONE writeBytes instead of one
//    strided writePixels per row -- 58 DMA chunks rather than 250 calls. Measured WORSE:
//    16.7 ms against 13.3 ms for the same ~119 KB. The PSRAM-to-PSRAM pack copy costs more
//    than the call overhead it saves, which also says the per-transaction overhead is small
//    (~8 us), so there is nothing to win by enlarging ESP32QSPI_MAX_PIXELS_AT_ONCE either.
//
// What the numbers say instead: ~119 KB in 13.3 ms is 8.9 MB/s, i.e. about 84 Mbit/s, and the
// bus is configured with SPICOMMON_BUSFLAG_GPIO_PINS -- the GPIO matrix rather than the SPI
// IOMUX pins -- which holds SPI2 far below the 80 MHz LCD_QSPI_HZ asks for. The transfer is at
// the bus floor and no amount of restructuring the push will move it.
//
// So the remaining lever on tearing is the SIZE OF THE BOX, not the speed of the write. A
// smaller dirty area is a shorter transfer is a narrower tear window, which is the second
// reason (after frame cost) to want sweepLength and sweepTrailSteps lower.
static lv_coord_t s_boxX1 = 0, s_boxY1 = 0, s_boxX2 = -1, s_boxY2 = -1;   // x2 < x1 means none
// Is the staged single-transaction flush live at all?
//
// A switch, because it is the one change in this file that touches EVERY screen, and "the clock
// got slower after the flush rewrite" has to be answerable by measurement rather than by my
// arithmetic about it. stage=0 restores the original behaviour exactly: render a strip, push
// that strip, move on -- tearing and all.
//
// Note what it costs even when the timing looks small. At rotation 0 this path writes 434 KB
// into s_frameBuf and reads it back out again for every FULL-SCREEN frame, and PSRAM bandwidth
// is shared with everything else that frame is doing -- the clock composes into a PSRAM canvas
// and rotates its hands out of PSRAM. Contention does not show up in s_flushUs; it shows up as
// everything else getting slower, which is exactly the shape of the fault being chased.
static bool       s_stageFlush = true;
static bool       s_teOn   = false;              // panel is emitting TE and we wait for it
static uint32_t   s_teWaitUs = 0;                // cumulative us spent waiting, for /health

// Park the write in the panel's vertical blanking. TE rises as blanking starts, so sync to
// that edge: drain any pulse already in progress, then wait for the next rise.
//
// Bounded, and the bound is the point. A TE that never arrives (wrong pin, panel not
// emitting, 0R not fitted on some board revision) must cost one frame of latency and then
// be ignored -- never hang the render loop, which on this device also means never starve the
// knob or the watchdog.
static inline void wait_for_te(void) {
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    if (!s_teOn) return;
    const uint32_t t0 = micros();
    while (digitalRead(PIN_LCD_TE) == HIGH && (micros() - t0) < 25000) { }
    while (digitalRead(PIN_LCD_TE) == LOW  && (micros() - t0) < 25000) { }
    s_teWaitUs += micros() - t0;
#endif
}
uint32_t display_te_wait_us() { return s_teWaitUs; }
void display_set_stage(bool on) {
    if (s_stageFlush == on) return;
    s_stageFlush = on;
    s_boxX1 = 0; s_boxY1 = 0; s_boxX2 = -1; s_boxY2 = -1;   // drop any part-built box
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);
    Serial.printf("[display] staged flush -> %s\n", on ? "on (one transaction per frame)"
                                                        : "off (per-strip, the original path)");
}
bool display_stage(void) { return s_stageFlush; }
void display_set_te(bool on) {
    s_teOn = on;
    Serial.printf("[display] TE sync -> %s\n", on ? "on" : "off");
}
bool display_te(void) { return s_teOn; }

// LVGL -> panel, applying the chosen rotation while pushing.
//   0°   : straight through.
//   180° : reverse the flat block in place — no scratch buffer.
//   90°/270° : the block transposes (w<->h), so it can't be reversed in place; copy it
//              rotated into a PSRAM scratch buffer. That buffer MUST live in PSRAM — an
//              internal-RAM one starves the mbedTLS handshake and kills the ADS-B feed.
//   other: update the logical framebuffer and inverse-sample the rotated dirty bounds.
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const uint32_t t_flush0 = micros();
    const int w = (int)(area->x2 - area->x1 + 1);
    // How much screen area is actually being redrawn. Slowing the sweep's redraw rate by
    // half changed the frame cost by 0.2%, which means the cost is not "many small
    // invalidations" — so the question is whether each frame is repainting a small dirty
    // box or the entire 466x466 screen. Accumulate the pixels flushed per second and let
    // /health report it, instead of reasoning about LVGL's invalidation behaviour.
    s_flushedPx += (uint32_t)w * (uint32_t)(area->y2 - area->y1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    const uint16_t angle = s_rot;
    const bool arbitrary = (angle != 0 && angle != 90 && angle != 180 && angle != 270);
    // Mirror into the logical framebuffer ONLY at non-cardinal angles (it's what
    // flush_arbitrary samples from). At 0/90/180/270 this copy would be pure overhead
    // on every frame for every user; skipping it keeps those paths exactly as before.
    // Switching TO an arbitrary angle invalidates the whole screen (see setRotation),
    // so the framebuffer is fully repopulated before it is ever sampled.
    if (arbitrary && s_frameBuf) {
        for (int row = 0; row < h; ++row) {
            memcpy(s_frameBuf + (area->y1 + row) * SCREEN_W + area->x1,
                   px + row * w, (size_t)w * sizeof(lv_color_t));
        }
    }

    if (arbitrary && s_frameBuf && s_rotBuf) {
        flush_arbitrary(area);
        if (lv_disp_flush_is_last(drv)) s_frameCount++;
        s_flushUs += micros() - t_flush0;
        lv_disp_flush_ready(drv);
        return;
    }

    // Rotation 0: stage into the framebuffer and push one box per frame (see above).
    // With staging off, control falls through to the legacy per-strip path below, which already
    // handles angle 0 as a straight-through push.
    if (s_stageFlush && angle == 0 && s_frameBuf) {
        for (int row = 0; row < h; ++row) {
            memcpy(s_frameBuf + (size_t)(area->y1 + row) * SCREEN_W + area->x1,
                   px + (size_t)row * w, (size_t)w * sizeof(lv_color_t));
        }
        if (s_boxX2 < s_boxX1) {
            s_boxX1 = area->x1; s_boxY1 = area->y1; s_boxX2 = area->x2; s_boxY2 = area->y2;
        } else {
            if (area->x1 < s_boxX1) s_boxX1 = area->x1;
            if (area->y1 < s_boxY1) s_boxY1 = area->y1;
            if (area->x2 > s_boxX2) s_boxX2 = area->x2;
            if (area->y2 > s_boxY2) s_boxY2 = area->y2;
        }
        if (lv_disp_flush_is_last(drv)) {
            if (s_boxX2 >= s_boxX1 && s_boxY2 >= s_boxY1) {
                const uint16_t bw = (uint16_t)(s_boxX2 - s_boxX1 + 1);
                const uint16_t bh = (uint16_t)(s_boxY2 - s_boxY1 + 1);
                s_pushedPx += (uint32_t)bw * bh;
                wait_for_te();
                s_gfx->startWrite();
                s_gfx->writeAddrWindow((int16_t)s_boxX1, (int16_t)s_boxY1, bw, bh);
                for (uint16_t r = 0; r < bh; ++r) {
                    s_bus->writePixels((uint16_t *)(s_frameBuf + (size_t)(s_boxY1 + r) * SCREEN_W + s_boxX1),
                                       bw);
                }
                s_gfx->endWrite();
            }
            s_boxX1 = 0; s_boxY1 = 0; s_boxX2 = -1; s_boxY2 = -1;
            s_frameCount++;
        }
        s_flushUs += micros() - t_flush0;
        lv_disp_flush_ready(drv);
        return;
    }

    lv_color_t *out = px;
    int16_t  dx = area->x1, dy = area->y1;
    uint16_t dw = (uint16_t)w, dh = (uint16_t)h;

    switch (angle) {
        case 180:
            for (int i = 0, j = w * h - 1; i < j; ++i, --j) { lv_color_t t = px[i]; px[i] = px[j]; px[j] = t; }
            dx = (int16_t)(SCREEN_W - 1 - area->x2);
            dy = (int16_t)(SCREEN_H - 1 - area->y2);
            break;
        case 90:
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[i * h + (h - 1 - j)] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = (int16_t)(SCREEN_H - 1 - area->y2); dy = area->x1;
            }
            break;
        case 270:
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[(w - 1 - i) * h + j] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = area->y1; dy = (int16_t)(SCREEN_W - 1 - area->x2);
            }
            break;
        default: break;  // 0°
    }
    draw_block(dx, dy, out, dw, dh);
    if (lv_disp_flush_is_last(drv)) {
        s_frameCount++;
        // The end of the frame IS the moment the pixels are on the panel: draw_block writes
        // over QSPI and blocks, so nothing is queued behind this.
        if (s_inputAtMs && !s_logQuiet) {
            // The AREA repainted, as well as the time. 146 ms for a text swap only makes
            // sense if the whole 466x466 is being pushed, and this says whether it is:
            // 100% means one full screen, 200% means two. If it is a full screen then the
            // cost is the menu's text canvas being full-screen with alpha, so any change
            // to a word invalidates the entire display, and the fix is to make the canvas
            // the size of the text rather than the size of the panel.
            const uint32_t px = s_flushedPx - s_inputPx0;
            Serial.printf("[display] input -> glass: %lums, repainted %lu%% of the screen\n",
                          (unsigned long)(millis() - s_inputAtMs),
                          (unsigned long)(px * 100UL / ((uint32_t)SCREEN_W * SCREEN_H)));
            s_inputAtMs = 0;
        }
        else if (s_inputAtMs) s_inputAtMs = 0;   // quiet, but not stale next time round
    }
    s_flushUs += micros() - t_flush0;
    lv_disp_flush_ready(drv);
}

// CO5300 (QSPI) requires 2-pixel-aligned flush windows: even start, odd end.
// Without this, partial-area updates (e.g. the radar sweep) tear / ghost / flicker.
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    (void)drv;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// Touch is deliberately not wired up. The Orb is knob-only: see the input model in
// docs/ARCHITECTURE.md and section 2 of orb-user-requirements.md. The CST9217 driver
// (src/touch_cst9217.cpp) is kept in the repo but excluded from both build envs via
// build_src_filter in platformio.ini, so it costs zero bytes while staying available
// if a touch feature is ever wanted. The pointer indev registration and the
// physical-to-logical rotation mapping that used to live here were removed with it.


// Same running ledger as main.cpp's boot checkpoints, scoped inside this function.
// display::begin() was measured taking 4.6 MB of the 8 MB PSRAM budget — 57% of the
// chip, before a single app loads — and only ~900 KB of that was accounted for. These
// marks split it by name so the rest stops being a mystery.
static void dmark(const char *what) {
    static uint32_t prev = 0;
    const uint32_t now = (uint32_t)ESP.getFreePsram();
    Serial.printf("[psram/display] %-24s free %6u KB", what, (unsigned)(now / 1024));
    if (prev && prev >= now) Serial.printf("   (-%u KB)", (unsigned)((prev - now) / 1024));
    else if (prev)           Serial.printf("   (+%u KB)", (unsigned)((now - prev) / 1024));
    Serial.println();
    prev = now;
}

namespace display {

bool begin() {
    Serial.println("[display] init CO5300 QSPI...");
    s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK,
                                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0 /*rotation*/,
                               SCREEN_W, SCREEN_H,
                               LCD_COL_OFFSET, LCD_ROW_OFFSET, 0, 0);
    if (!s_gfx->begin(LCD_QSPI_HZ)) {
        Serial.println("[display] gfx->begin() FAILED");
        return false;
    }
    dmark("after gfx begin");
    // Turn the panel's tearing-effect output ON. Arduino_CO5300's init sequence has the
    // TEARON line commented out, so the panel ships with it disabled and the GPIO the
    // schematic routes it to reads nothing. 0x35 with 0x00 = pulse on vertical blanking
    // only (0x01 would add horizontal, which is noise for a whole-band write).
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    pinMode(PIN_LCD_TE, INPUT);
    s_bus->beginWrite();
    s_bus->writeC8D8(0x35, 0x00);   // CO5300_WC_TEARON
    s_bus->endWrite();
    // Proof the signal is actually moving, rather than an assumption that it is. A pin that
    // never changes means the wait would burn its whole timeout every frame, so it is better
    // to find that out here, once, than to pay for it silently forever.
    {
        const int first = digitalRead(PIN_LCD_TE);
        bool moved = false;
        const uint32_t t0 = micros();
        while ((micros() - t0) < 50000) if (digitalRead(PIN_LCD_TE) != first) { moved = true; break; }
        s_teOn = moved;
        Serial.printf("[display] LCD_TE on GPIO%d: %s -> TE sync %s\n", PIN_LCD_TE,
                      moved ? "toggling" : "STUCK (no pulse in 50 ms)",
                      moved ? "on" : "off");
    }
#endif
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setBrightness(BRIGHTNESS_DEFAULT);
    Serial.println("[display] panel up; init LVGL...");

    dmark("before lv_init");
    lv_init();
    dmark("after lv_init");

    // Draw scratch in INTERNAL DMA RAM: rendering anti-aliased graphics into PSRAM is
    // slow (that, not QSPI bandwidth, was the bottleneck). Keep the active buffer in fast
    // internal SRAM; single partial buffer to stay within the internal-RAM budget.
    const size_t buf_px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = nullptr;
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    dmark("after draw buffer");
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_px);

    // Rotation buffers live in PSRAM so the internal contiguous block needed by TLS remains
    // available. The full logical framebuffer makes inverse sampling at arbitrary angles
    // possible without holes; the smaller scratch holds transposed blocks or output batches.
    s_rotBuf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_frameBuf = (lv_color_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H,
                                                sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_rotBuf || !s_frameBuf) {
        Serial.println("[display] WARNING: rotation buffer allocation failed; arbitrary angles unavailable");
    }

    dmark("after rot+frame buffers");
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = SCREEN_W;
    s_disp_drv.ver_res  = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;     // CO5300 needs 2-px-aligned windows
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    // No touch indev is registered. Knob only. See the note above touch_read_cb's
    // former home, further up this file.

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    dmark("before ui_create");
    ui_create();                   // Flight Tracker scope + detail card + weather view
    dmark("after ui_create");
    Serial.println("[display] LVGL ready");
    return true;
}

void loop() {
    const uint32_t t0 = micros();
    lv_timer_handler();
    s_lvglUs += micros() - t0;
}

void markInput(uint32_t ms) { s_inputAtMs = ms ? ms : 1; s_inputPx0 = s_flushedPx; }

void setBrightness(uint8_t v) { if (s_gfx) s_gfx->setBrightness(v); }

void setRotation(uint16_t degrees) {
    uint16_t normalized = (uint16_t)(degrees % 360);
    if ((normalized == 90 || normalized == 270) && !s_rotBuf) {
        Serial.println("[display] quarter-turn rotation unavailable without the PSRAM scratch buffer");
        normalized = 0;
    }
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270
        && (!s_frameBuf || !s_rotBuf)) {
        Serial.println("[display] arbitrary rotation unavailable without both PSRAM buffers");
        normalized = 0;
    }
    if (normalized == s_rot) return;
    const float radians = normalized * ((float)M_PI / 180.0f);
    s_rotCos = cosf(radians);
    s_rotSin = sinf(radians);
    s_rotCosQ16 = (int32_t)lroundf(s_rotCos * 65536.0f);
    s_rotSinQ16 = (int32_t)lroundf(s_rotSin * 65536.0f);
    s_rot = normalized;
    if (s_gfx) s_gfx->fillScreen(RGB565_BLACK);  // clear pixels no longer covered after an angle change
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);   // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(NULL); }
void noteActivity() { lv_disp_trig_activity(NULL); }

} // namespace display
