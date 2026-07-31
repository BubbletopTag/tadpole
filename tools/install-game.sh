#!/bin/bash
# Tadpole — install a LeapFrog game backup (LFManager-style .tar) into /LF/Bulk.
#
#   ./tools/install-game.sh games/FOF.tar [more.tar ...]
#   ./tools/install-game.sh games/*.tar
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

    if [ "$type" = Application ] && ! grep -q '^ProfileAccess=' "$dest/meta.inf" 2>/dev/null; then
        printf 'ProfileAccess=-1,0,1,2,3\n' >> "$dest/meta.inf"
    fi
    printf "  %-12s %-26s %s\n" "$type" "$pid" "$name"

    local dep
    dep="$(field "$meta" Depends)"
    [ -n "$dep" ] && echo "      needs: $dep"
    return 0
}

for tar in "$@"; do
    [ -f "$tar" ] || { echo "no such file: $tar" >&2; continue; }
    echo "$(basename "$tar"):"
    # Every meta.inf in the archive is a package. Multi-package backups bundle
    # a shared library pack alongside the game.
    tar tf "$tar" 2>/dev/null | grep -E '(^|/)meta\.inf$' | while read -r m; do
        install_one "$tar" "$m"
    done
done

echo
echo "installed into $BULK"
