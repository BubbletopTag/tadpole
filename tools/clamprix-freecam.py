#!/usr/bin/env python3
"""Turn SpongeBob: The Clam Prix into a track flycam.

    ./tools/clamprix-freecam.py --check   <path-to-App.so>
    ./tools/clamprix-freecam.py           <path-to-App.so>
    ./tools/clamprix-freecam.py --revert  <path-to-App.so>

WHAT THIS IS. Virtuos shipped the retail Clam Prix with its development
cameras still linked in and its symbol table intact. `CameraManager::init()`
builds FOUR cameras and leaves them all live:

    mCameraList[0]  DebugCamera
    mCameraList[1]  FreeCamera     <- walk / yaw / pitch / roll / processInput
    mCameraList[2]  RaceCamera     <- the one the retail game selects
    mCameraList[3]  RaceCamera

`FreeCamera` is a complete flycam: it reads the button mask out of the
InputManager singleton every frame and translates and rotates itself. Nothing
in the retail build ever selects it — `CameraManager::setCamera()` is called
from exactly two places, and the reachable one asks for camera 2.

So this is not a reimplementation of anything. It flips the argument.

WHY A PATCH SCRIPT AND NOT A PATCHED BINARY. Clam Prix is still LeapFrog's
copyright however thoroughly the servers have forgotten it. What is
distributed here is a list of byte offsets and the ARM instruction words to
write there — this file contains no LeapFrog code, and does nothing at all
without a copy of App.so you already own. `ORIG` is a few bytes per site, and
is here so the script can REFUSE to write to the wrong place rather than
corrupt somebody's game.

The file it patches lives at

    <install>/LF/Bulk/ProgramFiles/LST3-0x00180025-000000/App.so
"""
import argparse, hashlib, os, shutil, struct, sys

# The retail build this was derived from. meta.inf Version="1.2.1.0",
# PartNumber 152-12859, Developer="Virtuos".
RETAIL_SHA256 = "3f02360b49dbb9681e1e5763e1d4c253728ecfb8c3149e33e78116f5872f6db2"
RETAIL_SIZE   = 9779634

# THE PATCH TABLE.
#
# `off` is a FILE offset, which for this binary is also the virtual address:
# its first LOAD segment maps vaddr 0 at file offset 0, so .text at 0x1cd7b0
# sits at 0x1cd7b0 in the file. Do not carry that assumption to another title.
#
# Each entry is (name, off, orig, new, why).
PATCHES = [
    (
        "camera",
        0x1D1E68,
        bytes.fromhex("0200a0e3"),      # mov r0, #2   -> CameraType 2, RaceCamera
        bytes.fromhex("0100a0e3"),      # mov r0, #1   -> CameraType 1, FreeCamera
        "Scene::load() picks the camera for a loaded track. Ask it for the "
        "FreeCamera that ArtViewState uses instead of the RaceCamera.",
    ),
    (
        "assert",
        0x1D0B8C,
        bytes.fromhex("3900000a"),      # beq  <assert>
        bytes.fromhex("0000a0e1"),      # mov  r0, r0   (the classic ARM nop)
        "Scene::PreRenderScene() does dynamic_cast<RaceCamera*> on the current "
        "camera and asserts the result — SR_Scene.cpp:545, 'RaceCameraRef'. It "
        "then ignores the cast and reads the view matrix from the BASE Camera "
        "at +132, so the check is the only thing in the way. Drop the branch, "
        "not the cast: the cast is harmless and r0 is dead after it.",
    ),
    (
        "input",
        0x293620,
        bytes.fromhex("81c9fceb"),      # bl CameraBehavior::update(long)
        bytes.fromhex("2242ffeb"),      # bl CameraManager::update()
        "Nothing in the retail build ever calls CameraManager::update(), which "
        "is the one function that would drive mCurrentCamera->processInput(). "
        "Car::updateAfterCollision() calls CameraBehavior::update() once a "
        "frame, guarded by the car actually owning one — that is the player's "
        "car and nobody else's. Redirect it. The RaceCamera stops being driven, "
        "which costs nothing when it is not the camera being rendered from.",
    ),
]


def pristine_digest(data):
    """sha256 of the file with every patch site put BACK to its retail bytes.

    Hashing the file as it sits would call an already-patched App.so an
    unknown build, which is both wrong and frightening — it reads as "your
    game is corrupt" when the truth is "you ran this yesterday". Revert into
    a scratch copy and hash that, so identity is a property of the title and
    not of whether the mod is currently on.
    """
    tmp = bytearray(data)
    for name, off, orig, new, why in PATCHES:
        if tmp[off:off + len(new)] == new:
            tmp[off:off + len(orig)] = orig
    return hashlib.sha256(bytes(tmp)).hexdigest()


def identify(data, path):
    """Refuse politely rather than corrupt a file we do not recognise."""
    if len(data) != RETAIL_SIZE:
        return ("%s is %d bytes; the retail Clam Prix App.so is %d. This is "
                "probably not the right file." % (path, len(data), RETAIL_SIZE))
    return None


def state_of(data):
    """Per patch: 'orig', 'patched', or 'unknown'."""
    out = []
    for name, off, orig, new, why in PATCHES:
        cur = data[off:off + len(orig)]
        out.append((name, "orig" if cur == orig else
                          "patched" if cur == new else "unknown", cur))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("appso", help="path to the title's App.so")
    ap.add_argument("--check", action="store_true", help="report and change nothing")
    ap.add_argument("--revert", action="store_true", help="put the retail bytes back")
    ap.add_argument("--out", help="write here instead of patching in place")
    ap.add_argument("--force", action="store_true",
                    help="patch even if the file is not the build this was derived from")
    ap.add_argument("--only", help="comma-separated patch names, for bisecting a bad result")
    a = ap.parse_args()

    data = bytearray(open(a.appso, "rb").read())
    chosen = set(a.only.split(",")) if a.only else {p[0] for p in PATCHES}
    unknown = chosen - {p[0] for p in PATCHES}
    if unknown:
        sys.exit("no such patch: %s (have: %s)"
                 % (", ".join(sorted(unknown)), ", ".join(p[0] for p in PATCHES)))

    problem = identify(data, a.appso)
    if problem and not a.force:
        sys.exit("refusing: " + problem + "\n(--force overrides)")

    digest = pristine_digest(data)
    known = digest == RETAIL_SHA256
    states = state_of(data)
    print("%s\n  sha256 %s (unpatched)%s" % (a.appso, digest,
          "  retail 1.2.1.0 — recognised" if known else
          "  NOT the build this was derived from"))
    for name, st, cur in states:
        print("  %-10s %-8s bytes %s" % (name, st, cur.hex()))

    if a.check:
        return

    if any(st == "unknown" for _, st, _ in states) and not a.force:
        sys.exit("refusing: a patch site holds bytes that are neither the retail "
                 "instruction nor ours. Wrong build, or already modified.\n"
                 "(--force overrides)")

    want = "orig" if a.revert else "patched"
    todo = [(n, o, orig, new, w) for (n, o, orig, new, w), (_, st, _)
            in zip(PATCHES, states) if st != want and n in chosen]
    if not todo:
        print("\nnothing to do — already %s." % want)
        return

    for name, off, orig, new, why in todo:
        data[off:off + len(new)] = orig if a.revert else new
        print("\n%s @ %#x: %s -> %s\n  %s"
              % (name, off, (new if a.revert else orig).hex(),
                 (orig if a.revert else new).hex(), why))

    dst = a.out or a.appso
    if not a.out:
        # A BACKUP THE FIRST TIME ONLY. Patching twice must not overwrite the
        # backup with an already-patched file.
        bak = a.appso + ".orig"
        if not os.path.exists(bak):
            shutil.copy2(a.appso, bak)
            print("\nbacked up retail App.so -> %s" % bak)
    tmp = dst + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    shutil.copystat(a.appso, tmp)
    os.replace(tmp, dst)
    print("wrote %s" % dst)


if __name__ == "__main__":
    main()
