#pragma once
// The Scores screen's data: the owner's teams and each one's current or next game, from
// ESPN's public scoreboard feed (site.api.espn.com, plain HTTP, no key).
//
// Teams come from /teams.txt on the SD card, one "<league> <team>" per line, e.g.
//     nhl car
//     nfl dal
// Leagues: nfl nhl nba mlb wnba mls, and college: ncaaf (football), ncaam / ncaaw (men's /
// women's basketball), ncaabb (baseball). The team is ESPN's abbreviation; college teams add
// ESPN's team number, which is what tells them apart: "ncaaf unc 153".
// No file means the Carolina Hurricanes and the New Jersey Devils.
//
// The network task calls fetchStep(); the display thread copies snapshots out with team().
// Each refresh reads the league scoreboard (today's games, live scores). A team not playing
// today shows its last finished game, from its schedule; its team page gives the name,
// colour and record. While a game is live it refreshes every
// 30 seconds with the screen showing, every two minutes without; otherwise every 30 minutes.
// Each team's schedule (the upcoming-games page) is read every six hours.
#include <stddef.h>
#include <stdint.h>

namespace score_store {

constexpr int MAX_TEAMS = 6;

enum State { NONE = 0, PRE, LIVE, FINAL };

struct Side {
    char     abbr[8];
    char     id[8];             // ESPN team number (how college teams are told apart)
    char     name[24];          // short name, e.g. "Hurricanes"
    uint32_t color;             // 0xRRGGBB, the team's own
    int      score;
    char     record[16];        // "5-1-0"
};

struct Game {
    int  state;                 // State
    long start;                 // unix seconds (UTC)
    char detail[32];            // ESPN's one-liner: "2nd - 12:34", "Final/OT", "Sat, 10/17 - 7:00 PM EDT"
    int  period;
    char clock[10];             // "12:34"
    Side away, home;
};

struct Upcoming {
    long start;                 // unix seconds (UTC)
    char opp[8];                // opponent abbreviation
    char oppName[24];           // "Rangers"
    char oppId[8];              // opponent's ESPN team number
    bool home;                  // this team is at home
};
constexpr int MAX_UPCOMING = 6;

struct Team {
    char     league[8];
    char     abbr[8];
    char     id[8];             // ESPN team number from teams.txt ("" for pro teams given by code)
    char     name[28];          // display name, e.g. "Carolina Hurricanes"
    uint32_t color;
    char     record[16];
    bool     have;              // a game (or at least the team) has been read
    bool     myHome;            // this team is the home side of `game`
    Game     game;
    Upcoming next[MAX_UPCOMING];   // the next games from the team's schedule, soonest first
    int      nextN;
    Game     last;              // the most recent finished game, from the schedule
    bool     haveLast;
    Game     today;             // today's game from the schedule (college: no league scoreboard)
    bool     haveToday;
};

void begin();                   // read /teams.txt (call after the SD card is mounted)
void reload();                  // teams.txt changed: the network task re-reads it and fetches at once

// A league's teams for the /teams web page, as a small JSON array of
// {"abbr","name","logo"}. nullptr while it is being fetched: the first call queues the fetch
// for the network task (the web server's task has neither the stack nor the time for it),
// and the page asks again. Kept once fetched. `league` as in teams.txt ("nfl").
const char *team_list(const char *league);

// The /teams page's three requests, shared by the device's web server and the simulator's so
// the two cannot differ. Each is safe to call from the web server's task.
// /teamlist: 200 with *json set, 202 while the network task fetches it, 404 for a league the
// page does not list (college teams are added by their espn.com address).
int  team_list_reply(const char *league, const char **json);
// /teams.json: the chosen teams as [{"league","abbr","id","name"}]. Returns the length.
int  teams_json(char *out, size_t n);
// /teamsave: one "<league> <code> [number]" per line. Every line is checked (a known league,
// letters and digits, a number for college teams, six at most), then /teams.txt is written and
// the teams re-read. false with a reason in err when anything is refused; nothing is written.
bool save_teams(const char *body, char *err, size_t errLen);
bool fetchStep();               // network task: refresh when due. true = new data
void setVisible(bool on);       // the screen is showing: live games refresh faster

int  count();
bool team(int i, Team &out);    // false if i is out of range
// Just the identity of team i, for the /teams web page: a Team is too big for the web
// handler's stack. league, abbr and id get 8 bytes each; name nameLen.
bool entry(int i, char *league, char *abbr, char *id, char *name, size_t nameLen);
const char *problem();          // "" when all is well
long updatedAt();               // unix seconds of the last good refresh, 0 if none

// Exposed for tests and the simulator: parse one ESPN response into the teams.
// `league` is the league the response belongs to.
bool parse_scoreboard(const char *json, size_t len, const char *league);
bool parse_team_page(const char *json, size_t len, const char *league, const char *abbr);
// merge: add to what an earlier call found instead of starting again (football clubs read
// their results and their fixtures as two requests; see fetchStep).
bool parse_schedule(const char *json, size_t len, const char *league, const char *abbr, long now, bool merge = false);
// Unix seconds from ESPN's "2026-10-17T23:00Z" (seconds optional). 0 if unreadable.
long parse_time(const char *iso);

}  // namespace score_store
