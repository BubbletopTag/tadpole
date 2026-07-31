#!/usr/bin/env python3
"""lfsh — run commands on a LeapPad over its busybox telnetd.

The device runs telnetd with auto-login to a root shell (enabled by
/flags/developer), so there is no authentication step. Python 3.13 removed
telnetlib, so this speaks just enough of the protocol itself: refuse every
option negotiation, then treat the stream as raw bytes.

Commands are delimited with a sentinel echo rather than by matching the shell
prompt, which is far more robust when output contains '#'.

    tools/lfsh.py 192.168.0.111 "cat /proc/mtd"
    tools/lfsh.py 192.168.0.111 -f cmds.txt
    tools/lfsh.py 192.168.0.111 --get /proc/mtd out.txt      # text-safe pull
"""

import argparse
import socket
import sys
import time
import uuid

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class LeapShell:
    def __init__(self, host, port=23, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        # let the shell come up and drain the banner
        self._drain(1.5)

    # -- protocol ---------------------------------------------------------
    def _filter(self, data):
        """Strip telnet negotiation, refusing every option."""
        out, reply, i = bytearray(), bytearray(), 0
        while i < len(data):
            b = data[i]
            if b != IAC:
                out.append(b)
                i += 1
                continue
            if i + 1 >= len(data):
                break
            cmd = data[i + 1]
            if cmd in (DO, DONT, WILL, WONT):
                if i + 2 >= len(data):
                    break
                opt = data[i + 2]
                # say no to everything
                reply += bytes((IAC, WONT if cmd in (DO, DONT) else DONT, opt))
                i += 3
            elif cmd == SB:
                j = data.find(bytes((IAC, SE)), i)
                i = len(data) if j < 0 else j + 2
            elif cmd == IAC:
                out.append(IAC)
                i += 2
            else:
                i += 2
        if reply:
            try:
                self.sock.sendall(bytes(reply))
            except OSError:
                pass
        return bytes(out)

    def _drain(self, seconds):
        end = time.time() + seconds
        self.sock.settimeout(0.3)
        while time.time() < end:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buf += self._filter(chunk)
            except socket.timeout:
                pass
            except OSError:
                break
        self.buf = b""

    # -- commands ---------------------------------------------------------
    def run(self, cmd, timeout=120):
        """Run one command, return its stdout+stderr as bytes."""
        tag = uuid.uuid4().hex[:10].upper()
        self.buf = b""
        self.sock.settimeout(1.0)
        # The shell echoes back what we type, so a naive sentinel matches the
        # echo instead of the result. Split the literal with an empty string:
        # the echoed line reads  LF""SH<tag>  while the OUTPUT reads LFSH<tag>,
        # so searching for the joined form can only hit the real thing.
        self.sock.sendall(f'{cmd} 2>&1; echo "LF""SH{tag}"$?\n'.encode())

        marker = f"LFSH{tag}".encode()
        end = time.time() + timeout
        while time.time() < end:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buf += self._filter(chunk)
                if marker in self.buf:
                    break
            except socket.timeout:
                continue
            except OSError:
                break

        text = self.buf
        idx = text.find(marker)
        if idx < 0:
            return text  # timed out; hand back whatever arrived
        body = text[:idx]
        # drop the echoed command line
        nl = body.find(b"\n")
        if nl >= 0 and cmd.encode()[:20] in body[:nl + 1]:
            body = body[nl + 1:]
        return body.replace(b"\r\n", b"\n").rstrip(b"\n")

    def close(self):
        try:
            self.sock.sendall(b"exit\n")
        except OSError:
            pass
        self.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("cmd", nargs="?")
    ap.add_argument("-f", "--file", help="file of commands, one per line")
    ap.add_argument("-o", "--out", help="write output here instead of stdout")
    ap.add_argument("-t", "--timeout", type=int, default=120)
    args = ap.parse_args()

    sh = LeapShell(args.host)
    chunks = []
    try:
        cmds = []
        if args.file:
            cmds = [l.rstrip("\n") for l in open(args.file)
                    if l.strip() and not l.startswith("#")]
        elif args.cmd:
            cmds = [args.cmd]
        else:
            ap.error("give a command or -f")

        for c in cmds:
            if len(cmds) > 1:
                chunks.append(f"\n===== {c} =====\n".encode())
            chunks.append(sh.run(c, timeout=args.timeout))
            chunks.append(b"\n")
    finally:
        sh.close()

    data = b"".join(chunks)
    if args.out:
        open(args.out, "wb").write(data)
        print(f"wrote {args.out} ({len(data)} bytes)")
    else:
        sys.stdout.buffer.write(data)


if __name__ == "__main__":
    main()
