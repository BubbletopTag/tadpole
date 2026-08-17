#!/usr/bin/env python3
"""make-egl-real.py — the stock libEGL, renamed so our impersonator can sit on
top of it, and repaired where the vendor's own linker got the file wrong.

    ./tools/make-egl-real.py <rootfs>/usr/lib/libEGL.so runtime/shimlibs-egl/libEGLreal.so

WHY A COPY AT ALL. The shim cannot use LD_PRELOAD (uClibc is built without it),
so it impersonates a library the target already links, and on every Qt LeapPad
the one library every module links is libEGL.so. But libEGL is not only EGL —
it also DEFINES globals that the GL driver imports, so replacing it outright
kills every guest that loads the GL stack. The stock library is therefore kept
in the process underneath ours, under a name that cannot collide.

TWO EDITS, both to the copy, neither to rootfs/.

1. libdl.so.0 -> libdl.so.9 in NEEDED. Same length, so it patches in place.
   Without it a SECOND copy of the framebuffer shim loads underneath the first,
   with its own static state. (No-op on a firmware whose libEGL does not link
   libdl — the LeapPad3's Mali build does not, so nothing is rewritten there.)

2. .dynsym's sh_info, on the LeapPad3 only, where it is simply WRONG.

   sh_info on a symbol table means "index of the first non-local symbol", and
   the ELF spec requires every STB_LOCAL symbol to precede it. The LeapPad3's
   libEGL.so is ARM's Mali binary, and it declares sh_info = 3 while symbols 3
   and 4 are still local:

       3: 0012aca0  0 NOTYPE  LOCAL  DEFAULT  ABS __exidx_start
       4: 0012acc0  0 NOTYPE  LOCAL  DEFAULT  ABS __exidx_end
       5: 00053a18 88 FUNC    GLOBAL DEFAULT   10 _gles2_program_...

   The device's own uClibc loader never looks, so the bug shipped. lld does
   look, and refuses to link against the file at all:

       ld.lld: error: invalid local symbol '__exidx_start' in global part
                      of symbol table

   Raising sh_info to 5 states what is already true of the symbol order. It
   moves no bytes and rebinds nothing — the alternative, promoting the two to
   STB_GLOBAL, would make a library's private unwind-table bounds preemptible
   by anything else that defines the same names.
"""
import pathlib
import struct
import sys

SHT_DYNSYM = 11
STB_LOCAL = 0
SHDR_SIZE = 40          # Elf32_Shdr
SYM_SIZE = 16           # Elf32_Sym
SH_INFO_OFF = 28        # offset of sh_info within Elf32_Shdr


def dynsym_sections(b):
    """-> [(shdr_file_offset, sh_offset, sh_size, sh_info)] for each SHT_DYNSYM."""
    if b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 1:
        sys.exit("not a 32-bit little-endian ELF")
    e_shoff, = struct.unpack_from("<I", b, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", b, 0x2E)
    if e_shentsize != SHDR_SIZE:
        sys.exit(f"unexpected section header size {e_shentsize}")
    out = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from("<I", b, off + 4)
        if sh_type != SHT_DYNSYM:
            continue
        sh_offset, sh_size = struct.unpack_from("<II", b, off + 16)
        sh_info, = struct.unpack_from("<I", b, off + SH_INFO_OFF)
        out.append((off, sh_offset, sh_size, sh_info))
    return out


def first_global(b, sh_offset, sh_size):
    """-> index of the first non-STB_LOCAL symbol, and whether any local
    follows it (in which case sh_info alone cannot describe the table)."""
    n = sh_size // SYM_SIZE
    first = n
    trailing_local = False
    for i in range(n):
        st_info = b[sh_offset + i * SYM_SIZE + 12]
        local = (st_info >> 4) == STB_LOCAL
        if local:
            if i > first:
                trailing_local = True
        elif i < first:
            first = i
    return first, trailing_local


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    src, dst = pathlib.Path(argv[1]).resolve(), pathlib.Path(argv[2])
    if not src.exists():
        sys.exit(f"no libEGL.so at {src}")
    b = bytearray(src.read_bytes())

    n = b.count(b"libdl.so.0\x00")
    if n:
        b = bytearray(bytes(b).replace(b"libdl.so.0\x00", b"libdl.so.9\x00"))

    fixed = 0
    for shdr_off, sh_offset, sh_size, sh_info in dynsym_sections(b):
        want, trailing = first_global(b, sh_offset, sh_size)
        if trailing:
            # Locals on both sides of the boundary: sh_info cannot express
            # that, and reordering the table would invalidate every relocation
            # that indexes it. Say so rather than write a file that is wrong
            # in a new way.
            sys.exit(f"{src.name}: .dynsym has local symbols after index "
                     f"{want}; sh_info cannot be repaired by itself")
        if want != sh_info:
            struct.pack_into("<I", b, shdr_off + SH_INFO_OFF, want)
            fixed += 1
            print(f"  .dynsym sh_info {sh_info} -> {want} "
                  f"(symbols {sh_info}..{want - 1} are local)")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(b))
    print(f"wrote {dst} from {src.name}"
          f"{'' if n else '  (no libdl.so.0 to rename)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
