#include "score_store.h"
#include "config.h"
#include "theme_sd.h"
#include <ArduinoJson.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#else
#include "native_http.h"
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
} Serial;
#endif
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

namespace score_store {
namespace {

// ---------------------------------------------------------------- memory and locking
// Everything here lives in PSRAM: internal RAM is what WiFi and the display are made of
// (see sat_store.cpp), and the responses are tens to hundreds of KB.
#ifdef ARDUINO
void *ext_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *ext_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void  ext_free(void *p) { heap_caps_free(p); }
struct PsramJson : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
PsramJson s_json;
#define JSON_DOC(name) JsonDocument name(&s_json)
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard() { portENTER_CRITICAL(&s_mux); } ~Guard() { portEXIT_CRITICAL(&s_mux); } };
uint32_t now_ms() { return millis(); }
#else
void *ext_alloc(size_t n) { return malloc(n); }
void *ext_realloc(void *p, size_t n) { return realloc(p, n); }
void  ext_free(void *p) { free(p); }
#define JSON_DOC(name) JsonDocument name
std::mutex s_mtx;
struct Guard { Guard() { s_mtx.lock(); } ~Guard() { s_mtx.unlock(); } };
uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

constexpr const char *LIST_PATH = "/teams.txt";
// ESPN nests far deeper than ArduinoJson's default limit of 10, and the limit counts the
// levels the filter throws away too: every team page failed with "TooDeep" in 2.26.0.
constexpr uint8_t NEST = 64;
constexpr size_t MAX_REPLY = 2u * 1024u * 1024u;       // a season schedule is a few hundred KB
constexpr const char *DEFAULT_LIST = "nhl car\nnhl nj\n";

constexpr uint32_t IDLE_MS      = 30u * 60u * 1000u;   // no game on: twice an hour
constexpr uint32_t SOON_MS      = 5u * 60u * 1000u;    // a game today that has not started
constexpr uint32_t LIVE_SHOWN   = 30u * 1000u;         // live, and somebody is looking
constexpr uint32_t LIVE_HIDDEN  = 2u * 60u * 1000u;    // live, screen not showing
constexpr uint32_t RETRY_MS     = 2u * 60u * 1000u;    // after a failure

Team    *s_teams = nullptr;     // shared: written under the lock, read by the display thread
Team    *s_work  = nullptr;     // the network task's scratch copy
bool     s_found[MAX_TEAMS];    // this refresh found a game for team i
int      s_n = 0;
uint32_t s_nextAt = 0;
uint32_t s_schedAt[MAX_TEAMS];     // when each team's schedule is next due (0 = now)
bool     s_schedStale = false;      // set by parse_schedule: a past game is not final yet
// Hourly: often enough that a moved start time shows within the hour, rare enough to be no
// load at all. A finished game also brings it forward (see fetchStep), and the result is kept
// on the card so a restart does not have to ask again.
constexpr uint32_t SCHED_MS = 60u * 60u * 1000u, SCHED_RETRY_MS = 15u * 60u * 1000u;
long     s_cacheAt[MAX_TEAMS];      // unix time the cached schedule was fetched (0 = none)
constexpr uint32_t SCHED_STALE_MS = 20u * 60u * 1000u;
bool     s_tried = false;
volatile bool s_visible = false;
volatile bool s_reload = false;

// League team lists for the /teams page (see team_list()). Index = position in LEAGUES.
constexpr const char *LEAGUES[] = {"nfl", "nhl", "nba", "mlb", "wnba", "mls", "ncaaf", "ncaam", "ncaaw", "ncaabb"};
constexpr int NLEAGUES = sizeof(LEAGUES) / sizeof(LEAGUES[0]);
char *volatile s_list[NLEAGUES] = {};        // published JSON, PSRAM, never freed
volatile int   s_listWant = -1;              // league the page is waiting for
uint32_t       s_listFailAt[NLEAGUES] = {};  // last failed try, so a dead network is not hammered
int league_index(const char *lg) { for (int i = 0; i < NLEAGUES; ++i) if (!strcmp(LEAGUES[i], lg)) return i; return -1; }
long     s_updated = 0;
char     s_problem[48] = "";

void set_problem(const char *p) { Guard g; snprintf(s_problem, sizeof(s_problem), "%s", p); }

// League -> ESPN's sport/league path.
const char *path_of(const char *league) {
    static const struct { const char *k, *v; } map[] = {
        {"nfl", "football/nfl"}, {"nhl", "hockey/nhl"}, {"nba", "basketball/nba"},
        {"mlb", "baseball/mlb"}, {"wnba", "basketball/wnba"}, {"mls", "soccer/usa.1"},
        {"ncaaf", "football/college-football"}, {"ncaam", "basketball/mens-college-basketball"},
        {"ncaaw", "basketball/womens-college-basketball"}, {"ncaabb", "baseball/college-baseball"},
    };
    for (auto &m : map) if (!strcmp(m.k, league)) return m.v;
    return nullptr;
}

// College: hundreds of teams, abbreviations that repeat, and a league scoreboard far too big
// to re-read every 30 seconds. These go by team number and read their own schedule instead.
bool is_college(const char *league) { return !strncmp(league, "ncaa", 4); }

void lower(char *s) { for (; *s; ++s) *s = (char)tolower((unsigned char)*s); }

void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

uint32_t hex_color(const char *s) { return (s && *s) ? (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFF : 0x808080; }

// Scores arrive as a string on the scoreboard and as {value, displayValue} on a team page.
int score_of(JsonVariantConst v) {
    if (v.is<const char *>()) return atoi(v.as<const char *>());
    if (v.is<int>()) return v.as<int>();
    if (v["displayValue"].is<const char *>()) return atoi(v["displayValue"].as<const char *>());
    return (int)(v["value"] | 0.0);
}

void read_side(JsonVariantConst c, Side &s) {
    JsonVariantConst t = c["team"];
    copy(s.abbr, sizeof(s.abbr), t["abbreviation"] | "");
    copy(s.id, sizeof(s.id), t["id"] | "");
    copy(s.name, sizeof(s.name), t["shortDisplayName"] | (t["name"] | (t["abbreviation"] | "")));
    s.color = hex_color(t["color"] | "");
    s.score = score_of(c["score"]);
    const char *rec = c["records"][0]["summary"] | (c["record"][0]["displayValue"] | "");
    copy(s.record, sizeof(s.record), rec);
}

// One competition into a Game; false if it is not a two-sided game.
bool read_game(JsonVariantConst ev, Game &g) {
    JsonVariantConst comp = ev["competitions"][0];
    JsonArrayConst cs = comp["competitors"].as<JsonArrayConst>();
    if (cs.size() < 2) return false;
    memset(&g, 0, sizeof(g));
    for (JsonVariantConst c : cs) {
        const char *ha = c["homeAway"] | "";
        read_side(c, !strcmp(ha, "home") ? g.home : g.away);
    }
    g.start = parse_time(ev["date"] | (comp["date"] | ""));
    JsonVariantConst st = comp["status"]["type"].isNull() ? ev["status"] : comp["status"];
    const char *state = st["type"]["state"] | "";
    g.state = !strcmp(state, "in") ? LIVE : !strcmp(state, "post") ? FINAL : !strcmp(state, "pre") ? PRE : NONE;
    copy(g.detail, sizeof(g.detail), st["type"]["shortDetail"] | (st["type"]["detail"] | ""));
    g.period = st["period"] | 0;
    copy(g.clock, sizeof(g.clock), st["displayClock"] | "");
    return true;
}

// Is this side the owner's team? By number when both have one (college), else by code.
// How ESPN's addresses name a team: its number when it has one (college), else its code.
void team_key(const Team &t, char out[8]) {
    copy(out, 8, t.id[0] ? t.id : t.abbr);
    lower(out);
}

bool same_local_day(long a, long b) {
    const time_t ta = (time_t)a, tb = (time_t)b;
    struct tm x, y;
    localtime_r(&ta, &x);
    localtime_r(&tb, &y);
    return x.tm_year == y.tm_year && x.tm_yday == y.tm_yday;
}

bool same_team(const Team &t, const char *abbr, const char *id) {
    if (t.id[0] && id && id[0]) return !strcmp(t.id, id);
    return !strcasecmp(t.abbr, abbr);
}

// Fill team i from game g (also the team's own name/colour/record, as the game reports them).
void take_game(Team &t, const Game &g) {
    t.game = g;
    t.myHome = same_team(t, g.home.abbr, g.home.id);
    const Side &me = t.myHome ? g.home : g.away;
    if (me.color != 0x808080) t.color = me.color;
    // A college team added from a pasted espn.com address has only a stand-in code: the
    // real one ("ECU") arrives with its first game.
    if (t.id[0] && me.abbr[0] && !strcmp(me.id, t.id)) copy(t.abbr, sizeof(t.abbr), me.abbr);
    if (me.record[0]) copy(t.record, sizeof(t.record), me.record);
    t.have = true;
}

void game_filter(JsonObject f) {
    f["date"] = true;
    // .to<JsonObject>() CREATES the member. Holding a JsonVariant for a key that does not
    // exist yet gives a null handle, and every write through it is silently dropped.
    JsonObject comp = f["competitions"][0].to<JsonObject>();
    comp["date"] = true;
    JsonObject c = comp["competitors"][0].to<JsonObject>();
    c["homeAway"] = true;
    c["score"] = true;
    c["records"][0]["summary"] = true;
    c["record"][0]["displayValue"] = true;
    // Plain arrays, not braced lists: a braced list needs <initializer_list>, which the device
    // build does not pull in on its own (the 2.18.4 build break).
    static const char *const TEAM_KEYS[] = {"id", "abbreviation", "displayName", "shortDisplayName", "name", "color"};
    for (const char *k : TEAM_KEYS) c["team"][k] = true;
    JsonObject statuses[2] = {comp["status"].to<JsonObject>(), f["status"].to<JsonObject>()};
    for (JsonObject s : statuses) {
        s["period"] = true;
        s["displayClock"] = true;
        s["type"]["state"] = true;
        s["type"]["shortDetail"] = true;
        s["type"]["detail"] = true;
    }
}

// ---------------------------------------------------------------- HTTP
// A reply held in PSRAM. Not an Arduino String: growing a String to a 200-400 KB scoreboard
// or season schedule fails silently once no single block that size is left, and getString()
// then hands back "" with nothing in the log, which is exactly how 2.26.0 showed no games.
struct Payload {
    char  *data = nullptr;
    size_t len = 0, cap = 0;
    Payload() = default;
    Payload(const Payload &) = delete;
    Payload &operator=(const Payload &) = delete;
    ~Payload() { if (data) ext_free(data); }
    const char *c_str() const { return data ? data : ""; }
    size_t length() const { return len; }
    bool append(const char *p, size_t n) {
        if (len + n + 1 > cap) {
            size_t want = cap ? cap * 2 : 64 * 1024;
            while (want < len + n + 1) want *= 2;
            if (want > MAX_REPLY) return false;
            char *grown = (char *)ext_realloc(data, want);
            if (!grown) return false;
            data = grown; cap = want;
        }
        memcpy(data + len, p, n);
        len += n;
        data[len] = 0;
        return true;
    }
};

bool http_get(const char *url, Payload &body) {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);
    http.setTimeout(10000);
    // Plain HTTP only (no TLS on this board). A redirect to https:// is reported, not followed.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.useHTTP10(true);        // no chunked encoding: the body arrives as plain bytes
    if (!http.begin(client, url)) return false;
    // setUserAgent, not addHeader: HTTPClient silently drops a "User-Agent" passed to
    // addHeader and sends its own "ESP32HTTPClient", so the device and the simulator used to
    // introduce themselves differently (and the simulator's longer name was refused).
    http.setUserAgent(ESPN_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        Serial.printf("[scores] %s: HTTP %d\n", url, status);
        if (status == 301 || status == 302 || status == 307 || status == 308) set_problem("Scores source wants HTTPS");
        http.end();
        return false;
    }
    // HTTP/1.0 was asked for (below), so the body is never chunked: read it raw until the
    // server closes, in 2 KB steps through a stack buffer, straight into PSRAM.
    WiFiClient *st = http.getStreamPtr();
    char chunk[2048];
    uint32_t last = millis();
    bool full = false;
    while (millis() - last < 10000) {
        const int avail = st->available();
        if (avail > 0) {
            const int rd = st->readBytes(chunk, avail < (int)sizeof(chunk) ? avail : (int)sizeof(chunk));
            if (rd > 0) {
                if (!body.append(chunk, (size_t)rd)) { full = true; break; }
                last = millis();
            }
        } else if (!http.connected()) {
            break;
        } else {
            delay(5);
        }
    }
    http.end();
    if (full) Serial.printf("[scores] %s: reply too big or out of memory at %u KB\n", url, (unsigned)(body.len / 1024));
    else if (!body.len) Serial.printf("[scores] %s: empty reply\n", url);
    return !full && body.len > 0;
#else
    // SIM_SCORES_DIR: canned replies instead of the network, named after the request:
    // <league>_scoreboard.json, <league>_<abbr>.json and <league>_<abbr>_schedule.json.
    if (const char *dir = getenv("SIM_SCORES_DIR")) {
        const char *sp = strstr(url, "/sports/");
        if (!sp) return false;
        const char *league = nullptr;          // the Orb's own code for the league in the URL
        for (const char *l : LEAGUES) {
            const char *pth = path_of(l);
            if (!strncmp(sp + 8, pth, strlen(pth)) && sp[8 + strlen(pth)] == '/') league = l;
        }
        if (!league) return false;
        const char *tm = strstr(url, "/teams/");
        std::string rest = tm ? tm + 7 : strstr(url, "/teams?") ? "teams" : "scoreboard";   // "car", "car/schedule", "teams"
        for (char &ch : rest) if (ch == '/') ch = '_';
        std::string path = std::string(dir) + "/" + league + "_" + rest + ".json";
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[4096]; size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, r);
        fclose(f);
        return body.len > 0;
    }
    std::string s;
    return native_https_get(url, ESPN_USER_AGENT, s, 10000) && !s.empty() && body.append(s.data(), s.size());
#endif
}

// ---------------------------------------------------------------- schedule cache
// /scores/<league>_<team>.bin: the parsed schedule (upcoming games, the last result), so a
// restart shows them at once and does not fetch again within the hour. Raw struct, guarded by
// a magic and the record size: a firmware whose layout differs simply ignores old files.
struct CacheRec {
    char     magic[4];
    uint32_t size;
    long     savedAt;
    Upcoming next[MAX_UPCOMING];
    int      nextN;
    Game     last;
    bool     haveLast;
    Game     today;                     // so a restart mid-game knows a college game is on
    bool     haveToday;
};

void cache_path(const Team &t, char *out, size_t n) {
    char k[8]; team_key(t, k);
    snprintf(out, n, "/scores/%s_%s.bin", t.league, k);
}

void cache_save(const Team &t, long now) {
    if (now < 1700000000) return;                   // no clock, no way to know its age later
    CacheRec *c = (CacheRec *)ext_alloc(sizeof(CacheRec));
    if (!c) return;
    memset(c, 0, sizeof(*c));
    memcpy(c->magic, "SCH2", 4);
    c->size = sizeof(CacheRec);
    c->savedAt = now;
    memcpy(c->next, t.next, sizeof(c->next));
    c->nextN = t.nextN;
    c->last = t.last;
    c->haveLast = t.haveLast;
    c->today = t.today;
    c->haveToday = t.haveToday;
    char path[48]; cache_path(t, path, sizeof(path));
    theme_sd::make_dir("/scores");
    theme_sd::write_whole(path, (const uint8_t *)c, sizeof(*c));
    ext_free(c);
}

// Fills the schedule parts of t; returns when it was fetched, 0 if there is no usable cache.
long cache_load(Team &t) {
    char path[48]; cache_path(t, path, sizeof(path));
    size_t len = 0;
    uint8_t *b = theme_sd::read_whole(path, len, sizeof(CacheRec) + 16);
    if (!b) return 0;
    long at = 0;
    const CacheRec *c = (const CacheRec *)b;
    if (len == sizeof(CacheRec) && !memcmp(c->magic, "SCH2", 4) && c->size == sizeof(CacheRec) &&
        c->nextN >= 0 && c->nextN <= MAX_UPCOMING) {
        memcpy(t.next, c->next, sizeof(t.next));
        t.nextN = c->nextN;
        t.last = c->last;
        t.haveLast = c->haveLast;
        // today's game only if it is still today
        const long now = (long)time(nullptr);
        t.haveToday = c->haveToday && now > 1700000000 && (c->today.state == LIVE || same_local_day(c->today.start, now));
        if (t.haveToday) t.today = c->today;
        at = c->savedAt;
    }
    theme_sd::free(b);
    return at;
}

void load_list() {
    size_t len = 0;
    uint8_t *b = theme_sd::read_whole(LIST_PATH, len, 4096);
    const char *txt = b ? (const char *)b : DEFAULT_LIST;
    const size_t n = b ? len : strlen(DEFAULT_LIST);
    // Built in s_work, then swapped in under the lock: the display thread may be reading.
    int n2 = 0;
    size_t i = 0;
    while (i < n && n2 < MAX_TEAMS) {
        char line[48]; size_t k = 0;
        while (i < n && txt[i] != '\n' && k < sizeof(line) - 1) { if (txt[i] != '\r') line[k++] = txt[i]; ++i; }
        while (i < n && txt[i] != '\n') ++i;
        ++i;
        line[k] = 0;
        if (!k || line[0] == '#') continue;
        char lg[8] = "", ab[8] = "", id[8] = "";
        if (sscanf(line, "%7s %7s %7s", lg, ab, id) < 2) continue;
        for (const char *c = id; *c; ++c) if (!isdigit((unsigned char)*c)) { id[0] = 0; break; }
        lower(lg);
        if (!path_of(lg)) { Serial.printf("[scores] %s: unknown league '%s'\n", LIST_PATH, lg); continue; }
        Team &t = s_work[n2];
        memset(&t, 0, sizeof(t));
        copy(t.league, sizeof(t.league), lg);
        for (char *p = ab; *p; ++p) *p = (char)toupper((unsigned char)*p);
        copy(t.abbr, sizeof(t.abbr), ab);
        copy(t.id, sizeof(t.id), id);
        copy(t.name, sizeof(t.name), ab);
        t.color = 0x808080;
        s_cacheAt[n2] = cache_load(t);
        if (s_cacheAt[n2]) {
            // shown straight away: the last result and the upcoming list, until the network
            // says otherwise
            if (t.haveLast) { t.game = t.last; t.myHome = same_team(t, t.last.home.abbr, t.last.home.id); t.have = true; }
            Serial.printf("[scores] %s %s: %d upcoming from the card\n", t.league, t.abbr, t.nextN);
        }
        ++n2;
    }
    if (b) theme_sd::free(b);
    {
        Guard g;
        memcpy(s_teams, s_work, sizeof(Team) * MAX_TEAMS);
        s_n = n2;
    }
    Serial.printf("[scores] %d team(s) from %s\n", n2, b ? LIST_PATH : "the defaults");
}

}  // namespace

// ---------------------------------------------------------------- parsing (public for tests)

long parse_time(const char *iso) {
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5) return 0;
    // days from civil (Howard Hinnant), so no timegm() and no TZ games
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

bool parse_scoreboard(const char *json, size_t len, const char *league) {
    JSON_DOC(filter);
    game_filter(filter["events"][0].to<JsonObject>());
    JSON_DOC(doc);
    const DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(NEST));
    if (err) { Serial.printf("[scores] %s scoreboard: %s (%u bytes)\n", league, err.c_str(), (unsigned)len); return false; }
    for (JsonVariantConst ev : doc["events"].as<JsonArrayConst>()) {
        Game g;
        if (!read_game(ev, g)) continue;
        for (int i = 0; i < s_n; ++i) {
            Team &t = s_work[i];
            if (strcmp(t.league, league) || s_found[i]) continue;
            if (same_team(t, g.home.abbr, g.home.id) || same_team(t, g.away.abbr, g.away.id)) {
                take_game(t, g);
                const Side &me = t.myHome ? g.home : g.away;
                if (me.name[0]) copy(t.name, sizeof(t.name), me.name);
                s_found[i] = true;
            }
        }
    }
    return true;
}

bool parse_team_page(const char *json, size_t len, const char *league, const char *abbr) {
    JSON_DOC(filter);
    JsonObject tf = filter["team"].to<JsonObject>();
    static const char *const KEYS[] = {"abbreviation", "displayName", "shortDisplayName", "color"};
    for (const char *k : KEYS) tf[k] = true;
    tf["record"]["items"][0]["summary"] = true;
    game_filter(tf["nextEvent"][0].to<JsonObject>());
    JSON_DOC(doc);
    const DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(NEST));
    if (err) { Serial.printf("[scores] %s/%s team page: %s\n", league, abbr, err.c_str()); return false; }
    JsonVariantConst team = doc["team"];
    if (team.isNull()) return false;
    for (int i = 0; i < s_n; ++i) {
        Team &t = s_work[i];
        if (strcmp(t.league, league) || strcasecmp(t.abbr, abbr)) continue;
        copy(t.name, sizeof(t.name), team["shortDisplayName"] | (team["displayName"] | t.abbr));
        t.color = hex_color(team["color"] | "");
        if (t.id[0] && (team["abbreviation"] | "")[0]) copy(t.abbr, sizeof(t.abbr), team["abbreviation"] | "");  // see take_game
        copy(t.record, sizeof(t.record), team["record"]["items"][0]["summary"] | "");
        t.have = true;
        Game g;
        if (read_game(team["nextEvent"][0], g)) {
            take_game(t, g);
            copy(t.name, sizeof(t.name), team["shortDisplayName"] | (team["displayName"] | t.abbr));
        } else {
            memset(&t.game, 0, sizeof(t.game));
        }
        return true;
    }
    return false;
}

bool parse_schedule(const char *json, size_t len, const char *league, const char *abbr, long now) {
    JSON_DOC(filter);
    game_filter(filter["events"][0].to<JsonObject>());   // whole games: the last one is shown with its score
    JSON_DOC(doc);
    const DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(NEST));
    if (err) { Serial.printf("[scores] %s/%s schedule: %s (%u bytes)\n", league, abbr, err.c_str(), (unsigned)len); return false; }
    for (int i = 0; i < s_n; ++i) {
        Team &t = s_work[i];
        if (strcmp(t.league, league) || strcasecmp(t.abbr, abbr)) continue;
        t.nextN = 0;
        t.haveLast = false;
        t.haveToday = false;
        long lastStart = 0;
        s_schedStale = false;
        for (JsonVariantConst e : doc["events"].as<JsonArrayConst>()) {
            JsonVariantConst cp = e["competitions"][0];
            const char *state = cp["status"]["type"]["state"] | "pre";
            const long st = parse_time(e["date"] | (cp["date"] | ""));
            // today's game, whatever its state: what a college team shows, having no league
            // scoreboard (see is_college). Live counts as today even past midnight.
            if (!strcmp(state, "in") || (now > 1700000000 && st && same_local_day(st, now))) {
                Game g;
                if (read_game(e, g) && (!t.haveToday || g.state == LIVE)) { t.today = g; t.haveToday = true; }
            }
            if (!strcmp(state, "post")) {
                // the latest finished game (the list is in date order, but do not rely on it)
                Game g;
                if (st >= lastStart && read_game(e, g) && g.state == FINAL) { t.last = g; t.haveLast = true; lastStart = st; }
                continue;
            }
            // a game that should be over but is not marked final yet: look again soon, so
            // the "last game" score is not hours behind
            if (now > 1700000000 && st && st < now - 3 * 3600) { s_schedStale = true; continue; }
            if (!strcmp(state, "in")) continue;          // being played: on the score page, not "upcoming"
            if (t.nextN >= MAX_UPCOMING) continue;
            Upcoming &u = t.next[t.nextN];
            memset(&u, 0, sizeof(u));
            u.start = st;
            for (JsonVariantConst k : cp["competitors"].as<JsonArrayConst>()) {
                const char *ab = k["team"]["abbreviation"] | "";
                if (same_team(t, ab, k["team"]["id"] | "")) { u.home = !strcmp(k["homeAway"] | "", "home"); continue; }
                copy(u.opp, sizeof(u.opp), ab);
                copy(u.oppId, sizeof(u.oppId), k["team"]["id"] | "");
                copy(u.oppName, sizeof(u.oppName), k["team"]["shortDisplayName"] | (k["team"]["displayName"] | ab));
            }
            if (u.opp[0]) ++t.nextN;
        }
        return true;
    }
    return false;
}

// The league's teams as [{"abbr":..,"name":..,"logo":..}], sorted as ESPN sends them.
static char *build_team_list(const char *json, size_t len) {
    JSON_DOC(filter);
    JsonObject t = filter["sports"][0]["leagues"][0]["teams"][0]["team"].to<JsonObject>();
    t["id"] = true;
    t["abbreviation"] = true;
    t["displayName"] = true;
    t["logos"][0]["href"] = true;
    JSON_DOC(doc);
    const DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(NEST));
    if (err) { Serial.printf("[scores] team list: %s\n", err.c_str()); return nullptr; }
    Payload out;
    out.append("[", 1);
    bool first = true;
    for (JsonVariantConst e : doc["sports"][0]["leagues"][0]["teams"].as<JsonArrayConst>()) {
        JsonVariantConst tm = e["team"];
        const char *vals[4] = {tm["abbreviation"] | "", tm["displayName"] | "", tm["logos"][0]["href"] | "", tm["id"] | ""};
        if (!vals[0][0]) continue;
        static const char *const KEYS[4] = {"{\"abbr\":\"", "\",\"name\":\"", "\",\"logo\":\"", "\",\"id\":\""};
        if (!first) out.append(",", 1);
        first = false;
        for (int k = 0; k < 4; ++k) {
            out.append(KEYS[k], strlen(KEYS[k]));
            for (const char *c = vals[k]; *c; ++c) {          // nothing that could end the string
                const char ch = (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) ? '\'' : *c;
                out.append(&ch, 1);
            }
        }
        out.append("\"}", 2);
    }
    if (!out.append("]", 1)) return nullptr;
    char *pub = out.data;            // hand the buffer over rather than copy it
    out.data = nullptr;
    return pub;
}

static void list_step() {
    const int li = s_listWant;
    if (li < 0 || li >= NLEAGUES || s_list[li]) { s_listWant = -1; return; }
    if (s_listFailAt[li] && now_ms() - s_listFailAt[li] < 15000) return;
    char url[160];
    snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/%s/teams?limit=200", path_of(LEAGUES[li]));
    Payload body;
    char *list = (http_get(url, body)) ? build_team_list(body.c_str(), body.length()) : nullptr;
    if (list) {
        s_list[li] = list;
        Serial.printf("[scores] %s team list ready for the /teams page (%u bytes)\n", LEAGUES[li], (unsigned)strlen(list));
    } else {
        s_listFailAt[li] = now_ms() | 1;
        Serial.printf("[scores] %s team list: could not fetch it\n", LEAGUES[li]);
    }
    if (s_listWant == li) s_listWant = -1;
}

// ---------------------------------------------------------------- the public face

void begin() {
    if (!s_teams) s_teams = (Team *)ext_alloc(sizeof(Team) * MAX_TEAMS);
    if (!s_work)  s_work  = (Team *)ext_alloc(sizeof(Team) * MAX_TEAMS);
    if (!s_teams || !s_work) { s_n = 0; return; }
    memset(s_teams, 0, sizeof(Team) * MAX_TEAMS);
    memset(s_work, 0, sizeof(Team) * MAX_TEAMS);
    load_list();
}

void reload() { s_reload = true; }

int team_list_reply(const char *league, const char **json) {
    *json = nullptr;
    if (league_index(league) < 0 || is_college(league)) return 404;
    *json = team_list(league);
    return *json ? 200 : 202;
}

int teams_json(char *out, size_t n) {
    size_t o = 0;
    auto put = [&](const char *s) { while (*s && o + 1 < n) out[o++] = *s++; };
    put("[");
    char lg[8], ab[8], id[8], nm[32];
    for (int i = 0; entry(i, lg, ab, id, nm, sizeof(nm)); ++i) {
        for (char *c = nm; *c; ++c) if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = '\'';
        if (i) put(",");
        put("{\"league\":\""); put(lg); put("\",\"abbr\":\""); put(ab);
        put("\",\"id\":\""); put(id); put("\",\"name\":\""); put(nm); put("\"}");
    }
    put("]");
    out[o < n ? o : n - 1] = 0;
    return (int)o;
}

bool save_teams(const char *body, char *err, size_t errLen) {
    char clean[6 * 32 + 1];
    size_t o = 0;
    int n = 0;
    const char *p = body ? body : "";
    while (*p) {
        char line[64]; size_t k = 0;
        while (*p && *p != '\n') { if (k < sizeof(line) - 1 && *p != '\r') line[k++] = *p; ++p; }
        if (*p == '\n') ++p;
        line[k] = 0;
        char lg[16] = "", ab[16] = "", id[16] = "", extra[4] = "";
        const int got = sscanf(line, "%15s %15s %15s %3s", lg, ab, id, extra);
        if (got <= 0) continue;                                  // a blank line
        lower(lg); lower(ab);
        const bool college = is_college(lg);
        bool ok = got >= 2 && got <= 3 && league_index(lg) >= 0 && strlen(ab) >= 1 && strlen(ab) <= 7;
        for (const char *c = ab; ok && *c; ++c) ok = isalnum((unsigned char)*c);
        ok = ok && strlen(id) <= 7 && (!college || id[0]);       // college teams need their number
        for (const char *c = id; ok && *c; ++c) ok = isdigit((unsigned char)*c);
        if (!ok) { snprintf(err, errLen, "bad team: %.40s", line); return false; }
        if (++n > MAX_TEAMS) { snprintf(err, errLen, "six teams at most"); return false; }
        o += snprintf(clean + o, sizeof(clean) - o, id[0] ? "%s %s %s\n" : "%s %s\n", lg, ab, id);
    }
    if (!theme_sd::write_whole(LIST_PATH, (const uint8_t *)clean, o)) {
        snprintf(err, errLen, "could not write teams.txt (is the SD card in?)");
        return false;
    }
    Serial.printf("[scores] teams.txt saved from the web page: %d team(s)\n", n);
    reload();
    return true;
}

const char *team_list(const char *league) {
    const int li = league_index(league);
    // Pro leagues only: a college list is hundreds of teams to download for one pick, so the
    // page adds college teams from a pasted espn.com address instead.
    if (li < 0 || is_college(league)) return nullptr;
    if (s_list[li]) return s_list[li];
    s_listWant = li;
    return nullptr;
}

void setVisible(bool on) {
    const bool was = s_visible;
    s_visible = on;
    if (on && !was) {
        // coming into view during a live game: refresh soon rather than up to two minutes late
        Guard g;
        for (int i = 0; i < s_n; ++i)
            if (s_teams[i].game.state == LIVE) { s_nextAt = now_ms(); break; }
    }
}

bool fetchStep() {
    if (!s_teams || !s_work) return false;
#ifdef ARDUINO
    if (WiFi.status() == WL_CONNECTED)
#endif
        list_step();                 // somebody on the /teams page is waiting: before anything else
    if (s_reload) {
        // teams.txt changed (the /teams page): read it again and fetch everything now
        s_reload = false;
        memset(s_work, 0, sizeof(Team) * MAX_TEAMS);
        load_list();
        memset(s_schedAt, 0, sizeof(s_schedAt));
        // (load_list has just re-read the cache times for the new list)
        s_tried = false;
        s_updated = 0;
    }
    if (s_n == 0) return false;
    if (s_tried && (int32_t)(now_ms() - s_nextAt) < 0) return false;
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return false;
#endif
    s_tried = true;
    { Guard g; memcpy(s_work, s_teams, sizeof(Team) * s_n); }
    memset(s_found, 0, sizeof(s_found));

    bool ok = true;
    char url[160];
    // 1) each league's scoreboard once: today's games, live scores
    for (int i = 0; i < s_n; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j) if (!strcmp(s_work[j].league, s_work[i].league)) seen = true;
        if (seen || is_college(s_work[i].league)) continue;    // college: see step 2
        snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/%s/scoreboard", path_of(s_work[i].league));
        Payload body;
        if (!http_get(url, body) || !parse_scoreboard(body.c_str(), body.length(), s_work[i].league)) ok = false;
    }
    // 2) schedules: the upcoming page and the last finished game. Hourly; sooner while a game
    // that has been played is not marked final yet, and straight after one ends. A college
    // team's schedule is also its live source (no league scoreboard), so around game time it
    // is read at the live pace.
    const long now = (long)time(nullptr);
    for (int i = 0; i < s_n; ++i) {
        const Team &w = s_work[i];
        const bool college = is_college(w.league);
        // A game that just ended: its final score and the new "next game" are wanted now.
        const bool justEnded = s_teams[i].game.state == LIVE && w.game.state == FINAL;
        // College reads its schedule at the live pace only around game time: from half an hour
        // before the start until the game is final.
        const bool collegeLive = college && ((w.haveToday && w.today.state != FINAL) ||
                                 (w.nextN && now > 1700000000 && w.next[0].start - now < 30 * 60));
        // After a restart: a cached schedule younger than an hour is still good.
        if (!s_schedAt[i] && s_cacheAt[i] && now > 1700000000 && now - s_cacheAt[i] < (long)(SCHED_MS / 1000))
            s_schedAt[i] = (now_ms() + (uint32_t)(SCHED_MS - (now - s_cacheAt[i]) * 1000)) | 1;
        const bool due = justEnded || collegeLive || !s_schedAt[i] || (int32_t)(now_ms() - s_schedAt[i]) >= 0;
        if (due) {
            char ab[8]; team_key(s_work[i], ab);
            snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/%s/teams/%s/schedule", path_of(s_work[i].league), ab);
            Payload body;
            const bool got = http_get(url, body) && parse_schedule(body.c_str(), body.length(), s_work[i].league, s_work[i].abbr, now);
            s_schedAt[i] = (now_ms() + (!got ? SCHED_RETRY_MS : s_schedStale ? SCHED_STALE_MS : SCHED_MS)) | 1;
            if (got) { cache_save(s_work[i], now); s_cacheAt[i] = now; }
            if (got) Serial.printf("[scores] %s %s: schedule %u KB, %d upcoming, last game %s\n", s_work[i].league, s_work[i].abbr,
                                   (unsigned)(body.length() / 1024), s_work[i].nextN, s_work[i].haveLast ? s_work[i].last.detail : "none");
            if (!got && college) ok = false;
        }
        // today's college game, from this read or the last one
        if (college && s_work[i].haveToday) {
            Team &t = s_work[i];
            take_game(t, t.today);
            const Side &me = t.myHome ? t.today.home : t.today.away;
            if (me.name[0]) copy(t.name, sizeof(t.name), me.name);
            s_found[i] = true;
        }
    }

    // 3) a team not playing today: its name, colour and record from its team page, and its
    // last finished game (or, before its first one, the next)
    for (int i = 0; i < s_n; ++i) {
        if (s_found[i]) continue;
        char ab[8]; team_key(s_work[i], ab);
        snprintf(url, sizeof(url), "http://site.api.espn.com/apis/site/v2/sports/%s/teams/%s", path_of(s_work[i].league), ab);
        Payload body;
        if (!http_get(url, body) || !parse_team_page(body.c_str(), body.length(), s_work[i].league, s_work[i].abbr)) ok = false;
        if (s_work[i].haveLast) take_game(s_work[i], s_work[i].last);
    }

    // when to look again
    uint32_t wait = IDLE_MS;
    for (int i = 0; i < s_n; ++i) {
        const Game &g = s_work[i].game;
        if (g.state == LIVE) wait = s_visible ? LIVE_SHOWN : LIVE_HIDDEN;
        else if (g.state == PRE && g.start && now > 1700000000 && g.start - now < 12 * 3600) {
            // starting today: look again near face-off, then every few minutes until it does
            const long until = (g.start - now - 10 * 60) * 1000L;
            const uint32_t w = until > (long)SOON_MS ? (uint32_t)(until < (long)IDLE_MS ? until : (long)IDLE_MS) : SOON_MS;
            if (w < wait) wait = w;
        }
    }
    if (!ok) wait = wait < RETRY_MS ? wait : RETRY_MS;
    s_nextAt = now_ms() + wait;

    bool any = false;
    for (int i = 0; i < s_n; ++i) any |= s_work[i].have;
    {
        Guard g;
        memcpy(s_teams, s_work, sizeof(Team) * s_n);
        if (any) s_updated = now;
    }
    set_problem(any ? "" : (ok ? "No games found" : "Scores unreachable"));
    for (int i = 0; i < s_n; ++i) {
        const Team &t = s_work[i];
        const Game &g = t.game;
        Serial.printf("[scores] %s %s: %s %d @ %s %d  %s  (next look in %lus)\n", t.league, t.abbr,
                      g.away.abbr, g.away.score, g.home.abbr, g.home.score,
                      g.state == NONE ? "no game" : g.detail, (unsigned long)(wait / 1000));
    }
    return true;
}

int  count() { return s_n; }
bool team(int i, Team &out) {
    if (i < 0 || i >= s_n || !s_teams) return false;
    Guard g;
    out = s_teams[i];
    return true;
}
bool entry(int i, char *league, char *abbr, char *id, char *name, size_t nameLen) {
    if (i < 0 || i >= s_n || !s_teams) return false;
    // plain byte copies under the lock: nothing in a critical section that might allocate
    auto put = [](char *dst, size_t n, const char *src) {
        size_t k = 0;
        while (k + 1 < n && src[k]) { dst[k] = src[k]; ++k; }
        dst[k] = 0;
    };
    Guard g;
    put(league, 8, s_teams[i].league);
    put(abbr, 8, s_teams[i].abbr);
    put(id, 8, s_teams[i].id);
    put(name, nameLen, s_teams[i].name);
    return true;
}
const char *problem() { return s_problem; }
long updatedAt() { return s_updated; }

}  // namespace score_store
