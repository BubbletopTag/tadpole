#!/bin/bash
# Tadpole — check every dependency at once and say how to install what is missing.
#
#   ./tools/check-deps.sh            everything
#   ./tools/check-deps.sh firmware   only what install-firmware.sh needs
#
# WHY THIS EXISTS. Installing firmware failed three times in a row, each time on
# a different missing Python module, because ubi_reader imports its dependencies
# lazily and the first failure hides the next:
#
#     ModuleNotFoundError: No module named 'lzallright'      (install it)
#     ModuleNotFoundError: No module named 'cryptography'    (install it)
#     ...
#
# Reporting one missing thing at a time is a poor experience when the whole list
# is knowable up front. This checks everything and prints one command.

set -u
WHAT="${1:-all}"
DISTRO=""
command -v pacman  >/dev/null && DISTRO=arch
command -v apt-get >/dev/null && DISTRO=debian
command -v dnf     >/dev/null && DISTRO=fedora

missing_pac=""; missing_aur=""; missing_pip=""
ok=0; bad=0

have_cmd() { command -v "$1" >/dev/null 2>&1; }
have_py()  { python3 -c "import $1" >/dev/null 2>&1; }
have_pc()  { pkg-config --exists "$1" 2>/dev/null; }

report() {                      # $1=ok/no  $2=label  $3=why
    if [ "$1" = ok ]; then
        printf "  \033[32m+\033[0m %-22s %s\n" "$2" "$3"; ok=$((ok+1))
    else
        printf "  \033[31m-\033[0m %-22s %s\n" "$2" "$3"; bad=$((bad+1))
    fi
}
need() {                        # $1=ok/no $2=label $3=why $4=arch $5=debian $6=aur|pip
    report "$1" "$2" "$3"
    [ "$1" = ok ] && return
    case "$DISTRO" in
        arch)   [ "${6:-}" = aur ] && missing_aur="$missing_aur $4" || missing_pac="$missing_pac $4" ;;
        debian) missing_pac="$missing_pac $5" ;;
        *)      missing_pac="$missing_pac $4" ;;
    esac
    [ "${6:-}" = pip ] && missing_pip="$missing_pip $2"
}

echo "Tadpole dependency check"
echo

if [ "$WHAT" = all ]; then
    echo "To run:"
    have_cmd qemu-arm && s=ok || s=no
    need "$s" qemu-arm "runs the guest's ARM code" qemu-user qemu-user
    have_pc sdl2 || have_cmd sdl2-config; [ $? = 0 ] && s=ok || s=no
    need "$s" SDL2 "window, input, audio" sdl2 libsdl2-dev
    have_pc gl && s=ok || s=no
    need "$s" OpenGL "host-GPU rendering" mesa libgl1-mesa-dev
    have_pc zlib && s=ok || s=no
    need "$s" zlib "the viewer decodes its icon" zlib zlib1g-dev
    echo

    echo "To build:"
    have_cmd clang && s=ok || s=no
    need "$s" clang "cross-compiles the shim to ARM" clang clang
    have_cmd ld.lld && s=ok || s=no
    need "$s" lld "the ARM linker" lld lld
    have_cmd make && s=ok || s=no
    need "$s" make "" make make
    have_cmd python3 && s=ok || s=no
    need "$s" python3 "build and analysis tooling" python python3
    echo
fi

echo "To install firmware:"
have_cmd unzip && s=ok || s=no
need "$s" unzip ".lfp packages are ZIP" unzip unzip
have_cmd bzcat && s=ok || s=no
need "$s" bzip2 ".lf2 packages are bzip2 tar" bzip2 bzip2
have_cmd ubireader_extract_files && s=ok || s=no
need "$s" ubi_reader "reads the UBIFS root filesystem" python-ubi-reader python3-ubi-reader aur

# ubi_reader's own dependencies, from its package metadata. It imports these
# lazily, so a missing one surfaces only when extraction is already running —
# which is why they are checked here explicitly rather than left to chance.
# Which of these live in Arch's official repos and which are AUR-only was
# checked with `pacman -Si`, not assumed — suggesting `yay` for a package in
# `extra` sends people to the AUR for no reason.
#   extra: python-cryptography, python-zstandard
#   AUR:   python-lzallright, python-ubi-reader
for pair in "lzallright:LZO decompression:aur" \
            "cryptography:UBIFS encryption support:repo" \
            "zstandard:zstd decompression:repo"; do
    mod="${pair%%:*}"; rest="${pair#*:}"
    why="${rest%%:*}"; src="${rest#*:}"
    have_py "$mod" && s=ok || s=no
    if [ "$src" = aur ]; then
        need "$s" "$mod" "$why (ubi_reader)" "python-$mod" "python3-$mod" aur
    else
        need "$s" "$mod" "$why (ubi_reader)" "python-$mod" "python3-$mod"
    fi
done

echo
if [ "$bad" = 0 ]; then
    echo "All $ok dependencies present."
    exit 0
fi

echo "$bad missing. Install with:"
echo
case "$DISTRO" in
    arch)
        [ -n "$missing_pac" ] && echo "    sudo pacman -S$missing_pac"
        [ -n "$missing_aur" ] && echo "    yay -S$missing_aur"
        ;;
    debian)
        [ -n "$missing_pac" ] && echo "    sudo apt install$missing_pac"
        echo
        echo "  If a python3-* package is unavailable, use pip instead:"
        echo "    pip install --user ubi_reader lzallright cryptography zstandard"
        ;;
    fedora)
        [ -n "$missing_pac" ] && echo "    sudo dnf install$missing_pac"
        ;;
    *)
        echo "    packages:$missing_pac$missing_aur"
        ;;
esac
exit 1
