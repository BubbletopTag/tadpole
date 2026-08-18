#!/bin/bash
# Tadpole — install a LeapFrog game backup (LFManager-style .tar) into /LF/Bulk.
#
#   ./tools/install-game.sh games/FOF.tar [more.tar ...]
#   ./tools/install-game.sh games/*.tar
#   ./tools/install-game.sh --fix-saves          create missing save directories
#                                                (NOT yet cleared library-wide —
#                                                 see HANDOVER before using it)
#
# Installing a game as an APPLICATION is the approach that actually works, and
# it is what LFManager does on real hardware. Emulating an inserted cartridge
# gets the tile to appear but the UI never resolves its GameInfo.
#
# THREE tar shapes, all present in real backups:
#
#   flat            meta.inf at the top          FOF, B10, GLB, UP, ClamPrix
#   self-wrapped    <NAME>/meta.inf              MIP
#   multi-package   <NAME>/meta.inf + lib/...    COOKING, pixar_pals
#
# The self-wrapped and multi-package ones are why "LFManager sends it but it
# never appears": an installer that only looks for a top-level meta.inf finds
# nothing and silently installs nothing.
#
# Destination by Type, same rules as tools/install-content.sh (from lfpkg):
#
#   Application  -> Bulk/ProgramFiles/<PackageID>/
#   System       -> Base/<PackageID>/
#   Download     -> Bulk/Downloads/<PackageID>/
#
# ProfileAccess is appended to Applications that lack it — without it the home
# picker filters the app out. See "Missing home-screen apps" in HANDOVER.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
BULK="${TADPOLE_BULK:-$PROJ/runtime/sysroot/LF/Bulk}"
BASE="${TADPOLE_BASE:-$PROJ/runtime/sysroot/LF/Base}"

[ $# -gt 0 ] || { echo "usage: $0 <game.tar> [...]" >&2; exit 2; }

# ANCHORED TO THE START OF A LINE, and that is not tidiness.
#
# Unanchored, `field "$meta" Icon` matches inside `LargeIcon="preview.png"` —
# grep -o is happy to start a match in the middle of a word — and meta.inf
# happens to list LargeIcon FIRST, so asking for the icon returned the store
# banner. The same trap is set for Name inside ShortName and Version inside
# MetaVersion; those two only work today because the shorter key happens to
# come first in the files we have. meta.inf is line-oriented, so anchor it.
field() { grep -oE "^[[:space:]]*$2=\"[^\"]*\"" <<<"$1" | head -1 | cut -d'"' -f2; }

# The same read out of a JSON object: "Key" : "value", any spacing.
json_str() { grep -oE "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$1" 2>/dev/null |
             head -1 | sed 's/.*:[[:space:]]*"//; s/"$//'; }

# THE PRODUCT ID, OUT OF THE PACKAGE ID. `LST3-0x00180002-000000` -> `0x00180002`.
#
# It is the middle field, and it is the one thing about a package that names the
# TITLE rather than the release: the device keys a game's save directory, its
# CaboCompatibility entry and its GameViewFrame on this and nothing else. Read
# from the PackageID rather than from ProductID= because the two are written
# inconsistently across real packages — `0x00180002`, `"00180013"`, plain
# decimal — while the PackageID's middle field never varies.
product_of() { printf '%s' "$1" | cut -d- -f2 | tr 'A-Z' 'a-z'; }

# ONE PRODUCT, ONE APPLICATION — remove any OTHER package for the same title.
#
# The same game exists as several SKUs with different PackageID prefixes: Ni Hao
# Kai-lan is `LST3-0x00180002-000000` as a Leapster Explorer release and
# `MHRS-0x00180002-000000` as the LeapPad one, same product id, same App.so,
# different wrappers. Installing both is not installing two things: it is the
# same title twice, and the home screen shows it twice — measured, two tiles for
# one game, because the package-manager daemon builds a row per meta.inf and
# neither package knows about the other.
#
# It is worse than cosmetic. Saves live in Data/Local/<profile>/<ProductID>/,
# keyed on the product id alone, so the two entries also SHARE A SAVE FILE and
# write over each other's progress.
#
# So the one being installed replaces the one that is there, and says so. Only
# Applications: expansion packs share a product id with their game and are
# always Type=MicroDownload, which lands somewhere else entirely.
purge_same_product() {              # $1=PackageID being installed
    local newpid="$1" prod other obase ometa opid otype oname
    prod="$(product_of "$newpid")"
    [ -n "$prod" ] || return 0
    for other in "$BULK"/ProgramFiles/*/; do
        [ -f "$other/meta.inf" ] || continue
        obase="$(basename "$other")"
        ometa="$(cat "$other/meta.inf" 2>/dev/null)" || continue
        opid="$(field "$ometa" PackageID)"
        otype="$(field "$ometa" Type)"
        [ "$otype" = Application ] || continue
        [ -n "$opid" ] || continue
        [ "$opid" = "$newpid" ] && continue          # the same package: rm -rf has it
        [ "$(product_of "$opid")" = "$prod" ] || continue
        oname="$(field "$ometa" Name)"
        [ -n "$obase" ] && rm -rf "$BULK/ProgramFiles/$obase"
        printf "  %-12s %-26s %s\n" "replaced" "$opid" "$oname"
    done
}

# A PNG THE HOME SCREEN CAN ACTUALLY DRAW.
#
# The picker asks liblfp for a preview icon and liblfp reads GameInfo.json's
# IconPADS / IconPHRS / IconTHDS / IconTHD1 — a PNG path relative to the package.
# meta.inf's Icon= is only the fallback for a package with NO GameInfo.json.
#
# A Leapster Explorer title has a GameInfo.json in the OLDER shape —
# Title / Subtitle / LargeIcon / Audio — with none of those four keys, and its
# meta.inf says `Icon="icon.swf"` because the Leapster's and the LeapPad2's home
# screens are Flash and render a Flash icon. The Qt picker on the newer devices
# cannot, so it stores an EMPTY preview icon and draws the Adobe placeholder in
# the tile. Measured on Ni Hao Kai-lan: LocalPackageInfo.db holds
# PreviewIcon='' for it, against 'BaseImage.png' for every title that works.
#
# The artwork is already in the package — icon64.png, nihao_icon_large.png —
# so this points the key the firmware reads at the file the package already
# ships. Nothing is invented and nothing is downloaded.
pick_icon() {                       # $1=package dir -> a PNG in it, or nothing
    local d="$1" meta c
    meta="$(cat "$d/meta.inf" 2>/dev/null)"
    # THE PACKAGE'S OWN CHOICE FIRST — but only the fields that mean "tile".
    #
    # meta.inf's LargeIcon= is deliberately NOT consulted: it names a wide
    # store banner, not an icon, and in the Ni Hao backup it names preview.png,
    # which in that package is TINKER BELL ARTWORK. Somebody at LeapFrog shipped
    # the wrong file and nothing on the Leapster ever drew it, so the mistake
    # went unnoticed for sixteen years. GameInfo.json's LargeIcon is the
    # Leapster home screen's own tile art and is the right one.
    for c in "$(field "$meta" Icon)" "$(json_str "$d/GameInfo.json" LargeIcon)" \
             BaseImage.png BaseIcon.png icon.png icon64.png 82x88.png \
             PopUpIcon.png; do
        case "$c" in
            *.png|*.PNG) [ -f "$d/$c" ] && { printf '%s\n' "$c"; return 0; } ;;
        esac
    done
    for c in "$d"/*[iI]con*.png; do
        [ -f "$c" ] && { basename "$c"; return 0; }
    done
    return 0
}

# Add "IconPADS": "<png>" as the first member of GameInfo.json.
#
# A TEXT INSERT, NOT A REWRITE. The file belongs to the title and its other keys
# are none of our business; re-serialising it through a JSON library would also
# reformat it, which makes every later diff of a package unreadable. The only
# case needing care is an object with no members, where a trailing comma would
# be invalid JSON — hence the test for an existing "key": pair.
add_icon_key() {                    # $1=GameInfo.json  $2=png
    local sep=""
    grep -qE '"[^"]*"[[:space:]]*:' "$1" && sep=","
    # NO NEWLINE IS INTRODUCED. These files are CRLF as often as LF, and a
    # helpfully-added "\n" leaves one line in the other style — harmless to a
    # JSON parser, and enough to make this and install-game.py produce files
    # that differ, which is the one thing the port must never do.
    awk -v png="$2" -v sep="$sep" '
        !ins && index($0, "{") {
            i = index($0, "{")
            printf "%s{\"IconPADS\": \"%s\"%s%s\n",
                   substr($0, 1, i - 1), png, sep, substr($0, i + 1)
            ins = 1
            next
        }
        { print }
    ' "$1" > "$1.tadpole" && mv "$1.tadpole" "$1"
}

install_one() {                     # $1=tar  $2=meta.inf path inside it
    local tar="$1" metapath="$2" prefix meta type pid name dest icon
    prefix="$(dirname "$metapath")"
    meta="$(tar xOf "$tar" "$metapath" 2>/dev/null)" || return 0
    type="$(field "$meta" Type)"
    pid="$(field "$meta" PackageID)"
    name="$(field "$meta" Name)"
    [ -n "$pid" ] || return 0

    case "$type" in
        Application) dest="$BULK/ProgramFiles/$pid" ;;
        System)      dest="$BASE/$pid" ;;
        Download|MicroDownload) dest="$BULK/Downloads/$pid" ;;
        *)           printf "  %-12s %-26s %s (skipped)\n" "$type" "$pid" "$name"; return 0 ;;
    esac

    [ "$type" = Application ] && purge_same_product "$pid"

    rm -rf "$dest"; mkdir -p "$dest"
    if [ "$prefix" = "." ]; then
        tar xf "$tar" -C "$dest"
    else
        # Strip the wrapper directory so the package contents land directly in
        # <dest>, matching how a flat archive installs.
        tar xf "$tar" -C "$dest" --strip-components=1 "$prefix"
    fi

    # THE NEWLINE IS NOT OPTIONAL. A good number of these meta.inf files end
    # WITHOUT one — the last line is `DeviceAccess=1` and then the file simply
    # stops — so appending straight to the end produced
    #
    #     DeviceAccess=1ProfileAccess=-1,0,1,2,3
    #
    # which is two fields lost at once: DeviceAccess reads as garbage, and
    # ProfileAccess is no longer at the start of a line so nothing finds it.
    # The title installs, reports success, and then never appears on the home
    # screen, because that is exactly what a missing ProfileAccess does. Six
    # titles here were in that state.
    #
    # It also hides itself: the `grep -q '^ProfileAccess='` guard cannot see
    # the mangled copy either, so re-installing appends a SECOND one and the
    # line grows.
    if [ "$type" = Application ] && ! grep -q '^ProfileAccess=' "$dest/meta.inf" 2>/dev/null; then
        [ -s "$dest/meta.inf" ] && [ -n "$(tail -c 1 "$dest/meta.inf")" ] &&
            printf '\n' >> "$dest/meta.inf"
        printf 'ProfileAccess=-1,0,1,2,3\n' >> "$dest/meta.inf"
    fi

    # THE MANIFEST IS INPUT, NOT STATE — SO STOP THE DAEMON REWRITING IT.
    #
    # On a Qt device (LeapPad3, Ultra) the package-manager daemon's
    # RebuildPackageDatabase parses every meta.inf at boot and WRITES EACH ONE
    # BACK from its own parsed model. That model has no room for Device=, so it
    # writes `Device=""`, and a quoted ProductID comes back as 0x00000000.
    # Measured by diffing the pristine rootfs against the sysroot after one
    # boot: `-Device="LeapsterExplorer"` `+Device=""`, `+Size=0`.
    #
    # THAT COSTS THE TITLE ITS ON-SCREEN CONTROLS. A Leapster Explorer game is
    # played inside a GameViewFrame — the border with A, B, Home, Pause and
    # Hint drawn around a 320x240 window — and BrioWrapper decides whether to
    # put one up from the package's device type. Measured on Ni Hao Kai-lan:
    # with Device="" the game gets the whole 480x272 panel and no buttons and
    # the log never mentions a ViewFrame; with Device="LeapsterExplorer"
    # restored it logs
    #     PushEnterApp: loading ViewFrame /LF/Bulk/Downloads/PADS-0x00210008-
    #                   210000/LST3-0x00180002-000000/ViewFrame.json
    #     CGameViewFrame::Enter
    # the game is given its 320x240 box at (17,16), and tapping where the JSON
    # puts A, B and Home drives the game.
    #
    # A read-only meta.inf makes the rewrite fail. The daemon says so twice and
    # carries on: the package still registers, the tile still appears, and the
    # title still launches — all three checked on a booted device, not assumed.
    # What it loses is bookkeeping it recomputes anyway (`Size=0`).
    #
    # ONLY PACKAGES WITH SOMETHING TO LOSE. A LeapPad-native backup declares no
    # Device= at all, so the rewrite takes nothing from it and there is no
    # reason to make its manifest immutable. `rm -rf` above still replaces a
    # read-only file on reinstall, because the DIRECTORY stays writable.
    if [ "$type" = Application ] && [ -n "$(field "$meta" Device)" ]; then
        chmod 0444 "$dest/meta.inf" 2>/dev/null || true
    fi

    # See pick_icon: a GameInfo.json that names none of the icon keys the
    # firmware reads leaves the tile with a placeholder, and the older Leapster
    # shape names none of them. Only ever ADDS a key, and only when there is a
    # real PNG in the package to point it at, so a package that already declares
    # its icon properly is left alone.
    if [ "$type" = Application ] && [ -f "$dest/GameInfo.json" ] &&
       ! grep -qE '"Icon(PADS|PHRS|THDS|THD1)"' "$dest/GameInfo.json"; then
        icon="$(pick_icon "$dest")"
        if [ -n "$icon" ]; then
            add_icon_key "$dest/GameInfo.json" "$icon"
            printf "  %-12s %-26s %s\n" "icon" "$pid" "$icon"
        fi
    fi

    # THE SAVE AREA HAS TO EXIST BEFORE FIRST LAUNCH, and nothing creates it.
    #
    # A title's documents live in Bulk/Data/Local/<profile>/<PackageID>/, which
    # AppManager announces as "Set doc base to:" and then assumes is there. It
    # is NOT created on demand: measured, with mkdir interception working in the
    # shim and a full launch traced, the guest never calls mkdir for this path
    # at all. On hardware it already exists; the three titles here that have one
    # got it from the transplanted /LF/Bulk, not from anything we did.
    #
    # WHAT ITS ABSENCE COSTS is a whole title, silently. Cooking! Recipes on the
    # Road logs one line —
    #     fopenAtomic(.../SAVE.DAT): mkstemp failed us!
    # — and then calls dslib::PanicScreen::showDirect(msg, true), whose
    # terminate() is an unconditional spin loop. The title hangs at 100% CPU on
    # a white screen, having drawn nothing, with no crash and no message,
    # because this build compiles the panic screen's own addDirect() down to
    # `bx lr` and the text is never rendered.
    #
    # Per EXISTING profile, not a fixed list: the profiles are whatever
    # Bulk/Data/Local already holds, and inventing 0..3 would litter the tree
    # with directories for accounts that do not exist.
    if [ "$type" = Application ]; then
        local prof
        for prof in "$BULK"/Data/Local/*/; do
            [ -d "$prof" ] || continue
            mkdir -p "$prof$pid"
        done
    fi
    printf "  %-12s %-26s %s\n" "$type" "$pid" "$name"

    local dep
    dep="$(field "$meta" Depends)"
    [ -n "$dep" ] && echo "      needs: $dep"
    return 0
}

# BACKFILL FOR TITLES ALREADY ON DISK. The save-area fix above only runs at
# install time, so without this every title installed before it stays broken and
# the only remedy is reinstalling the whole library.
if [ "${1:-}" = "--fix-saves" ]; then
    made=0
    for d in "$BULK"/ProgramFiles/*/; do
        [ -f "$d/meta.inf" ] || continue
        grep -q '^Type="\?Application' "$d/meta.inf" 2>/dev/null || continue
        id="$(basename "$d")"
        for prof in "$BULK"/Data/Local/*/; do
            [ -d "$prof" ] || continue
            [ -d "$prof$id" ] && continue
            mkdir -p "$prof$id" && made=$((made+1))
        done
    done
    echo "created $made missing save directories under $BULK/Data/Local"
    exit 0
fi

# A LIST IN A FILE, for the viewer's game library.
#
# Ticking twenty titles and pressing Install is an ordinary thing to do there,
# and twenty paths — each of which may contain spaces, brackets and accented
# characters — do not belong on a command line. One path per line, no quoting
# rules to get wrong, and the file is still sitting there afterwards if an
# install needs explaining.
if [ "${1:-}" = "--from-list" ]; then
    LIST="${2:-}"
    [ -f "$LIST" ] || { echo "no such list: $LIST" >&2; exit 2; }
    set --
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        set -- "$@" "$line"
    done < "$LIST"
    [ $# -gt 0 ] || { echo "nothing listed in $LIST" >&2; exit 2; }
    echo "installing $# title(s)"
fi

# IS THIS A DIDJ PACKAGE? -> 0 if yes.
#
# Didj games arrive here because this is the installer the viewer's "Install
# .tar directly" runs, and a user with a Didj dump has no reason to know it is a
# different kind of package. They need a conversion this script does not do — see
# tools/install-didj.sh — so recognise them and hand them over rather than
# installing something that will not launch.
#
# The test is Device="Didj" in a meta.inf, read from either archive shape: the
# community's Didj dumps circulate as .zip while everything else here is .tar.
is_didj() {                         # $1 = archive
    local list m meta
    if [ "$(head -c2 "$1" 2>/dev/null)" = "PK" ]; then
        command -v unzip >/dev/null 2>&1 || return 1
        list="$(unzip -Z1 "$1" 2>/dev/null)"
    else
        list="$(tar tf "$1" 2>/dev/null)"
    fi
    m="$(grep -E '(^|/)meta\.inf$' <<<"$list" | grep -v '/DAmeta\.inf$' | head -1)"
    [ -n "$m" ] || return 1
    if [ "$(head -c2 "$1" 2>/dev/null)" = "PK" ]; then
        meta="$(unzip -p "$1" "$m" 2>/dev/null)"
    else
        meta="$(tar xOf "$1" "$m" 2>/dev/null)"
    fi
    grep -qE '^[[:space:]]*Device[[:space:]]*=[[:space:]]*"Didj"' <<<"$meta"
}

count=0; total=$#
for tar in "$@"; do
    count=$((count+1))
    [ -f "$tar" ] || { echo "no such file: $tar" >&2; continue; }
    if is_didj "$tar"; then
        # DELEGATE, and let it report — including its own progress header, which
        # is why this returns before the one below is printed. install-didj.sh
        # refuses with a message naming the missing step when the compatibility
        # files are not there, which is the one thing a user in this position
        # needs told.
        "$HERE/install-didj.sh" "$tar" || true
        continue
    fi
    # The viewer shows these lines as progress; with a batch of thirty, "which
    # one is it on" is the only question anyone has.
    echo "[$count/$total] $(basename "$tar"):"
    # Every meta.inf in the archive is a package. Multi-package backups bundle
    # a shared library pack alongside the game.
    tar tf "$tar" 2>/dev/null | grep -E '(^|/)meta\.inf$' | while read -r m; do
        install_one "$tar" "$m"
    done
done

echo
echo "installed into $BULK"
