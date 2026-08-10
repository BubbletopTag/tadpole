#!/usr/bin/env python3
# Tadpole — create a player profile, so the device does not have to.
#
#   tools/make-profile.py --name "Ada" --grade 2
#   tools/make-profile.py --name "Ada" --grade 2 --picture face.jpg
#   tools/make-profile.py --list
#
# This is tools/make-profile.sh as one program, for the same reason
# install-firmware.py exists: Windows has no shell to run the .sh. The shell
# version remains the Linux entry point and is unchanged. Every format
# decision below was read off a working device's filesystem, not guessed, and
# the comments explaining them are the shell script's — they are the valuable
# part and they did not change by being translated.
#
# WHY THIS EXISTS. A freshly installed system boots to Create Profile and
# stops there: the screen draws, and nothing you press gets past it. That is a
# real bug worth fixing one day, and it is also completely in the way — with no
# profile there is no home screen, no games, nothing. A profile is five lines
# of text in a file, so Tadpole writes them and the device finds a profile
# already there.
#
# WHAT A PROFILE IS:
#
#   LF/Bulk/Data/Local/<slot>/                 slot is 0..3, plus "All"
#     profile.dsc            ID, Name, Points, Grade, NumLoginsSinceLastConnect
#     profile_private.dsc    ProfilePicture=<path>
#     ProfilePicture/        holding ProfilePicture.jpg
#     PAD2-0x1F1E0002-100000/UIData.json       the shell's own per-profile state
#     <PackageID>/                             one save directory per title
#
# THE PICTURE IS A .jpg AND THE NAME IS NOT NEGOTIABLE: CreateProfile.swf
# carries the string "ProfilePicture.jpg" and the plugin exposes
# SetProfilePicture(ustring), so the shell looks for that name. A .png copied
# in under that name would be loaded as a JPEG and fail.
#
# Numbers are written as 0x%08X because that is the format every profile.dsc
# on the device uses; the shell parses them either way, but matching what the
# device writes means a profile made here is indistinguishable from one made
# there.

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.environ.get("TADPOLE_PROJECT") or os.path.dirname(HERE)
SYSROOT = os.environ.get("TADPOLE_SYSROOT") or os.path.join(PROJ, "runtime", "sysroot")
LOCAL = os.path.join(SYSROOT, "LF", "Bulk", "Data", "Local")
SHELL_PKG = "PAD2-0x1F1E0002-100000"


def read_dsc(path):
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def cmd_list():
    if not os.path.isdir(LOCAL):
        return 0
    for slot in sorted(os.listdir(LOCAL)):
        d = os.path.join(LOCAL, slot)
        if not os.path.isfile(os.path.join(d, "profile.dsc")):
            continue
        p = read_dsc(os.path.join(d, "profile.dsc"))
        print("  slot %-4s %-20s grade %s"
              % (slot, p.get("Name", ""), p.get("Grade", "")))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--name", default="")
    ap.add_argument("--grade", default="1")
    ap.add_argument("--slot", default="")
    ap.add_argument("--picture", default="")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv[1:])

    if not os.path.isdir(os.path.join(SYSROOT, "LF", "Bulk")):
        print("no system files at %s — install firmware first." % SYSROOT,
              file=sys.stderr)
        return 1

    if args.list:
        return cmd_list()

    if not args.name:
        print("usage: make-profile.py --name NAME [--grade N] [--picture FILE.jpg]",
              file=sys.stderr)
        return 2

    # The device's own screens accept a short name; keep the same shape rather
    # than letting a 200-character name into a fixed-width UI.
    name = args.name.replace("\n", "").replace("\r", "")[:20]

    try:
        grade = int(args.grade)
    except ValueError:
        grade = 1
    grade = max(0, min(12, grade))

    # FIRST FREE SLOT, unless asked for one. Overwriting slot 0 because it is
    # the obvious number would quietly replace whoever is already using it.
    slot = args.slot
    if not slot:
        for s in ("0", "1", "2", "3"):
            if not os.path.isfile(os.path.join(LOCAL, s, "profile.dsc")):
                slot = s
                break
        if not slot:
            print("all four profile slots are in use.", file=sys.stderr)
            return 1

    d = os.path.join(LOCAL, slot)
    for sub in (os.path.join(d, SHELL_PKG), os.path.join(d, "ProfilePicture"),
                os.path.join(LOCAL, "All", SHELL_PKG)):
        os.makedirs(sub, exist_ok=True)

    with open(os.path.join(d, "profile.dsc"), "w", encoding="utf-8", newline="\n") as f:
        f.write("ID=0x%08X\nName=%s\nPoints=0x%08X\nGrade=0x%08X\n"
                "NumLoginsSinceLastConnect=0x%08X\n" % (int(slot), name, 0, grade, 0))

    picpath = ""
    if args.picture:
        if not os.path.isfile(args.picture):
            print("  no such picture: %s (profile created without one)"
                  % args.picture, file=sys.stderr)
        elif args.picture.lower().endswith((".jpg", ".jpeg")):
            shutil.copyfile(args.picture,
                            os.path.join(d, "ProfilePicture", "ProfilePicture.jpg"))
            picpath = "/LF/Bulk/Data/Local/%s/ProfilePicture/ProfilePicture.jpg" % slot
        else:
            # Saying why beats copying a PNG to a .jpg name and leaving the
            # shell to fail at loading it.
            print("  picture must be a .jpg — %s was not copied"
                  % os.path.basename(args.picture), file=sys.stderr)

    with open(os.path.join(d, "profile_private.dsc"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("ProfilePicture=%s\n" % picpath)

    # The shell keeps per-profile state here and expects the directory to exist.
    uidata = os.path.join(d, SHELL_PKG, "UIData.json")
    if not os.path.isfile(uidata):
        with open(uidata, "w", encoding="utf-8", newline="\n") as f:
            f.write('{"_connectAlreadyPlayed": false}\n')

    # EVERY INSTALLED TITLE NEEDS ITS SAVE DIRECTORY under the new profile, and
    # nothing creates them on demand — a title whose save area is missing hangs
    # on a white screen with no message. install-game.py already knows how; a
    # profile made after the games were installed would otherwise start out
    # broken. sys.executable, because the Python that ships with Tadpole on
    # Windows is not on PATH and answers to no name.
    fixer = os.path.join(HERE, "install-game.py")
    if os.path.isfile(fixer):
        env = dict(os.environ)
        env["TADPOLE_BULK"] = os.path.join(SYSROOT, "LF", "Bulk")
        env["TADPOLE_BASE"] = os.path.join(SYSROOT, "LF", "Base")
        try:
            subprocess.call([sys.executable, fixer, "--fix-saves"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            pass

    print("  profile %s: %s, grade %d%s"
          % (slot, name, grade, ", with a picture" if picpath else ""))
    print("  %s" % d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
