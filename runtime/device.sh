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
# Precedence: $TADPOLE_DEVICE  >  ui.cfg device=  >  autodetect  >  leappad2.

tad_device_dir() {
    echo "${TADPOLE_DEVICES:-$(dirname "${BASH_SOURCE[0]}")/devices}"
}

# -> the DEV_ID for an installed rootfs, by matching Firmware/meta.inf's
# Device= against each profile's DEV_META_DEVICE. Empty if nothing matches.
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

tad_ui_cfg_device() {
    local cfg="${XDG_CONFIG_HOME:-$HOME/.config}/tadpole/ui.cfg"
    [ -r "$cfg" ] || return 0
    # ui.cfg is "key value", space-separated — not key=value. See ui_cfg_save().
    sed -n 's/^device[[:space:]][[:space:]]*//p' "$cfg" | tail -1
}

# Load the profile for $1 (a DEV_ID), or the resolved device if no argument.
tad_load_device() {
    local want="${1:-}" conf
    if [ -z "$want" ]; then
        want="${TADPOLE_DEVICE:-}"
        [ -n "$want" ] || want="$(tad_ui_cfg_device)"
        [ -n "$want" ] || want="$(tad_detect_device "${ROOTFS:-}")"
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
