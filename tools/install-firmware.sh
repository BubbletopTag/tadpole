#!/bin/bash
# Tadpole — install LeapPad2 system files from LFConnect firmware packages.
#
#   ./tools/install-firmware.sh <LFC_Downloads dir | .lfp | .lf2 | .zip>
#
# You supply the firmware. Tadpole ships no LeapFrog code — see README.md for how
# to obtain it legally from hardware you own.
#
# WHAT THE PACKAGES ACTUALLY ARE
# ------------------------------
#   .lf2   bzip2 tar        despite the extension
#   .lfp   ordinary ZIP
#
# Both hold a package directory with a meta.inf manifest. LFConnect's download
# cache is a flat pile of hash-named files, so the only way to know what any of
# them is, is to read the manifest inside. This scans them all.
#
# The one that matters here is Type=DiskImage, Name=Firmware-Base:
#
#   Firmware-Base/4,2268688,kernel.bin
#   Firmware-Base/5,53477376,C4G-E1M-W4K-erootfs.ubi     <- the root filesystem
#   Firmware-Base/meta.inf                                  Version="4.6.0.784"
#
# THE ROOT FILESYSTEM IS A UBIFS VOLUME, not a tar. Reading it needs ubi_reader,
# which is not vendored — writing a UBIFS reader is a project in itself, and the
# tool already exists. If it is missing this says so precisely instead of half
# installing something.
#
# Content packages (Applications, Downloads, DeviceAssets, LanguagePacks) are
# handled by install-content.sh, which already knows the destination rules; this
# calls it once the base system is in place.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
SRC="${1:-}"
ROOTFS_DIR="$PROJ/rootfs"

die() { echo "error: $*" >&2; exit 1; }
note() { echo "  $*"; }

[ -n "$SRC" ] || die "usage: $0 <LFC_Downloads dir | .lfp | .lf2 | .zip>"
[ -e "$SRC" ] || die "no such path: $SRC"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/tadpole-fw.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

# ---- 1. gather candidate packages ---------------------------------------
mkdir -p "$STAGE/pkgs"
case "$SRC" in
    *.zip)
        command -v unzip >/dev/null || die "need 'unzip' for a .zip"
        echo "==> unpacking $SRC"
        unzip -q -o "$SRC" -d "$STAGE/outer" || die "unzip failed"
        find "$STAGE/outer" -type f \( -iname '*.lf2' -o -iname '*.lfp' \) \
             -exec cp {} "$STAGE/pkgs/" \; ;;
    *.lf2|*.lfp)
        cp "$SRC" "$STAGE/pkgs/" ;;
    *)
        [ -d "$SRC" ] || die "not a directory or a known archive: $SRC"
        # LFConnect keeps downloads in <dir>/cache; accept either level.
        for d in "$SRC/cache" "$SRC"; do
            [ -d "$d" ] || continue
            find "$d" -maxdepth 1 -type f \( -iname '*.lf2' -o -iname '*.lfp' \) \
                 -exec cp {} "$STAGE/pkgs/" \; 2>/dev/null
        done ;;
esac

count=$(find "$STAGE/pkgs" -type f | wc -l)
[ "$count" -gt 0 ] || die "no .lf2 or .lfp packages found in $SRC"
echo "==> $count package(s) to inspect"

read_meta() {                       # $1=archive -> meta.inf on stdout
    case "$1" in
        *.lfp) unzip -p "$1" '*meta.inf' 2>/dev/null ;;
        *.lf2) bzcat "$1" 2>/dev/null | tar xO --wildcards '*meta.inf' 2>/dev/null ;;
    esac
}
# Anchored at line start: an unanchored "Version=" also matches MetaVersion="1.0",
# which is how this first reported the firmware as version 1.0.
field() { grep -oE "^$2=\"[^\"]*\"" <<<"$1" | head -1 | cut -d'"' -f2; }

# ---- 2. find the base firmware ------------------------------------------
FW=""; FWVER=""
for f in "$STAGE/pkgs"/*; do
    meta="$(read_meta "$f")"
    [ -n "$meta" ] || continue
    if [ "$(field "$meta" Type)" = DiskImage ] && \
       [ "$(field "$meta" Name)" = Firmware-Base ]; then
        FW="$f"; FWVER="$(field "$meta" Version)"
        break
    fi
done

if [ -z "$FW" ]; then
    echo "no Firmware-Base (Type=DiskImage) package here." >&2
    echo "Packages present:" >&2
    for f in "$STAGE/pkgs"/*; do
        meta="$(read_meta "$f")"
        printf "    %-14s %s\n" "$(field "$meta" Type)" "$(field "$meta" Name)" >&2
    done
    die "supply the full LFC_Downloads directory, which contains it"
fi
echo "==> Firmware-Base version $FWVER"

# ---- 3. extract it, and find the root filesystem ------------------------
mkdir -p "$STAGE/fw"
case "$FW" in
    *.lfp) unzip -q -o "$FW" -d "$STAGE/fw" ;;
    *.lf2) bzcat "$FW" | tar x -C "$STAGE/fw" ;;
esac

UBI="$(find "$STAGE/fw" -type f -iname '*erootfs*.ubi' | head -1)"
[ -n "$UBI" ] || UBI="$(find "$STAGE/fw" -type f -iname '*.ubi' | head -1)"
[ -n "$UBI" ] || die "no .ubi root filesystem inside Firmware-Base"
note "root filesystem: $(basename "$UBI") ($(stat -c %s "$UBI") bytes)"

KERNEL="$(find "$STAGE/fw" -type f -iname '*kernel.bin' | head -1)"
[ -n "$KERNEL" ] && note "kernel: $(basename "$KERNEL")"

DEST="$ROOTFS_DIR/stock-$FWVER"
if [ -d "$DEST/ubi_rfs" ] && [ -n "$(ls -A "$DEST/ubi_rfs" 2>/dev/null)" ]; then
    echo "==> $DEST/ubi_rfs already populated — leaving it alone"
    echo "    (delete it first if you want to re-extract)"
else
    # ---- 4. UBIFS -> a directory tree ----------------------------------
    EXTRACT=""
    for t in ubireader_extract_files ubidump; do
        command -v "$t" >/dev/null && { EXTRACT="$t"; break; }
    done
    if [ -z "$EXTRACT" ]; then
        cat >&2 <<'MSG'

The root filesystem is a UBIFS volume and no extractor is installed.

Install ONE of these, then run this script again:

    pipx install ubi_reader           # recommended, isolated
    pip install --user ubi_reader     # if pip is available
    # Arch:   yay -S python-ubi-reader
    # Debian: apt install python3-ubi-reader

Tadpole does not vendor a UBIFS reader: it is a substantial piece of software
that already exists, and shipping a half-working copy would fail in ways that
look like emulator bugs.
MSG
        die "cannot extract $(basename "$UBI") without ubi_reader"
    fi

    echo "==> extracting the root filesystem with $EXTRACT (this takes a while)"
    mkdir -p "$STAGE/rfs"
    ( cd "$STAGE/rfs" && "$EXTRACT" -o out "$UBI" >/dev/null 2>&1 ) || \
        die "$EXTRACT failed on $(basename "$UBI")"

    # ubi_reader nests its output; find the tree that actually looks like a root.
    ROOT=""
    while IFS= read -r d; do
        ok=1
        for need in bin lib sbin etc; do [ -d "$d/$need" ] || { ok=0; break; }; done
        [ "$ok" = 1 ] && { ROOT="$d"; break; }
    done < <(find "$STAGE/rfs" -maxdepth 5 -type d 2>/dev/null)
    [ -n "$ROOT" ] || die "extraction produced no recognisable root filesystem"

    echo "==> installing to $DEST/ubi_rfs"
    mkdir -p "$DEST"
    rm -rf "$DEST/ubi_rfs"
    cp -a "$ROOT" "$DEST/ubi_rfs" || die "copy failed"
    [ -n "$KERNEL" ] && cp "$KERNEL" "$DEST/kernel.bin"
fi

# ---- 5. content packages -------------------------------------------------
echo "==> installing content packages"
CACHE_DIR="$STAGE/pkgs"
if [ -x "$HERE/install-content.sh" ]; then
    "$HERE/install-content.sh" "$PROJ/runtime/sysroot/LF/Bulk" "$CACHE_DIR" || \
        echo "  (install-content.sh reported problems; system files are still in place)" >&2
else
    echo "  install-content.sh missing; skipping content" >&2
fi

# ---- 6. build the sysroot -----------------------------------------------
if [ -x "$PROJ/runtime/setup-sysroot.sh" ]; then
    echo "==> building the sysroot"
    "$PROJ/runtime/setup-sysroot.sh" || echo "  setup-sysroot.sh failed" >&2
fi

echo
echo "Done. Firmware $FWVER installed under rootfs/stock-$FWVER/"
echo "Start Tadpole with:  ./tadpole.sh"
