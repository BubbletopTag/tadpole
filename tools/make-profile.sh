#!/bin/bash
# Tadpole — create a player profile, so the device does not have to.
#
#   ./tools/make-profile.sh --name "Ada" --grade 2
#   ./tools/make-profile.sh --name "Ada" --grade 2 --picture ~/face.jpg
#   ./tools/make-profile.sh --list
#
# WHY THIS EXISTS. A freshly installed system boots to Create Profile and
# stops there: the screen draws, and nothing you press gets past it. That is a
# real bug worth fixing one day, and it is also completely in the way — with no
# profile there is no home screen, no games, nothing. A profile is five lines
# of text in a file, so Tadpole writes them and the device finds a profile
# already there.
#
# WHAT A PROFILE IS, read off a working device's filesystem rather than
# guessed:
#
#   LF/Bulk/Data/Local/<slot>/                 slot is 0..3, plus "All"
#     profile.dsc            ID, Name, Points, Grade, NumLoginsSinceLastConnect
#     profile_private.dsc    ProfilePicture=<path>
#     ProfilePicture/        holding ProfilePicture.jpg
#     PAD2-0x1F1E0002-100000/UIData.json       the shell's own per-profile state
#     <PackageID>/                             one save directory per title
#
# THE PICTURE IS A .jpg AND THE NAME IS NOT NEGOTIABLE: CreateProfile.swf
# carries the string "ProfilePicture.jpg" and the plugin exposes
# SetProfilePicture(ustring), so the shell looks for that name. A .png copied
# in under that name would be loaded as a JPEG and fail.
#
# Numbers are written as 0x%08X because that is the format every profile.dsc
# on the device uses; the shell parses them either way, but matching what the
# device writes means a profile made here is indistinguishable from one made
# there.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="${TADPOLE_PROJECT:-$(dirname "$HERE")}"
SYSROOT="${TADPOLE_SYSROOT:-$PROJ/runtime/sysroot}"
LOCAL="$SYSROOT/LF/Bulk/Data/Local"
# The shell's per-profile state package is a per-DEVICE PackageID, so read it
# from the device profile rather than hardcoding the LeapPad2's.
if [ -r "$PROJ/runtime/device.sh" ]; then
    ROOTFS=""
    for cand in "$PROJ"/rootfs/*/emmc_rfs "$PROJ"/rootfs/*/ubi_rfs "$PROJ"/rootfs/*/*/ubi_rfs; do
        [ -d "$cand" ] && { ROOTFS="$cand"; break; }
    done
    . "$PROJ/runtime/device.sh"
    tad_load_device || true
fi
SHELL_PKG="${DEV_UIPKG:-PAD2-0x1F1E0002-100000}"

NAME=""; GRADE=1; SLOT=""; PIC=""; LIST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --name)    shift; NAME="${1:-}" ;;
        --grade)   shift; GRADE="${1:-1}" ;;
        --slot)    shift; SLOT="${1:-}" ;;
        --picture) shift; PIC="${1:-}" ;;
        --list)    LIST=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift || true
done

[ -d "$SYSROOT/LF/Bulk" ] || {
    echo "no system files at $SYSROOT — install firmware first." >&2; exit 1; }

if [ "$LIST" = 1 ]; then
    for d in "$LOCAL"/*/; do
        [ -f "$d/profile.dsc" ] || continue
        n=$(grep -oE '^Name=.*' "$d/profile.dsc" | cut -d= -f2-)
        g=$(grep -oE '^Grade=.*' "$d/profile.dsc" | cut -d= -f2-)
        printf "  slot %-4s %-20s grade %s\n" "$(basename "$d")" "$n" "$g"
    done
    exit 0
fi

[ -n "$NAME" ] || { echo "usage: $0 --name NAME [--grade N] [--picture FILE.jpg]" >&2; exit 2; }

# The device's own screens accept a short name; keep the same shape rather than
# letting a 200-character name into a fixed-width UI.
NAME="$(printf '%s' "$NAME" | tr -d '\n\r' | cut -c1-20)"
case "$GRADE" in ''|*[!0-9]*) GRADE=1 ;; esac
[ "$GRADE" -lt 0 ] && GRADE=0
[ "$GRADE" -gt 12 ] && GRADE=12

# FIRST FREE SLOT, unless asked for one. Overwriting slot 0 because it is the
# obvious number would quietly replace whoever is already using it.
if [ -z "$SLOT" ]; then
    for s in 0 1 2 3; do
        [ -f "$LOCAL/$s/profile.dsc" ] || { SLOT="$s"; break; }
    done
    [ -n "$SLOT" ] || { echo "all four profile slots are in use." >&2; exit 1; }
fi

DIR="$LOCAL/$SLOT"
mkdir -p "$DIR/$SHELL_PKG" "$DIR/ProfilePicture" "$LOCAL/All/$SHELL_PKG" || exit 1

printf 'ID=0x%08X\nName=%s\nPoints=0x%08X\nGrade=0x%08X\nNumLoginsSinceLastConnect=0x%08X\n' \
    "$SLOT" "$NAME" 0 "$GRADE" 0 > "$DIR/profile.dsc"

PICPATH=""
if [ -n "$PIC" ]; then
    if [ ! -f "$PIC" ]; then
        echo "  no such picture: $PIC (profile created without one)" >&2
    else
        case "$(printf '%s' "$PIC" | tr 'A-Z' 'a-z')" in
            *.jpg|*.jpeg)
                cp "$PIC" "$DIR/ProfilePicture/ProfilePicture.jpg" &&
                    PICPATH="/LF/Bulk/Data/Local/$SLOT/ProfilePicture/ProfilePicture.jpg" ;;
            *)
                # Saying why beats copying a PNG to a .jpg name and leaving the
                # shell to fail at loading it.
                echo "  picture must be a .jpg — $(basename "$PIC") was not copied" >&2 ;;
        esac
    fi
fi
printf 'ProfilePicture=%s\n' "$PICPATH" > "$DIR/profile_private.dsc"

# The shell keeps per-profile state here and expects the directory to exist.
[ -f "$DIR/$SHELL_PKG/UIData.json" ] || \
    printf '{"_connectAlreadyPlayed": false}\n' > "$DIR/$SHELL_PKG/UIData.json"

# EVERY INSTALLED TITLE NEEDS ITS SAVE DIRECTORY under the new profile, and
# nothing creates them on demand — a title whose save area is missing hangs on
# a white screen with no message. install-game.sh already knows how; a profile
# made after the games were installed would otherwise start out broken.
if [ -x "$HERE/install-game.sh" ]; then
    TADPOLE_BULK="$SYSROOT/LF/Bulk" TADPOLE_BASE="$SYSROOT/LF/Base" \
        "$HERE/install-game.sh" --fix-saves >/dev/null 2>&1
fi

echo "  profile $SLOT: $NAME, grade $GRADE${PICPATH:+, with a picture}"
echo "  $DIR"
