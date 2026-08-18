#!/bin/bash
# Tadpole — Didj support: the compatibility files, and Didj game installs.
#
#   ./tools/install-didj.sh --setup DIDJ.zip [ControlOverlay.zip]
#   ./tools/install-didj.sh --status
#   ./tools/install-didj.sh <game.zip|game.tar> [more...]
#   ./tools/install-didj.sh --to-tar <game.zip|game.lfp> [more...]
#
# WHAT A DIDJ GAME IS, AND WHY IT NEEDS ANY OF THIS
# -------------------------------------------------
# The Didj is a 2008 LeapFrog handheld, older than the Leapster Explorer and
# older still than the LeapPad2 whose firmware Tadpole runs. Its games are ARM
# binaries in the same package shape — meta.inf, App.so, asset tree — but they
# declare Device="Didj", and the LeapPad2's AppManager will not launch a package
# whose Device is not its own.
#
# LeapFrog shipped the bridge themselves: the Leapster Explorer carried a set of
# Didj compatibility files (DidjAvatars, DidjMDLs and per-title DidjPatches) and
# AppManager loads them at startup — "End Load DidjPatches" appears in the log of
# every title Tadpole runs, on a stock install, with no Didj game present. The
# support is already in the firmware; what is missing on a fresh install is the
# DATA, which lives in a package Tadpole cannot ship because it is LeapFrog's.
#
# So there are exactly two things to do, and this script does both:
#
#   1. ONCE:      put the compatibility files in LF/Base, where AppManager looks.
#   2. PER GAME:  install the package, then make three edits to its meta.inf and
#                 drop in a controller overlay.
#
# WHY THE meta.inf HAS TO BE EDITED
# ---------------------------------
#   Device="Didj" -> "LeapsterExplorer"
#       The launch gate. Left alone, the title is installed, listed, and refuses
#       to start.
#
#   every line naming a .png is deleted, then the artwork lines are put back
#       The guide deletes them wholesale, which also removes Icon= and
#       PreviewImage= and leaves the home picker with no tile to draw — a red X
#       reading "MISS?". So they go back, naming the title's OWN artwork; the
#       overlay's characterless art is the fallback for a dump that has none.
#
#   the home-screen tile is regenerated
#       The picker draws IconPADS from GameInfo.json AT NATIVE SIZE, and every
#       stock package ships that at 82x88. A Didj icon is 64x64, so it is
#       resampled up and written over BaseIcon.png. See --refresh.
#
#   ProfileAccess is appended when absent
#       Not in the community guide, and not optional: without it the home picker
#       filters the title out of every profile and the game is invisible on the
#       home screen even though it is installed and launchable by name. Didj
#       packages predate the field entirely, so this is always the case for them.
#       Same fix, same reason, as tools/install-game.sh.
#
# THE CONTROLLER OVERLAY
# ----------------------
# Didj games expect the Didj's controls — a d-pad, A/B, and two shoulder
# buttons — and the LeapPad2 has no shoulder buttons at all. The overlay is a
# PADS device-asset that draws the button strip beside the 320x240 game and maps
# the missing ones onto combinations (A+B, L+R). It is generic artwork made for
# this purpose, carrying no title-specific or character images, so one overlay
# serves every Didj game and it is applied to each one at install time.
#
# The guide this automates was written by Dr. RNG for the Tadpole Discord, and
# the overlay was contributed by a member who asked not to be named.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
BULK="${TADPOLE_BULK:-$PROJ/runtime/sysroot/LF/Bulk}"
BASE="${TADPOLE_BASE:-$PROJ/runtime/sysroot/LF/Base}"
# The overlay is staged HOST-side, not in the guest filesystem: it is installer
# input reused by every future Didj game, and it must survive both a sysroot
# rebuild and the guest never knowing it exists.
DIDJ_DIR="${TADPOLE_DIDJ:-$PROJ/runtime/didj}"
OVERLAY="$DIDJ_DIR/overlay"

die() { echo "install-didj: $*" >&2; exit 1; }

# THE MARKER IS THE REAL THING, not a stamp file. "Is Didj support installed"
# has exactly one meaning that matters — can AppManager find the patches — so
# ask that, and a user who installed the files by hand from the guide is
# correctly detected as already set up.
didj_ready() { [ -d "$BASE/DidjPatches" ]; }
overlay_ready() { [ -f "$OVERLAY/GameInfo.json" ]; }

# ---- archive helpers -------------------------------------------------------
#
# Both shapes are real: the community dumps circulate as .zip, while everything
# else in Tadpole's library is LFManager-style .tar. One code path each, chosen
# by what the file actually is rather than by its extension, because a .zip
# named .tar is a support question nobody should have to answer twice.
is_zip() { [ "$(head -c2 "$1" 2>/dev/null)" = "PK" ]; }

arc_list() {                        # $1=archive -> one path per line
    if is_zip "$1"; then unzip -Z1 "$1"; else tar tf "$1"; fi
}
arc_cat() {                         # $1=archive $2=member -> stdout
    if is_zip "$1"; then unzip -p "$1" "$2"; else tar xOf "$1" "$2"; fi
}
arc_extract() {                     # $1=archive $2=dest $3=prefix ("." = flat)
    if is_zip "$1"; then
        if [ "$3" = "." ]; then
            unzip -o -q "$1" -d "$2"
        else
            # Strip the wrapper directory so the package lands directly in
            # <dest>, matching how a flat archive installs.
            local tmp; tmp="$(mktemp -d)"
            unzip -o -q "$1" -d "$tmp"
            (cd "$tmp/$3" && tar cf - .) | (cd "$2" && tar xf -)
            rm -rf "$tmp"
        fi
    else
        if [ "$3" = "." ]; then tar xf "$1" -C "$2"
        else tar xf "$1" -C "$2" --strip-components=1 "$3"; fi
    fi
}

field() { grep -oE "$2=\"[^\"]*\"" <<<"$1" | head -1 | cut -d'"' -f2; }

# SEPARATELY SETTABLE, because the two pieces are two different downloads and
# the wizard picks one file at a time. Someone who already has the compatibility
# files from following the guide by hand needs only this half.
install_overlay() {
    local overlayzip="$1"
    [ -f "$overlayzip" ] || die "no such file: $overlayzip"
    arc_list "$overlayzip" | grep -q 'GameInfo.json' \
        || die "$(basename "$overlayzip") has no GameInfo.json — that is not the controller overlay"
    rm -rf "$OVERLAY"; mkdir -p "$OVERLAY"
    arc_extract "$overlayzip" "$OVERLAY" "."
    echo "  controller overlay staged in ${OVERLAY#$PROJ/}/"
}

# ---- one-time setup --------------------------------------------------------
setup() {
    local didjzip="${1:-}" overlayzip="${2:-}"
    [ -n "$didjzip" ] || die "usage: $0 --setup DIDJ.zip [ControlOverlay.zip]"
    [ -f "$didjzip" ] || die "no such file: $didjzip"
    [ -d "$BASE" ] || die "no LF/Base at $BASE — install the system firmware first"

    # VALIDATE BEFORE WRITING. The wrong zip here scatters junk through the
    # firmware tree, and the failure would not show up until a game refused to
    # start with nothing to connect it to this step.
    arc_list "$didjzip" | grep -q '^DidjPatches/' \
        || die "$(basename "$didjzip") has no DidjPatches/ — that is not the Didj compatibility package"

    echo "Installing Didj compatibility files into ${BASE#$PROJ/}/"
    arc_extract "$didjzip" "$BASE" "."
    local n
    n="$(find "$BASE/DidjPatches" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)"
    echo "  DidjAvatars, DidjMDLs, DidjPatches ($n title patch(es))"

    [ -n "$overlayzip" ] && install_overlay "$overlayzip"

    echo
    if overlay_ready; then
        echo "Didj support is ready. Install games with:"
        echo "  ./tools/install-didj.sh <game.zip>"
    else
        echo "Compatibility files installed, but NO controller overlay was given."
        echo "Didj games need one — they use shoulder buttons the LeapPad2 lacks."
        echo "Re-run with:  $0 --setup $didjzip ControlOverlay.zip"
    fi
}

# ---- per-game install ------------------------------------------------------
install_one() {
    local arc="$1" metapath="$2" prefix meta dev pid name tld dest prof
    local own_icon own_preview own_large art new

    prefix="$(dirname "$metapath")"
    meta="$(arc_cat "$arc" "$metapath" 2>/dev/null)" || return 0
    dev="$(field "$meta" Device)"
    [ "$dev" = "Didj" ] || return 0          # not ours; install-game.sh has it

    pid="$(field "$meta" PackageID)"
    name="$(field "$meta" Name)"
    # THE FOLDER IS NAMED BY THE 3LD, not the PackageID that every other package
    # in the library uses. DidjPatches is indexed by that three-letter code —
    # DidjPatches/SNC for Sonic — and this is the name the guide's working
    # recipe produces, so it is the one kept.
    tld="$(field "$meta" 3LD)"
    [ -n "$tld" ] || tld="$(field "$meta" ShortName)"
    [ -n "$tld" ] || tld="$pid"
    # READ THESE BEFORE THE DELETE WIPES THEM. The title's own artwork is what
    # the restored Icon=/PreviewImage= lines should name; see below.
    own_icon="$(field "$meta" Icon)"
    own_preview="$(field "$meta" PreviewImage)"
    own_large="$(field "$meta" LargeIcon)"
    [ -n "$tld" ] || return 0
    dest="$BULK/ProgramFiles/$tld"

    rm -rf "$dest"; mkdir -p "$dest"
    arc_extract "$arc" "$dest" "$prefix"

    # ---- the meta.inf edits, in the order the guide gives them --------------
    sed -i 's/^\([[:space:]]*Device[[:space:]]*=[[:space:]]*"\)Didj"/\1LeapsterExplorer"/' \
        "$dest/meta.inf"
    sed -i '/\.png/d' "$dest/meta.inf"
    grep -q '^ProfileAccess=' "$dest/meta.inf" 2>/dev/null \
        || printf 'ProfileAccess=-1,0,1,2,3\n' >> "$dest/meta.inf"

    # ---- the overlay -------------------------------------------------------
    overlay_ready && cp -a "$OVERLAY"/. "$dest"/

    # ---- PUT THE ARTWORK LINES BACK, THE TITLE'S OWN FIRST -----------------
    #
    # Deleting every .png line takes Icon= and PreviewImage= with it, and nothing
    # replaces them — so the home picker has no icon to draw and falls back to a
    # tile with a red X reading "MISS?". The title is installed and launches; it
    # just looks broken sitting on the home screen, which is where every user
    # meets it. So the fields have to come back. The question is what they point
    # at, and the answer is NOT the generic art if the game brought its own.
    #
    # A Didj package ships real artwork — Sonic has IconNormal.png (64x64) and
    # Description.png (256x128) — and the LeapPad2 picker scales whatever it is
    # given. Measured across the installed library, no two titles agree on a
    # size: Ben 10 is 60x57, its middle icon 64x64, its large one 90x77, and the
    # overlay's generic tile is 82x88. A 64x64 Didj icon is therefore nothing
    # unusual, and the blanket "delete every .png line" in the written guide is
    # broader than it needs to be: it throws away good artwork to be rid of the
    # field, when the field was what the picker wanted all along.
    #
    # Fall back to the overlay's characterless art only when the title really has
    # nothing, so a dump missing its icons still gets a tile rather than a red X.
    #
    # Appended AFTER the delete rather than rewritten in place, because the
    # delete is what the guide specifies and keeping the two steps separate means
    # the recipe is still recognisable to anyone following it by hand.
    pick_art() {                    # $1 = the title's own name, $2 = fallback
        [ -n "$1" ] && [ -f "$dest/$1" ] && { printf '%s' "$1"; return; }
        [ -f "$dest/$2" ] && printf '%s' "$2"
    }
    art="$(pick_art "$own_icon" iconLPAD.png)"
    [ -n "$art" ] && printf 'Icon="%s"\n' "$art" >> "$dest/meta.inf"
    art="$(pick_art "$own_preview" previewimage.png)"
    [ -n "$art" ] && printf 'PreviewImage="%s"\n' "$art" >> "$dest/meta.inf"
    # LargeIcon IS THE THIRD ARTWORK FIELD, and it was being dropped: the ".png"
    # delete takes it like the other two and only two were ever put back. Sonic
    # and SuperChicks have none, so nothing showed it; JetPack Heroes has
    # LargeIcon="Description.png", a 256x128 PNG shipped inside the package.
    # It is a real field — "LargeIcon" is in the meta.inf key table in
    # LF/Base/lib/libLightningJSON.so beside AppSo and PreviewImage, and stock
    # LST3-0x00180010-000000 and LST3-0x00180002-000000 both carry it. No
    # fallback: the overlay has no large art, and no LargeIcon at all is what
    # every other package on the device has.
    if [ -n "$own_large" ] && [ -f "$dest/$own_large" ]; then
        printf 'LargeIcon="%s"\n' "$own_large" >> "$dest/meta.inf"
    fi

    # ---- packagefiles.md5 ---------------------------------------------------
    #
    # NOTHING ON THE DEVICE ENFORCES IT — measured: 31 of the 124 installed
    # packages in a working sysroot already disagree with their own manifest,
    # every one on ./meta.inf, because install-game.sh appended ProfileAccess
    # after LeapFrog generated it. This is for whoever later runs md5sum -c and
    # has to decide whether the one failing line is ours or a bad download.
    # ONE line re-hashed; the rest stays byte-for-byte as the vendor wrote it,
    # for the reason micromods.py's update_checksum spells out.
    if [ -f "$dest/packagefiles.md5" ] \
       && grep -q '[[:space:]]\./meta\.inf$' "$dest/packagefiles.md5"; then
        new="$(md5sum "$dest/meta.inf" | cut -d' ' -f1)"
        sed -i "s|^[0-9a-f]\{32\}\([[:space:]]*\)\./meta\.inf$|$new\1./meta.inf|" \
            "$dest/packagefiles.md5"
    fi

    # ---- THE HOME-SCREEN TILE ----------------------------------------------
    #
    # IconPADS in GameInfo.json is what the home picker actually draws, and it
    # draws it AT NATIVE SIZE: a Didj title's own 64x64 icon dropped in unchanged
    # renders as a small badge adrift in the middle of an 82x88 card. So it has
    # to be resampled, and resampling is the one step this script cannot do in
    # shell — it goes to Python, the same hand-off the downloads use. Overwriting
    # BaseIcon.png keeps the file name every other package on the device uses.
    #
    # Best effort throughout: no Python, an unreadable icon, or a dump with no
    # artwork at all just leaves the overlay's generic tile in place, which is a
    # far better outcome than failing the install over a picture.
    if [ -n "$own_icon" ] && [ -f "$dest/$own_icon" ]; then
        . "$HERE/lib-deps.sh"
        PY="$(tad_python || true)"
        [ -n "$PY" ] && "$PY" "$HERE/install-didj.py" --make-tile \
            "$dest/$own_icon" "$dest/BaseIcon.png" >/dev/null 2>&1 || true
    fi
    # The picker prints IconLabel down the side of the tile — "Game", "eBook",
    # "Creativity" on stock titles. Saying "Didj" there is free and true.
    if [ -f "$dest/GameInfo.json" ]; then
        sed -i 's/"IconLabel"[[:space:]]*:[[:space:]]*"[^"]*"/"IconLabel":"Didj"/' \
            "$dest/GameInfo.json"
    fi

    # The save area, for the reason tools/install-game.sh spells out at length:
    # nothing creates it on demand and a title that cannot open its documents
    # can hang on a white screen with no message. Keyed by PackageID, which the
    # meta.inf keeps even though the folder does not.
    if [ -n "$pid" ]; then
        for prof in "$BULK"/Data/Local/*/; do
            [ -d "$prof" ] || continue
            mkdir -p "$prof$pid"
        done
    fi

    printf "  %-12s %-26s %s\n" "Didj" "$tld" "$name"
    installed=$((installed+1))
    return 0
}

# ---- entry point -----------------------------------------------------------
case "${1:-}" in
    # DOWNLOADS GO TO PYTHON, the same way tools/online-update.sh hands off:
    # TLS, redirects and progress are already solved there, and netssl knows how
    # to explain a certificate failure on an old machine. One implementation.
    # Same hand-off as the downloads: the tile resampler lives in Python.
    # --to-tar joins them for the same reason: it needs the tile resampler, and
    # writing an LFManager-shaped tar — lfu:lfu, 0777 files, 0700 dirs — is a
    # job for tarfile rather than for GNU tar flags that differ on every host.
    --refresh|--fetch-compat|--fetch-overlay|--to-tar)
        . "$HERE/lib-deps.sh"
        PY="$(tad_python || true)"
        [ -n "$PY" ] || die "no python3 available to download with."
        exec "$PY" "$HERE/install-didj.py" "$@" ;;
    --setup)  shift; setup "$@"; exit 0 ;;
    --overlay)
        shift
        [ -n "${1:-}" ] || die "usage: $0 --overlay ControlOverlay.zip"
        echo "Staging the Didj controller overlay"
        install_overlay "$1"
        exit 0 ;;
    --status)
        if didj_ready; then
            printf 'compat=yes patches=%s\n' \
                "$(find "$BASE/DidjPatches" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)"
        else
            printf 'compat=no patches=0\n'
        fi
        printf 'overlay=%s\n' "$(overlay_ready && echo yes || echo no)"
        exit 0 ;;
    "") die "usage: $0 --setup DIDJ.zip [ControlOverlay.zip] | --status | <game.zip> [...]" ;;
esac

# REFUSE EARLY AND SAY WHICH STEP IS MISSING. Installing a Didj game without the
# compatibility files produces a title that appears on the home screen and then
# will not start, which is indistinguishable from a broken dump.
didj_ready || die "Didj compatibility files are not installed.
  Run:  $0 --setup DIDJ.zip ControlOverlay.zip
  (or use the setup wizard's \"Didj games\" page)"

installed=0
count=0; total=$#
for arc in "$@"; do
    count=$((count+1))
    [ -f "$arc" ] || { echo "no such file: $arc" >&2; continue; }
    echo "[$count/$total] $(basename "$arc"):"
    while read -r m; do
        [ -n "$m" ] || continue
        install_one "$arc" "$m"
    done < <(arc_list "$arc" 2>/dev/null | grep -E '(^|/)meta\.inf$' | grep -v '/DAmeta\.inf$')
done

if [ "$installed" = 0 ]; then
    echo
    echo "No Didj packages found. A Didj game's meta.inf says Device=\"Didj\";"
    echo "for anything else use ./tools/install-game.sh"
    exit 1
fi
echo
echo "installed $installed Didj title(s) into $BULK/ProgramFiles"
