#pragma once
// Themes over WiFi, pulled by the Orb from the account it belongs to.
//
// The cable moves about 17 KB a second, so a theme with pictures on every screen takes
// minutes, and a first sync of a five-theme account takes most of an hour. WiFi moves
// hundreds of KB a second. Orb Studio cannot push over the network (it is a secure page and
// this board can only speak plain HTTP), so the direction is turned round: Studio puts the
// built files on the account, tells the Orb over the cable "go and get them" (or the owner
// asks from the Settings menu, later), and the Orb fetches its own themes.
//
// The account is the source of truth and the card is a mirror of it: the manifest names
// every file of every theme that belongs on this Orb, with the same content hashes the
// card's own theme.json carries, so only files that differ are fetched, and folders the
// manifest does not name are removed. Everything runs from loop() (the SD card is only
// ever touched from there), one file per step, narrated on the update panel.
//
// The token is the Orb's claim: an account handed it over the cable once, and from then on
// the Orb can ask that account for its themes without a computer. Kept in NVS.
#include <stdint.h>
namespace theme_pull {
void begin();                            // read the claim from NVS; once, at boot
bool claim(const char *token, const char *owner);   // remember which account this Orb belongs to
bool claimed();
const char *token();
const char *owner();                     // the account's public id, "" before any claim
void forget();                           // token and owner gone: the Orb belongs to nobody
// Start a pull. False, with lastError() saying why, when there is no WiFi, no claim, or a
// pull is already running. Progress arrives through step().
bool start();
void step();                             // one unit of work; call from loop()
bool active();
// "idle" | "manifest" | "files" | "removing" | "restarting" | "done" | "failed"
const char *status();
int done();
int total();
uint32_t bytesDone();
uint32_t bytesTotal();
const char *lastError();
// main.cpp's deferred theme switch, which validates the slug and reboots from loop().
void setSwitchHook(bool (*hook)(const char *slug));
} // namespace theme_pull
