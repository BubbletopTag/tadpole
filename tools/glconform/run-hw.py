#!/usr/bin/env python3
"""run-hw.py — push glconform to a real LeapPad2 and run it there.

    ./tools/glconform/run-hw.py 192.168.0.111
    ./tools/glconform/run-hw.py 192.168.0.111 --keep     # leave the binary on

Pairs with run-emu.sh: the SAME binary, the real VR5 driver instead of our shim.
Developer mode (/flags/developer) gives both halves of what that needs — vsftpd
for the push and busybox telnetd for the run — so nothing here reimplements
either. The push is plain ftplib; the run goes through tools/lfsh.py, which
already knows how to drive that telnet auto-login shell.

VERIFIED ON HARDWARE, so these are facts rather than assumptions:
  - anonymous FTP login succeeds and lands at /
  - /tmp is writable and executable
  - the binary's DT_NEEDED list resolves against stock /usr/lib — see build.sh,
    which explains why it lists nine libraries instead of two

STDOUT AND STDERR ARE SEPARATED on the device, for the same reason run-emu.sh
does it: anything the GL stack writes to fd 2 would otherwise interleave
mid-line with glconform's buffered RESULT lines and split them in half.
"""
import argparse
import ftplib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))
LFSH = os.path.join(PROJ, "tools", "lfsh.py")
BIN = os.path.join(PROJ, "runtime", "glconform")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--user", default="anonymous")
    ap.add_argument("--password", default="")
    ap.add_argument("--remote-dir", default="/tmp")
    ap.add_argument("--timeout", type=int, default=75)
    ap.add_argument("--keep", action="store_true",
                    help="leave the binary and its stderr log on the device")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "hw.log"))
    args = ap.parse_args()

    if not os.path.exists(BIN):
        sys.exit("runtime/glconform not built — run ./tools/glconform/build.sh first")

    remote = args.remote_dir.rstrip("/") + "/glconform"
    errlog = args.remote_dir.rstrip("/") + "/glconform.err"

    print(f"ftp: {BIN} -> {args.host}:{remote}")
    ftp = ftplib.FTP()
    ftp.connect(args.host, timeout=20)
    ftp.login(args.user, args.password)
    try:
        ftp.cwd(args.remote_dir)
    except ftplib.error_perm as e:
        sys.exit(f"cwd to {args.remote_dir} failed: {e} (try a different --remote-dir)")
    with open(BIN, "rb") as f:
        ftp.storbinary("STOR glconform", f)
    ftp.quit()

    # One telnet round trip: chmod, run with the streams split, then print the
    # stderr log behind a marker this script can split on.
    # THE MARKER IS SPLIT ACROSS A SHELL CONCATENATION on purpose. busybox
    # telnetd echoes the command line back before running it, so a marker
    # written literally appears in the output BEFORE any real output does, and
    # splitting on its first occurrence puts the entire log on the wrong side of
    # the split. `"A""B"` reads as AB only after the shell has parsed it, so the
    # echoed command never contains the string being searched for. tools/lfsh.py
    # uses the same trick for its own end-of-output sentinel.
    cleanup = "" if args.keep else f"; rm -f {remote} {errlog}"
    cmd = (f"chmod +x {remote}; {remote} 2>{errlog}; echo RC=$?;"
           f' echo "__GLCONFORM""_STDERR__"; cat {errlog}{cleanup}')
    print("telnet: running")
    out = subprocess.run([sys.executable, LFSH, args.host, cmd,
                          "-t", str(args.timeout)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        sys.exit(f"lfsh.py failed (exit {out.returncode})")

    stdout, _, stderr = out.stdout.partition("__GLCONFORM_STDERR__")
    lines = [l for l in stdout.splitlines()
             if l.startswith(("META ", "EGLINIT ", "RESULT "))]

    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {args.out} ({len(lines)} lines)")

    if stderr.strip():
        errpath = os.path.splitext(args.out)[0] + ".gl.log"
        with open(errpath, "w") as f:
            f.write(stderr.lstrip("\r\n"))
        print(f"      {errpath} (device stderr)")

    if not any(l.startswith("META done=1") for l in lines):
        print("NOTE: no 'META done=1' — the run did not reach the end.\n"
              "      A 'can't resolve symbol' line in the stderr log means the\n"
              "      DT_NEEDED list in build.sh needs another library; uClibc\n"
              "      binds lazily, so that failure happens mid-run, not at load.",
              file=sys.stderr)
        print(out.stdout, file=sys.stderr)


if __name__ == "__main__":
    main()
