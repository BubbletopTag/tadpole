#!/bin/bash
# Tadpole — install LFConnect content packages into the emulated /LF/Bulk.
#
# LeapFrog ships content as .lfp (ZIP) and .lf2 (bzip2 tar) with a meta.inf
# manifest inside. This mirrors what LFConnect does on-device.
#
# Install rules, derived from a live LeapPad2 (reference/device-capture/):
#
#   Type=Application            -> /LF/Bulk/ProgramFiles/<PackageID>/
#   Type=Download|MicroDownload -> /LF/Bulk/Downloads/<PackageID>/
#   Type=LanguagePack           -> /LF/Bulk/           (tarball self-wraps)
#   Type=Music|MusicInfo        -> /LF/Bulk/Music/<PackageID>/
#   Type=DeviceAsset            -> /LF/Bulk/Data/DeviceAssets/<PackageID>/
#   Type=DiskImage|System       -> skipped (firmware, not content)
#
# NOTE on naming: the real device has SOME packages installed under their
# Name (KeyboardWidget, CameraWidget, ...) and others under their PackageID
# (PAD2-0x001E0003-000006 = PaintWidget, PAD2-0x001E0013-000000 = My Books).
# The meta.inf fields do not distinguish the two cases, so this looks
# historical rather than rule-driven. AppManager discovers packages by
# scanning and reading meta.inf, so PackageID is used uniformly here — the
# device itself demonstrates that form is accepted.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
BULK="${1:-$PROJ/runtime/sysroot/LF/Bulk}"
CACHE="${2:-$PROJ/sources/nxp320/LFC_full/LFC_Downloads/cache}"

[ -d "$CACHE" ] || { echo "no cache at $CACHE" >&2; exit 1; }
mkdir -p "$BULK"

# unzip and bzcat when the machine has them, tools/pkgtool.py (Python stdlib)
# when it does not — the AppImage's bundle carries a Python and no unzip. See
# tools/lib-deps.sh.
. "$HERE/lib-deps.sh"
PY="$(tad_python || true)"
pytool() { [ -n "$PY" ] && "$PY" "$HERE/pkgtool.py" "$@" 2>/dev/null; }

read_meta() {                      # $1=archive -> meta.inf on stdout
    case "$1" in
        *.lfp) command -v unzip >/dev/null && { unzip -p "$1" '*meta.inf' 2>/dev/null; return; } ;;
        *.lf2) command -v bzcat >/dev/null && { bzcat "$1" 2>/dev/null | tar xO --wildcards '*meta.inf' 2>/dev/null; return; } ;;
    esac
    pytool meta "$1"
}
list_archive() {                   # $1=archive -> member paths on stdout
    case "$1" in
        *.lfp) command -v unzip >/dev/null && { unzip -Z1 "$1" 2>/dev/null; return; } ;;
        *.lf2) command -v bzcat >/dev/null && { bzcat "$1" 2>/dev/null | tar tf - 2>/dev/null; return; } ;;
    esac
    pytool list "$1"
}

# Some packages are FLAT (meta.inf at the top) and some SELF-WRAP in a single
# directory of their own — the widgets do, the LanguagePacks do. Extracting a
# self-wrapping archive into a named directory buries meta.inf one level down,
# and CSystemData::FindWidget checks FileExists(<entry>/meta.inf), so the
# widget becomes invisible. Detect the wrapper and extract to the PARENT so it
# names its own directory.
self_wraps() {                     # $1=archive
    top=$(list_archive "$1" | sed 's|/.*||' | sort -u | grep -v '^$')
    [ "$(echo "$top" | wc -l)" -eq 1 ] &&
        ! list_archive "$1" | grep -qx 'meta.inf'
}

extract_to() {                     # $1=archive $2=destdir
    mkdir -p "$2"
    case "$1" in
        *.lfp) command -v unzip >/dev/null && { unzip -qo "$1" -d "$2" 2>/dev/null; return; } ;;
        *.lf2) command -v bzcat >/dev/null && { bzcat "$1" 2>/dev/null | tar x -C "$2" 2>/dev/null; return; } ;;
    esac
    pytool extract "$1" "$2"
}

# Extract so that $2 ends up being the package directory itself, whether the
# archive is flat or self-wrapping.
extract_as() {                     # $1=archive $2=intended package dir
    if self_wraps "$1"; then
        mkdir -p "$(dirname "$2")"
        extract_to "$1" "$(dirname "$2")"
    else
        extract_to "$1" "$2"
    fi
}
field() { echo "$1" | grep -oE "$2=\"[^\"]*\"" | head -1 | cut -d'"' -f2; }

n_app=0; n_dl=0; n_lang=0; n_music=0; n_other=0; n_skip=0

# TWO PASSES. A DeviceAsset installs into its PARENT package's directory, so
# every Application must exist before any DeviceAsset is processed. lfpkg
# installs one package at a time and never hits this; a batch installer does.
for pass in 1 2; do
for f in "$CACHE"/*.lfp "$CACHE"/*.lf2; do
    [ -e "$f" ] || continue
    meta=$(read_meta "$f") || true
    [ -n "$meta" ] || { n_skip=$((n_skip+1)); continue; }

    type=$(field "$meta" Type)
    pid=$(field "$meta" PackageID)
    name=$(field "$meta" Name)
    [ -n "$pid" ] || pid="unknown-$(basename "$f" | cut -c1-8)"

    # pass 1: everything except DeviceAssets. pass 2: DeviceAssets only.
    if [ "$pass" = 1 ] && [ "$type" = DeviceAsset ]; then continue; fi
    if [ "$pass" = 2 ] && [ "$type" != DeviceAsset ]; then continue; fi

    case "$type" in
        Application)
            # EXACTLY what lfpkg does: extract_as picks the wrapper name for
            # self-wrapping archives and the PackageID for flat ones. Do NOT
            # also install under Name — that was an experiment while chasing
            # FindWidget and it installs every app TWICE, leaving duplicate
            # meta.inf files that confuse anything enumerating ProgramFiles.
            extract_as "$f" "$BULK/ProgramFiles/$pid";  n_app=$((n_app+1)) ;;
        Download|MicroDownload)
            extract_to "$f" "$BULK/Downloads/$pid";     n_dl=$((n_dl+1)) ;;
        LanguagePack)
            extract_to "$f" "$BULK";                    n_lang=$((n_lang+1)) ;;
        Music|MusicInfo)
            extract_to "$f" "$BULK/Music/$pid";         n_music=$((n_music+1)) ;;
        DeviceAsset)
            # DeviceAssets carry an app's home-screen icon and GameInfo.json,
            # and lfpkg installs them INTO THE PARENT PACKAGE'S DIRECTORY, not
            # a directory of their own. Its rule (usr/bin/lfpkg):
            #     PRODUCT_ID    = field 2 of the DA PackageID
            #     DA_PRODUCT_ID = PRODUCT_ID-<field 3 with -DA- -> -00->
            #     parent        = dirname of the meta.inf containing that
            # e.g. PADS-0x001F0006-DA0000 -> 0x001F0006-000000 -> the
            # "Camera & Video Recorder" package. Without this the app is
            # installed but has no icon, so the home screen never shows it.
            da_prod=$(echo "$pid" | cut -d- -f2)
            da_seq=$(echo "$pid" | sed 's:-DA:-00:' | cut -d- -f3)
            da_key="$da_prod-$da_seq"
            parent=$(find "$BULK" -name meta.inf -exec grep -l "$da_key" {} + 2>/dev/null | head -1)
            if [ -n "$parent" ]; then
                extract_as "$f" "$(dirname "$parent")"
                extract_to "$f" "$(dirname "$parent")"
                n_other=$((n_other+1))
                printf "  %-14s %-26s -> %s\n" "$type" "$pid" \
                       "$(basename "$(dirname "$parent")")"
                continue
            fi
            echo "  DeviceAsset    $pid -> no parent for $da_key (skipped)" >&2
            n_skip=$((n_skip+1)); continue ;;
        DiskImage|System)
            n_skip=$((n_skip+1)); continue ;;
        *)
            extract_to "$f" "$BULK/Downloads/$pid";     n_other=$((n_other+1)) ;;
    esac
    printf "  %-14s %-26s %s\n" "$type" "$pid" "$name"
done
done

# ProfileAccess — WITHOUT THIS THE HOME SCREEN IS NEARLY EMPTY.
#
# The home picker (HomePickerState::GetAllIcons -> getProgramFileApps) does not
# show every installed Application. It walks the sort file
#     /LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json
# and, for each PackageID listed there, checks which PROFILES may see the app.
# ProfileAccess is that list; -1,0,1,2,3 means "everyone, all four slots".
#
# The LFConnect downloads omit it, so Camera, Gallery, Pet Pad and Music were
# installed but invisible. The factory Bulk partition has it — confirmed on the
# live device, reference/device-capture/leappad2-working/05-meta-inf.txt:
#
#     Size=0x02373000
#     DeviceAccess=0x00000000
#     ProfileAccess=-1,0,1,2,3
#
# Measured on a headless boot (tools/probe-home.sh), varying only Camera:
#
#     pristine                                        3 tiles
#     + Device="LeapPad2Explorer"                     3 tiles
#     + DeviceAccess=1                                4 tiles   <- MISLEADING
#     + DeviceAccess=0x00000000 (the device's value)  3 tiles
#     + ProfileAccess=-1,0,1,2,3 alone                7 tiles   <- the real fix
#
# DeviceAccess=1 was a FALSE POSITIVE: it made tiles appear, but the hardware
# has DeviceAccess=0x00000000 and still shows them, so it was never the filter.
# Checking the device's actual VALUE rather than just the key's presence is
# what exposed that. Both fields are written here to match the factory exactly;
# only ProfileAccess is load-bearing.
#
# Scoped deliberately to packages the sort file names — the picker never
# considers anything else, so there is no reason to touch it.
SORTFILE="$(dirname "$BULK")/Base/LpadAssets_en/Data/ProgramFileAppOrder.json"
if [ -f "$SORTFILE" ]; then
    n_da=0
    while read -r pid; do
        [ -n "$pid" ] || continue
        for m in "$BULK"/ProgramFiles/*/meta.inf; do
            [ -f "$m" ] || continue
            grep -q "PackageID=\"$pid\"" "$m" || continue
            grep -q '^ProfileAccess=' "$m" && continue
            grep -q '^DeviceAccess=' "$m" ||
                printf 'DeviceAccess=0x00000000\n' >> "$m"
            printf 'ProfileAccess=-1,0,1,2,3\n' >> "$m"
            n_da=$((n_da+1))
            echo "  ProfileAccess -> $(basename "$(dirname "$m")")"
        done
    done <<EOF
$(grep -oE '"[A-Z0-9]+-0x[0-9A-Fa-f]+-[0-9A-Za-z]+"' "$SORTFILE" | tr -d '"')
EOF
    [ "$n_da" = 0 ] && echo "  ProfileAccess: nothing to add"
else
    echo "  WARNING: no sort file at $SORTFILE — home screen may be empty" >&2
fi

# Runtime bits the firmware expects to already exist (see docs/device-deps.md:
# BaseUtils::CreateFile recurses forever on a missing deep path).
mkdir -p "$BULK/Data/Uploads/0" "$BULK/Data/Downloads" "$BULK/Data/Settings"

# THE PROFILE DIRECTORIES. THIS IS THE "MY PROFILE WILL NOT SAVE" BUG.
#
# Data/Local/<profile>/ is where a player's identity lives, and NOTHING creates
# it. The firmware does not ship it and the guest never calls mkdir for it — it
# simply writes into it and fails, silently as far as the UI is concerned:
#
#     PlayerProfilePlugin::setName, name = Tp          <- accepted, in memory
#     fopenAtomic(/LF/Bulk/Data/Local/0/./profile.dsc): mkstemp failed us!
#     CFileIO::Write() - failed opening .../profile.dsc for writing.
#
# So the name is taken, the home screen shows no name because there is nothing
# to read back, and the next boot returns to CREATE PROFILE. Reported by many
# people on fresh firmware, and "fixed" until now by transplanting an entire
# /LF/Bulk off real hardware — which worked only because a real device has
# these directories already.
#
# PAD2-0x1F1E0002-100000 is the second half of it: the per-profile UI store,
# where UIData.json holds the wallpaper choice. Without it the wallpaper reverts
# to the default on every boot, which is the other half of the same report.
#
# The remaining three match what a real device carries per profile, and are the
# system apps that keep per-player state. Verified end to end: with these in
# place, a name set in About Me survives a reboot and the home screen shows it.
for _p in 0 1 2 3 All; do
    mkdir -p "$BULK/Data/Local/$_p"/{PAD2-0x1F1E0002-100000,PAD2-0x001F0005-000000,PAD2-0x001E0010-000000,Pets,Photos}
done
# settings.cfg, verbatim shape from the live device
[ -e "$BULK/settings.cfg" ] || cat > "$BULK/settings.cfg" <<'EOF'
Locale=en-us
RTCAccuracy=0x00000001
CurrProfile=0x00000000
EOF

echo
echo "installed: $n_app apps, $n_dl downloads, $n_lang language packs,"
echo "           $n_music music, $n_other other, $n_skip skipped (firmware)"
echo "bulk: $BULK"
