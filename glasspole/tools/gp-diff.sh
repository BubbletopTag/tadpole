#!/bin/bash
# Glasspole against qemu-arm, on the guest's own busybox.
#
#   ./glasspole/tools/gp-diff.sh              run every case
#   ./glasspole/tools/gp-diff.sh id free      run the named cases
#   GP_VERBOSE=1 ./glasspole/tools/gp-diff.sh show the diff for each failure
#   GP_XFAIL=1   ./glasspole/tools/gp-diff.sh run the by-design cases too
#
# WHY THIS EXISTS. On Linux qemu-arm sits next to glasspole as a known-correct
# implementation, and the stock rootfs ships a busybox that exercises most of
# the syscall surface without anyone writing a test. Pointing both emulators at
# the same binary and diffing the output is the cheapest oracle this project
# will ever have: within minutes of existing it found a struct stat64 that was
# 96 bytes where the ARM EABI says 104, a shell that spins forever on a missing
# dup2, and directories reporting st_nlink 1 — all of which had survived a
# working home screen.
#
# So: every fix to the syscall layer earns a case here, and the suite is the
# regression net for the next one. A case that PASSES is worth as much as one
# that fails; it is what stops the next fix breaking what already worked.
#
# ARGV[0] IS THE RESOLVED HOST PATH IN BOTH, and it has to be resolved rather
# than merely absolute. busybox quotes argv[0] in its own diagnostics, and
# qemu-arm realpath()s the program before handing it over — so
# $SYSROOT/bin/busybox, which is a symlink into rootfs/<version>/ubi_rfs,
# reaches the guest under two different names and EVERY case fails on a line
# that has nothing to do with the emulator. readlink -f up front makes the two
# strings the same one. An absolute host path falls through HostPath's sysroot
# lookup unchanged, so glasspole is content with it too.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"

SYSROOT="${TADPOLE_SYSROOT:-$PROJ/runtime/sysroot}"
GLASSPOLE="${GLASSPOLE:-$PROJ/glasspole/build/glasspole}"
. "$PROJ/tools/lib-deps.sh"
QEMU="${TADPOLE_QEMU_REF:-$(tad_qemu || true)}"

BB="$(readlink -f "$SYSROOT/bin/busybox" 2>/dev/null || echo "$SYSROOT/bin/busybox")"

# A CASE THAT HANGS MUST NOT HANG THE SUITE, and one already does: busybox ash
# meets the missing dup2, gets ENOSYS from a redirection, and retries forever —
# 21.9 MB of "sh: 0: Function not implemented" in twenty seconds. An emulator
# under test is exactly the thing that spins, so bound every case in both time
# and size, and report the timeout as the failure it is.
GP_TIMEOUT="${GP_TIMEOUT:-20}"
GP_MAXBYTES="${GP_MAXBYTES:-262144}"

for req in "$QEMU:qemu-arm (install qemu-user or run tools/fetch-deps.sh)" \
           "$GLASSPOLE:glasspole (ninja -C glasspole/build)" \
           "$BB:the guest's busybox (runtime/setup-sysroot.sh)"; do
    p="${req%%:*}"; what="${req#*:}"
    if [ -z "$p" ] || [ ! -x "$p" ]; then
        echo "gp-diff: missing $what" >&2
        exit 2
    fi
done

# ---- the cases ------------------------------------------------------------
#
#   name | xfail-reason | arguments
#
# An empty middle field means the two emulators MUST agree. A non-empty one
# means they are expected to differ and says why — those are skipped unless
# GP_XFAIL=1, because a suite whose green light includes known failures is not
# a green light. Every by-design divergence has to earn its line of prose here;
# "it has always differed" is not a reason.
#
# Each case notes what it reaches. Adding one with no note is how a suite turns
# into a pile.
CASES=(
    'ls-l||ls -l /bin/busybox'                    # stat64: st_size, st_nlink, uid/gid, times
    'ls-la||ls -la /LF/Bulk/ProgramFiles'         # getdents past one buffer: 110 entries
    'ls-root||ls /'                               # getdents, and the sysroot symlink farm
    'stat-dir||ls -ld /LF/Bulk'                   # stat64 on a directory: st_nlink is not 1
    'id|uid and gid match; the SUPPLEMENTARY GROUPS cannot. qemu passes the host account through, so it reports whatever groups the developer happens to be in; glasspole reports one fixed console identity. Run it and read the first two fields|id'
    'date||date -u -d @1000000000'                # a fixed instant, so it is comparable
    'md5sum||md5sum /bin/busybox'                 # read() over a whole file
    'wc||wc -c /bin/busybox'                      # read() to EOF, and the count
    'head||head -c 64 /bin/busybox'               # short reads, binary through stdout
    'cat||cat /etc/profile'                       # the ordinary path
    'ulimit||sh -c ulimit'                        # ugetrlimit through a shell
    'pwd|glasspole VIRTUALISES the working directory and qemu does not, so pwd is / here and the host'\''s directory there. This case passed until stat64 was fixed, and only by accident: st_ino sat outside the short struct, ash'\''s check that $PWD and . are the same directory compared the same stack garbage twice and agreed|sh -c pwd'
    'readlink|matching qemu means resolving a guest path to an absolute HOST path and handing it to the guest, which is the one thing the sysroot exists to prevent. EINVAL — "not a link" — makes uClibc fall through instead. A real answer needs gp_readlink in host.h and both backends|readlink -f /bin/busybox'
    'redir||sh -c "echo hello; echo err >&2"'     # dup2 — the shell's redirection
    'du|the same dangling-symlink problem as rc-d, reached through a walk: /etc/rc.d holds links whose targets the sysroot does not carry, lstat64 follows them, and each answers ENOENT where qemu reports a link|du -s /etc'
    # -u _ because `_` is the invoking SHELL's bookkeeping variable, set to
    # whichever emulator binary it just ran, so it can never match and says
    # nothing about either. Everything else here must.
    'env||env -u _'                               # the environment the guest was handed
    'pipeline|process creation is a design boundary, not a gap: host.h has no fork, so a shell PIPELINE cannot work and "sh: pipe call failed" is the honest answer. Kept as a case so the day that changes, this notices|sh -c "env | sort"'
    'uname|the console'\''s kernel is reported, not the host'\''s — deliberate, and the guest checks the release string|uname -a'
    'df|statfs describes the CONSOLE'\''s storage, not the host'\''s — deliberate, so the guest does not believe it has 2 TB|df -h /'
    'free|sysinfo describes the console'\''s 128 MB, not the host'\''s — deliberate. The SWAP figures are a real bug inside a deliberate difference: read them, do not diff them|free'
    'rc-d|lstat64 FOLLOWS symlinks so the sysroot'\''s symlink farm looks like a filesystem, which turns a DANGLING link into ENOENT where qemu reports the link. Needs a gp_lstat in host.h and both backends|ls -l /etc/rc.d'
)

# emulator-command... outfile — combined output, bounded in time and size, with
# glasspole's own diagnostics stripped. Those go to the host's stderr and the
# guest cannot reach them, so they are not part of what is being compared.
#
# Written to a FILE rather than captured in a variable: several of these cases
# put binary on stdout, and command substitution eats NUL bytes and trailing
# newlines. `head -c 64 /bin/busybox` is in the suite precisely because it is
# binary, so the rig has to be able to carry it.
# The two streams are kept APART and only joined at the end, which is not
# fussiness. glasspole's own diagnostics go to stderr and are filtered out by
# the `[glasspole] ` prefix — but `head -c 64 /bin/busybox` puts binary on
# stdout with no trailing newline, so a merged stream leaves "[glasspole] guest
# exited" welded onto the end of the last data line where no line-oriented
# filter can reach it. Separate captures, one filter on stderr, then join.
run_one() {
    local out="${!#}"
    set -- "${@:1:$#-1}"
    timeout "$GP_TIMEOUT" "$@" > "$out.out" 2> "$out.err"
    local rc=$?
    {
        head -c "$GP_MAXBYTES" "$out.out"
        printf '\n--- stderr ---\n'
        sed -e '/^\[glasspole\] /d' "$out.err" | head -c "$GP_MAXBYTES"
        [ "$rc" = 124 ] && printf '<<gp-diff: killed after %ss>>\n' "$GP_TIMEOUT"
    } > "$out"
    rm -f "$out.out" "$out.err"
    return 0
}

only=("$@")
want() {
    [ ${#only[@]} -eq 0 ] && return 0
    local n
    for n in "${only[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

pass=0; fail=0; skip=0; failed=()
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for case in "${CASES[@]}"; do
    name="${case%%|*}"
    rest="${case#*|}"
    xfail="${rest%%|*}"
    args="${rest#*|}"
    want "$name" || continue

    if [ -n "$xfail" ] && [ -z "${GP_XFAIL:-}" ] && [ ${#only[@]} -eq 0 ]; then
        printf '  --    %-10s by design: %s\n' "$name" "${xfail:0:60}…"
        skip=$((skip + 1))
        continue
    fi

    # eval, so a case can carry quoting of its own — sh -c "echo hi" has to
    # reach busybox as one argument, not three.
    eval "set -- $args"
    run_one "$QEMU"      -L "$SYSROOT" "$BB" "$@" "$tmp/q"
    run_one "$GLASSPOLE" -L "$SYSROOT" "$BB" "$@" "$tmp/g"

    if cmp -s "$tmp/q" "$tmp/g"; then
        printf '  ok    %-10s %s\n' "$name" "$args"
        pass=$((pass + 1))
    else
        printf '  FAIL  %-10s %s\n' "$name" "$args"
        [ -n "$xfail" ] && printf '        (by design: %s)\n' "$xfail"
        fail=$((fail + 1))
        failed+=("$name")
        if [ -n "${GP_VERBOSE:-}" ]; then
            diff -u --label "qemu-arm" --label "glasspole" "$tmp/q" "$tmp/g" \
                | sed -e 's/^/        /' | head -60
        fi
    fi
done

echo
[ "$skip" -gt 0 ] && echo "gp-diff: $skip case(s) differ by design — GP_XFAIL=1 to see them"
if [ "$fail" -eq 0 ]; then
    echo "gp-diff: $pass/$pass identical to qemu-arm"
    exit 0
fi
echo "gp-diff: $pass ok, $fail differ — ${failed[*]}"
echo "         GP_VERBOSE=1 $0 ${failed[*]}   to see them"
exit 1
