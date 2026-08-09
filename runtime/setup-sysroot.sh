#!/bin/bash
# Tadpole — build the guest sysroot.
#
# Keeps rootfs/ pristine: the read-only parts of the stock filesystem are
# symlinked, and everything the guest needs to WRITE or that we have to fake
# is a real directory here.
#
# Every value below was derived from the firmware itself — see
# docs/device-deps.md for how, and PLAN.txt 1b for the rootfs inventory.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
# DISCOVER the rootfs. A hardcoded version works only for the tree it was
# written against: install-firmware.sh lays out rootfs/<version>/ubi_rfs, while
# the original hand-extracted copy had an extra numeric level. Both are valid,
# and the version depends on whichever device the firmware came from.
ROOTFS=""
for cand in "$PROJ"/rootfs/*/ubi_rfs "$PROJ"/rootfs/*/*/ubi_rfs; do
    [ -d "$cand" ] || continue
    ROOTFS="$cand"; break
done
: "${ROOTFS:=$PROJ/rootfs/MISSING/ubi_rfs}"
SYSROOT="$HERE/sysroot"
CACHE="$PROJ/sources/nxp320/LFC_full/LFC_Downloads/cache"

[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 1; }

# HOW "LINK" IS SPELLED HERE. On Linux it is a symlink, as it always was. On
# MSYS it is a COPY: MSYS's default `ln -s` writes WSL-style reparse points
# (tag 0xA000001D) that native Win32 programs cannot follow — glasspole.exe
# gets ENOENT straight through them — and true NTFS symlinks need a privilege
# an ordinary session does not hold. Copying costs ~100 MB of disk and keeps
# every property that matters: rootfs/ stays pristine, and the sysroot is
# readable by native code. The [ -L ] re-run guards below stay correct either
# way — a copy is not a link, so they simply never fire on MSYS.
case "$(uname -s)" in
    MSYS*|MINGW*) lns() { rm -rf "$2"; cp -a "$1" "$2"; } ;;
    *)            lns() { ln -sfn "$1" "$2"; } ;;
esac

echo "==> sysroot skeleton"
mkdir -p "$SYSROOT"
cd "$SYSROOT"
# read-only parts of the stock image
for d in bin boot etc Firmware lib linuxrc mnt sbin erootfs.md5; do
    lns "$ROOTFS/$d" "$d"
done
# /LF must be real: Bulk and Cart are written to at runtime.
# Only unlink if it is still the symlink from a previous layout — removing a
# real directory here would wipe installed Bulk content, and `rm -f` on a
# directory fails, which under `set -e` silently aborts the whole script.
[ -L LF ] && rm -f LF
mkdir -p LF/Bulk LF/Cart
lns "$ROOTFS/LF/Base" LF/Base
mkdir -p dev/input sys proc tmp flags

echo "==> /usr overlay (absolute-path library lookups)"
# Some binaries resolve libraries by absolute path (e.g. /usr/lib/libpthread.so.0)
# even though libpthread actually lives in /lib. qemu-user then falls through to
# the HOST /usr/lib and hands the guest an x86-64 object. Make /usr a real dir
# whose lib/ contains every guest .so, so absolute lookups always hit ARM.
[ -L usr ] && rm -f usr
mkdir -p usr
for d in "$ROOTFS"/usr/*; do
    b="$(basename "$d")"
    [ "$b" = lib ] || lns "$d" "usr/$b"
done
mkdir -p usr/lib
for src in "$ROOTFS"/usr/lib "$ROOTFS"/lib; do
    for f in "$src"/*; do
        [ -e "$f" ] || continue
        lns "$f" "usr/lib/$(basename "$f")" 2>/dev/null || true
    done
done

# ONE libdl, NOT TWO — this is what made native Brio apps crash on launch.
#
# Our shim impersonates libdl.so.0 and is found via LD_LIBRARY_PATH. But
# /lib/libdl.so.0 and /usr/lib/libdl.so.0 pointed at the REAL uClibc libdl, so
# anything resolved by absolute path loaded a SECOND dl provider alongside the
# shim. The process ended up with both mapped at once:
#
#     0x443b0000  runtime/shimlibs/libdl.so.0     (ours)
#     0x44e40000  /lib/libdl-0.9.32.1-git.so      (the real one, again)
#
# Two providers of dlopen/dlsym/dlclose in one link map. Loading was fine;
# UNLOADING was not. Switching apps calls CAppManager::ExitPopUnloadApp ->
# dlclose(/LF/Base/LPAD/), and the loader walked that duplicated scope straight
# into a SIGSEGV inside ld-uClibc's _dl_find_hash (symbol lookup). Every native
# app died the same way: home screen animates, screen blanks, segfault.
#
# Point both paths at the shim so there is exactly one libdl. dlopen/dlsym/
# dlclose still resolve, through the shim's DT_NEEDED on the renamed real
# libdl.so.9.
SHIM_DL="$PROJ/runtime/shimlibs/libdl.so.0"
if [ -e "$SHIM_DL" ]; then
    lns "$SHIM_DL" lib/libdl.so.0
    lns "$SHIM_DL" usr/lib/libdl.so.0
else
    echo "    WARNING: shim libdl not built yet — run 'cd tadpole && make shim'," >&2
    echo "    then re-run this script, or native apps will crash on launch." >&2
fi

echo "==> /etc overlay (ALSA null sink)"
# Brio's CAudioMixer opens "plugdmix" and falls back to "plughw:0,0". Neither
# works here: the guest's alsa-lib 1.0.24 cannot drive the host card through
# qemu ("Invalid value for card"). CAudioMixer then RETRIES FOREVER, and the
# resulting spin starves the UI thread so nothing ever renders.
# Giving it a null sink makes the open succeed, the loop stop, and the UI draw.
# This SILENCES audio rather than implementing it — real output should route
# through the shim to SDL, the same way the framebuffer does.
[ -L etc ] && rm -f etc
mkdir -p etc
for f in "$ROOTFS"/etc/*; do
    b="$(basename "$f")"
    [ "$b" = asound.conf ] || lns "$f" "etc/$b"
done
cat > etc/asound.conf <<'ASOUND'
# Tadpole: null audio sink. See runtime/setup-sysroot.sh for why.
pcm.plugdmix   { type null }
pcm.dmixbrio   { type null }
pcm.!default   { type null }
ctl.!default   { type hw card 0 }
ASOUND

echo "==> /var/sounds video symlinks (rcS does this per-platform)"
# The shipped symlinks point at LucyAssets — a DIFFERENT board. rcS repoints
# them at boot according to /sys/devices/system/board/platform, and for
# VALENCIA they must resolve to LpadAssets. We never run rcS, so without this
# they dangle and VideoDaemon exits immediately instead of serving its socket.
[ -L var ] && rm -f var
mkdir -p var
for d in "$ROOTFS"/var/*; do
    b="$(basename "$d")"
    [ "$b" = sounds ] || lns "$d" "var/$b"
done
mkdir -p var/sounds
for f in "$ROOTFS"/var/sounds/*; do
    lns "$f" "var/sounds/$(basename "$f")" 2>/dev/null || true
done
for v in StartupVideo.ogg ShutdownVideo.ogg TransitionVideo.ogg; do
    [ -e "$ROOTFS/LF/Base/LpadAssets/Video/$v" ] &&
        lns "$ROOTFS/LF/Base/LpadAssets/Video/$v" "var/sounds/$v"
done
if [ -e "$ROOTFS/LF/Base/LpadAssets/Video/powerdown.wav" ]; then
    lns "$ROOTFS/LF/Base/LpadAssets/Video/powerdown.wav" var/sounds/powerdown.wav
fi

echo "==> sysfs (rcS branches on platform; libDisplay reads lcd_size)"
mkdir -p sys/devices/system/board \
         sys/devices/platform/lf2000-power \
         sys/devices/platform/lf2000-aclmtr \
         sys/devices/platform/lf1000-gpio \
         sys/devices/platform/lf1000-dpc \
         sys/class/graphics/fb0
printf 'VALENCIA' > sys/devices/system/board/platform
# All values below read off a live LeapPad2 — reference/device-capture/.
printf 'LPAD'      > sys/devices/system/board/platform_family
printf '0x310'     > sys/devices/system/board/system_rev
printf '480x272'   > sys/devices/system/board/lcd_size         # format is %ux%u
printf 'ILI6480G2' > sys/devices/system/board/lcd_type
printf 'K&D-1'     > sys/devices/system/board/lcd_mfg
printf 'K&D-1'     > sys/devices/system/board/lcd_mfg_get
printf '480'      > sys/devices/platform/lf1000-dpc/xres
printf '272'      > sys/devices/platform/lf1000-dpc/yres
printf '0'        > sys/devices/platform/lf1000-gpio/board_id
printf '1'        > sys/devices/platform/lf2000-power/status   # 1 = EXTERNAL
printf '0'        > sys/class/graphics/fb0/rotate

echo "==> device nodes"
# These must EXIST as directory entries or the guest stops enumerating after
# event1. The shim intercepts open() on them regardless of content.
# The required set is confirmed by usr/bin/make_dev_nodes.sh in the firmware.
for i in $(seq 0 24); do : > "dev/input/event$i"; done
for i in 0 1 2;      do : > "dev/fb$i"; done

echo "==> /proc/asound (audio codec identity)"
# CAudioModule reads /proc/asound/card0/id to identify the codec and logs
#   "Found codec: %s, legacy=%d"
# On real hardware that is socaudiolfp100 (confirmed in
# LF/Base/MfgTest/MfgTest_ReleaseNotes.txt). We do NOT have this file, and
# qemu-user's -L falls through to the HOST path for anything missing — so the
# guest was reading the developer machine's /proc/asound/card0/id and finding
# "PCH", the host's Intel HDA codec. Give it the device's identity instead.
mkdir -p proc/asound/card0
printf 'socaudiolfp100\n' > proc/asound/card0/id

echo "==> /proc/mtd + MfgData"
# CMfgData::Init parses /proc/mtd for a partition named MfgData0, then opens
# /dev/mtd<N>. Without this it fails to init and CMfgData::Read segfaults
# inside libc. With it, the locale lookup degrades gracefully to "en-us".
# Sizes/erasesizes are the documented LeapPad2 partition table.
cat > proc/mtd <<'EOF'
dev:    size   erasesize  name
mtd0: 0007e000 00001000 "NOR_Boot"
mtd1: 00001000 00001000 "MfgData0"
mtd2: 00001000 00001000 "MfgData1"
mtd3: 00400000 00100000 "Reserved"
mtd4: 01000000 00100000 "Kernel"
mtd5: 0a000000 00100000 "RFS"
mtd6: f4c00000 00100000 "Bulk"
EOF
python3 - <<'PY'
for n, size in ((0, 0x7e000), (1, 0x1000), (2, 0x1000)):
    with open(f"dev/mtd{n}", "wb") as f:
        f.truncate(size)          # sparse; do not materialise RFS/Bulk
PY

echo "==> /LF/Bulk content"
# BaseUtils::CreateFile recurses to create missing parents but only ever
# retries mkdir("/LF"), so a missing deep path loops ~175k times until the
# stack blows. On hardware these already exist. Create them.
mkdir -p LF/Bulk/Data/Uploads/0 LF/Bulk/Data/Downloads LF/Bulk/Data/Settings
# Per-profile save area. Mirrors /LF/Bulk/Data on a live device: profiles 0-3
# plus All. Flash titles write <profile>/saveGame.xml here and fail hard
# without it (CAtomicFile::CAtomicFile(..., w): failed).
# Per-profile UI state. The live device has, for each profile:
#   Data/Local/<n>/PAD2-0x1F1E0002-100000/{ProgramFileAppOrder.json,UIData.json}
# AppManager logs "CJSonFile::Load() failed" for both when they are absent.
# Seed ProgramFileAppOrder.json from the base defaults so the home screen has
# an app list to work from.
UIPKG=PAD2-0x1F1E0002-100000
for prof in 0 1 2 3 All; do
    mkdir -p "LF/Bulk/Data/Local/$prof/$UIPKG"
    # UIData.json — verbatim shape from the working device
    # (reference/device-capture/leappad2-working/02-uidata.txt). AppManager
    # logs "CJSonFile::Load() failed" for it when absent.
    if [ ! -f "LF/Bulk/Data/Local/$prof/$UIPKG/UIData.json" ]; then
        printf '%s\n' '{"BadgeNumber": 0,"HasProfileBeenViewed": true,"_bookPickerWasLaunchedAtleastOnce": true,"_connectAlreadyPlayed": false}' \
            > "LF/Bulk/Data/Local/$prof/$UIPKG/UIData.json"
    fi
    # NOTE: do NOT seed ProgramFileAppOrder.json from the base defaults. The
    # working device's per-profile copy holds only TWO entries
    #   {"PAD2-0x001E0010-000000": 60, "PAD2-0x001E0013-000000": 40}
    # i.e. SneakPeekWidget and My Books — the same two tiles our home screen
    # already shows. It records user ORDERING, not the full app list, so
    # copying the 8-entry base file over it is wrong.
done
mkdir -p LF/Bulk/Data/Uploads/1 LF/Bulk/Data/Uploads/2 LF/Bulk/Data/Uploads/3
: > tmp/bulk_ready                 # rcS's "Bulk is mounted" flag

# Runtime files captured from a live booted device (reference/device-capture/
# 05-runtime-files.txt). Contents matter: bulk_ready is "1", not empty.
printf '1\n'                        > tmp/bulk_ready
printf '0'                          > tmp/splash
: > tmp/initial
printf '7, CARTRIDGE_STATE_REINSERT' > tmp/cart_brio_state
printf '7'                          > flags/volume
printf '1357015435'                 > flags/lasttime
# tslib calibration. The DEVICE's values map raw ADC readings to screen
# coordinates: "39032 -162 -4245764 28 20690 -1583256 65536". Our viewer
# already sends screen coordinates, so we want the IDENTITY transform
# instead — tslib's linear module computes
#     x' = (a0 + a1*x + a2*y) / a6
# so a = 0,65536,0, 0,0,65536, 65536 passes input through unchanged.
printf '0 65536 0 0 0 65536 65536\n' > flags/pointercal
# /flags/developer is what enables telnetd+vsftpd on real hardware; harmless
# here and some code paths check it.
: > flags/developer

# /flags/poweron STOPS THE DEVICE SWITCHING ITSELF OFF.
#
# libLightningBase reads it beside /tmp/shutdown and /flags/apprelaunch — the
# inactivity machinery — and without it the emulator powers down after about a
# minute of no input, which on a desktop reads as "it crashed" rather than "a
# battery-powered toy saved its battery". MfgTest/ResetUnit.sh deletes it as
# part of a factory reset, which is the tell that it is a persistent flag like
# /flags/developer above rather than something set at runtime.
: > flags/poweron

# Touchscreen tuning, mirroring /flags/set-ts.sh on the device
mkdir -p sys/devices/platform/lf2000-touchscreen
printf '23'    > sys/devices/platform/lf2000-touchscreen/max_tnt_down
printf '521'   > sys/devices/platform/lf2000-touchscreen/min_tnt_up
printf '5'     > sys/devices/platform/lf2000-touchscreen/max_delta_tnt
printf '0'     > sys/devices/platform/lf2000-touchscreen/tnt_mode
printf '%s' '-1' > sys/devices/platform/lf2000-touchscreen/averaging

# CriticalDoom.json (LF/Base/LpadAssets_en/Data/) lists ten folders that must
# exist or HasDoomPackageCritical reports doom. We have the two LanguagePacks
# from the LFConnect cache; the ProgramFiles widgets and two Downloads live on
# the Bulk partition, which we do not have a dump of yet.
if [ -d "$CACHE" ]; then
    for f in "$CACHE"/*.lf2; do
        [ -e "$f" ] || continue
        id=$(bzcat "$f" | tar xO --wildcards '*meta.inf' 2>/dev/null |
             grep -oE 'PackageID="[^"]+"' | cut -d'"' -f2 || true)
        type=$(bzcat "$f" | tar xO --wildcards '*meta.inf' 2>/dev/null |
             grep -oE 'Type="[^"]+"' | cut -d'"' -f2 || true)
        case "$type" in
            LanguagePack) bzcat "$f" | tar x -C LF/Bulk 2>/dev/null || true ;;
            Download|MicroDownload)
                mkdir -p "LF/Bulk/Downloads/$id"
                bzcat "$f" | tar x -C "LF/Bulk/Downloads/$id" 2>/dev/null || true ;;
        esac
    done
fi
mkdir -p LF/Bulk/ProgramFiles/{KeyboardWidget,CameraWidget,PhotoEditor,SneakPeekWidget}
mkdir -p LF/Bulk/Downloads/PAD2-0x00210008-200000 LF/Bulk/Downloads/PADS-0x1F1E0002-300000


# ---- runtime/libs: the guest's own shared libraries ------------------------
#
# LD_LIBRARY_PATH (see tadpole.sh) is shimlibs-z:shimlibs:libs — the first two
# are ours, and this is where the GUEST's libraries have to be found. The
# emulator cannot start without it:
#
#     AppManager: can't load library 'libVideoMPI.so'
#
# It was a hand-made directory of symlinks that nothing regenerated, so it
# existed only in the tree it was built in. A fresh install — the AppImage, or
# any clone — had an empty libs/ and could never boot, with an error that points
# at a library rather than at the missing directory.
#
# Symlinks rather than copies: the rootfs is the single source of truth, and
# 168 duplicated files would drift.
echo "==> runtime/libs"
LIBDIR="$PROJ/runtime/libs"
mkdir -p "$LIBDIR"
find "$LIBDIR" -maxdepth 1 -type l -delete 2>/dev/null
nlib=0
for d in /lib /usr/lib /LF/Base/lib /LF/Base/Brio/lib /LF/Base/Flash/lib; do
    [ -d "$ROOTFS$d" ] || continue
    for so in "$ROOTFS$d"/*.so*; do
        [ -e "$so" ] || continue
        lns "$so" "$LIBDIR/$(basename "$so")"
        nlib=$((nlib+1))
    done
done
echo "    $nlib libraries linked"

echo
echo "sysroot ready: $SYSROOT"
echo "  LF/Bulk:  $(ls LF/Bulk | tr '\n' ' ')"
echo "  run with: $HERE/run.sh"
