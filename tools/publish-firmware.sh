#!/usr/bin/env bash
# Publish the built firmware into Orb Studio's public/firmware, with a manifest.
#
# Why this exists. Studio ships four binaries and offers to flash them, and that copy was
# made by hand. On 2026-08-21 the bundle had drifted to a THEME_CAPS 5 build while the
# source was at 8, so Studio's own Flash button quietly rolled a device back three
# capability levels and reported "the newest there is" while doing it. Both builds call
# themselves v1.4.2, because FW_VERSION tracks releases and not what the firmware can read.
#
# So: never copy these by hand again, and always write down what the copy can do.
set -euo pipefail
cd "$(dirname "$0")/.."

# --beta publishes into public/firmware/beta instead, which only the administrator's Studio
# offers. For a day spent chasing one fault on a real Orb: twelve releases is right for the
# person holding the device and wrong for everybody watching, and an update prompt that
# arrives twelve times stops meaning anything.
#
# Promoting is the same command without the flag. Nothing copies beta to stable on its own,
# deliberately: a build becomes public because somebody decided it was, not because it was
# the last one built.
BETA=0
for a in "$@"; do [ "$a" = "--beta" ] && BETA=1; done

PUBLIC="${STUDIO_DIR:-$HOME/Developer/hf-sites/buildtheorb/app/public/firmware}"
STUDIO="$PUBLIC"
if [ "$BETA" = "1" ]; then
  STUDIO="$PUBLIC/beta"
  mkdir -p "$STUDIO"
fi
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
BUILD=".pio/build/esp32-s3-amoled-175"

[ -d "$STUDIO" ] || { echo "no Studio firmware dir at $STUDIO" >&2; exit 1; }

echo "building..."
# The build's own exit status, not a grep for the word SUCCESS in its last two lines. That
# read was a race against `set -o pipefail`: grep -q stops at its match, the tail feeding it
# takes a SIGPIPE, and the pipeline reports tail's 141 for a build that worked. It passed for
# weeks and then refused to publish 2.16.59, from a clean tree, because the build was cached
# and therefore fast. The log is kept so a real failure can be read rather than guessed at.
BUILD_LOG=$(mktemp -t orb-build)
if ! "$PIO" run -e esp32-s3-amoled-175 >"$BUILD_LOG" 2>&1; then
  echo "build failed:" >&2; tail -25 "$BUILD_LOG" >&2; rm -f "$BUILD_LOG"; exit 1
fi
rm -f "$BUILD_LOG"

VERSION=$(grep -oE '#define FW_VERSION "[^"]+"' src/config.h | grep -oE '"[^"]+"' | tr -d '"')
CAPS=$(grep -oE 'constexpr int THEME_CAPS = [0-9]+' src/theme_style.h | grep -oE '[0-9]+$')
[ -n "$VERSION" ] && [ -n "$CAPS" ] || { echo "could not read FW_VERSION/THEME_CAPS" >&2; exit 1; }

# A NEW BINARY UNDER AN OLD VERSION NUMBER IS INVISIBLE, and this is the failure that
# actually keeps happening. Studio decides whether to offer an update by comparing version
# STRINGS: the Orb says 1.63.1, the bundle says 1.63.1, so Studio reports "up to date" and
# there is no button to press. The change is sitting right there in the bundle and cannot
# be reached. It looks like the flasher is broken; nothing is broken, the build was just
# never given a name of its own.
#
# So: if the binary moved and FW_VERSION did not, refuse. Bumping is one edit, and it is
# the edit that makes the work reachable.
OLD_VERSION=$(grep -o '"version"[^,]*' "$STUDIO/manifest.json" 2>/dev/null | grep -o '[0-9][0-9.]*' || echo "none")
if [ "$OLD_VERSION" = "$VERSION" ] && [ -f "$STUDIO/firmware.bin" ] \
   && ! cmp -s "$BUILD/firmware.bin" "$STUDIO/firmware.bin"; then
  echo >&2
  echo "REFUSING TO PUBLISH: the firmware changed but FW_VERSION is still $VERSION." >&2
  echo >&2
  echo "  Studio compares version strings, so an Orb already running $VERSION would be" >&2
  echo "  told it is up to date and would never be offered this build." >&2
  echo >&2
  echo "  Bump FW_VERSION in src/config.h, then run this again." >&2
  exit 1
fi

for f in bootloader.bin partitions.bin firmware.bin; do
  cp "$BUILD/$f" "$STUDIO/$f"
done
# boot_app0 comes from the framework, not our build, and only changes with the core.
BOOT0=$(find "$HOME/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -1)
[ -n "$BOOT0" ] && cp "$BOOT0" "$STUDIO/boot_app0.bin"

# WHAT CHANGED, for the person about to press Flash. Lifted out of RELEASE_NOTES.md rather
# than written here or pulled from git: commit subjects are written for whoever reads the
# history, and the question on that card is "what do I get if I press this". Zion asked for
# these beside the button on 2026-09-24, "for people who want an understanding of what
# changed".
#
# Refused when the section is missing, the same way this script refuses a version that was
# not bumped. An update nobody can read about is one people put off, and the moment this is
# optional is the moment it is skipped on the release that most needed it.
# Each "- " bullet becomes one line, with wrapped continuations folded back onto it.
NOTES=$(awk -v v="## $VERSION" '
  $0 == v { on = 1; next }
  on && /^## / { exit }
  !on { next }
  /^- / { if (line != "") print line; sub(/^- /, ""); line = $0; next }
  /^[[:space:]]+[^[:space:]]/ && line != "" { sub(/^[[:space:]]+/, ""); line = line " " $0; next }
  END { if (line != "") print line }
' RELEASE_NOTES.md)
if [ -z "$NOTES" ]; then
  echo "REFUSING TO PUBLISH: RELEASE_NOTES.md has no '## $VERSION' section." >&2
  echo >&2
  echo "  Studio prints that section beside the Flash button, so an update with none is one" >&2
  echo "  nobody can read about before installing it. Add a few lines and run this again." >&2
  exit 1
fi
# One JSON string per line, escaped for quotes and backslashes.
NOTES_JSON=$(printf '%s\n' "$NOTES" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/",/' | sed '$ s/,$//')

cat > "$STUDIO/manifest.json" <<JSON
{
  "version": "$VERSION",
  "caps": $CAPS,
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "bytes": $(stat -f%z "$STUDIO/firmware.bin" 2>/dev/null || stat -c%s "$STUDIO/firmware.bin"),
  "notes": [
$NOTES_JSON
  ]
}
JSON

echo "published v$VERSION caps $CAPS -> $STUDIO"
cat "$STUDIO/manifest.json"

# THIS IS NOT THE LAST STEP, and forgetting that has now shipped an old firmware twice.
#
# Copying the binary into public/firmware only puts it on disk. Orb Studio serves what its
# BUILD contains, so until the site is rebuilt and deployed the flasher hands out whatever
# version was current the last time somebody ran vite build. The second time this happened,
# the Orb was flashed BACKWARDS over a newer build from the cable, which looks like the
# update silently failing.
#
# So the script says what is left rather than trusting anyone to remember, and it says it
# after checking, because a reminder that fires when it is not needed gets ignored.
# Derived from PUBLIC, never from STUDIO: a beta publish writes one folder deeper, and
# deriving this from there pointed the check at a path that does not exist, which reads as
# "the bundle is stale" every single time.
DIST_MANIFEST="$(dirname "$PUBLIC")/../dist/client/firmware/manifest.json"
[ "$BETA" = "1" ] && DIST_MANIFEST="$(dirname "$PUBLIC")/../dist/client/firmware/beta/manifest.json"
DIST_VERSION="$(grep -o '"version"[^,]*' "$DIST_MANIFEST" 2>/dev/null | grep -o '[0-9][0-9.]*' || echo "none")"
if [ "$DIST_VERSION" != "$VERSION" ]; then
  echo
  echo "=============================================================="
  echo " NOT DONE YET. Orb Studio still serves ${DIST_VERSION}."
  echo " Flashing from Studio right now would install ${DIST_VERSION}, not ${VERSION}."
  echo
  echo "   cd $(dirname "$STUDIO")/.. && bun run refresh-default && bun run build && bunx wrangler deploy --name buildtheorb"
  echo "   (refresh-default pulls Zion's Default theme off his account into the build, so an edit he made in Studio ships)"
  echo "=============================================================="
fi
echo
echo " Then, once the commit is made: tools/release-firmware.sh"
echo " (pushes and cuts the GitHub release for v$VERSION from these same files)"
