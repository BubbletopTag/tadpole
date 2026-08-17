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
#
# emmc_rfs IS A THIRD LAYOUT, AND NOT COSMETIC. The LeapPad2 and the Ultra are
# NAND devices: their Firmware-Base carries a UBI volume, which has to be
# unpacked with ubireader. The LeapPad3 is eMMC — its package is
# `Type="DiskImage"` and holds firmware/emmc/2ext4/3/RFS, which despite living
# under a path naming a filesystem is a plain GNU tar of the root. So there is
# no UBI step for it at all, and calling the result ubi_rfs would be a lie that
# the next person has to disprove.
ROOTFS=""
for cand in "$PROJ"/rootfs/*/emmc_rfs "$PROJ"/rootfs/*/ubi_rfs "$PROJ"/rootfs/*/*/ubi_rfs; do
    [ -d "$cand" ] || continue
    ROOTFS="$cand"; break
done
: "${ROOTFS:=$PROJ/rootfs/MISSING/ubi_rfs}"
SYSROOT="$HERE/sysroot"
CACHE="$PROJ/sources/nxp320/LFC_full/LFC_Downloads/cache"

[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 1; }

# WHICH DEVICE. Everything below used to be LeapPad2 constants inline. They now
# come from runtime/devices/<id>.conf, chosen by reading the installed
# firmware's own Firmware/meta.inf — see runtime/device.sh.
. "$HERE/device.sh"
tad_load_device
echo "==> device: $DEV_NAME ($DEV_ID, $DEV_LCD) — $(basename "$DEV_CONF")"

# AN ABSOLUTE SYMLINK INSIDE THE ROOTFS RESOLVES ON THE HOST, NOT IN IT.
#
# The Ultra ships usr/lib/libustring.so as a symlink to the absolute path
# /usr/lib/libglibmm-2.4.so.1 — correct on the device, where that IS the
# guest's glibmm. Symlink runtime/libs/libustring.so straight at it and the
# chain ends at the DEVELOPER'S /usr/lib/libglibmm-2.4.so.1, so the guest is
# handed an x86-64 object and says something that sounds like a build problem:
#
#     VideoDaemon: '/usr/lib/libustring.so' is not an ELF executable for ARM
#
# Re-root any absolute link target under $ROOTFS. Relative links need no help:
# they resolve inside the rootfs on their own.
rootfs_target() {
    local p="$1" t
    if [ -L "$p" ]; then
        t="$(readlink "$p")"
        case "$t" in
            /*) [ -e "$ROOTFS$t" ] && { printf '%s' "$ROOTFS$t"; return; } ;;
        esac
    fi
    printf '%s' "$p"
}

# meta.inf IS NOT READ-ONLY, AND SYMLINKING IT CORRUPTS THE ROOTFS.
#
# Everything below assumes the stock image is read-only, so it is symlinked
# rather than copied. That held until a device with a package-manager daemon:
# its RebuildPackageDatabase walks every installed package, parses each
# meta.inf — and WRITES EACH ONE BACK normalised. Through a symlink, that lands
# in rootfs/, which is supposed to be pristine.
#
# The damage is quiet and it is not cosmetic. The rewrite drops fields the
# daemon does not care about and appends its own:
#
#     -Device="LeapPad3"          +Device=""          +Size=0
#
# and `Device=` is exactly what runtime/device.sh matches to decide WHICH
# DEVICE THIS IS. So the first boot of a LeapPad3 erased the evidence that it
# was a LeapPad3, and the second one detected a LeapPad2, booted AppManager
# against a Qt firmware, and failed in a way that had nothing to do with the
# actual cause. 52 files on this image, 41 of them under Firmware/.
#
# Mirror the directory instead: a real directory wherever a meta.inf lives,
# with a COPY of the meta.inf and a symlink for every other entry. That is 52
# small files rather than the 93 MB copy of LF/Base that copying outright would
# cost, and the guest's writes land in the sysroot where they belong.
shadow_meta() {
    local src="$1" dst="$2" e b
    mkdir -p "$dst"
    for e in "$src"/*; do
        [ -e "$e" ] || continue
        b="$(basename "$e")"
        if [ "$b" = meta.inf ]; then
            cp -f "$e" "$dst/$b"
        elif [ -d "$e" ] && [ -n "$(find "$e" -name meta.inf -print -quit 2>/dev/null)" ]; then
            [ -L "$dst/$b" ] && rm -f "$dst/$b"
            shadow_meta "$e" "$dst/$b"
        else
            ln -sfn "$e" "$dst/$b"
        fi
    done
}

echo "==> sysroot skeleton"
mkdir -p "$SYSROOT"
cd "$SYSROOT"
# read-only parts of the stock image
for d in bin boot etc lib linuxrc mnt sbin erootfs.md5; do
    ln -sfn "$ROOTFS/$d" "$d"
done
# Firmware/ is nothing BUT meta.inf files, and the daemon rewrites all of them.
[ -L Firmware ] && rm -f Firmware
shadow_meta "$ROOTFS/Firmware" Firmware
# /LF must be real: Bulk and Cart are written to at runtime.
# Only unlink if it is still the symlink from a previous layout — removing a
# real directory here would wipe installed Bulk content, and `rm -f` on a
# directory fails, which under `set -e` silently aborts the whole script.
[ -L LF ] && rm -f LF
mkdir -p LF/Bulk LF/Cart
[ -L LF/Base ] && rm -f LF/Base
shadow_meta "$ROOTFS/LF/Base" LF/Base
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
    [ "$b" = lib ] || ln -sfn "$d" "usr/$b"
done
mkdir -p usr/lib
for src in "$ROOTFS"/usr/lib "$ROOTFS"/lib; do
    for f in "$src"/*; do
        [ -e "$f" ] || continue
        ln -sfn "$(rootfs_target "$f")" "usr/lib/$(basename "$f")" 2>/dev/null || true
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
#
# NOT ON A Qt DEVICE. The paragraph above is about AppManager, which reaches
# the shim THROUGH libdl — so there, one libdl and it must be ours. The Ultra
# reaches it through libEGL instead, and pointing libdl at the shim as well
# would put a SECOND copy in the process the moment anything resolves libdl by
# absolute path. Two shims chain into each other and recurse until the stack
# is gone (see the note in tadpole.sh). Leave the real libdl alone here.
SHIM_DL="$PROJ/runtime/shimlibs/libdl.so.0"
if [ "${DEV_HAS_QT:-0}" = 1 ]; then
    echo "    libdl left as the device's own ($DEV_NAME injects via libEGL)"
elif [ -e "$SHIM_DL" ]; then
    ln -sfn "$SHIM_DL" lib/libdl.so.0
    ln -sfn "$SHIM_DL" usr/lib/libdl.so.0
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
    [ "$b" = asound.conf ] || ln -sfn "$f" "etc/$b"
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
#
# The asset directory is per-device ($DEV_ASSETS): VALENCIA and MADRID use
# LpadAssets, RIO (the Ultra) keeps its boot and shutdown audio in LF/Base/Qt
# and only borrows TransitionVideo.ogg from LpadAssets. Both layouts are just
# "look in $DEV_ASSETS first, fall back to LpadAssets", which is what the loop
# below does.
[ -L var ] && rm -f var
mkdir -p var
for d in "$ROOTFS"/var/*; do
    b="$(basename "$d")"
    [ "$b" = sounds ] || ln -sfn "$d" "var/$b"
done
mkdir -p var/sounds
for f in "$ROOTFS"/var/sounds/*; do
    ln -sfn "$f" "var/sounds/$(basename "$f")" 2>/dev/null || true
done
# DEV_SOUNDS is "<name in /var/sounds> <path under the rootfs>", one per line,
# transcribed from this device's branch of rcS. Do not be tempted to derive it:
# on RIO the source names do not match the destinations (boot.ogg becomes
# StartupVideo.ogg), so only the firmware's own table is right.
printf '%s\n' "$DEV_SOUNDS" | while read -r name src; do
    [ -n "$name" ] || continue
    if [ -e "$ROOTFS/$src" ]; then
        ln -sfn "$ROOTFS/$src" "var/sounds/$name"
    else
        echo "    note: no $src (var/sounds/$name will be absent)" >&2
    fi
done

echo "==> sysfs (rcS branches on platform; libDisplay reads lcd_size)"
mkdir -p sys/devices/system/board \
         "sys/devices/platform/$DEV_POWER_DEV" \
         "sys/devices/platform/$DEV_ACLMTR_DEV" \
         "sys/devices/platform/$DEV_GPIO_DEV" \
         "sys/devices/platform/$DEV_DPC_DEV" \
         sys/class/graphics/fb0
# For LeapPad2 all of these were read off a live device —
# reference/device-capture/. For other devices see the UNVERIFIED markers in
# the profile.
printf '%s' "$DEV_PLATFORM"        > sys/devices/system/board/platform
printf '%s' "$DEV_PLATFORM_FAMILY" > sys/devices/system/board/platform_family
printf '%s' "$DEV_SYSTEM_REV"      > sys/devices/system/board/system_rev
printf '%s' "$DEV_LCD"             > sys/devices/system/board/lcd_size  # %ux%u
printf '%s' "$DEV_LCD_TYPE"        > sys/devices/system/board/lcd_type
printf '%s' "$DEV_LCD_MFG"         > sys/devices/system/board/lcd_mfg
printf '%s' "$DEV_LCD_MFG"         > sys/devices/system/board/lcd_mfg_get
printf '%s' "${DEV_LCD%x*}"        > "sys/devices/platform/$DEV_DPC_DEV/xres"
printf '%s' "${DEV_LCD#*x}"        > "sys/devices/platform/$DEV_DPC_DEV/yres"
printf '0'                         > "sys/devices/platform/$DEV_GPIO_DEV/board_id"
printf '1'                         > "sys/devices/platform/$DEV_POWER_DEV/status"  # 1 = EXTERNAL
printf '0'                         > sys/class/graphics/fb0/rotate

echo "==> device nodes"
# These must EXIST as directory entries or the guest stops enumerating after
# event1. The shim intercepts open() on them regardless of content.
# The required set is confirmed by usr/bin/make_dev_nodes.sh in the firmware.
for i in $(seq 0 24); do : > "dev/input/event$i"; done
# The device's udev makes this; nothing here does, and tslib opens it BY THIS
# NAME (TSLIB_TSDEVICE in /etc/profile). The shim maps it to event2, the node
# whose EVIOCGNAME is "touchscreen interface". A plain file is enough — the
# shim intercepts open() on it regardless of what is inside.
: > dev/input/touchscreen0
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
printf '%s\n' "$DEV_CODEC" > proc/asound/card0/id

echo "==> /proc/mtd + MfgData"
# CMfgData::Init parses /proc/mtd for a partition named MfgData0, then opens
# /dev/mtd<N>. Without this it fails to init and CMfgData::Read segfaults
# inside libc. With it, the locale lookup degrades gracefully to "en-us".
# Sizes/erasesizes are the documented LeapPad2 partition table.
{ echo "dev:    size   erasesize  name"; printf '%s\n' "$DEV_MTD"; } > proc/mtd
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
UIPKG="$DEV_UIPKG"
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

# Qt Embedded's server socket directory. AppServer runs with -qws, which makes
# it the QWS SERVER, and the first thing that does is bind a listening socket:
#
#     QWSServerSocket: could not bind to file /tmp/qtembedded-0/QtEmbedded-0
#     FATAL........: Failed to bind to /tmp/qtembedded-0/QtEmbedded-0
#
# On the device /tmp is a writable tmpfs and Qt creates this itself; here the
# parent has to exist first. 0700 is what Qt sets it to, and it checks.
if [ "${DEV_HAS_QT:-0}" = 1 ]; then
    mkdir -p tmp/qtembedded-0
    chmod 700 tmp/qtembedded-0
    # QWS_DATA_HOME / QWS_CACHE_HOME / TMPDIR out of the device's /etc/profile.
    mkdir -p LF/Bulk/Data/Local/All/qws/share LF/Bulk/Data/Local/All/qws/cache
fi

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

# Touchscreen tuning, mirroring /flags/set-ts.sh on the device.
# The Ultra's rcS also reads $DEV_TOUCH_DEV/firmware_version and logs it as
# "Neonode Version", so that node has to exist even though nothing acts on it.
mkdir -p "sys/devices/platform/$DEV_TOUCH_DEV"
printf '23'      > "sys/devices/platform/$DEV_TOUCH_DEV/max_tnt_down"
printf '521'     > "sys/devices/platform/$DEV_TOUCH_DEV/min_tnt_up"
printf '5'       > "sys/devices/platform/$DEV_TOUCH_DEV/max_delta_tnt"
printf '0'       > "sys/devices/platform/$DEV_TOUCH_DEV/tnt_mode"
printf '%s' '-1' > "sys/devices/platform/$DEV_TOUCH_DEV/averaging"
printf '0\n'     > "sys/devices/platform/$DEV_TOUCH_DEV/firmware_version"

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
# /LF/Base/Qt/lib is the Ultra's: 40-odd libQtApp*.so that only its shell
# links. It is simply absent on a LeapPad2, so listing it costs nothing there.
for d in /lib /usr/lib /LF/Base/lib /LF/Base/Brio/lib /LF/Base/Flash/lib \
         /LF/Base/Qt/lib; do
    [ -d "$ROOTFS$d" ] || continue
    for so in "$ROOTFS$d"/*.so*; do
        [ -e "$so" ] || continue
        ln -sfn "$(rootfs_target "$so")" "$LIBDIR/$(basename "$so")"
        nlib=$((nlib+1))
    done
done
echo "    $nlib libraries linked"

echo
echo "sysroot ready: $SYSROOT"
echo "  LF/Bulk:  $(ls LF/Bulk | tr '\n' ' ')"
echo "  run with: $HERE/run.sh"
