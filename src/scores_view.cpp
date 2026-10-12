#include "scores_view.h"
#include "score_store.h"
#include "logo_store.h"
#include "orb_empty_img.h"
#include "theme_style.h"
#include "theme_sd.h"
#include <PNGdec.h>
#include <atomic>
#include <new>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
static void *bg_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void bg_free(void *p) { heap_caps_free(p); }
#else
#include <stdlib.h>
static struct { template <class... A> void printf(const char *f, A... a) const { ::printf(f, a...); } } Serial;
static void *bg_alloc(size_t n) { return malloc(n); }
static void bg_free(void *p) { free(p); }
#endif

namespace {

using score_store::Team;
using score_store::Game;
using score_store::Side;
using score_store::Upcoming;
constexpr int UP = score_store::MAX_UPCOMING;

// Game page: two logos, the short name under each, then the score and the record.
constexpr int LOGO_Y = 146, AWAY_X = 128, HOME_X = 338, DISC_R = 46;
// Upcoming page: one row per game.
constexpr int ROW_Y = 112, ROW_H = 46, ROW_LOGO = 36, ROW_LOGO_X = 112;

lv_obj_t *s_scr, *s_art, *s_title, *s_head;
lv_obj_t *s_logo[2], *s_abbr[2], *s_short[2], *s_score[2], *s_rec[2], *s_at;
lv_obj_t *s_rowLogo[UP], *s_rowAbbr[UP], *s_rowWhen[UP], *s_rowOpp[UP];
lv_obj_t *s_status, *s_sub, *s_foot;
lv_color_t c_accent, c_text, c_dim, c_faint, c_live;
bool     s_active = false, s_built = false;
int      s_page = 0;                 // team * 2 + (0 = game, 1 = upcoming)
uint32_t s_lastPaint = 0;
long     s_paintedAt = -1;
unsigned s_paintedLogos = 0;
Team     s_t;
bool     s_have = false;
bool     s_disc[2] = {false, false};  // no logo for that side: draw a team-colour disc instead

void colors() {
    c_accent = lv_color_hex(theme_style::radar().sweepColor);
    c_text = lv_color_hex(0xECECEC);
    c_dim = lv_color_hex(0x8A8A8A);
    c_faint = lv_color_hex(0x3A3A3A);
    c_live = lv_color_hex(0xFF3B30);
}

// ---------------------------------------------------------------- background
lv_obj_t *s_frame, *s_bg;
uint8_t      *s_bgBuf = nullptr;       // RGB565 + alpha, 3 bytes a pixel
lv_img_dsc_t  s_bgDsc;
std::atomic<unsigned> s_bgGen{1};      // bumped when the picture is saved or removed
unsigned      s_bgLoaded = 0;

PNG     *s_png = nullptr;
uint8_t *s_dst = nullptr;
int      s_dstW = 0;
uint16_t s_line[466];

int bg_line(PNGDRAW *d) {
    uint8_t *o = s_dst + (size_t)d->y * s_dstW * 3;
    const uint8_t *src = d->pPixels;
    if (d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && d->iBpp == 8) {
        for (int x = 0; x < d->iWidth; ++x, src += 4, o += 3) {
            const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
            o[0] = v & 0xFF; o[1] = v >> 8; o[2] = src[3];
        }
    } else {
        // every other kind of PNG: its colours through PNGdec, fully opaque
        s_png->getLineAsRGB565(d, s_line, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
        for (int x = 0; x < d->iWidth; ++x, o += 3) { o[0] = s_line[x] & 0xFF; o[1] = s_line[x] >> 8; o[2] = 0xFF; }
    }
    return 1;
}

void bg_release() {
    if (s_bg) { lv_img_cache_invalidate_src(&s_bgDsc); lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN); }
    if (s_bgBuf) { bg_free(s_bgBuf); s_bgBuf = nullptr; }
}

// /scores.png from the card, decoded once each time the screen is built or the picture changes.
void bg_load() {
    s_bgLoaded = s_bgGen.load();
    bg_release();
    size_t len = 0;
    uint8_t *file = theme_sd::read_whole(scoresview::BG_PATH, len, scoresview::BG_MAX_BYTES);
    if (!file) return;                                  // none: the frame alone
    if (!s_png) { void *m = bg_alloc(sizeof(PNG)); if (m) s_png = new (m) PNG(); }
    bool ok = false;
    int w = 0, h = 0;
    if (s_png && s_png->openRAM(file, (int)len, bg_line) == PNG_SUCCESS) {
        w = s_png->getWidth(); h = s_png->getHeight();
        if (w > 0 && h > 0 && w <= 466 && h <= 466 && (s_bgBuf = (uint8_t *)bg_alloc((size_t)w * h * 3))) {
            s_dst = s_bgBuf; s_dstW = w;
            ok = s_png->decode(nullptr, 0) == PNG_SUCCESS;
        }
        s_png->close();
    }
    theme_sd::free(file);
    if (!ok) {
        Serial.printf("[scores] %s: not drawn (%dx%d; a PNG up to 466x466 is needed)\n", scoresview::BG_PATH, w, h);
        if (s_bgBuf) { bg_free(s_bgBuf); s_bgBuf = nullptr; }
        return;
    }
    memset(&s_bgDsc, 0, sizeof(s_bgDsc));
    s_bgDsc.header.w = w;
    s_bgDsc.header.h = h;
    s_bgDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_bgDsc.data_size = (uint32_t)w * h * 3;
    s_bgDsc.data = s_bgBuf;
    lv_img_set_src(s_bg, &s_bgDsc);
    lv_obj_align(s_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[scores] background %s, %dx%d\n", scoresview::BG_PATH, w, h);
}

// The default background: a ring just inside the edge of the dial, in the theme's accent.
void frame_cb(lv_event_t *e) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE; d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c_accent; d.border_width = 3; d.border_opa = LV_OPA_70;
    lv_area_t outer = {6, 6, 459, 459};
    lv_draw_rect(ctx, &d, &outer);
    d.border_color = c_accent; d.border_width = 1; d.border_opa = LV_OPA_30;
    lv_area_t inner = {16, 16, 449, 449};
    lv_draw_rect(ctx, &d, &inner);
}

void upper(char *s) { for (; *s; ++s) *s = (char)toupper((unsigned char)*s); }
void show(lv_obj_t *o, bool on) { if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

void draw_cb(lv_event_t *e) {
    if (!s_have || (s_page & 1) || s_t.game.state == score_store::NONE) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const Side *side[2] = {&s_t.game.away, &s_t.game.home};
    const int xs[2] = {AWAY_X, HOME_X};
    for (int i = 0; i < 2; ++i) {
        if (!s_disc[i]) continue;
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE; d.bg_color = lv_color_hex(side[i]->color); d.bg_opa = LV_OPA_COVER;
        d.border_color = c_faint; d.border_width = 3; d.border_opa = LV_OPA_80;
        lv_area_t a = {(lv_coord_t)(xs[i] - DISC_R), (lv_coord_t)(LOGO_Y - DISC_R), (lv_coord_t)(xs[i] + DISC_R), (lv_coord_t)(LOGO_Y + DISC_R)};
        lv_draw_rect(ctx, &d, &a);
    }
    if (s_t.game.state == score_store::LIVE) {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE; d.bg_color = c_live; d.bg_opa = LV_OPA_COVER;
        lv_area_t r = {233 - 5, 320 - 5, 233 + 5, 320 + 5};
        lv_draw_rect(ctx, &d, &r);
    }
}

lv_obj_t *mk_label(const lv_font_t *f, lv_color_t col, int x, int y, int w, lv_text_align_t ta = LV_TEXT_ALIGN_CENTER) {
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, ta, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    if (ta == LV_TEXT_ALIGN_CENTER) lv_obj_align(l, LV_ALIGN_TOP_MID, x, y);
    else lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *mk_img(int cx, int cy, int size) {
    lv_obj_t *im = lv_img_create(s_scr);
    lv_obj_set_size(im, logo_store::SIZE, logo_store::SIZE);
    lv_obj_set_pos(im, cx - logo_store::SIZE / 2, cy - logo_store::SIZE / 2);
    if (size != logo_store::SIZE) lv_img_set_zoom(im, (uint16_t)(256 * size / logo_store::SIZE));
    lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
    return im;
}

// "TONIGHT", "TOMORROW", "SAT OCT 17", and "7:00 PM", in the Orb's own time zone.
void when_text(long start, char *day, size_t dn, char *hm, size_t hn) {
    const time_t st = (time_t)start, now = time(nullptr);
    struct tm a, b;
    localtime_r(&st, &a);
    localtime_r(&now, &b);
    int h = a.tm_hour % 12; if (!h) h = 12;
    snprintf(hm, hn, "%d:%02d %s", h, a.tm_min, a.tm_hour < 12 ? "AM" : "PM");
    const int dayDiff = (a.tm_year - b.tm_year) * 366 + (a.tm_yday - b.tm_yday);
    if (now > 1700000000 && dayDiff == 0) snprintf(day, dn, "%s", a.tm_hour >= 17 ? "TONIGHT" : "TODAY");
    else if (now > 1700000000 && dayDiff == 1) snprintf(day, dn, "TOMORROW");
    else { strftime(day, dn, "%a %b %d", &a); upper(day); }
}

// A hidden logo points at an empty image, never at a decoded one: the logo cache may hand
// that buffer to another team on the next paint.
void hide_logo(lv_obj_t *img) { lv_img_set_src(img, orb_empty_img()); show(img, false); }

void set_logo(lv_obj_t *img, const char *league, const char *abbr, const char *id) {
    const lv_img_dsc_t *d = logo_store::get(league, abbr, id);
    if (d) { lv_img_set_src(img, d); show(img, true); }
    else hide_logo(img);
}

void hide_game() {
    for (int i = 0; i < 2; ++i) {
        hide_logo(s_logo[i]);
        lv_label_set_text(s_abbr[i], ""); lv_label_set_text(s_short[i], "");
        lv_label_set_text(s_score[i], ""); lv_label_set_text(s_rec[i], "");
        s_disc[i] = false;
    }
    lv_label_set_text(s_at, "");
}

void hide_rows() {
    for (int i = 0; i < UP; ++i) { hide_logo(s_rowLogo[i]); lv_label_set_text(s_rowAbbr[i], ""); lv_label_set_text(s_rowWhen[i], ""); lv_label_set_text(s_rowOpp[i], ""); }
}

void paint_game() {
    hide_rows();
    lv_label_set_text(s_head, "");
    const Game &g = s_t.game;
    if (g.state == score_store::NONE) {
        hide_game();
        char nm[32]; snprintf(nm, sizeof(nm), "%s", s_t.name); upper(nm);
        lv_label_set_text(s_head, nm);
        lv_obj_set_style_text_color(s_head, lv_color_hex(s_t.color), 0);
        lv_label_set_text(s_status, "NO GAME SCHEDULED");
        lv_obj_set_style_text_color(s_status, c_dim, 0);
        lv_label_set_text_fmt(s_sub, "%s  %s", s_t.abbr, s_t.record);
        return;
    }
    const Side *side[2] = {&g.away, &g.home};
    const bool started = g.state == score_store::LIVE || g.state == score_store::FINAL;
    for (int i = 0; i < 2; ++i) {
        set_logo(s_logo[i], s_t.league, side[i]->abbr, side[i]->id);
        s_disc[i] = lv_obj_has_flag(s_logo[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_abbr[i], s_disc[i] ? side[i]->abbr : "");
        char sn[24]; snprintf(sn, sizeof(sn), "%s", side[i]->name[0] ? side[i]->name : side[i]->abbr); upper(sn);
        lv_label_set_text(s_short[i], sn);
        // the owner's team in the accent colour
        const bool mine = (i == 1) == s_t.myHome;
        lv_obj_set_style_text_color(s_short[i], mine ? c_accent : c_text, 0);
        lv_label_set_text(s_rec[i], side[i]->record);
        if (started) lv_label_set_text_fmt(s_score[i], "%d", side[i]->score);
        else lv_label_set_text(s_score[i], "");
        const bool lost = g.state == score_store::FINAL && side[i]->score < side[1 - i]->score;
        lv_obj_set_style_text_color(s_score[i], lost ? c_dim : c_text, 0);
    }
    lv_label_set_text(s_at, "@");
    char st[40], sub[48];
    if (g.state == score_store::LIVE) {
        snprintf(st, sizeof(st), "%s", g.detail); upper(st);
        lv_label_set_text(s_status, st);
        lv_obj_set_style_text_color(s_status, c_live, 0);
        lv_label_set_text(s_sub, "LIVE");
    } else if (g.state == score_store::FINAL) {
        snprintf(st, sizeof(st), "%s", g.detail[0] ? g.detail : "Final"); upper(st);
        lv_label_set_text(s_status, st);
        lv_obj_set_style_text_color(s_status, c_text, 0);
        const Side &me = s_t.myHome ? g.home : g.away, &them = s_t.myHome ? g.away : g.home;
        // a final from an earlier day (no game today) says which day it was
        char day[24] = "", hm[16];
        if (g.start) {
            when_text(g.start, day, sizeof(day), hm, sizeof(hm));
            if (!strcmp(day, "TODAY") || !strcmp(day, "TONIGHT")) day[0] = 0;
        }
        // level at the end: a draw in football (soccer), a tie anywhere else it can happen
        static const char *const SOCCER[] = {"mls", "epl", "laliga", "bundes", "seriea", "ligue1", "ucl"};
        bool soccer = false;
        for (const char *l : SOCCER) if (!strcmp(s_t.league, l)) soccer = true;
        const char *res = me.score > them.score ? "WIN" : me.score < them.score ? "LOSS" : soccer ? "DRAW" : "TIE";
        if (day[0] && res[0]) snprintf(sub, sizeof(sub), "%s  -  %s %s", day, s_t.abbr, res);
        else if (res[0]) snprintf(sub, sizeof(sub), "%s %s", s_t.abbr, res);
        else snprintf(sub, sizeof(sub), "%s", day);
        lv_label_set_text(s_sub, sub);
    } else {
        char day[24], hm[16];
        when_text(g.start, day, sizeof(day), hm, sizeof(hm));
        snprintf(st, sizeof(st), "%s  %s", day, hm);
        lv_label_set_text(s_status, st);
        lv_obj_set_style_text_color(s_status, c_accent, 0);
        lv_label_set_text(s_sub, "");
    }
}

void paint_upcoming() {
    hide_game();
    char hd[48]; snprintf(hd, sizeof(hd), "%s  -  NEXT GAMES", s_t.name); upper(hd);
    lv_label_set_text(s_head, hd);
    lv_obj_set_style_text_color(s_head, lv_color_hex(s_t.color), 0);
    for (int i = 0; i < UP; ++i) {
        if (i >= s_t.nextN) { hide_logo(s_rowLogo[i]); lv_label_set_text(s_rowAbbr[i], ""); lv_label_set_text(s_rowWhen[i], ""); lv_label_set_text(s_rowOpp[i], ""); continue; }
        const Upcoming &u = s_t.next[i];
        set_logo(s_rowLogo[i], s_t.league, u.opp, u.oppId);
        lv_label_set_text(s_rowAbbr[i], lv_obj_has_flag(s_rowLogo[i], LV_OBJ_FLAG_HIDDEN) ? u.opp : "");
        char day[24], hm[16], line[48];
        when_text(u.start, day, sizeof(day), hm, sizeof(hm));
        snprintf(line, sizeof(line), "%s  %s", day, hm);
        lv_label_set_text(s_rowWhen[i], line);
        snprintf(line, sizeof(line), "%s %s", u.home ? "VS" : "AT", u.oppName[0] ? u.oppName : u.opp); upper(line);
        lv_label_set_text(s_rowOpp[i], line);
    }
    lv_label_set_text(s_status, s_t.nextN ? "" : "NO UPCOMING GAMES");
    lv_obj_set_style_text_color(s_status, c_dim, 0);
    lv_label_set_text(s_sub, s_t.nextN ? "" : "The schedule arrives within a few minutes");
}

void paint() {
    const int n = score_store::count();
    if (n > 0 && s_page >= n * 2) s_page = 0;
    s_have = score_store::team(s_page / 2, s_t);
    const uint32_t acc = lv_color_to32(c_accent) & 0xFFFFFF;
    lv_label_set_text_fmt(s_title, "SCORES #%06X ///#", (unsigned)acc);
    if (!s_have) {
        hide_game(); hide_rows();
        lv_label_set_text(s_head, "");
        lv_label_set_text(s_status, n ? "Waiting for scores" : "No teams set");
        lv_obj_set_style_text_color(s_status, c_text, 0);
        lv_label_set_text(s_sub, n ? "They arrive within a minute of WiFi connecting" : "Add /teams.txt to the SD card");
        lv_label_set_text(s_foot, "");
        lv_obj_invalidate(s_art);
        return;
    }
    if (s_page & 1) paint_upcoming(); else paint_game();

    char foot[64], upd[16] = "";
    const long u = score_store::updatedAt();
    if (u > 0) {
        const time_t ut = (time_t)u; struct tm t; localtime_r(&ut, &t);
        int h = t.tm_hour % 12; if (!h) h = 12;
        snprintf(upd, sizeof(upd), "%d:%02d", h, t.tm_min);
    }
    const char *next = (s_page & 1) ? (n > 1 ? "NEXT TEAM" : "SCORE") : "UPCOMING";
    if (n > 1) snprintf(foot, sizeof(foot), "%d/%d  -  TAP: %s", s_page / 2 + 1, n, next);
    else snprintf(foot, sizeof(foot), "TAP: %s", next);
    if (upd[0] && !(s_page & 1)) { strncat(foot, "  -  ", sizeof(foot) - strlen(foot) - 1); strncat(foot, upd, sizeof(foot) - strlen(foot) - 1); }
    lv_label_set_text(s_foot, foot);
    lv_obj_invalidate(s_art);
}

void build_contents() {
    s_built = true;
    // under everything else: the frame, then the owner's picture over it
    s_frame = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_frame);
    lv_obj_set_size(s_frame, 466, 466);
    lv_obj_set_pos(s_frame, 0, 0);
    lv_obj_clear_flag(s_frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_frame, frame_cb, LV_EVENT_DRAW_MAIN, nullptr);
    s_bg = lv_img_create(s_scr);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    bg_load();

    s_art = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_art);
    lv_obj_set_size(s_art, 466, 466);
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_clear_flag(s_art, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_art, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    s_title = mk_label(&lv_font_montserrat_22, c_text, 0, 34, 300);   // where the Weather page puts its title
    lv_label_set_recolor(s_title, true);
    s_head = mk_label(&lv_font_montserrat_14, c_accent, 0, 78, 320);
    lv_obj_set_style_text_letter_space(s_head, 2, 0);

    const int xs[2] = {AWAY_X, HOME_X};
    for (int i = 0; i < 2; ++i) {
        s_logo[i]  = mk_img(xs[i], LOGO_Y, logo_store::SIZE);
        s_abbr[i]  = mk_label(&lv_font_montserrat_28, lv_color_white(), xs[i] - 233, LOGO_Y - 16, 110);
        s_short[i] = mk_label(&lv_font_montserrat_14, c_text, xs[i] - 233, LOGO_Y + 54, 200);
        s_score[i] = mk_label(&lv_font_montserrat_48, c_text, xs[i] - 233, LOGO_Y + 76, 110);
        s_rec[i]   = mk_label(&lv_font_montserrat_12, c_dim, xs[i] - 233, LOGO_Y + 132, 110);
    }
    s_at = mk_label(&lv_font_montserrat_20, c_dim, 0, LOGO_Y - 12, 40);

    for (int i = 0; i < UP; ++i) {
        const int y = ROW_Y + i * ROW_H;
        s_rowLogo[i] = mk_img(ROW_LOGO_X, y + ROW_LOGO / 2, ROW_LOGO);
        s_rowAbbr[i] = mk_label(&lv_font_montserrat_12, c_text, ROW_LOGO_X - 233, y + 11, 48);   // when there is no logo
        s_rowWhen[i] = mk_label(&lv_font_montserrat_14, c_text, ROW_LOGO_X + 30, y, 240, LV_TEXT_ALIGN_LEFT);
        s_rowOpp[i]  = mk_label(&lv_font_montserrat_12, c_dim, ROW_LOGO_X + 30, y + 19, 240, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_letter_space(s_rowOpp[i], 1, 0);
    }

    s_status = mk_label(&lv_font_montserrat_20, c_text, 0, 336, 330);
    s_sub = mk_label(&lv_font_montserrat_14, c_dim, 0, 366, 300);
    lv_obj_set_style_text_letter_space(s_sub, 1, 0);
    s_foot = mk_label(&lv_font_montserrat_12, c_dim, 0, 400, 250);    // inside a framed plate's ring
    lv_obj_set_style_text_letter_space(s_foot, 1, 0);

}

}  // namespace

// Built when the screen is first opened and deleted when app_shell evicts it (addLazy):
// nothing of this screen sits in memory while another one is showing.
lv_obj_t *scoresview::build() {
    colors();
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    build_contents();
    return s_scr;
}

void scoresview::destroy() {
    bg_release();
    if (s_scr) lv_obj_del(s_scr);
    s_scr = nullptr;
    s_frame = s_bg = nullptr;
    s_built = false;
    s_active = false;
    logo_store::release_all();      // after the objects that pointed at them are gone
}

void scoresview::onEnter() {
    s_active = true;
    colors();
    score_store::setVisible(true);
    s_paintedAt = -1;
    s_lastPaint = 0;
    tick();
}

void scoresview::onExit() {
    s_active = false;
    score_store::setVisible(false);
}

void scoresview::onPress() {
    const int n = score_store::count();
    if (n > 0) s_page = (s_page + 1) % (n * 2);
    s_lastPaint = 0;
    tick();
}

void scoresview::tick() {
    if (!s_active || !s_built) return;
    if (s_bgLoaded != s_bgGen.load()) bg_load();         // uploaded or removed on the /teams page
    const uint32_t now = lv_tick_get();
    // repaint when the scores change, when a logo arrives, and every 20 s anyway so
    // "TONIGHT" turns over on time
    const long u = score_store::updatedAt();
    const unsigned lg = logo_store::generation();
    if (!s_lastPaint || u != s_paintedAt || lg != s_paintedLogos || now - s_lastPaint >= 20000) {
        s_lastPaint = now ? now : 1;
        s_paintedAt = u;
        s_paintedLogos = lg;
        paint();
    }
}

// ---------------------------------------------------------------- the /teams page's picture

bool scoresview::save_background(const uint8_t *png, size_t len, char *err, size_t errLen) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (len < 24 || memcmp(png, SIG, 8) || memcmp(png + 12, "IHDR", 4)) { snprintf(err, errLen, "That is not a PNG file."); return false; }
    if (len > BG_MAX_BYTES) { snprintf(err, errLen, "Too big: %u KB, the most is %u KB.", (unsigned)(len / 1024), (unsigned)(BG_MAX_BYTES / 1024)); return false; }
    const uint32_t w = (uint32_t)png[16] << 24 | (uint32_t)png[17] << 16 | (uint32_t)png[18] << 8 | png[19];
    const uint32_t h = (uint32_t)png[20] << 24 | (uint32_t)png[21] << 16 | (uint32_t)png[22] << 8 | png[23];
    if (!w || !h || w > 466 || h > 466) { snprintf(err, errLen, "The picture is %ux%u; the screen is 466x466, so it can be at most that.", (unsigned)w, (unsigned)h); return false; }
    if (!theme_sd::write_whole(BG_PATH, png, len)) { snprintf(err, errLen, "Could not write it to the SD card."); return false; }
    s_bgGen.fetch_add(1);
    return true;
}

bool scoresview::remove_background() {
    const bool ok = theme_sd::remove(BG_PATH);
    s_bgGen.fetch_add(1);
    return ok;
}

bool scoresview::has_background() { return theme_sd::exists(BG_PATH); }
