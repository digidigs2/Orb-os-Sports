#include "theme_pull.h"
#include "config.h"
#include "sdcard.h"
#include "theme_select.h"
#include "update_ui.h"
#include "chime_library.h"
#include <string.h>
#include <stdio.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <ArduinoJson.h>
#endif

namespace theme_pull {
namespace {

constexpr int    PULL_TOKEN_MAX     = 72;
constexpr int    MAX_FILES     = 16 * 40;   // sixteen themes of forty files, the Orb's ceiling
constexpr int    FILE_NAME_MAX      = 40;
constexpr int    PULL_SHA_LEN       = 64;
constexpr uint32_t MANIFEST_MAX = 256 * 1024;

char  s_token[PULL_TOKEN_MAX + 1] = "";
char  s_owner[48] = "";
bool  (*s_switch)(const char *) = nullptr;

// One file to fetch. The sha is the address on the account; the hash is Studio's own
// fileHash, the number the card's theme.json carries, so the next pull can skip it.
struct Job { char slug[theme_select::MAX_SLUG_LEN]; char name[FILE_NAME_MAX]; char sha[PULL_SHA_LEN + 1]; uint32_t bytes; };
Job     *s_jobs      = nullptr;
int      s_jobN      = 0;
int      s_jobDone   = 0;
uint32_t s_bytesAll  = 0;
uint32_t s_bytesDone = 0;
char     s_active[theme_select::MAX_SLUG_LEN] = "";
// Folders on the card the manifest does not name.
char     s_remove[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
int      s_removeN   = 0;
bool     s_changed   = false;

enum State { IDLE, MANIFEST, FILES, REMOVING, RESTARTING, DONE, FAILED };
State s_state = IDLE;
char  s_err[96] = "";
const char *NAMES[] = { "idle", "manifest", "files", "removing", "restarting", "done", "failed" };

void fail(const char *why) {
    snprintf(s_err, sizeof(s_err), "%s", why);
    s_state = FAILED;
#ifdef ARDUINO
    Serial.printf("[pull] failed: %s\n", why);
#endif
    update_ui::bake_done();   // takes the panel down; nothing was left half-written (see the .part rule)
}

#ifdef ARDUINO
// The hashes the card holds for one theme, from the theme.json the last finished install
// wrote. Filtered so a manifest of a hundred files costs a few hundred bytes of document.
bool card_hashes(const char *slug, JsonDocument &out) {
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/theme.json", slug);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    JsonDocument filter;
    filter["fileHashes"] = true;
    const DeserializationError err = deserializeJson(out, f, DeserializationOption::Filter(filter));
    f.close();
    return !err && out["fileHashes"].is<JsonObjectConst>();
}

bool http_to_file(const char *url, const char *path, uint32_t expect) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(20000);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) { http.end(); Serial.printf("[pull] HTTP %d for %s\n", status, url); return false; }
    // Written beside the real name and renamed only once every byte is down, so a fetch
    // that dies partway leaves the file the card had. writeToStream handles chunked
    // answers as well as sized ones, which getStream alone would not.
    char part[128];
    snprintf(part, sizeof(part), "%s.part", path);
    SD.remove(part);
    File f = SD.open(part, FILE_WRITE);
    if (!f) { http.end(); return false; }
    int n = 0;
    if (http.getSize() > 0) {
        // Sized answer: read it in 4 KB pieces, telling the update panel after each so its
        // twelve-second watchdog never mistakes a big file on slow WiFi for a dead transfer.
        static uint8_t buf[4096];
        WiFiClient *in = http.getStreamPtr();
        const uint32_t deadline = millis() + 60000;
        while ((uint32_t)n < expect && http.connected() && (int32_t)(millis() - deadline) < 0) {
            const size_t got = in->readBytes(buf, sizeof(buf) < expect - n ? sizeof(buf) : expect - n);
            if (!got) { if (!in->available()) delay(5); continue; }
            if (f.write(buf, got) != got) { n = -1; break; }
            n += (int)got;
            update_ui::file_progress(path + 8, s_jobDone, s_bytesDone + (uint32_t)n);
        }
    } else {
        n = http.writeToStream(&f);   // chunked: the client unframes it
    }
    f.close();
    http.end();
    if (n < 0 || (uint32_t)n != expect) {
        Serial.printf("[pull] %s: got %d of %lu bytes\n", path, n, (unsigned long)expect);
        SD.remove(part);
        return false;
    }
    SD.remove(path);
    if (!SD.rename(part, path)) { SD.remove(part); return false; }
    return true;
}

bool fetch_manifest() {
    if (WiFi.status() != WL_CONNECTED) { fail("no WiFi"); return false; }
    char url[256];
    snprintf(url, sizeof(url), "http://%s/api/orb/manifest?t=%s", INTEL_GATEWAY_HOST, s_token);
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(15000);
    if (!http.begin(client, url)) { fail("could not reach the account"); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status == 401 || status == 403) { http.end(); fail("this Orb's claim was not accepted"); return false; }
    if (status != 200) { http.end(); fail("the account did not answer"); return false; }
    String body = http.getString();
    http.end();
    if (body.length() == 0 || body.length() > MANIFEST_MAX) { fail("bad manifest"); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) { fail("manifest would not parse"); return false; }
    snprintf(s_active, sizeof(s_active), "%s", (const char *)(doc["active"] | ""));
    JsonArrayConst themes = doc["themes"].as<JsonArrayConst>();

    // Which folders stay. Everything else on the card goes, except what the Orb is wearing
    // right now: that one cannot come out from under the screens drawing it, and Studio
    // removes it after the restart if it is not in the manifest.
    static char onCard[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    const int nCard = theme_select::listInstalled(onCard);
    s_removeN = 0;
    for (int i = 0; i < nCard; ++i) {
        bool keep = false;
        for (JsonObjectConst t : themes) if (!strcmp(onCard[i], (const char *)(t["slug"] | ""))) { keep = true; break; }
        if (!keep && strcmp(onCard[i], theme_select::activeSlug()) != 0)
            snprintf(s_remove[s_removeN++], theme_select::MAX_SLUG_LEN, "%s", onCard[i]);
    }

    // What to fetch: every file whose hash the card does not already hold. theme.json and
    // _installed always go, last, the same order the cable uses, so a pull that dies leaves
    // the old manifest and the next one re-sends what it had already sent.
    s_jobN = 0; s_bytesAll = 0;
    for (JsonObjectConst t : themes) {
        const char *slug = t["slug"] | "";
        if (!slug[0]) continue;
        JsonDocument have;
        const bool known = card_hashes(slug, have);
        JsonArrayConst files = t["files"].as<JsonArrayConst>();
        for (int pass = 0; pass < 3; ++pass) {
            for (JsonObjectConst f : files) {
                const char *name = f["n"] | "";
                const bool manifest  = !strcmp(name, "theme.json");
                const bool sentinel  = !strcmp(name, "_installed");
                const int  order     = sentinel ? 2 : manifest ? 1 : 0;
                if (order != pass || !name[0]) continue;
                const uint32_t h = f["h"] | 0u;
                if (!manifest && !sentinel && known && have["fileHashes"][name].is<uint32_t>()
                    && have["fileHashes"][name].as<uint32_t>() == h) continue;
                if (s_jobN >= MAX_FILES) break;
                Job &j = s_jobs[s_jobN++];
                snprintf(j.slug, sizeof(j.slug), "%s", slug);
                snprintf(j.name, sizeof(j.name), "%s", name);
                snprintf(j.sha,  sizeof(j.sha),  "%s", (const char *)(f["s"] | ""));
                j.bytes = f["b"] | 0u;
                s_bytesAll += j.bytes;
            }
        }
    }
    // EVERY THEME ABOUT TO CHANGE STOPS CLAIMING TO BE FINISHED, before a byte of it moves.
    //
    // Each file lands atomically here, written to .part and renamed, so no single file is
    // ever half there. The SET is not atomic: a pull that stops after three of ten files
    // leaves a theme whose art is partly new and partly old, and until now that folder still
    // carried the _installed sentinel from the install before it, so it swore it was whole.
    // The sentinel is one of the files being fetched and goes last, which puts the claim back
    // only when the set really is complete. Same reasoning as the cable path, same fix.
    for (int i = 0; i < s_jobN; ++i) {
        bool first = true;
        for (int j = 0; j < i; ++j) if (!strcmp(s_jobs[j].slug, s_jobs[i].slug)) { first = false; break; }
        if (!first) continue;
        char marker[128];
        snprintf(marker, sizeof(marker), "/themes/%s/_installed", s_jobs[i].slug);
        if (SD.exists(marker)) SD.remove(marker);
    }
    Serial.printf("[pull] manifest: %d file(s) to fetch (%lu KB), %d folder(s) to remove, active '%s'\n",
                  s_jobN, (unsigned long)(s_bytesAll / 1024), s_removeN, s_active);
    return true;
}
#endif

} // namespace

void begin() {
#ifdef ARDUINO
    Preferences p;
    if (p.begin("orbpull", true)) {
        p.getString("token", s_token, sizeof(s_token));
        p.getString("owner", s_owner, sizeof(s_owner));
        p.end();
    }
#endif
}

static bool safe_id(const char *v, size_t max) {
    if (!v || !*v || strlen(v) > max) return false;
    for (const char *c = v; *c; ++c) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') return false;
    return true;
}

// The owner is the account's public id, kept beside the token so that hello can say WHOSE
// Orb this is. Studio compares it with the signed-in account and, on a mismatch, offers
// the handover rather than quietly syncing a stranger's themes over the owner's.
bool claim(const char *token, const char *owner) {
    if (!safe_id(token, PULL_TOKEN_MAX)) return false;
    if (owner && *owner && !safe_id(owner, sizeof(s_owner) - 1)) return false;
    snprintf(s_token, sizeof(s_token), "%s", token);
    snprintf(s_owner, sizeof(s_owner), "%s", (owner && *owner) ? owner : "");
#ifdef ARDUINO
    Preferences p;
    if (p.begin("orbpull", false)) { p.putString("token", s_token); p.putString("owner", s_owner); p.end(); }
#endif
    return true;
}

void forget() {
    s_token[0] = 0; s_owner[0] = 0;
#ifdef ARDUINO
    Preferences p;
    if (p.begin("orbpull", false)) { p.clear(); p.end(); }
#endif
}

bool claimed() { return s_token[0] != '\0'; }
const char *token() { return s_token; }
const char *owner() { return s_owner; }
void setSwitchHook(bool (*hook)(const char *)) { s_switch = hook; }

bool start() {
    if (active()) { snprintf(s_err, sizeof(s_err), "a sync is already running"); return false; }
    if (!claimed()) { snprintf(s_err, sizeof(s_err), "this Orb has not been claimed by an account"); return false; }
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) { snprintf(s_err, sizeof(s_err), "no WiFi"); return false; }
    if (!sdcard::mounted()) { snprintf(s_err, sizeof(s_err), "no SD card"); return false; }
    if (!s_jobs) s_jobs = (Job *)heap_caps_malloc(sizeof(Job) * MAX_FILES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jobs) { snprintf(s_err, sizeof(s_err), "out of memory"); return false; }
#endif
    s_err[0] = '\0';
    s_jobN = s_jobDone = 0; s_bytesAll = s_bytesDone = 0; s_removeN = 0; s_changed = false;
    s_state = MANIFEST;
    update_ui::file_received("your themes, over WiFi", 0);
    return true;
}

bool active() { return s_state == MANIFEST || s_state == FILES || s_state == REMOVING; }
const char *status() { return NAMES[s_state]; }
int done() { return s_jobDone; }
int total() { return s_jobN; }
uint32_t bytesDone() { return s_bytesDone; }
uint32_t bytesTotal() { return s_bytesAll; }
const char *lastError() { return s_err; }

void step() {
#ifdef ARDUINO
    switch (s_state) {
        case MANIFEST:
            if (!fetch_manifest()) return;
            s_state = FILES;
            return;
        case FILES: {
            if (s_jobDone >= s_jobN) { s_state = REMOVING; return; }
            const Job &j = s_jobs[s_jobDone];
            char dir[64], path[128], url[200];
            snprintf(dir,  sizeof(dir),  "/themes/%s", j.slug);
            snprintf(path, sizeof(path), "%s/%s", dir, j.name);
            snprintf(url,  sizeof(url),  "http://%s/api/assets/%s", INTEL_GATEWAY_HOST, j.sha);
            if (!SD.exists("/themes")) SD.mkdir("/themes");
            if (!SD.exists(dir)) SD.mkdir(dir);
            update_ui::file_progress(j.name, s_jobDone, s_bytesDone);
            // Two tries: a WiFi hiccup on one file is not a reason to abandon a sync.
            bool ok = http_to_file(url, path, j.bytes);
            if (!ok) ok = http_to_file(url, path, j.bytes);
            if (!ok) { fail("a file could not be fetched"); return; }
            s_bytesDone += j.bytes;
            s_jobDone++;
            s_changed = true;
            update_ui::file_received(j.name, s_jobDone);
            return;
        }
        case REMOVING:
            for (int i = 0; i < s_removeN; ++i) {
                if (theme_select::removeInstalled(s_remove[i])) s_changed = true;
                Serial.printf("[pull] removed '%s'\n", s_remove[i]);
            }
            if (s_removeN) chime_library::rescan();
            s_removeN = 0;
            // Onto the manifest's theme, restarting so the bake runs, if anything moved or
            // the Orb is wearing something else. A pull that fetched nothing and changed
            // nothing simply ends.
            if (s_active[0] && (s_changed || strcmp(s_active, theme_select::activeSlug()) != 0) && s_switch && s_switch(s_active)) {
                s_state = RESTARTING;
                Serial.println("[pull] done, restarting");
            } else {
                s_state = DONE;
                update_ui::bake_done();
                Serial.println("[pull] done, nothing to restart for");
            }
            return;
        default:
            return;
    }
#endif
}

} // namespace theme_pull
