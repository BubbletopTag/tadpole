#!/bin/bash
# Tadpole — check every dependency at once and say how to install what is missing.
#
#   ./tools/check-deps.sh            everything
#   ./tools/check-deps.sh firmware   only what install-firmware.sh needs
#   ./tools/check-deps.sh --quiet    say nothing, just set the exit status
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
#
# MOST OF THE LIST CAN NOW BE ANSWERED WITH "bundled". tools/fetch-deps.sh
# stages qemu-arm and a Python with ubi_reader into build/deps/, and the
# AppImage ships them, so on a normal install there is nothing to install. This
# script's job is to tell those two worlds apart rather than sending someone to
# the AUR for a package they already have.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
. "$HERE/lib-deps.sh"

WHAT="all"; QUIET=0
for a in "$@"; do
    case "$a" in
        --quiet) QUIET=1 ;;
        *)       WHAT="$a" ;;
    esac
done

DISTRO=""
command -v pacman  >/dev/null && DISTRO=arch
command -v apt-get >/dev/null && DISTRO=debian
command -v dnf     >/dev/null && DISTRO=fedora

missing_pac=""; missing_aur=""; missing_pip=""
ok=0; bad=0

have_cmd() { command -v "$1" >/dev/null 2>&1; }
have_pc()  { pkg-config --exists "$1" 2>/dev/null; }
# Ask the Python that would actually be USED, not whichever one is first on the
# PATH: with the bundle staged those are different interpreters, and asking the
# wrong one is how a self-contained install reports its own tools missing.
have_pymod() {
    local py; py="$(tad_python || true)"
    [ -n "$py" ] || return 1
    "$py" -c "import $1" >/dev/null 2>&1
}

say() { [ "$QUIET" = 1 ] || printf '%b' "$*"; }

report() {                      # $1=ok/bundled/no  $2=label  $3=why
    case "$1" in
        ok)      say "  \033[32m+\033[0m $(printf '%-22s' "$2") $3\n"; ok=$((ok+1)) ;;
        bundled) say "  \033[32m+\033[0m $(printf '%-22s' "$2") $3 \033[2m(bundled)\033[0m\n"; ok=$((ok+1)) ;;
        *)       say "  \033[31m-\033[0m $(printf '%-22s' "$2") $3\n"; bad=$((bad+1)) ;;
    esac
}
need() {                        # $1=state $2=label $3=why $4=arch $5=debian $6=aur|pip
    report "$1" "$2" "$3"
    case "$1" in ok|bundled) return ;; esac
    # "pip" means NO DISTRIBUTION PACKAGES IT. ubi_reader and lzallright are
    # the whole reason this case exists: there is no python3-ubi-reader on
    # Debian or Ubuntu and none in the Arch repos, and printing an apt line for
    # a package that does not exist is worse than printing nothing.
    if [ "${6:-}" = pip ]; then missing_pip="$missing_pip $2"; return; fi
    case "$DISTRO" in
        arch)   [ "${6:-}" = aur ] && missing_aur="$missing_aur $4" || missing_pac="$missing_pac $4" ;;
        debian) missing_pac="$missing_pac $5" ;;
        *)      missing_pac="$missing_pac $4" ;;
    esac
}

say "Tadpole dependency check\n\n"
if tad_have_bundle; then
    say "Bundled runtime in $TADPOLE_DEPS\n"
    say "  the ARM engine and the firmware tools come with Tadpole;\n"
    say "  nothing to install.\n\n"
fi

if [ "$WHAT" = all ]; then
    say "To run:\n"
    # WHICHEVER ENGINE tad_qemu() WOULD PICK, reported under its own name and
    # in the same order — a check that answers about qemu-arm while Tadpole
    # runs on glasspole is telling the truth about the wrong program.
    #
    # The bundled qemu is a static binary that runs anywhere; a host qemu-arm is
    # just as good, and so is a glasspole built in this checkout. Any of them
    # satisfies this. qemu-user stays the package named when there is nothing
    # at all, because it is one command away and glasspole has to be built.
    engine=qemu-arm
    if   [ -x "$TADPOLE_DEPS/bin/glasspole" ];     then s=bundled; engine=glasspole
    elif [ -x "$PROJ/glasspole/build/glasspole" ]; then s=ok;      engine=glasspole
    elif [ -x "$TADPOLE_DEPS/bin/qemu-arm" ];      then s=bundled
    elif have_cmd glasspole;                       then s=ok;      engine=glasspole
    elif have_cmd qemu-arm;                        then s=ok
    else                                                s=no; fi
    need "$s" "$engine" "runs the guest's ARM code" qemu-user qemu-user

    if have_pc sdl2 || have_cmd sdl2-config; then s=ok; else s=no; fi
    # Arch dropped `sdl2`; sdl2-compat provides the ABI (on top of SDL3).
    need "$s" SDL2 "window, input, audio" sdl2-compat libsdl2-dev
    have_pc gl && s=ok || s=no
    need "$s" OpenGL "host-GPU rendering" mesa libgl1-mesa-dev
    have_pc zlib && s=ok || s=no
    need "$s" zlib "the viewer decodes its icon" zlib zlib1g-dev
    say "\n"

    # ONLY IN A SOURCE CHECKOUT. An installed AppImage has no compiler to run
    # and nothing to compile, so listing clang, lld and python3 there reports
    # a self-contained install as "1 missing" over a build tool nobody needs.
    if [ -f "$PROJ/tadpole/Makefile" ]; then
    say "To build:\n"
    have_cmd clang && s=ok || s=no
    need "$s" clang "cross-compiles the shim to ARM" clang clang
    have_cmd ld.lld && s=ok || s=no
    need "$s" lld "the ARM linker" lld lld
    have_cmd make && s=ok || s=no
    need "$s" make "" make make
    have_cmd python3 && s=ok || s=no
    need "$s" python3 "build and analysis tooling" python python3
    say "\n"
    fi
fi

say "To install firmware:\n"

# unzip and bzip2 are no longer required: tools/pkgtool.py reads both formats
# with Python's stdlib. Report them as the shortcut they are, not as a blocker.
if   have_cmd unzip;             then s=ok
elif tad_python >/dev/null;      then s=bundled
else                                  s=no; fi
need "$s" unzip ".lfp packages are ZIP" unzip unzip
if   have_cmd bzcat;             then s=ok
elif tad_python >/dev/null;      then s=bundled
else                                  s=no; fi
need "$s" bzip2 ".lf2 packages are bzip2 tar" bzip2 bzip2

# ubi_reader AND its three compression backends, in ONE question. It imports
# lzallright, zstandard and cryptography at module scope, so "can this Python
# import ubireader.ubifs.misc" is the only test that means anything — asking
# about them one at a time reported success on installs that then failed.
if tad_python_with_ubireader >/dev/null; then
    [ -x "$TADPOLE_DEPS/python/bin/python3" ] && s=bundled || s=ok
else
    s=no
fi
need "$s" ubi_reader "reads the UBIFS root filesystem" "" "" pip
if [ "$s" = no ]; then
    # Say WHICH part is missing: "install ubi_reader" is unhelpful advice to
    # someone who has it and is missing only the LZO backend.
    for mod in ubireader lzallright cryptography zstandard; do
        have_pymod "$mod" || say "      missing Python module: $mod\n"
    done
fi

say "\n"
if [ "$bad" = 0 ]; then
    say "All $ok dependencies present.\n"
    exit 0
fi

say "$bad missing. The simplest fix, which installs nothing system-wide:\n\n"
say "    ./tools/fetch-deps.sh\n\n"
say "Or with your package manager:\n\n"
case "$DISTRO" in
    arch)
        [ -n "$missing_pac" ] && say "    sudo pacman -S$missing_pac\n"
        [ -n "$missing_aur" ] && say "    yay -S$missing_aur\n"
        ;;
    debian)
        [ -n "$missing_pac" ] && say "    sudo apt install$missing_pac\n"
        ;;
    fedora)
        [ -n "$missing_pac" ] && say "    sudo dnf install$missing_pac\n"
        ;;
    *)
        say "    packages:$missing_pac$missing_aur\n"
        ;;
esac
if [ -n "$missing_pip" ]; then
    say "\n  No distribution packages these; pip or the AUR only:\n"
    say "    pip install --user ubi_reader lzallright\n"
fi
exit 1
