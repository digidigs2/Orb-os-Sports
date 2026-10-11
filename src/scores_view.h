#pragma once
#include <lvgl.h>

// Scores: the owner's teams (score_store.h, /teams.txt), one at a time. Each side of the game
// is the team's logo with its short name under it, then the score; below, the period and
// clock while live, the final, or the day and time of the next game. A second page per team
// lists its next games.
//
// Registered with app_shell::addLazy(): built when opened, deleted when evicted.
// Press or tap: a team's score -> its upcoming games -> the next team.
namespace scoresview {
    lv_obj_t *build();
    void      destroy();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      tick();             // every loop pass on the UI thread; cheap unless shown
}
