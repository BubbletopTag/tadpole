# Tadpole — resolve which device we are emulating, and load its profile.
#
# Source this, do not run it. It sets the DEV_* variables documented in
# runtime/devices/*.conf, plus ROOTFS and DEV_CONF.
#
# WHY AUTODETECT RATHER THAN A SETTING. The device is a property of the
# firmware that is installed, not a preference: booting a LeapPad2 rootfs with
# the Ultra's 1024x600 sysfs values produces a working emulator showing a
# corrupt screen, which is a miserable thing to debug. Every firmware carries
# its own answer in Firmware/meta.inf, so read it there and let the explicit
# setting exist only to override.
#
# SEVERAL DEVICES CAN BE INSTALLED AT ONCE, and that changes what the setting
# means rather than removing it. See "THE SHAPE OF A MULTI-DEVICE INSTALL"
# below: the saved device now SELECTS which installed tree is live, and the
# firmware still decides what that tree IS. The two answers can no longer
# disagree, because the profile is always loaded for whatever is actually
# assembled — never for a preference.
#
# Precedence: $TADPOLE_DEVICE  >  ui.cfg device=  >  whatever is active now.
# A device that is not installed is ignored at every step, with a note.
#
# ---- THE SHAPE OF A MULTI-DEVICE INSTALL ---------------------------------
#
#   rootfs/<anything>/{emmc_rfs,ubi_rfs}   extracted firmware — already one per
#                                          device, and already able to coexist
#   runtime/sysroot                        the ACTIVE device's assembled tree
#   runtime/libs                           the ACTIVE device's guest libraries
#   runtime/installs/<devid>/sysroot       a parked device's tree
#   runtime/installs/<devid>/libs          ...and its libraries
#   runtime/sysroot/.tadpole-device        one line: whose tree this is
#
# WHY runtime/sysroot IS STILL A DIRECTORY AND NOT A SYMLINK. Three things
# outside this file's reach hardcode that literal path — tadpole_boot.c reads
# the boot logo out of it, tadpole_view.c builds glasspole's Windows command
# line from it and validates .swf paths against it, and tadpole/Makefile links
# ARM objects against ../runtime/libs/libc.so.0. A symlink would also be a
# Windows-only trap: MSYS symlinks are invisible to native code, which is
# exactly why setup-sysroot.sh's lns() is a copy there. So switching device is
# a pair of renames — park the live tree under runtime/installs/<id>, move the
# wanted one out — which is O(1), needs no symlink support, and leaves both
# halves visible on disk if it is ever interrupted.
#
# WHY THIS IS NOT PROFLIGATE WITH DISK. An assembled sysroot is mostly symlinks
# into its own rootfs plus the few directories the guest writes to; the bulk of
# it is /LF/Bulk, which is that device's own installed titles and must not be
# shared anyway (a LeapPad3 profile is not a Leapster profile). The real
# duplication between two devices is their two extracted firmware trees under
# rootfs/, and those are already separate downloads of genuinely different
# images. tools/dedupe-rootfs.sh hardlinks the parts that are byte-identical —
# the Leapster GS and the LeapPad2 share 30 of 31 Brio libraries and the whole
# GLES stack — for anyone who wants the space back.

tad_device_dir() {
    echo "${TADPOLE_DEVICES:-$(dirname "${BASH_SOURCE[0]}")/devices}"
}

# The runtime/ directory this file lives in, and the project above it.
# Absolute, because half the callers cd somewhere first.
tad_runtime_dir() {
    ( cd "$(dirname "${BASH_SOURCE[0]}")" && pwd )
}
tad_proj_dir() {
    dirname "$(tad_runtime_dir)"
}

# WHERE PARKED DEVICES LIVE. One directory per DEV_ID, each holding the same
# two things runtime/ holds for the live one.
tad_installs_dir() {
    echo "$(tad_runtime_dir)/installs"
}

# -> the DEV_ID for an installed rootfs, by matching Firmware/meta.inf's
# Device= against each profile's DEV_META_DEVICE. Empty if nothing matches.
#
# GIVE THIS A ROOTFS, NEVER AN ASSEMBLED SYSROOT. The sysroot has a copy of
# Firmware/meta.inf — shadow_meta() makes real copies precisely so the guest
# can write to them — and package-manager's RebuildPackageDatabase rewrites
# every one of them with `Device=""`. So the sysroot's answer is blank after
# the first boot, which is why the active device is recorded in a marker file
# instead of re-detected. See tad_active_device().
tad_detect_device() {
    local rootfs="$1" meta dev conf
    meta="$rootfs/Firmware/meta.inf"
    [ -r "$meta" ] || return 0
    dev="$(sed -n 's/^Device="\([^"]*\)".*/\1/p' "$meta" | head -1)"
    [ -n "$dev" ] || return 0
    for conf in "$(tad_device_dir)"/*.conf; do
        [ -r "$conf" ] || continue
        # Match on the declared name only. Sourcing every profile to read one
        # field would clobber the caller's environment.
        if [ "$(sed -n 's/^DEV_META_DEVICE=\(.*\)/\1/p' "$conf" | head -1)" = "$dev" ]; then
            sed -n 's/^DEV_ID=\(.*\)/\1/p' "$conf" | head -1
            return 0
        fi
    done
    # A firmware we have no profile for. Say so rather than silently booting it
    # as a LeapPad2.
    echo "tadpole: no device profile for Device=\"$dev\"" >&2
    return 0
}

# Every extracted firmware tree in this checkout, one path per line.
#
# THREE LAYOUTS, ALL VALID, and this is the one place that knows them:
# install-firmware.sh writes rootfs/<version>/ubi_rfs, the LeapPad3's eMMC
# image extracts to emmc_rfs (a plain tar, no UBI step — see setup-sysroot.sh),
# and the original hand-extracted copy had an extra numeric level.
tad_rootfs_list() {
    local proj cand
    proj="$(tad_proj_dir)"
    for cand in "$proj"/rootfs/*/emmc_rfs "$proj"/rootfs/*/ubi_rfs \
                "$proj"/rootfs/*/*/ubi_rfs; do
        [ -d "$cand" ] && printf '%s\n' "$cand"
    done
}

# -> the rootfs belonging to DEV_ID $1, or empty.
tad_rootfs_for_device() {
    local want="$1" cand
    [ -n "$want" ] || return 0
    while IFS= read -r cand; do
        [ -n "$cand" ] || continue
        # 2>/dev/null: an unrecognised firmware sitting in rootfs/ should not
        # print "no device profile for..." once per device we ask about.
        if [ "$(tad_detect_device "$cand" 2>/dev/null)" = "$want" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done < <(tad_rootfs_list)
}

# -> every DEV_ID that has firmware extracted here, one per line.
tad_installed_devices() {
    local cand id
    while IFS= read -r cand; do
        [ -n "$cand" ] || continue
        id="$(tad_detect_device "$cand" 2>/dev/null)"
        [ -n "$id" ] && printf '%s\n' "$id"
    done < <(tad_rootfs_list) | sort -u
}

# -> the DEV_ID whose tree is assembled at runtime/sysroot right now, or empty.
#
# THE MARKER FILE IS THE ANSWER, and it is written by setup-sysroot.sh at the
# end of every build. The two fallbacks below are for trees assembled before
# the marker existed, so an upgrade does not present itself as "no device
# installed": runtime/sysroot/bin is a symlink into the rootfs it was built
# from, which identifies it exactly, and failing that a checkout with exactly
# one firmware can only be that one.
tad_active_device() {
    local sysroot marker link id all
    sysroot="$(tad_runtime_dir)/sysroot"
    marker="$sysroot/.tadpole-device"
    if [ -r "$marker" ]; then
        head -1 "$marker" | tr -d '[:space:]'
        return 0
    fi
    [ -d "$sysroot" ] || return 0
    if [ -L "$sysroot/bin" ]; then
        link="$(readlink "$sysroot/bin")"
        id="$(tad_detect_device "$(dirname "$link")" 2>/dev/null)"
        [ -n "$id" ] && { printf '%s\n' "$id"; return 0; }
    fi
    all="$(tad_installed_devices)"
    [ "$(printf '%s\n' "$all" | grep -c .)" = 1 ] && printf '%s\n' "$all"
    return 0
}

# -> "active", "installed", "parked" or "" for DEV_ID $1.
#
#   active     its tree is at runtime/sysroot and it is what boots now
#   parked     assembled and waiting under runtime/installs/<id>
#   installed  firmware extracted, but no sysroot built for it yet
tad_device_state() {
    local id="$1"
    [ -n "$id" ] || return 0
    if [ "$id" = "$(tad_active_device)" ]; then echo active; return 0; fi
    if [ -d "$(tad_installs_dir)/$id/sysroot" ]; then echo parked; return 0; fi
    [ -n "$(tad_rootfs_for_device "$id")" ] && echo installed
    return 0
}

tad_ui_cfg_device() {
    local cfg="${XDG_CONFIG_HOME:-$HOME/.config}/tadpole/ui.cfg"
    [ -r "$cfg" ] || return 0
    # ui.cfg is "key value", space-separated — not key=value. See ui_cfg_save().
    sed -n 's/^device[[:space:]][[:space:]]*//p' "$cfg" | tail -1
}

# -> which device SHOULD be live, given the environment, the saved setting and
# what is actually installed. Reads only; moves nothing.
#
# THE OLD RULE WAS "THE INSTALLED FIRMWARE OUTRANKS THE SAVED SETTING", and the
# bug it was written against is worth restating because the fix has to keep
# being immune to it. A stale `device leappad2` line — written by the wizard
# before any firmware existed, which is exactly when the wizard asks — used to
# beat autodetect on a LeapPad Ultra install. The result is not an error
# message: it is a LeapPad2 boot against an Ultra rootfs, running AppManager
# instead of AppServer, with the geometry a quarter of the panel.
#
# WITH SEVERAL DEVICES INSTALLED THAT RULE CANNOT STAND AS WRITTEN, because
# "the installed firmware" stops being a single thing. What replaces it is
# stronger, not weaker: the setting may only name a device that IS installed,
# and naming one switches to it wholesale — rootfs, sysroot, libraries and
# profile together. A setting that names something not installed is ignored
# out loud. So the pairing that caused the original bug is now unreachable by
# construction: the profile is loaded for the tree that is assembled, and
# switching the tree is what the setting does.
tad_resolve_device() {
    local want active cfg
    active="$(tad_active_device)"
    want="${TADPOLE_DEVICE:-}"
    [ -n "$want" ] || want="$(tad_ui_cfg_device)"
    if [ -n "$want" ] && [ "$want" != "$active" ]; then
        case "$(tad_device_state "$want")" in
            parked|installed|active) ;;
            *)  if [ -n "$active" ]; then
                    echo "tadpole: no firmware installed for '$want' —" \
                         "staying on $active" >&2
                    want="$active"
                fi ;;
        esac
    fi
    [ -n "$want" ] || want="$active"
    printf '%s\n' "$want"
}

# Make DEV_ID $1 the live device: park whatever is live now, move $1's tree in.
#
# Two renames per direction and nothing copied, so this costs the same whether
# the tree is 3 MB or 30 GB. It is deliberately NOT called from
# tad_load_device(): every tool in the repo sources that to read the geometry,
# and a function that reads a profile must never move a filesystem out from
# under a running guest as a side effect. tadpole.sh and setup-sysroot.sh call
# this explicitly, which are the two places that own the tree.
#
# -> 0 if $1 is live afterwards, 1 if it could not be.
tad_activate_device() {
    local want="$1" active inst here parked
    [ -n "$want" ] || return 1
    active="$(tad_active_device)"
    [ "$want" = "$active" ] && return 0
    here="$(tad_runtime_dir)"
    inst="$(tad_installs_dir)"

    # NOT WHEN THE SYSROOT HAS BEEN POINTED SOMEWHERE ELSE. TADPOLE_SYSROOT is
    # how a worker or an A/B capture runs against a tree that is not this
    # checkout's; swapping the checkout's tree underneath that would be a
    # surprise with no upside, and the caller did not ask for it.
    if [ -n "${TADPOLE_SYSROOT:-}" ]; then
        echo "tadpole: TADPOLE_SYSROOT is set — not switching device" >&2
        return 1
    fi

    parked="$inst/$want/sysroot"
    if [ ! -d "$parked" ]; then
        # No assembled tree, but possibly firmware waiting to be assembled.
        # Building is setup-sysroot.sh's job, not ours; say which it is.
        if [ -n "$(tad_rootfs_for_device "$want")" ]; then
            echo "tadpole: $want has firmware but no sysroot yet —" \
                 "run ./runtime/setup-sysroot.sh $want" >&2
        else
            echo "tadpole: no install for device '$want'" >&2
        fi
        return 1
    fi

    if [ -n "$active" ] && [ -d "$here/sysroot" ]; then
        mkdir -p "$inst/$active" || return 1
        rm -rf "$inst/$active/sysroot" "$inst/$active/libs"
        mv "$here/sysroot" "$inst/$active/sysroot" || return 1
        [ -d "$here/libs" ] && mv "$here/libs" "$inst/$active/libs"
    else
        # An unlabelled tree we cannot attribute to a device. Refuse rather
        # than park it under a name that may be wrong, or delete it.
        if [ -d "$here/sysroot" ]; then
            echo "tadpole: runtime/sysroot belongs to no known device —" \
                 "rebuild it before switching" >&2
            return 1
        fi
    fi
    mv "$inst/$want/sysroot" "$here/sysroot" || return 1
    [ -d "$inst/$want/libs" ] && mv "$inst/$want/libs" "$here/libs"
    rmdir "$inst/$want" 2>/dev/null || true
    return 0
}

# Park the live tree without bringing another one in. Used by setup-sysroot.sh
# when it is about to assemble a device that has never been built here.
tad_park_active() {
    local active inst here
    active="$(tad_active_device)"
    here="$(tad_runtime_dir)"
    inst="$(tad_installs_dir)"
    [ -n "$active" ] || return 0
    [ -d "$here/sysroot" ] || return 0
    mkdir -p "$inst/$active" || return 1
    rm -rf "$inst/$active/sysroot" "$inst/$active/libs"
    mv "$here/sysroot" "$inst/$active/sysroot" || return 1
    [ -d "$here/libs" ] && mv "$here/libs" "$inst/$active/libs"
    return 0
}

# Load the profile for $1 (a DEV_ID), or the live device if no argument.
#
# READ-ONLY. Nothing here moves a directory; see tad_activate_device().
tad_load_device() {
    local want="${1:-}" conf
    if [ -z "$want" ]; then
        want="$(tad_active_device)"
        # NOTHING ASSEMBLED YET — the first-run case, where the wizard's answer
        # is all there is and there is no firmware to contradict it.
        [ -n "$want" ] || want="${TADPOLE_DEVICE:-}"
        [ -n "$want" ] || want="$(tad_ui_cfg_device)"
        [ -n "$want" ] || want=leappad2
    fi
    conf="$(tad_device_dir)/$want.conf"
    if [ ! -r "$conf" ]; then
        echo "tadpole: unknown device '$want' (no $conf)" >&2
        return 1
    fi
    # shellcheck disable=SC1090
    . "$conf"
    DEV_CONF="$conf"
    export DEV_ID DEV_NAME DEV_CONF
}

# -> "id<TAB>name" for every profile, for the setup wizard's device list.
tad_list_devices() {
    local conf id name
    for conf in "$(tad_device_dir)"/*.conf; do
        [ -r "$conf" ] || continue
        id="$(sed -n 's/^DEV_ID=\(.*\)/\1/p' "$conf" | head -1)"
        name="$(sed -n 's/^DEV_NAME="\?\([^"]*\)"\?/\1/p' "$conf" | head -1)"
        printf '%s\t%s\n' "$id" "$name"
    done
}

# The same list with what is installed marked, for `./tadpole.sh --devices`.
tad_print_devices() {
    local id name state active
    active="$(tad_active_device)"
    while IFS="$(printf '\t')" read -r id name; do
        state="$(tad_device_state "$id")"
        case "$state" in
            active)    state="* active" ;;
            parked)    state="  installed" ;;
            installed) state="  firmware only (no sysroot yet)" ;;
            *)         state="  -" ;;
        esac
        printf '%-14s %-22s %s\n' "$id" "$name" "$state"
    done < <(tad_list_devices)
    [ -n "$active" ] || echo "(nothing assembled yet — run the setup wizard)"
}
