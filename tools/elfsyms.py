#!/usr/bin/env python3
"""Resolve an offset inside a guest .so or executable to a function name.

    from elfsyms import Symbols
    Symbols("App.so").lookup(0x1234)   -> ("CFoo::bar(int)", 0x40) or None

WHY THIS IS WORTH HAVING. The shim's crash reports name a library and an
offset — `App.so+0x0011f2c4` — because that is all a signal handler can safely
work out. Two crashes at the same offset are the same bug, which is enough to
CLUSTER them, and not nearly enough to fix one. The titles carry a full
.symtab: Clam Prix's App.so has 101,537 symbols in it. Reading that turns the
offset into a function name and a line of the report into a lead.

Pure stdlib, on purpose. This runs on a user's machine to triage a crash they
just had, and needing pyelftools for that would mean it usually does not run.

Demangling goes through c++filt when binutils is installed; without it the
mangled name is still far better than a hex offset, so its absence downgrades
the output rather than failing.
"""
import bisect
import os
import struct
import subprocess

__all__ = ["Symbols", "demangle"]

_DEMANGLE_CACHE = {}
_CXXFILT = None


def demangle(name):
    """Itanium C++ ABI name -> something readable, if c++filt is available."""
    global _CXXFILT
    if not name.startswith("_Z"):
        return name
    if name in _DEMANGLE_CACHE:
        return _DEMANGLE_CACHE[name]
    if _CXXFILT is None:
        _CXXFILT = False
        for exe in ("c++filt", "llvm-cxxfilt"):
            try:
                subprocess.run([exe, "--version"], capture_output=True, check=True)
                _CXXFILT = exe
                break
            except (OSError, subprocess.CalledProcessError):
                pass
    out = name
    if _CXXFILT:
        try:
            r = subprocess.run([_CXXFILT, name], capture_output=True, text=True)
            if r.returncode == 0 and r.stdout.strip():
                out = r.stdout.strip()
        except OSError:
            pass
    _DEMANGLE_CACHE[name] = out
    return out


class Symbols(object):
    """Sorted function symbols of one ELF, queryable by offset.

    32-bit little-endian ARM only — that is what every guest object is, and
    pretending otherwise would mean writing a general ELF reader for no reason.
    """

    def __init__(self, path):
        self.path = path
        self._addr = []          # sorted symbol values
        self._info = []          # (size, name) parallel to _addr
        try:
            self._load(path)
        except (OSError, struct.error, ValueError):
            self._addr, self._info = [], []

    def __bool__(self):
        return bool(self._addr)

    __nonzero__ = __bool__

    def _load(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            return                                   # not 32-bit LE ELF

        e_shoff, = struct.unpack_from("<I", data, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        if not e_shoff or not e_shnum:
            return

        def sh(i):
            o = e_shoff + i * e_shentsize
            # name type flags addr offset size link info align entsize
            return struct.unpack_from("<10I", data, o)

        syms = {}
        for i in range(e_shnum):
            s = sh(i)
            if s[1] in (2, 11):                      # SHT_SYMTAB, SHT_DYNSYM
                syms[s[1]] = (s[4], s[5], s[6], s[9])   # off size link entsize
        # PREFER .symtab. .dynsym holds only what is exported — for a C++ title
        # that is a small fraction, and the interesting frames are internal.
        chosen = syms.get(2) or syms.get(11)
        if not chosen:
            return
        off, size, link, entsize = chosen
        entsize = entsize or 16
        stroff, strsize = sh(link)[4], sh(link)[5]
        strtab = data[stroff:stroff + strsize]

        seen = {}
        for o in range(off, off + size, entsize):
            st_name, st_value, st_size, st_info = struct.unpack_from("<IIIB", data, o)
            if (st_info & 0xF) != 2:                 # STT_FUNC only
                continue
            if not st_value:
                continue
            end = strtab.find(b"\0", st_name)
            nm = strtab[st_name:end].decode("latin1", "replace")
            if not nm:
                continue
            # THUMB SYMBOLS HAVE BIT 0 SET. Leaving it on shifts every lookup
            # by one byte and puts addresses just before a function into the
            # one before it.
            v = st_value & ~1
            # Keep the entry with a real size when two share an address.
            if v not in seen or (st_size and not seen[v][0]):
                seen[v] = (st_size, nm)

        self._addr = sorted(seen)
        self._info = [seen[a] for a in self._addr]

    def lookup(self, offset):
        """-> (name, offset_into_function) or None."""
        if not self._addr:
            return None
        i = bisect.bisect_right(self._addr, offset) - 1
        if i < 0:
            return None
        base = self._addr[i]
        size, name = self._info[i]
        # A symbol with a size that the offset falls outside of is NOT a match:
        # saying "in foo+0x9000" when foo is 40 bytes long is worse than
        # admitting we do not know.
        if size and offset >= base + size:
            return None
        return name, offset - base


def find_object(name, roots=None):
    """Locate a guest object by basename under the installed packages."""
    roots = roots or []
    for r in roots:
        if not r or not os.path.isdir(r):
            continue
        for dirpath, _dirs, files in os.walk(r):
            if name in files:
                return os.path.join(dirpath, name)
    return None
