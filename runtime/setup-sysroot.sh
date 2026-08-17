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
# AN ABSOLUTE SYMLINK IS THE SECOND REASON A DIRECTORY CANNOT BE SYMLINKED.
#
# Same trap as rootfs_target() above, one level up: a link whose target starts
# with / resolves against the HOST's root, so through a symlinked directory the
# host sees it dangling and every stat() on it fails. For a library that is a
# confusing error; for /LF/Base/Qt/bin it is fatal and silent.
#
# ALL EIGHT ENTRIES IN /LF/Base/Qt/bin ARE ABSOLUTE SYMLINKS — BrioWrapper,
# SignIn, ParentPicker, InitialSetup, TimeLocked, FirmwareUpdateOobe,
# MainPicker, AppServer — each pointing at /LF/Base/Qt/Modules/<n>/<n>. That
# directory is on the guest's PATH, and AppServer launches its screens BY BARE
# NAME through it. So with the directory symlinked, QProcess could not start a
# single one, and it presented as three separate faults: no app would launch,
# the sign-in screen never appeared, and parent settings were unreachable.
# What it logs is "Application: 'BrioWrapper' crashed, exit=0, error=0" —
# error=0 being QProcess::FailedToStart, i.e. it never ran at all.
dir_has_abs_link() {
    local l
    while IFS= read -r l; do
        case "$(readlink "$l")" in /*) return 0 ;; esac
    done < <(find "$1" -type l 2>/dev/null)
    return 1
}

shadow_meta() {
    local src="$1" dst="$2" e b
    mkdir -p "$dst"
    for e in "$src"/*; do
        [ -e "$e" ] || [ -L "$e" ] || continue
        b="$(basename "$e")"
        if [ "$b" = meta.inf ]; then
            cp -f "$e" "$dst/$b"
        elif [ -d "$e" ] && { [ -n "$(find "$e" -name meta.inf -print -quit 2>/dev/null)" ] ||
                              dir_has_abs_link "$e"; }; then
            [ -L "$dst/$b" ] && rm -f "$dst/$b"
            shadow_meta "$e" "$dst/$b"
        else
            # rootfs_target re-roots an absolute link target under $ROOTFS;
            # a relative one is already correct and passes through unchanged.
            lns "$(rootfs_target "$e")" "$dst/$b"
        fi
    done
}
# HOW "LINK" IS SPELLED HERE. On Linux it is a symlink, as it always was. On
# MSYS it is a COPY: MSYS's default `ln -s` writes WSL-style reparse points
# (tag 0xA000001D) that native Win32 programs cannot follow — glasspole.exe
# gets ENOENT straight through them — and true NTFS symlinks need a privilege
# an ordinary session does not hold. Copying costs ~100 MB of disk and keeps
# every property that matters: rootfs/ stays pristine, and the sysroot is
# readable by native code. The [ -L ] re-run guards below stay correct either
# way — a copy is not a link, so they simply never fire on MSYS.
case "$(uname -s)" in
    MSYS*|MINGW*) lns() {
        # A dangling link is legal; a missing copy SOURCE under set -e would
        # abort the whole script (linuxrc, whose target never materialises on
        # Windows, did exactly that). Note it and carry on.
        [ -e "$1" ] || { echo "    (no $1 — skipped)"; return 0; }
        rm -rf "$2"; cp -a "$1" "$2"
    } ;;
    *)            lns() { ln -sfn "$1" "$2"; } ;;
esac

echo "==> sysroot skeleton"
mkdir -p "$SYSROOT"
cd "$SYSROOT"
# read-only parts of the stock image
for d in bin boot etc lib linuxrc mnt sbin erootfs.md5; do
    lns "$ROOTFS/$d" "$d"
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
    [ "$b" = lib ] || lns "$d" "usr/$b"
done
mkdir -p usr/lib
for src in "$ROOTFS"/usr/lib "$ROOTFS"/lib; do
    for f in "$src"/*; do
        [ -e "$f" ] || continue
        lns "$(rootfs_target "$f")" "usr/lib/$(basename "$f")" 2>/dev/null || true
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
#
# THIS IS NOW ONLY THE FALLBACK, AND IT IS WORTH KNOWING WHICH ONE YOU HAVE.
# shim/tadpole_asound.c replaces libasound.so.2 outright and pipes the PCM to
# the viewer, so when it is on the guest's path this file is never even parsed
# — our snd_pcm_open ignores the device name it is handed. When it is NOT, the
# real alsa-lib reads this, instantiates `type null`, and every sample is
# discarded while every call still returns success. The two are impossible to
# tell apart from the guest's own log, which says
#     set_hwparams: sample rate=32000 ... CallbackThread: starting audio thread
# either way. The thing that distinguishes them is $TADPOLE_DIR/audio.fmt: only
# our shim writes it. That is the first thing to check when there is no sound.
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

# ROOT CERTIFICATES FROM 2016 CANNOT VERIFY THE WEB OF 2026.
#
# The guest ALREADY HAS WORKING INTERNET and this is easy to miss: qemu-user
# passes socket calls to the host, so the guest shares the host's stack. DNS
# resolves, TCP connects, and a fetch of a real CDN object returns HTTP 200 in
# about a second — measured with `curl -k`.
#
# WITHOUT -k IT FAILS, and only at certificate verification. The image ships
# 158 roots frozen on 2016-03-22, including DigiCert_Global_Root_CA — but not
# DigiCert Global Root G2, which is what LeapFrog's own chain is rooted at
# today. tools/netssl.py exists for exactly this problem on the HOST side (an
# un-updated Windows 7 has the same gap); this is the guest's half of it.
#
# So overlay the bundle with the host's, which a maintained desktop keeps
# current. The certificates a machine trusts are the machine's own business,
# and copying them into the guest gives it the same view rather than a
# hand-picked one — no cert is added that this host does not already trust.
#
# etc/ssl is a symlink at the rootfs, so the certs directory has to be shadowed
# the same way /etc itself is: real directory, entries symlinked, the one file
# we replace written on top.
for hostbundle in /etc/ssl/certs/ca-certificates.crt \
                  /etc/pki/tls/certs/ca-bundle.crt \
                  /etc/ssl/cert.pem; do
    [ -r "$hostbundle" ] && break
    hostbundle=""
done
if [ -n "${hostbundle:-}" ] && [ -d "$ROOTFS/etc/ssl/certs" ]; then
    rm -f etc/ssl
    mkdir -p etc/ssl/certs
    for f in "$ROOTFS"/etc/ssl/*; do
        b="$(basename "$f")"
        [ "$b" = certs ] || lns "$f" "etc/ssl/$b"
    done
    for f in "$ROOTFS"/etc/ssl/certs/*; do
        b="$(basename "$f")"
        [ "$b" = ca-certificates.crt ] || lns "$f" "etc/ssl/certs/$b"
    done
    cp -f "$hostbundle" etc/ssl/certs/ca-certificates.crt
    echo "    CA bundle from $hostbundle (the image's own is from 2016)"
else
    echo "    note: no host CA bundle found — guest HTTPS will fail to verify" >&2
fi

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
# /var/run IS A tmpfs ON THE DEVICE, so it must be REAL and WRITABLE here.
#
# fstab says so outright:
#     /dev/ram0   /var/run   tmpfs   defaults,mode=755
# and symlinking it at the rootfs makes it read-only, which stops the system
# D-Bus daemon dead: it creates /var/run/dbus/system_bus_socket and a pidfile
# there, and cannot. The symptom is a long way from the cause — the network
# stack asks QDBusConnection::systemBus() for net.connman, gets nothing, logs
# "Can't get: wifi technology is NULL", and the out-of-box WiFi page loops
# because it can never resolve its own state.
# /var/lib HAS TO BE REAL TOO, for the same reason one level along: connman
# creates /var/lib/connman for its stored networks and cannot through a
# symlink into a read-only rootfs. It says
#     Failed to create storage directory: No such file or directory
# and then runs anyway with nothing remembered, which is survivable but not
# what the device does.
for d in "$ROOTFS"/var/*; do
    b="$(basename "$d")"
    case "$b" in sounds|run|lib) continue ;; esac
    lns "$d" "var/$b"
done
mkdir -p var/lib
for f in "$ROOTFS"/var/lib/*; do
    [ -e "$f" ] || continue
    lns "$f" "var/lib/$(basename "$f")" 2>/dev/null || true
done
mkdir -p var/run/dbus var/lib/dbus var/lib/connman
mkdir -p var/sounds
for f in "$ROOTFS"/var/sounds/*; do
    lns "$f" "var/sounds/$(basename "$f")" 2>/dev/null || true
done
# DEV_SOUNDS is "<name in /var/sounds> <path under the rootfs>", one per line,
# transcribed from this device's branch of rcS. Do not be tempted to derive it:
# on RIO the source names do not match the destinations (boot.ogg becomes
# StartupVideo.ogg), so only the firmware's own table is right.
printf '%s\n' "$DEV_SOUNDS" | while read -r name src; do
    [ -n "$name" ] || continue
    if [ -e "$ROOTFS/$src" ]; then
        lns "$ROOTFS/$src" "var/sounds/$name"
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
# A COMMAND NODE, NOT A STATE NODE. Writing 1 here is how the guest powers the
# device off, and it does so every time the idle timeout expires. On hardware
# that value dies with the kernel; here it is a file, and the next boot obeys
# it. Created 0 so it exists, and reset to 0 by tadpole.sh on every boot —
# see the note there, which is where the bug actually bites.
printf '0'                         > "sys/devices/platform/$DEV_POWER_DEV/shutdown"
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
# connman will not start without its storage directory, and rcS makes it:
#     mkdir -p /LF/Bulk/Data/var/lib/connman
# Without it: "Failed to create storage directory: No such file or directory".
mkdir -p LF/Bulk/Data/var/lib/connman
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
# already sends screen coordinates, so we want the IDENTITY transform instead.
#
# THE OFFSET IS THE THIRD FIELD, NOT THE FIRST, and getting that backwards is
# not a small error — it sends every touch to the same place. tslib's linear
# module computes
#
#     x' = (a[2] + a[0]*x + a[1]*y) / a[6]
#     y' = (a[5] + a[3]*x + a[4]*y) / a[6]
#
# so the file reads (xx, xy, xoff, yx, yy, yoff, scale). This was written as
# `0 65536 0 0 0 65536 65536` on a reading of the formula as
# `x' = (a0 + a1*x + a2*y)/a6` — offset first — and that transform is
#
#     x' = (0 + 0*x + 65536*y)/65536 = y
#     y' = (65536 + 0*x + 0*y)/65536 = 1
#
# Every tap anywhere on the screen arrived at y=1, with x carrying the tap's
# Y coordinate: the top edge, always. It presents as "touch lands in the wrong
# place", and every other part of the chain — the viewer's mapping, the shim,
# the event stream, tslib's own filters — measures as CORRECT while it happens,
# because they all are. The device's own numbers were the check that settles
# it: 39032 is a scale and -4245764 an offset, and they sit in positions 0
# and 2.
printf '65536 0 0 0 65536 0 65536\n' > flags/pointercal
# /flags/developer is what enables telnetd+vsftpd on real hardware; harmless
# here and some code paths check it.
: > flags/developer

# /flags/skip_oobe — GO STRAIGHT TO SIGN-IN, PAST THE OUT-OF-BOX WIZARD.
#
# The firmware's own escape hatch: the name is in libQtAppServer's strings
# beside FirmwareUpdateOobe, "complete" and "firmware-installed". Without it
# AppServer starts InitialSetup, and the second step of that is WiFi — which
# on a machine with no wireless hardware cannot be satisfied and cannot be
# skipped, so setup loops there and the device can never be used at all.
#
# With it, AppServer launches SignIn: the profile picker, with the parent and
# time-control buttons that were previously unreachable. Delete this file to
# walk the real out-of-box flow.
: > flags/skip_oobe

# /flags/poweron STOPS THE DEVICE SWITCHING ITSELF OFF.
#
# libLightningBase reads it beside /tmp/shutdown and /flags/apprelaunch — the
# inactivity machinery — and without it the emulator powers down after about a
# minute of no input, which on a desktop reads as "it crashed" rather than "a
# battery-powered toy saved its battery". MfgTest/ResetUnit.sh deletes it as
# part of a factory reset, which is the tell that it is a persistent flag like
# /flags/developer above rather than something set at runtime.
#
# It is NOT the same mechanism as /flags/idle_timeouts below: this one is
# Brio's, that one is the Qt shell's, and a Qt device needs both.
: > flags/poweron

# THE DEVICE POWERS ITSELF OFF AFTER FOUR MINUTES, AND SAYS SO IN PASSING.
#
#     [IdleTimeout] Timer intervals set to: 1st interval 120 seconds,
#                                           2nd interval 120 seconds.
#     [IdleTimeout] Second timeout happened... power off!
#
# Correct on a battery-powered tablet a child put down. Hostile in an emulator,
# where a cold boot under qemu already spends about a minute of the first 120
# seconds getting to the home screen — so the window for doing anything at all
# is short, and a session that is being watched rather than touched dies on its
# own. Worse, it dies through AppServer's shutdown path, which without
# VideoDaemon segfaults on the way out and reads like a crash rather than a
# deliberate power-off.
#
# This is not a patch: it is the firmware's own override, found in
# libQtAppServer's strings beside the message above —
#
#     [IdleTimeout] GetAlternativeIntervalValues(), read %s      /flags/idle_timeouts
#     [IdleTimeout] GetAlternativeIntervalValues() values: %d %d
#     [IdleTimeout] GetAlternativeIntervalValues() Disabling based on 0 values
#
# ONE VALUE PER LINE, AND DO NOT ASK FOR ZERO. Both of those were measured
# rather than read off the strings, and each looked like the obvious answer
# first:
#
#   * `0 0` on ONE line logs `values: 0 60000` — the first field parsed, the
#     second did not and kept an internal default. So it reads a line at a
#     time, not two numbers from one line.
#   * Zero does reach the branch that prints "Disabling", and then the device
#     powers off IMMEDIATELY: disable() is followed by the shutdown movie in
#     the next log line. Whatever that branch means, it is not "never time
#     out", and asking for it turns a four-minute reprieve into none at all.
#
# So: a day, twice, which no session will reach. Delete this file to get the
# hardware's own two minutes back.
printf '86400\n86400\n'             > flags/idle_timeouts

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
        lns "$(rootfs_target "$so")" "$LIBDIR/$(basename "$so")"
        nlib=$((nlib+1))
    done
done
echo "    $nlib libraries linked"

echo
echo "sysroot ready: $SYSROOT"
echo "  LF/Bulk:  $(ls LF/Bulk | tr '\n' ' ')"
echo "  run with: $HERE/run.sh"
