# Scores screen (Orb-os-Sports)

This build is Zion Brock's **The Orb OS 2.17.01** with one addition: a **Scores** screen for the
teams you follow. Everything else is Zion's firmware, unchanged.

## What it shows

- **Score page** for each team: both teams' logos with their names, the score, and the period
  and clock while a game is live; the final (and the day it was played) when there is no game
  today; or the day and time of the next game.
- **Upcoming page**: the team's next six games, with the opponent's logo, day, time, and home
  or away.
- Press the knob (or tap) to go: a team's score → its upcoming games → the next team.

Leagues: **NFL, NHL, NBA, MLB, WNBA**; football (soccer): **MLS, Premier League, La Liga,
Bundesliga, Serie A, Ligue 1, Champions League**; and college **football, men's and women's
basketball, and baseball**. Up to six teams. Without any set, it shows the Carolina Hurricanes
and the New Jersey Devils.

## Picking teams

With the Orb on your WiFi, open **http://theorb.local/teams** (or the Orb's IP address followed
by `/teams`). There is also a **Scores teams** button on the Orb's main web page.

- **Pro teams:** choose the league, then the team, and press **Add**.
- **College teams (or any team):** open the team on espn.com and paste its address, for example
  `https://www.espn.com/college-football/team/_/id/151/east-carolina-pirates`, then **Add**.
- **Football (soccer) clubs:** choose the league and pick the club, or choose the league and
  paste the club's address (`https://www.espn.com/soccer/team/_/id/384/crystal-palace`): a
  club's address does not say which league it plays in.
- Use ▲ ▼ to order them and ✕ to remove one, then **Save**. The Orb fetches the new teams'
  games straight away.

**Background:** a frame in the theme's accent colour, unless the card holds `/scores.png`
(uploaded from the teams page's *Background picture*, or copied there by hand). That picture
is drawn over the frame, centred, at most 466 × 466: opaque parts replace the frame,
transparent parts let it show through. Any PNG works; it is decoded when the screen opens
(about 650 KB of PSRAM while Scores is showing, given back when it closes).

The list is stored on the SD card as `/teams.txt`, one team per line (`nhl car`, `nfl dal`,
college and soccer teams with ESPN's team number: `ncaaf ecu 151`, `epl cry 384`). You can also edit that file by hand
and restart.

From a computer's command line, `tools/find_team.py` finds a team and can add it:

```
python3 tools/find_team.py ncaaf east carolina                    # prints: ncaaf ecu 151
python3 tools/find_team.py ncaaf east carolina --add 192.168.1.50 # and adds it to the Orb
```

## Trying it without an Orb: the desktop simulator

The simulator runs the whole firmware in a window on a computer (Linux or macOS), with
on-screen buttons for the knob, and fetches real scores from ESPN.

```
sudo apt install libsdl2-dev libcurl4-openssl-dev   # once (macOS: brew install sdl2)
mkdir -p sim/sdcard                                  # stands in for the SD card
pio run -e native -t exec
```

- Open Scores straight away: `SIM_APP=Scores .pio/build/native/program`
- **Pick teams in a browser at http://localhost:8080/teams**, the same page the Orb serves. It
  saves `sim/sdcard/teams.txt` and the simulator fetches the new teams at once.
  (`SIM_WEB_PORT=8081` for another port, `SIM_WEB_PORT=0` to turn it off.)
- Logos and the schedule cache land in `sim/sdcard/logos` and `sim/sdcard/scores`, and the
  terminal shows the same `[scores]` / `[logos]` lines as the Orb's serial log.

What it cannot show: the Orb's memory limits (a computer has far more), and the real ESP32
build (GitHub builds that on every push to `main`).

## Where the data comes from

ESPN's public scoreboard, team pages and schedules (`site.api.espn.com`, plain HTTP, no key).

- Live games refresh every 30 seconds while the Scores screen is showing, every 2 minutes
  otherwise; with no game on, every 30 minutes.
- Schedules (the upcoming page and the last result) are checked hourly and right after a game
  ends, and kept on the SD card in `/scores`, so they appear at once after a restart.
- Team logos are downloaded once (ESPN's dark-background versions) and kept in `/logos` on the
  card. To use your own, put a PNG there named `<league>_<team>.png` (for example
  `nhl_car.png`; college teams by number: `ncaaf_151.png`).

## The serial log

Lines starting `[scores]` and `[logos]` say what was fetched, for example:

```
[scores] nhl CAR: schedule 312 KB, 6 upcoming, last game Final
[scores] nhl CAR: CAR 0 @ PHI 0  Sun, October 11th at 7:00 PM EDT  (next look in ...)
[logos] nhl_car: downloaded
```

## Files added or changed

New: `src/score_store.*` (data), `src/scores_view.*` (screen), `src/logo_store.*` (logos),
`src/orb_empty_img.h`, `src/teams_page.h` (the /teams page), `src/sim_web.*` (the simulator's
web server), `tools/find_team.py`, this file. Changed: `src/main.cpp` (screen
registration, network step, the `/teams` page), `src/app_shell.h` (one slot),
`src/theme_sd.*` (small file writes), `src/sim_main.cpp` and `platformio.ini` (simulator).
