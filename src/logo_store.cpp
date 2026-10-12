#include "logo_store.h"
#include "config.h"
#include "net_fetch.h"
#include "theme_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#else
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <ctype.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace logo_store {
namespace {

#ifdef ARDUINO
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard() { portENTER_CRITICAL(&s_mux); } ~Guard() { portEXIT_CRITICAL(&s_mux); } };
uint32_t now_ms() { return millis(); }
#else
std::mutex s_mtx;
struct Guard { Guard() { s_mtx.lock(); } ~Guard() { s_mtx.unlock(); } };
uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

void *ext_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

constexpr int    MAX_SRC = 1024;                  // a bigger PNG than this is refused
constexpr size_t MAX_FILE = 512 * 1024;

// ---------------------------------------------------------------- decode
// Any colour type at 8 bits or fewer per sample (16-bit samples keep their high byte), into
// RGBA8888, then box-filtered down (or sampled up) to SIZE x SIZE, centred, aspect kept.
// screen_plate's decoder only reads RGBA, which is what Orb Studio writes; logos from
// anywhere else come as palettes, greyscale and plain RGB too.
PNG     *s_png = nullptr;
uint8_t *s_rgba = nullptr;     // the whole source image while decoding
int      s_srcW = 0;

int line_cb(PNGDRAW *d) {
    const uint8_t *p = d->pPixels;
    uint8_t *o = s_rgba + (size_t)d->y * s_srcW * 4;
    const int bpp = d->iBpp;
    const int step = bpp == 16 ? 2 : 1;             // 16-bit samples: take the high byte
    for (int x = 0; x < d->iWidth; ++x, o += 4) {
        switch (d->iPixelType) {
        case PNG_PIXEL_TRUECOLOR_ALPHA: { const uint8_t *q = p + x * 4 * step; o[0] = q[0]; o[1] = q[step]; o[2] = q[2 * step]; o[3] = q[3 * step]; break; }
        case PNG_PIXEL_TRUECOLOR:       { const uint8_t *q = p + x * 3 * step; o[0] = q[0]; o[1] = q[step]; o[2] = q[2 * step]; o[3] = 255; break; }
        case PNG_PIXEL_GRAY_ALPHA:      { const uint8_t *q = p + x * 2 * step; o[0] = o[1] = o[2] = q[0]; o[3] = q[step]; break; }
        case PNG_PIXEL_GRAYSCALE: {
            uint8_t v;
            if (bpp >= 8) v = p[x * step];
            else {
                const int per = 8 / bpp, shift = (per - 1 - x % per) * bpp;
                v = (uint8_t)(((p[x / per] >> shift) & ((1 << bpp) - 1)) * 255 / ((1 << bpp) - 1));
            }
            o[0] = o[1] = o[2] = v; o[3] = 255;
            break;
        }
        case PNG_PIXEL_INDEXED: {
            int idx;
            if (bpp == 8) idx = p[x];
            else { const int per = 8 / bpp, shift = (per - 1 - x % per) * bpp; idx = (p[x / per] >> shift) & ((1 << bpp) - 1); }
            const uint8_t *c = d->pPalette + idx * 3;
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
            o[3] = d->iHasAlpha ? d->pPalette[768 + idx] : 255;
            break;
        }
        default: o[0] = o[1] = o[2] = o[3] = 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------- cache
struct Slot {
    char         key[24] = "";      // "nhl_car"
    uint8_t     *buf = nullptr;     // SIZE*SIZE*3, or null when the card had none
    lv_img_dsc_t dsc;
    uint32_t     used = 0;
    uint32_t     gen = 0;           // download generation it last looked at
};
constexpr int SLOTS = 10;
Slot *s_slot = nullptr;             // SLOTS, in PSRAM: internal RAM is what WiFi runs on
volatile uint32_t s_gen = 1;        // bumped when a download lands, so misses look again

// download queue
struct Want { char league[8]; char abbr[8]; char id[8]; uint32_t failedAt; bool done; };
constexpr int WANTS = 24;
Want *s_want = nullptr;             // WANTS, in PSRAM
int  s_wantN = 0;

bool ensure_tables() {
    if (!s_slot) {
        void *m = ext_alloc(sizeof(Slot) * SLOTS);
        if (!m) return false;
        s_slot = (Slot *)m;
        for (int i = 0; i < SLOTS; ++i) new (&s_slot[i]) Slot();
    }
    if (!s_want) {
        s_want = (Want *)ext_alloc(sizeof(Want) * WANTS);
        if (!s_want) return false;
    }
    return true;
}

bool college(const char *league) { return !strncmp(league, "ncaa", 4); }

// "nhl_car"; college by number, "ncaaf_153": abbreviations repeat across schools
void make_key(char *out, size_t n, const char *league, const char *abbr, const char *id) {
    snprintf(out, n, "%s_%s", league, (college(league) && id && id[0]) ? id : abbr);
    for (char *c = out; *c; ++c) *c = (char)tolower((unsigned char)*c);
}

}  // namespace

bool decode_fit(const uint8_t *png, size_t len, uint8_t *out) {
    if (!s_png) {
        void *mem = ext_alloc(sizeof(PNG));
        if (!mem) return false;
        s_png = new (mem) PNG();
    }
    if (s_png->openRAM((uint8_t *)png, (int)len, line_cb) != PNG_SUCCESS) return false;
    const int w = s_png->getWidth(), h = s_png->getHeight();
    if (w <= 0 || h <= 0 || w > MAX_SRC || h > MAX_SRC) { s_png->close(); return false; }
    s_rgba = (uint8_t *)ext_alloc((size_t)w * h * 4);
    if (!s_rgba) { s_png->close(); return false; }
    memset(s_rgba, 0, (size_t)w * h * 4);
    s_srcW = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { heap_caps_free(s_rgba); s_rgba = nullptr; return false; }

    // fit: the longer side becomes SIZE
    const float sc = (float)(w > h ? w : h) / SIZE;
    const int dw = (int)(w / sc + 0.5f), dh = (int)(h / sc + 0.5f);
    const int ox = (SIZE - dw) / 2, oy = (SIZE - dh) / 2;
    memset(out, 0, SIZE * SIZE * 3);
    for (int y = 0; y < dh; ++y) {
        int y0 = (int)(y * sc), y1 = (int)((y + 1) * sc);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < dw; ++x) {
            int x0 = (int)(x * sc), x1 = (int)((x + 1) * sc);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            // premultiplied average, so transparent pixels' colour does not bleed into edges
            uint32_t R = 0, G = 0, B = 0, A = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t *q = s_rgba + ((size_t)sy * w + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, q += 4) { R += q[0] * q[3]; G += q[1] * q[3]; B += q[2] * q[3]; A += q[3]; ++n; }
            }
            uint8_t *o = out + ((size_t)(y + oy) * SIZE + (x + ox)) * 3;
            if (!A) continue;
            const uint8_t r8 = (uint8_t)(R / A), g8 = (uint8_t)(G / A), b8 = (uint8_t)(B / A);
            const uint16_t v = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
            o[0] = v & 0xFF; o[1] = v >> 8; o[2] = (uint8_t)(A / n);
        }
    }
    heap_caps_free(s_rgba);
    s_rgba = nullptr;
    return true;
}

void want(const char *league, const char *abbr, const char *id) {
    if (!league[0] || !abbr[0]) return;
    if (college(league) && !(id && id[0])) return;      // no number, no college logo
    Guard g;
    if (!s_want) return;      // tables come from get() on the UI thread
    for (int i = 0; i < s_wantN; ++i)
        if (!strcasecmp(s_want[i].league, league) && !strcasecmp(s_want[i].abbr, abbr)) return;
    if (s_wantN >= WANTS) return;
    Want &w = s_want[s_wantN++];
    snprintf(w.league, sizeof(w.league), "%s", league);
    snprintf(w.abbr, sizeof(w.abbr), "%s", abbr);
    snprintf(w.id, sizeof(w.id), "%s", id ? id : "");
    w.failedAt = 0; w.done = false;
}

const lv_img_dsc_t *get(const char *league, const char *abbr, const char *id) {
    if (!abbr || !abbr[0]) return nullptr;
    // Not under the lock: a critical section on the ESP32 must never call malloc. The tables
    // are created once, here on the UI thread, before anything else can read them.
    if (!ensure_tables()) return nullptr;
    char key[24];
    make_key(key, sizeof(key), league, abbr, id);
    const uint32_t gen = s_gen;
    Slot *hit = nullptr, *victim = nullptr;
    for (int i = 0; i < SLOTS && !hit; ++i) if (!strcmp(s_slot[i].key, key)) hit = &s_slot[i];
    if (!hit) {
        // an empty slot, else the one used longest ago
        for (int i = 0; i < SLOTS; ++i) {
            Slot &c = s_slot[i];
            if (!c.key[0]) { victim = &c; break; }
            if (!victim || c.used < victim->used) victim = &c;
        }
    }
    if (hit && (hit->buf || hit->gen == gen)) { hit->used = now_ms() | 1; return hit->buf ? &hit->dsc : nullptr; }
    Slot &s = hit ? *hit : *victim;
    if (!hit) {
        if (s.buf) heap_caps_free(s.buf);
        s = Slot();
        snprintf(s.key, sizeof(s.key), "%s", key);
    }
    s.gen = gen;
    s.used = now_ms() | 1;
    char path[48];
    snprintf(path, sizeof(path), "/logos/%s.png", key);
    size_t len = 0;
    uint8_t *png = theme_sd::read_whole(path, len, MAX_FILE);
    if (!png) { want(league, abbr, id); return nullptr; }
    uint8_t *buf = (uint8_t *)ext_alloc(SIZE * SIZE * 3);
    const bool ok = buf && decode_fit(png, len, buf);
    theme_sd::free(png);
    if (!ok) {
        if (buf) heap_caps_free(buf);
        Serial.printf("[logos] %s: not a PNG this can read\n", path);
        return nullptr;
    }
    s.buf = buf;
    s.dsc.header.always_zero = 0;
    s.dsc.header.w = SIZE;
    s.dsc.header.h = SIZE;
    s.dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s.dsc.data_size = SIZE * SIZE * 3;
    s.dsc.data = buf;
    return &s.dsc;
}

bool fetchStep() {
    Want w;
    int at = -1;
    {
        Guard g;
        if (!s_want) return false;
        const uint32_t now = now_ms();
        for (int i = 0; i < s_wantN; ++i) {
            if (s_want[i].done) continue;
            if (s_want[i].failedAt && now - s_want[i].failedAt < 60u * 60u * 1000u) continue;  // an hour between tries
            w = s_want[i]; at = i; break;
        }
    }
    if (at < 0) return false;
    char key[24], path[48], url[200];
    make_key(key, sizeof(key), w.league, w.abbr, w.id);
    snprintf(path, sizeof(path), "/logos/%s.png", key);
    // Already on the card (put there by hand, or fetched before a restart): nothing to do.
    size_t len = 0;
    if (uint8_t *b = theme_sd::read_whole(path, len, MAX_FILE)) {
        theme_sd::free(b);
        Guard g; s_want[at].done = true;
        ++s_gen;
        return true;
    }
    char lg[8], ab[8];
    snprintf(lg, sizeof(lg), "%s", w.league); snprintf(ab, sizeof(ab), "%s", w.abbr);
    for (char *c = lg; *c; ++c) *c = (char)tolower((unsigned char)*c);
    for (char *c = ab; *c; ++c) *c = (char)tolower((unsigned char)*c);
    // ESPN's image resizer, so the Orb receives ~100 px rather than 500. The dark variant is
    // drawn for dark backgrounds (no black outlines lost against the screen); not every team
    // has one, so the normal logo is the fallback.
    static const char *const VARIANTS[] = {"500-dark", "500"};
    uint8_t *data = nullptr; size_t got = 0;
    for (const char *v : VARIANTS) {
        // college logos live under "ncaa", one folder for every sport, filed by team number
        const bool col = college(lg);
        snprintf(url, sizeof(url), "http://a.espncdn.com/combiner/i?img=/i/teamlogos/%s/%s/%s.png&w=%d&h=%d&transparent=true",
                 col ? "ncaa" : lg, v, col ? w.id : ab, SIZE, SIZE);
        if (net_fetch_psram(url, ESPN_USER_AGENT, &data, &got, MAX_FILE, 6000, 10000) && got > 8 &&
            !memcmp(data, "\x89PNG", 4)) break;
        if (data) { heap_caps_free(data); data = nullptr; }
    }
    bool ok = false;
    if (data) {
        theme_sd::make_dir("/logos");
        ok = theme_sd::write_whole(path, data, got);
        heap_caps_free(data);
    }
    Serial.printf("[logos] %s: %s\n", key, ok ? "downloaded" : "could not download (drop a PNG into /logos to use your own)");
    Guard g;
    if (ok) { s_want[at].done = true; ++s_gen; }
    else s_want[at].failedAt = now_ms() | 1;
    return ok;
}

unsigned generation() { return s_gen; }

void release_all() {
    if (!s_slot) return;
    for (int i = 0; i < SLOTS; ++i) {
        if (s_slot[i].buf) heap_caps_free(s_slot[i].buf);
        s_slot[i] = Slot();
    }
}

}  // namespace logo_store
