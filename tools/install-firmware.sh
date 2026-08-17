#!/bin/bash
# Tadpole — install LeapPad2 system files from LFConnect firmware packages.
#
#   ./tools/install-firmware.sh <LFC_Downloads dir | .lfp | .lf2 | .zip>
#
# You supply the firmware. Tadpole ships no LeapFrog code; this reads the packages
# LFConnect leaves in its download cache on your own machine. See README.md.
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

# Bundled qemu / Python / ubi_reader when they are there, the host's when they
# are not. See tools/lib-deps.sh.
. "$HERE/lib-deps.sh"
PY="$(tad_python_with_ubireader || true)"

# Reading and unpacking a package: unzip and bzcat if the machine has them,
# otherwise pkgtool.py, which does the same with Python's stdlib. Both paths
# are exercised — the bundle has no unzip, and a source checkout usually does.
pkg_meta() {                        # $1=archive -> meta.inf on stdout
    case "$1" in
        *.lfp) command -v unzip >/dev/null && { unzip -p "$1" 'meta.inf' '*/meta.inf' 2>/dev/null; return; } ;;
        *.lf2) command -v bzcat >/dev/null && { bzcat "$1" 2>/dev/null | tar xO --wildcards '*meta.inf' 2>/dev/null; return; } ;;
    esac
    [ -n "$PY" ] && "$PY" "$HERE/pkgtool.py" meta "$1" 2>/dev/null
}
pkg_extract() {                     # $1=archive $2=dir
    mkdir -p "$2"
    case "$1" in
        *.lfp|*.zip) command -v unzip >/dev/null && { unzip -q -o "$1" -d "$2"; return; } ;;
        *.lf2)       command -v bzcat >/dev/null && { bzcat "$1" | tar x -C "$2"; return; } ;;
    esac
    [ -n "$PY" ] || die "no unzip/bzcat and no bundled Python: cannot unpack $(basename "$1")"
    "$PY" "$HERE/pkgtool.py" extract "$1" "$2"
}

[ -n "$SRC" ] || die "usage: $0 <LFC_Downloads dir | .lfp | .lf2 | .zip>"
[ -e "$SRC" ] || die "no such path: $SRC"

# CHECK EVERYTHING FIRST. ubi_reader imports its dependencies lazily, so a
# missing one surfaces minutes in, after the zip is unpacked and 70 packages
# scanned — and then only one at a time. Fail immediately with the whole list.
if [ -x "$HERE/check-deps.sh" ]; then
    "$HERE/check-deps.sh" firmware || exit 1
    echo
fi

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/tadpole-fw.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

# ---- 1. gather candidate packages ---------------------------------------
mkdir -p "$STAGE/pkgs"
case "$SRC" in
    *.zip)
        echo "==> unpacking $SRC"
        pkg_extract "$SRC" "$STAGE/outer" || die "could not unpack $SRC"
        find "$STAGE/outer" -type f \
             \( -iname '*.lf2' -o -iname '*.lfp' -o -iname '*.lf3' \) \
             -exec cp {} "$STAGE/pkgs/" \; ;;
    *.lf2|*.lfp|*.lf3)
        cp "$SRC" "$STAGE/pkgs/" ;;
    *)
        [ -d "$SRC" ] || die "not a directory or a known archive: $SRC"
        # LFConnect keeps downloads in <dir>/cache; accept either level.
        for d in "$SRC/cache" "$SRC"; do
            [ -d "$d" ] || continue
            find "$d" -maxdepth 1 -type f \
                 \( -iname '*.lf2' -o -iname '*.lfp' -o -iname '*.lf3' \) \
                 -exec cp {} "$STAGE/pkgs/" \; 2>/dev/null
        done ;;
esac

count=$(find "$STAGE/pkgs" -type f | wc -l)
[ "$count" -gt 0 ] || die "no .lf2 or .lfp packages found in $SRC"
echo "==> $count package(s) to inspect"

read_meta() { pkg_meta "$1"; }
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
pkg_extract "$FW" "$STAGE/fw" || die "could not unpack $(basename "$FW")"

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
    #
    # Three ways to reach ubi_reader, in decreasing order of how much the user
    # had to do to get it:
    #
    #   the bundled Python           tools/fetch-deps.sh staged it; nothing to
    #                                install, and it is what the AppImage uses
    #   a host python3 that has it   a source checkout that installed it
    #   ubireader_extract_files      the console script on the PATH
    #
    # Tadpole still does not VENDOR a UBIFS reader — it is a substantial piece
    # of software that already exists, and a half-working copy would fail in
    # ways that look like emulator bugs. It ships the real one.
    EXTRACT=""; EXTRACT_KIND=""
    if [ -n "$PY" ]; then
        EXTRACT="$PY"; EXTRACT_KIND=python
    else
        for t in ubireader_extract_files ubidump; do
            command -v "$t" >/dev/null && { EXTRACT="$t"; EXTRACT_KIND=cmd; break; }
        done
    fi
    if [ -z "$EXTRACT" ]; then
        cat >&2 <<'MSG'

The root filesystem is a UBIFS volume and no extractor is available.

The simplest fix, which installs nothing on your system:

    ./tools/fetch-deps.sh

That stages a private Python with ubi_reader into build/deps/ — the same one
the AppImage carries. Failing that, install ubi_reader yourself:

    pipx install ubi_reader           # recommended, isolated
    pip install --user ubi_reader
    # Arch:   yay -S python-ubi-reader
    # Debian: apt install python3-ubi-reader
MSG
        die "cannot extract $(basename "$UBI") without ubi_reader"
    fi

    echo "==> extracting the root filesystem with $(basename "$EXTRACT")"
    echo "    (a 53 MB volume — this takes a minute or two)"
    mkdir -p "$STAGE/rfs"
    # KEEP THE TOOL'S OWN ERROR. Swallowing it behind a generic "failed" hid a
    # real one: ubi_reader installed WITHOUT its LZO backend fails with
    # "ModuleNotFoundError: No module named 'lzallright'", which is a one-line
    # fix — but invisible if stderr goes to /dev/null.
    if [ "$EXTRACT_KIND" = python ]; then
        run_extract() { ( cd "$STAGE/rfs" && "$EXTRACT" "$HERE/pkgtool.py" ubi "$UBI" out ); }
    else
        run_extract() { ( cd "$STAGE/rfs" && "$EXTRACT" -o out "$UBI" ); }
    fi
    if ! run_extract >"$STAGE/ex.log" 2>&1; then
        echo "extraction failed. Its output:" >&2
        sed 's/^/    /' "$STAGE/ex.log" >&2 | tail -20
        if grep -q "lzallright\|No module named" "$STAGE/ex.log"; then
            cat >&2 <<'MSG'

ubi_reader is installed but cannot decompress LZO, which this filesystem uses.
Install its backend:

    pipx inject ubi_reader lzallright      # if you used pipx
    pip install --user lzallright
    # Arch: yay -S python-lzallright

MSG
        fi
        die "could not extract the root filesystem"
    fi

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

    # RESTORE THE EXECUTE BITS.
    #
    # ubi_reader only preserves permissions with -k, which requires root, so
    # everything comes out 0644 — including AppManager ("Exec format error") and
    # every shared library ("can't load library"). tools/fix-perms.py marks ELF
    # files and shebang scripts instead, which needs no privileges.
    echo "==> restoring execute permissions"
    "$HERE/fix-perms.py" "$DEST/ubi_rfs" || \
        echo "    (could not restore permissions)" >&2
fi

# ---- 5. build the sysroot ------------------------------------------------
# BEFORE the content pass: content packages install INTO the sysroot, and the
# sort file that decides what appears on the home screen lives there. Running
# them the other way round produced
#     WARNING: no sort file at .../LpadAssets_en/Data/ProgramFileAppOrder.json
# and content written where nothing would look for it.
if [ -x "$PROJ/runtime/setup-sysroot.sh" ]; then
    echo "==> building the sysroot"
    "$PROJ/runtime/setup-sysroot.sh" || echo "  setup-sysroot.sh failed" >&2
fi


# ---- 6. content packages -------------------------------------------------
echo "==> installing content packages"
CACHE_DIR="$STAGE/pkgs"
if [ -x "$HERE/install-content.sh" ]; then
    "$HERE/install-content.sh" "$PROJ/runtime/sysroot/LF/Bulk" "$CACHE_DIR" || \
        echo "  (install-content.sh reported problems; system files are still in place)" >&2
else
    echo "  install-content.sh missing; skipping content" >&2
fi

# ---- 7. digital purchases (.lf3) -----------------------------------------
#
# These are the titles LFConnect delivered as downloads rather than on a
# cartridge — including the ones bundled free with the device. They are the
# only encrypted thing in the whole set, and Tadpole ships no key, so this
# step is entirely optional: with a key the titles install, without one they
# are skipped and everything else is exactly as it was.
#
# THE MESSAGE IS UNCONDITIONAL. It is the one case where a user has files that
# plainly contain games and gets no games out of them, and "silently installed
# fewer things than you had" is a poor way to learn about it. It goes to
# stdout, so it also appears in the wizard's progress panel.
LF3_N=$(find "$STAGE/pkgs" -maxdepth 1 -type f -iname '*.lf3' 2>/dev/null | wc -l)
if [ "$LF3_N" -gt 0 ]; then
    echo "==> $LF3_N digital purchase(s) (.lf3)"
    if [ -n "$PY" ] && "$PY" "$HERE/lf3.py" --have-key >/dev/null 2>&1; then
        mkdir -p "$STAGE/lf3"
        if "$PY" "$HERE/lf3.py" "$STAGE/pkgs" -o "$STAGE/lf3" 2>&1 | sed 's/^/  /'; then
            for t in "$STAGE/lf3"/*.tar; do
                [ -f "$t" ] || continue
                "$HERE/install-game.sh" "$t" 2>&1 | sed 's/^/  /'
            done
        fi
    else
        echo "  can't open .lf3 files, decryption key missing."
        echo "  Put key in keys/lf3.keys"
        echo "  (everything else installed normally; only these titles were skipped)"
    fi
fi

echo
echo "Done. Firmware $FWVER installed under rootfs/stock-$FWVER/"
echo "Start Tadpole with:  ./tadpole.sh"
