#!/bin/bash
# Tadpole/Glasspole — cut a release, with BOTH platforms' assets on it.
#
#   ./tools/release.sh                 tag from today's date, build, publish
#   ./tools/release.sh --dry-run       build and report, publish nothing
#   ./tools/release.sh --tag tadpole-09082026-0004
#
# CALL THIS FROM THE POST-COMMIT AUTOMATION rather than uploading by hand.
# One release carries two assets — Tadpole-x86_64.AppImage for Linux and
# Glasspole-Setup.exe for Windows — because tools/check-update.py picks the
# one its own platform can run, and a release with only the Linux asset tells
# every Windows user their update is missing.
#
# The release BODY is the commit message, unchanged: that is the contract
# those messages have been written to all along.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
cd "$PROJ"

DRY=0
TAG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        --tag)     shift; TAG="${1:-}" ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v gh >/dev/null || { echo "gh is not installed" >&2; exit 1; }

# tadpole-DDMMYYYY-NNNN, the scheme check-update.py parses. NNNN counts
# releases made today, so the first of the day is 0001.
if [ -z "$TAG" ]; then
    day="$(date +%d%m%Y)"
    n=$(git tag -l "tadpole-$day-*" | wc -l)
    TAG="$(printf 'tadpole-%s-%04d' "$day" "$((n + 1))")"
fi
VERSION="${TAG#tadpole-}"
echo "==> $TAG"

# ---- build both ------------------------------------------------------------
echo "==> Linux AppImage"
if [ -x "$HERE/build-appimage.sh" ]; then
    TADPOLE_VERSION="$VERSION" "$HERE/build-appimage.sh"
else
    echo "  (no build-appimage.sh — skipping the Linux asset)" >&2
fi
APPIMAGE="$(ls -1 "$PROJ"/build/Tadpole-x86_64.AppImage \
                  "$PROJ"/Tadpole-x86_64.AppImage 2>/dev/null | head -1 || true)"

echo "==> Windows installer"
"$HERE/build-windows.sh" --installer --version "$VERSION"
SETUP="$PROJ/build/win/Glasspole-Setup.exe"

[ -f "$SETUP" ] || { echo "no installer at $SETUP" >&2; exit 1; }
ls -l ${APPIMAGE:+"$APPIMAGE"} "$SETUP"

if [ "$DRY" = 1 ]; then
    echo "dry run: nothing published"
    exit 0
fi

# ---- publish ---------------------------------------------------------------
# The body is the commit message this release is being cut from, verbatim.
BODY="$(git log -1 --format=%B)"
git tag -a "$TAG" -m "$TAG" 2>/dev/null || echo "  (tag exists, reusing)"
git push origin "$TAG"
gh release create "$TAG" --title "$TAG" --notes "$BODY" \
    ${APPIMAGE:+"$APPIMAGE"} "$SETUP"
echo "released $TAG"
