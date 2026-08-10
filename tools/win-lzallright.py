# lzallright, spelled in terms of python-lzo.
#
# SHIPPED AS lzallright.py INSIDE THE WINDOWS BUNDLE. tools/build-windows.sh
# copies this into the bundled interpreter's site-packages under that name; it
# is not imported by anything on Linux, where the real package is used.
#
# WHY THERE IS A SHIM AT ALL
# --------------------------
# The Windows bundle runs CPython 3.7.9, and that version is not a preference:
#
#   Python 3.8 loads every .pyd with LoadLibraryExW and the flags
#   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR.
#   On Windows 7 those flags do not exist unless KB2533623 is installed, and
#   LoadLibraryExW rejects them with ERROR_INVALID_PARAMETER — which surfaces,
#   unhelpfully, as:
#
#       ImportError: DLL load failed while importing _socket:
#                    The parameter is incorrect.
#
#   Every extension module fails, so the interpreter starts and then cannot
#   import sockets, bz2 or anything else with a .pyd. Python 3.7 loads them
#   the old way and works on an unpatched Windows 7. python37.dll does not
#   reference AddDllDirectory at all; python38.dll does.
#
# ubi_reader 0.8.9 is pure Python and parses cleanly against the 3.7 grammar,
# so it runs there happily — but it imports lzallright, whose only Windows
# wheel is cp38-abi3 and therefore cannot be used. python-lzo has a real cp37
# wheel. Same algorithm, different spelling, so this translates between them
# and ubi_reader never learns the difference.
#
# THE BYTE FORMAT IS NOT GUESSED. ubi_reader 0.8.2 — the last release before
# it moved to lzallright — called python-lzo exactly like this:
#
#     lzo.decompress(b''.join((b'\xf0', struct.pack('>I', unc_len), data)))
#
# 0xf0 marks an LZO1X stream with a known output size, followed by that size
# big-endian. This reproduces that call, so the two versions of ubi_reader
# decompress a UBIFS node the same way.

import struct

import lzo


class LZOError(Exception):
    """What ubi_reader catches. python-lzo raises lzo.error, which is not it."""


class LZOCompressor(object):
    @staticmethod
    def decompress(data, output_size_hint=0):
        # NOT A HINT, despite the name upstream chose: python-lzo needs the
        # exact uncompressed length in the header, and UBIFS always knows it
        # from the node it just read. Zero would mean "no idea", which this
        # format cannot express.
        try:
            return lzo.decompress(
                b"".join((b"\xf0", struct.pack(">I", output_size_hint), data)))
        except Exception as e:
            raise LZOError(str(e))
