#!/usr/bin/env python3
"""register-packages.py — put installed packages into the Ultra's package DBs.

    ./tools/register-packages.py                 register everything in Bulk
    ./tools/register-packages.py --list          show what is registered
    ./tools/register-packages.py --clean         start the DBs over

WHY THIS EXISTS, AND WHY THE LEAPPAD2 NEEDS NOTHING LIKE IT.

The LeapPad2's AppManager finds titles by SCANNING /LF/Bulk/ProgramFiles and
reading each meta.inf. Untar a package into place and it appears. The Ultra
does not scan. Its home screen asks liblfp, which asks two SQLite databases:

    /LF/Bulk/SharedPackageInfo.db     what is installed, and where
    /LF/Bulk/LocalPackageInfo.db      this device's view: icon, state, dates
                                      + ProfileAccess, which profiles may see it

Traced under strace, a booted MainPicker opens exactly those two files and
NEVER touches ProgramFiles. So a perfectly installed package that is not in
the databases does not exist as far as the home screen is concerned — which is
what an empty picker on a full Bulk partition actually means.

On hardware the databases arrive already populated on the Bulk partition (they
are in erootfs.md5 as ./LF/Bulk/*PackageInfo.db) and lfpkg maintains them from
then on. We install by untarring, so nothing maintains them, and there is no
Bulk dump to inherit them from.

THE SCHEMA IS NOT GUESSED. Every statement below is a literal string in
usr/lib/liblfp.so.1.0.0 — the two CREATE TABLEs, the two INSERTs and the
ProfileAccess table, copied verbatim rather than inferred from column use:

    CREATE TABLE Packages (PackageID TEXT, InstallDir TEXT, DeviceAccess
      NUMERIC, DeviceRequired NUMERIC, ProductID NUMERIC, InstalledVersion
      TEXT, Size NUMERIC, Type TEXT, DisplayName TEXT, CartBuddy NUMERIC)
    CREATE TABLE Packages (PackageID TEXT, PreviewIcon TEXT, Checksum TEXT,
      Hidden NUMERIC, MDLType TEXT, ParentApp NUMERIC, License NUMERIC, State
      NUMERIC, DeviceHidden NUMERIC, InstalledDate TEXT, LastPlayedDate TEXT,
      Type TEXT, CartBuddy NUMERIC, Redownload NUMERIC, DisplayName TEXT)
    CREATE TABLE ProfileAccess (PackageID TEXT, Slot NUMERIC)

They are not visible to `strings | grep -i "create table"` on the libraries you
would first reach for — only liblfp has them, and finding it meant asking which
library links libQtSqlE at all.
"""
import argparse
import os
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

SHARED_DDL = ("CREATE TABLE Packages (PackageID TEXT, InstallDir TEXT, "
              "DeviceAccess NUMERIC, DeviceRequired NUMERIC, ProductID NUMERIC, "
              "InstalledVersion TEXT, Size NUMERIC, Type TEXT, DisplayName TEXT, "
              "CartBuddy NUMERIC)")
LOCAL_DDL = ("CREATE TABLE Packages (PackageID TEXT, PreviewIcon TEXT, "
             "Checksum TEXT, Hidden NUMERIC, MDLType TEXT, ParentApp NUMERIC, "
             "License NUMERIC, State NUMERIC, DeviceHidden NUMERIC, "
             "InstalledDate TEXT, LastPlayedDate TEXT, Type TEXT, "
             "CartBuddy NUMERIC, Redownload NUMERIC, DisplayName TEXT)")
PROFILE_DDL = "CREATE TABLE ProfileAccess (PackageID TEXT, Slot NUMERIC)"


def meta(path):
    """-> dict from a meta.inf. Values are Key="quoted" or Key=0xBARE."""
    out = {}
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k:
                    out[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return out


def dirsize(path):
    n = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                n += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return n


def open_db(path, ddl):
    """Create the file and its schema if either is missing.

    The emulator leaves both databases as ZERO-BYTE files — the shim creates
    them on the guest's first open() and nothing ever writes a schema, because
    the guest only ever reads. An empty file is a valid empty SQLite database,
    so this is 'no tables', not 'corrupt'.
    """
    db = sqlite3.connect(path)
    have = {r[0] for r in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    for stmt in ddl:
        name = stmt.split("(")[0].split()[-1]
        if name not in have:
            db.execute(stmt)
    db.commit()
    return db


def profile_slots(spec):
    """ProfileAccess="-1,0,1,2,3" -> [-1, 0, 1, 2, 3].

    -1 is the 'All' pseudo-profile and is kept: install-game.sh treats it the
    same way, and a title restricted to real slots only is invisible before
    anyone signs in.
    """
    out = []
    for part in (spec or "").split(","):
        part = part.strip()
        if not part:
            continue
        try:
            out.append(int(part))
        except ValueError:
            pass
    return out or [-1, 0, 1, 2, 3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bulk", default=os.path.join(PROJ, "runtime/sysroot/LF/Bulk"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--clean", action="store_true")
    a = ap.parse_args()

    shared_path = os.path.join(a.bulk, "SharedPackageInfo.db")
    local_path = os.path.join(a.bulk, "LocalPackageInfo.db")
    if not os.path.isdir(a.bulk):
        sys.exit("no Bulk at %s" % a.bulk)

    if a.clean:
        for p in (shared_path, local_path):
            for suffix in ("", "-journal", "-wal", "-shm"):
                try:
                    os.unlink(p + suffix)
                except OSError:
                    pass
        print("databases removed")

    shared = open_db(shared_path, [SHARED_DDL])
    local = open_db(local_path, [LOCAL_DDL, PROFILE_DDL])

    if a.list:
        rows = list(shared.execute(
            "SELECT PackageID, Type, DisplayName FROM Packages ORDER BY Type, DisplayName"))
        for pid, typ, name in rows:
            print("  %-26s %-14s %s" % (pid, typ, name))
        print("\n%d registered" % len(rows))
        return

    progfiles = os.path.join(a.bulk, "ProgramFiles")
    now = time.strftime("%Y-%m-%dT%H:%M:%S")
    n = 0
    for entry in sorted(os.listdir(progfiles)) if os.path.isdir(progfiles) else []:
        d = os.path.join(progfiles, entry)
        m = meta(os.path.join(d, "meta.inf"))
        pid = m.get("PackageID")
        if not pid:
            continue
        typ = m.get("Type", "Application")
        name = m.get("Name", pid)
        # ProductID is written as 0x........ in meta.inf and stored NUMERIC.
        try:
            product = int(m.get("ProductID", "0"), 0)
        except ValueError:
            product = 0
        # The icon is a path RELATIVE TO THE INSTALL DIRECTORY; meta.inf's
        # Icon= names it. Flash-era packages call it PopUpIcon.png instead,
        # the same split tools/scan-games.py already handles.
        icon = m.get("Icon") or ""
        if not icon:
            for cand in ("BaseImage.png", "PopUpIcon.png", "icon.png"):
                if os.path.exists(os.path.join(d, cand)):
                    icon = cand
                    break
        install_dir = "/LF/Bulk/ProgramFiles/%s" % entry

        shared.execute("DELETE FROM Packages WHERE PackageID = ?", (pid,))
        shared.execute(
            "INSERT INTO Packages(PackageID, InstallDir, DeviceAccess, "
            "DeviceRequired, ProductID, InstalledVersion, Size, Type, "
            "DisplayName, CartBuddy) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (pid, install_dir, int(m.get("DeviceAccess", 1) or 1), 0, product,
             m.get("Version", "1.0.0.0"), dirsize(d), typ, name, 0))

        local.execute("DELETE FROM Packages WHERE PackageID = ?", (pid,))
        local.execute(
            "INSERT INTO Packages(PackageID, PreviewIcon, Checksum, Hidden, "
            "MDLType, ParentApp, License, State, DeviceHidden, InstalledDate, "
            "LastPlayedDate, Type, CartBuddy, Redownload, DisplayName) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            # State=1 and Hidden=0: installed, and shown. A package left at
            # State=0 is registered and still absent from the picker.
            # RELATIVE to InstallDir. meta.inf's own Icon= is relative
            # ("BaseImage.png"), and the reader joins it to InstallDir; an
            # absolute path here produces InstallDir + "/LF/Bulk/..." and a
            # null image, which is how the picker crashed the first time.
            (pid, icon, "", 0,
             "", 0, 0, 1, 0, now, now, typ, 0, 0, name))

        local.execute("DELETE FROM ProfileAccess WHERE PackageID = ?", (pid,))
        for slot in profile_slots(m.get("ProfileAccess")):
            local.execute(
                "INSERT INTO ProfileAccess(PackageID, Slot) VALUES (?,?)",
                (pid, slot))
        n += 1
        print("  %-26s %-14s %s" % (pid, typ, name))

    shared.commit()
    local.commit()
    shared.close()
    local.close()
    print("\n%d packages registered" % n)
    print("  %s" % shared_path)
    print("  %s" % local_path)


if __name__ == "__main__":
    main()
