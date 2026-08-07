#!/usr/bin/env python3
"""Ask a running VideoDaemon to play one of the system videos.

This exists because "make FMV work" had no reproduction. Every title that
plays video is expensive to reach, so the video path was only ever exercised
by accident. VideoDaemon is already running under every Tadpole boot and can
play a four-second clip on demand — that is the whole experiment.

    ./tools/vdplay.py transition        # 4.4s, the longest of the three
    ./tools/vdplay.py startup
    ./tools/vdplay.py shutdown          # NOTE: VideoDaemon exits afterwards

THE PROTOCOL, read out of the binary rather than guessed
    VideoDaemon listens on the AF_UNIX stream socket /tmp/video_events_socket
    (CreateListeningSocket + accept + recv). It recv()s exactly 8 bytes and
    switches on the SECOND word:

        4 -> /var/sounds/ShutdownVideo.ogg,   then the daemon shuts down
        5 -> /var/sounds/TransitionVideo.ogg
        6 -> /var/sounds/StartupVideo.ogg
        anything else -> "VideoDaemon: Invalid Request!"

    The first word is not examined; it is presumably an event id from the
    Brio event system. Anything less than 8 bytes is ignored and the daemon
    keeps reading, so a short write silently does nothing.

    The socket lives in the HOST's /tmp, not the sysroot: VideoDaemon runs
    under qemu-user and the shim's sysroot-first translation covers open(),
    not bind(). That is why this script can be plain host Python.
"""
import socket
import struct
import sys

REQUESTS = {"shutdown": 4, "transition": 5, "startup": 6}
SOCK = "/tmp/video_events_socket"


def main():
    args = sys.argv[1:]
    what = args[0] if args else "transition"
    path = SOCK
    if "-s" in args:
        path = args[args.index("-s") + 1]

    if what not in REQUESTS:
        sys.stderr.write("usage: vdplay.py [%s] [-s SOCKET]\n"
                         % "|".join(REQUESTS))
        return 2

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
    except OSError as e:
        sys.stderr.write(
            "vdplay: cannot reach VideoDaemon on %s (%s).\n"
            "        It is started by tadpole.sh's `ui` mode; check it is "
            "running with `pgrep -f VideoDaemon`.\n" % (path, e))
        return 1

    # NOTE: two Tadpole instances share this one host path, so a second boot
    # rebinds it and the first daemon is left listening on an orphaned inode.
    # If a request seems to reach nobody, check `ss -xl | grep video` for more
    # than one listener.
    s.sendall(struct.pack("<II", 0, REQUESTS[what]))
    s.close()
    print("sent %s (code %d) to %s" % (what, REQUESTS[what], path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
