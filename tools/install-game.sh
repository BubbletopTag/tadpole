#!/bin/bash
# Tadpole — install a LeapFrog game backup (LFManager-style .tar) into /LF/Bulk.
#
#   ./tools/install-game.sh games/FOF.tar [more.tar ...]
#   ./tools/install-game.sh games/*.tar
#   ./tools/install-game.sh --fix-saves          create missing save directories
#                                                (NOT yet cleared library-wide —
#                                                 see HANDOVER before using it)
#
# Installing a game as an APPLICATION is the approach that actually works, and
# it is what LFManager does on real hardware. Emulating an inserted cartridge
# gets the tile to appear but the UI never resolves its GameInfo.
#
# THREE tar shapes, all present in real backups:
#
#   flat            meta.inf at the top          FOF, B10, GLB, UP, ClamPrix
#   self-wrapped    <NAME>/meta.inf              MIP
#   multi-package   <NAME>/meta.inf + lib/...    COOKING, pixar_pals
#
# The self-wrapped and multi-package ones are why "LFManager sends it but it
# never appears": an installer that only looks for a top-level meta.inf finds
# nothing and silently installs nothing.
#
# Destination by Type, same rules as tools/install-content.sh (from lfpkg):
#
#   Application  -> Bulk/ProgramFiles/<PackageID>/
#   System       -> Base/<PackageID>/
#   Download     -> Bulk/Downloads/<PackageID>/
#
# ProfileAccess is appended to Applications that lack it — without it the home
# picker filters the app out. See "Missing home-screen apps" in HANDOVER.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
BULK="${TADPOLE_BULK:-$PROJ/runtime/sysroot/LF/Bulk}"
BASE="${TADPOLE_BASE:-$PROJ/runtime/sysroot/LF/Base}"

[ $# -gt 0 ] || { echo "usage: $0 <game.tar> [...]" >&2; exit 2; }

field() { grep -oE "$2=\"[^\"]*\"" <<<"$1" | head -1 | cut -d'"' -f2; }

install_one() {                     # $1=tar  $2=meta.inf path inside it
    local tar="$1" metapath="$2" prefix meta type pid name dest
    prefix="$(dirname "$metapath")"
    meta="$(tar xOf "$tar" "$metapath" 2>/dev/null)" || return 0
    type="$(field "$meta" Type)"
    pid="$(field "$meta" PackageID)"
    name="$(field "$meta" Name)"
    [ -n "$pid" ] || return 0

    case "$type" in
        Application) dest="$BULK/ProgramFiles/$pid" ;;
        System)      dest="$BASE/$pid" ;;
        Download|MicroDownload) dest="$BULK/Downloads/$pid" ;;
        *)           printf "  %-12s %-26s %s (skipped)\n" "$type" "$pid" "$name"; return 0 ;;
    esac

    rm -rf "$dest"; mkdir -p "$dest"
    if [ "$prefix" = "." ]; then
        tar xf "$tar" -C "$dest"
    else
        # Strip the wrapper directory so the package contents land directly in
        # <dest>, matching how a flat archive installs.
        tar xf "$tar" -C "$dest" --strip-components=1 "$prefix"
    fi

    # THE NEWLINE IS NOT OPTIONAL. A good number of these meta.inf files end
    # WITHOUT one — the last line is `DeviceAccess=1` and then the file simply
    # stops — so appending straight to the end produced
    #
    #     DeviceAccess=1ProfileAccess=-1,0,1,2,3
    #
    # which is two fields lost at once: DeviceAccess reads as garbage, and
    # ProfileAccess is no longer at the start of a line so nothing finds it.
    # The title installs, reports success, and then never appears on the home
    # screen, because that is exactly what a missing ProfileAccess does. Six
    # titles here were in that state.
    #
    # It also hides itself: the `grep -q '^ProfileAccess='` guard cannot see
    # the mangled copy either, so re-installing appends a SECOND one and the
    # line grows.
    if [ "$type" = Application ] && ! grep -q '^ProfileAccess=' "$dest/meta.inf" 2>/dev/null; then
        [ -s "$dest/meta.inf" ] && [ -n "$(tail -c 1 "$dest/meta.inf")" ] &&
            printf '\n' >> "$dest/meta.inf"
        printf 'ProfileAccess=-1,0,1,2,3\n' >> "$dest/meta.inf"
    fi

    # THE SAVE AREA HAS TO EXIST BEFORE FIRST LAUNCH, and nothing creates it.
    #
    # A title's documents live in Bulk/Data/Local/<profile>/<PackageID>/, which
    # AppManager announces as "Set doc base to:" and then assumes is there. It
    # is NOT created on demand: measured, with mkdir interception working in the
    # shim and a full launch traced, the guest never calls mkdir for this path
    # at all. On hardware it already exists; the three titles here that have one
    # got it from the transplanted /LF/Bulk, not from anything we did.
    #
    # WHAT ITS ABSENCE COSTS is a whole title, silently. Cooking! Recipes on the
    # Road logs one line —
    #     fopenAtomic(.../SAVE.DAT): mkstemp failed us!
    # — and then calls dslib::PanicScreen::showDirect(msg, true), whose
    # terminate() is an unconditional spin loop. The title hangs at 100% CPU on
    # a white screen, having drawn nothing, with no crash and no message,
    # because this build compiles the panic screen's own addDirect() down to
    # `bx lr` and the text is never rendered.
    #
    # Per EXISTING profile, not a fixed list: the profiles are whatever
    # Bulk/Data/Local already holds, and inventing 0..3 would litter the tree
    # with directories for accounts that do not exist.
    if [ "$type" = Application ]; then
        local prof
        for prof in "$BULK"/Data/Local/*/; do
            [ -d "$prof" ] || continue
            mkdir -p "$prof$pid"
        done
    fi
    printf "  %-12s %-26s %s\n" "$type" "$pid" "$name"

    local dep
    dep="$(field "$meta" Depends)"
    [ -n "$dep" ] && echo "      needs: $dep"
    return 0
}

# BACKFILL FOR TITLES ALREADY ON DISK. The save-area fix above only runs at
# install time, so without this every title installed before it stays broken and
# the only remedy is reinstalling the whole library.
if [ "${1:-}" = "--fix-saves" ]; then
    made=0
    for d in "$BULK"/ProgramFiles/*/; do
        [ -f "$d/meta.inf" ] || continue
        grep -q '^Type="\?Application' "$d/meta.inf" 2>/dev/null || continue
        id="$(basename "$d")"
        for prof in "$BULK"/Data/Local/*/; do
            [ -d "$prof" ] || continue
            [ -d "$prof$id" ] && continue
            mkdir -p "$prof$id" && made=$((made+1))
        done
    done
    echo "created $made missing save directories under $BULK/Data/Local"
    exit 0
fi

# A LIST IN A FILE, for the viewer's game library.
#
# Ticking twenty titles and pressing Install is an ordinary thing to do there,
# and twenty paths — each of which may contain spaces, brackets and accented
# characters — do not belong on a command line. One path per line, no quoting
# rules to get wrong, and the file is still sitting there afterwards if an
# install needs explaining.
if [ "${1:-}" = "--from-list" ]; then
    LIST="${2:-}"
    [ -f "$LIST" ] || { echo "no such list: $LIST" >&2; exit 2; }
    set --
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        set -- "$@" "$line"
    done < "$LIST"
    [ $# -gt 0 ] || { echo "nothing listed in $LIST" >&2; exit 2; }
    echo "installing $# title(s)"
fi

# IS THIS A DIDJ PACKAGE? -> 0 if yes.
#
# Didj games arrive here because this is the installer the viewer's "Install
# .tar directly" runs, and a user with a Didj dump has no reason to know it is a
# different kind of package. They need a conversion this script does not do — see
# tools/install-didj.sh — so recognise them and hand them over rather than
# installing something that will not launch.
#
# The test is Device="Didj" in a meta.inf, read from either archive shape: the
# community's Didj dumps circulate as .zip while everything else here is .tar.
is_didj() {                         # $1 = archive
    local list m meta
    if [ "$(head -c2 "$1" 2>/dev/null)" = "PK" ]; then
        command -v unzip >/dev/null 2>&1 || return 1
        list="$(unzip -Z1 "$1" 2>/dev/null)"
    else
        list="$(tar tf "$1" 2>/dev/null)"
    fi
    m="$(grep -E '(^|/)meta\.inf$' <<<"$list" | grep -v '/DAmeta\.inf$' | head -1)"
    [ -n "$m" ] || return 1
    if [ "$(head -c2 "$1" 2>/dev/null)" = "PK" ]; then
        meta="$(unzip -p "$1" "$m" 2>/dev/null)"
    else
        meta="$(tar xOf "$1" "$m" 2>/dev/null)"
    fi
    grep -qE '^[[:space:]]*Device[[:space:]]*=[[:space:]]*"Didj"' <<<"$meta"
}

count=0; total=$#
for tar in "$@"; do
    count=$((count+1))
    [ -f "$tar" ] || { echo "no such file: $tar" >&2; continue; }
    if is_didj "$tar"; then
        # DELEGATE, and let it report — including its own progress header, which
        # is why this returns before the one below is printed. install-didj.sh
        # refuses with a message naming the missing step when the compatibility
        # files are not there, which is the one thing a user in this position
        # needs told.
        "$HERE/install-didj.sh" "$tar" || true
        continue
    fi
    # The viewer shows these lines as progress; with a batch of thirty, "which
    # one is it on" is the only question anyone has.
    echo "[$count/$total] $(basename "$tar"):"
    # Every meta.inf in the archive is a package. Multi-package backups bundle
    # a shared library pack alongside the game.
    tar tf "$tar" 2>/dev/null | grep -E '(^|/)meta\.inf$' | while read -r m; do
        install_one "$tar" "$m"
    done
done

echo
echo "installed into $BULK"
