#pragma once
#include <lvgl.h>

// Scores: the owner's teams (score_store.h, /teams.txt), one at a time. Each side of the game
// is the team's logo with its short name under it, then the score; below, the period and
// clock while live, the final, or the day and time of the next game. A second page per team
// lists its next games.
//
// Registered with app_shell::addLazy(): built when opened, deleted when evicted.
// Press or tap: a team's score -> its upcoming games -> the next team.
//
// Background: a frame drawn in the theme's accent colour, unless the SD card holds
// /scores.png. That picture is drawn over the frame: an opaque one replaces it, one with
// transparent parts lets it show through. Up to 466x466, centred. The /teams page uploads
// and removes it (save_background / remove_background, shared with the simulator).
namespace scoresview {
    constexpr const char *BG_PATH = "/scores.png";
    constexpr size_t BG_MAX_BYTES = 1536u * 1024u;
    // Any thread. Checks the bytes are a PNG of at most 466x466, writes BG_PATH, and has the
    // screen pick it up. false with a reason in `err` otherwise.
    bool save_background(const uint8_t *png, size_t len, char *err, size_t errLen);
    bool remove_background();
    bool has_background();

    lv_obj_t *build();
    void      destroy();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      tick();             // every loop pass on the UI thread; cheap unless shown
}
