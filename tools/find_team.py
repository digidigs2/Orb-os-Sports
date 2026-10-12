#!/usr/bin/env python3
"""Find a team for the Orb's Scores screen, and optionally add it to the Orb.

    python3 tools/find_team.py <league> <words of the name>
    python3 tools/find_team.py ncaaf east carolina
    python3 tools/find_team.py nhl hurricanes --add 192.168.1.50

Leagues: nfl nhl nba mlb wnba, football (soccer) mls epl laliga bundes seriea ligue1 ucl, and
ncaaf ncaam ncaaw ncaabb (college: football, men's and women's basketball, baseball). Prints each match as the line teams.txt wants, e.g.

    ncaaf ecu 151    East Carolina Pirates

--add <orb address> adds the match (when exactly one) to the Orb's list, the same way the
theorb.local/teams page does, and the Orb fetches its games straight away.

Searches all of ESPN's teams for the league, every college division included. Needs only
Python's standard library and internet access.
"""
import json
import sys
import urllib.request

PATH = {
    "nfl": "football/nfl", "nhl": "hockey/nhl", "nba": "basketball/nba", "mlb": "baseball/mlb",
    "wnba": "basketball/wnba", "mls": "soccer/usa.1",
    "epl": "soccer/eng.1", "laliga": "soccer/esp.1", "bundes": "soccer/ger.1",
    "seriea": "soccer/ita.1", "ligue1": "soccer/fra.1", "ucl": "soccer/uefa.champions",
    "ncaaf": "football/college-football", "ncaam": "basketball/mens-college-basketball",
    "ncaaw": "basketball/womens-college-basketball", "ncaabb": "baseball/college-baseball",
}


def get_json(url, data=None, ctype=None):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": "orb-find-team/1.0",
                                                          **({"Content-Type": ctype} if ctype else {})})
    with urllib.request.urlopen(req, timeout=30) as r:
        body = r.read()
    return json.loads(body) if body[:1] in (b"[", b"{") else body.decode()


def main():
    args = sys.argv[1:]
    orb = None
    if "--add" in args:
        k = args.index("--add")
        if k + 1 >= len(args):
            sys.exit("--add needs the Orb's address, e.g. --add 192.168.1.50")
        orb = args[k + 1]
        del args[k:k + 2]
    if len(args) < 2 or args[0].lower() not in PATH:
        print(__doc__)
        sys.exit(1)
    league, words = args[0].lower(), [w.lower() for w in args[1:]]
    college = league.startswith("ncaa") or PATH[league].startswith("soccer/")   # saved with the team number
    data = get_json(f"http://site.api.espn.com/apis/site/v2/sports/{PATH[league]}/teams?limit=2000")
    teams = [t["team"] for t in data["sports"][0]["leagues"][0]["teams"]]
    hits = []
    for t in teams:
        hay = " ".join(str(t.get(k, "")) for k in ("displayName", "shortDisplayName", "abbreviation", "location", "nickname")).lower()
        if all(w in hay for w in words):
            hits.append(t)
    if not hits:
        sys.exit(f"No {league} team matches '{' '.join(words)}'.")
    for t in hits:
        line = f"{league} {t['abbreviation'].lower()}" + (f" {t['id']}" if college else "")
        print(f"{line:<22} {t['displayName']}")
    if not orb:
        return
    if len(hits) != 1:
        sys.exit(f"\n{len(hits)} matches: add more of the name so only one is left, then --add again.")
    t = hits[0]
    cur = get_json(f"http://{orb}/teams.json")
    lines = [f"{c['league']} {c['abbr'].lower()}" + (f" {c['id']}" if c.get("id") and (c["league"].startswith("ncaa") or PATH.get(c["league"], "").startswith("soccer/")) else "") for c in cur]
    new = f"{league} {t['abbreviation'].lower()}" + (f" {t['id']}" if college else "")
    if new in lines:
        sys.exit("Already on the Orb's list.")
    if len(lines) >= 6:
        sys.exit("The Orb already has six teams: remove one on theorb.local/teams first.")
    lines.append(new)
    get_json(f"http://{orb}/teamsave", data=("\n".join(lines) + "\n").encode(), ctype="text/plain")
    print(f"\nAdded {t['displayName']}. The Orb now follows:")
    for l in lines:
        print("  " + l)


if __name__ == "__main__":
    main()
