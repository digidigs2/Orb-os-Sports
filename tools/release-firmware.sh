#!/usr/bin/env bash
# Push the firmware commit and cut a GitHub release for the version Studio now publishes.
#
#   tools/release-firmware.sh            after publish-firmware.sh, with the commit made
#
# Why: the first stranger to build an Orb read the repo, saw a version behind what Studio
# was flashing, and asked for releases "at the points where you want to make it a
# deployable version" (CanadianAvenger, 2026-09-14). Studio's bundle is that point, so
# the release is cut from the same four files Studio flashes, named by version, with
# the manifest beside them. Refuses to run with uncommitted source, since a release must
# point at a commit that is what it says it is.
set -euo pipefail
cd "$(dirname "$0")/.."
STUDIO="${STUDIO_DIR:-$HOME/Developer/hf-sites/buildtheorb/app/public/firmware}"
REPO="Ziplock78/orb-os"

VERSION=$(grep -oE '#define FW_VERSION "[^"]+"' src/config.h | grep -oE '"[^"]+"' | tr -d '"')
CAPS=$(grep -oE 'constexpr int THEME_CAPS = [0-9]+' src/theme_style.h | grep -oE '[0-9]+$')
PUBLISHED=$(grep -o '"version"[^,]*' "$STUDIO/manifest.json" | grep -o '[0-9][0-9.]*')
[ "$PUBLISHED" = "$VERSION" ] || { echo "Studio has $PUBLISHED, source says $VERSION: run publish-firmware.sh first" >&2; exit 1; }
[ -z "$(git status --porcelain src tools platformio.ini)" ] || { echo "uncommitted changes in src/ or tools/: commit first" >&2; exit 1; }
TAG="v$VERSION"
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then echo "$TAG already exists" >&2; exit 1; fi

git push origin HEAD
git tag -a "$TAG" -m "orb-os $VERSION"
git push origin "$TAG"

TMP=$(mktemp -d)
for f in firmware bootloader partitions boot_app0; do cp "$STUDIO/$f.bin" "$TMP/orb-os-$VERSION-$f.bin"; done
cp "$STUDIO/manifest.json" "$TMP/manifest.json"
# The notes are the commit messages since the previous release, which is what changed.
PREV=$(git describe --tags --abbrev=0 "$TAG^" 2>/dev/null || true)
{
  echo "The firmware Orb Studio flashes as of $(date +%Y-%m-%d). Install it from Studio (My Orb, Flash) rather than by hand: a board needs all four files at four offsets, and Studio puts them where they go."
  echo
  echo "**Changes**"
  if [ -n "$PREV" ]; then git log --format='- %s' "$PREV..$TAG" -- src | grep -v '^- $' || true; else git log -1 --format='- %s'; fi
  echo
  echo "Files, for anyone flashing with esptool: \`bootloader.bin\` at 0x0, \`partitions.bin\` at 0x8000, \`boot_app0.bin\` at 0xe000, \`firmware.bin\` at 0x10000. Theme capability level $CAPS."
} > "$TMP/notes.md"
gh release create "$TAG" --repo "$REPO" --title "orb-os $VERSION" --notes-file "$TMP/notes.md" "$TMP"/orb-os-*.bin "$TMP/manifest.json"
rm -rf "$TMP"
echo "released $TAG"
