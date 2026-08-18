#!/usr/bin/env python3
"""Does music.db say what the Music app asks it, and do its paths resolve?

    ./tools/tests/musicdb_test.py

One script, no framework, the same shape as link_resolve_test.py. The fixture
is built here rather than read out of runtime/sysroot, because the firmware is
not redistributable and a test that needs a dumped LeapPad is a test nobody
runs.

TWO THINGS ARE BEING PROTECTED, and both have already been wrong once.

THE QUERIES. App.so embeds SQLite and asks it fixed questions; every SELECT
below is a string lifted out of that binary. A column renamed here is a Music
app that shows nothing, with no error anywhere, so the queries are run verbatim
rather than re-expressed.

THE PATH RULE. The app prepends "/LF/Bulk/Music/" itself and concatenates
Albums.Path and the item path with NO separator of its own. Storing
guest-absolute paths therefore yields the prefix three times over and every
open fails. This checks the concatenation the app actually performs, against a
file that actually exists.
"""
import importlib.util
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)

_spec = importlib.util.spec_from_file_location(
    "musicdb", os.path.join(TOOLS, "musicdb.py"))
musicdb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(musicdb)

fails = []


def check(name, got, want):
    if got != want:
        fails.append("%s\n     got:  %r\n     want: %r" % (name, got, want))


# The album package holds the audio and the art; the MusicInfo package beside
# it holds the CSV. CRLF line endings, and two track names contain commas —
# both exactly as LeapFrog ships them.
CSV = (
    ",Art/icon.png,Art/coverLarge.png,Art/cover.png,LeapFrog Learning Songs\r\n"
    '1,"Punc, Punc, Punctuation",Music/MUS_PuncPuncPunctuation_a7.ogg,59,\r\n'
    "2,Sticky Icky Vowels,Music/MUS_StickyIckyVowels_a7.ogg,52,\r\n"
    '4,"Hop, Hop, Hop",Music/MUS_HopHopHop_a7.ogg,61,\r\n'
    "9,A Song The Install Did Not Bring,Music/MUS_Absent_a7.ogg,40,\r\n"
)


def fixture(root):
    bulk = os.path.join(root, "LF", "Bulk")
    music = os.path.join(bulk, "Music")
    album = os.path.join(music, "MULT-0x001B00E2-000000")
    info = os.path.join(music, "MULT-0x001B00E2-777777")
    for d in (os.path.join(album, "Art"), os.path.join(album, "Music"), info):
        os.makedirs(d)
    for f in ("icon.png", "cover.png", "coverLarge.png"):
        open(os.path.join(album, "Art", f), "wb").close()
    for f in ("MUS_PuncPuncPunctuation_a7.ogg", "MUS_StickyIckyVowels_a7.ogg",
              "MUS_HopHopHop_a7.ogg"):
        open(os.path.join(album, "Music", f), "wb").close()
    with open(os.path.join(info, "AlbumInfo.csv"), "w", newline="") as fh:
        fh.write(CSV)
    return bulk, album, music


root = tempfile.mkdtemp(prefix="tadpole-musicdb-test-")
try:
    bulk, album_dir, music_dir = fixture(root)
    n = musicdb.build(bulk, quiet=True)
    check("albums built", n, 1)

    db = os.path.join(music_dir, "music.db")
    if not os.path.isfile(db):
        fails.append("no music.db was written")
        raise SystemExit  # nothing below can run

    conn = sqlite3.connect(db)

    # ---- the app's own queries, verbatim -----------------------------------
    rows = conn.execute(
        "SELECT IconPath,Path,AlbumID FROM Albums ORDER BY Name;").fetchall()
    check("one album row", len(rows), 1)
    icon, path, album_id = rows[0]

    # THE CONCATENATION THE APP PERFORMS. Guest-absolute paths made this
    # "/LF/Bulk/Music//LF/Bulk/Music/<pkg>/LF/Bulk/Music/<pkg>/Art/icon.png".
    guest = "/LF/Bulk/Music/" + path + icon
    check("icon path the app builds", guest,
          "/LF/Bulk/Music/MULT-0x001B00E2-000000/Art/icon.png")
    # ...and it has to name a file that is really there.
    on_disk = os.path.join(album_dir, "Art", "icon.png")
    check("icon exists on disk",
          os.path.isfile(os.path.join(music_dir, path.rstrip("/"),
                                      *icon.split("/"))), True)
    check("icon is the fixture's own file",
          os.path.normpath(os.path.join(music_dir, path, icon)),
          os.path.normpath(on_disk))

    covers = conn.execute("SELECT CoverPathLarge,CoverPath,CoverPathLargeLEX"
                          " FROM Albums WHERE AlbumID=?", (album_id,)).fetchone()
    check("cover columns", covers,
          ("Art/coverLarge.png", "Art/cover.png", "Art/coverLarge.png"))
    conn.execute("SELECT CreditsPath,CoverPath,Name From Albums Where AlbumID=?",
                 (album_id,)).fetchone()

    tracks = conn.execute(
        "SELECT Name,Path,TrackNumber,TrackID,TrackData FROM Tracks"
        " Where AlbumID=? ORDER BY TrackNumber", (album_id,)).fetchall()

    # A TRACK THE INSTALL DID NOT BRING IS A ROW THAT PLAYS SILENCE, so the
    # CSV's fourth song is listed and its .ogg is absent and it must not appear.
    check("only tracks whose audio is present", len(tracks), 3)
    check("quoted name keeps its commas", tracks[0][0],
          "Punc, Punc, Punctuation")
    check("track path is relative", tracks[0][1],
          "Music/MUS_PuncPuncPunctuation_a7.ogg")
    check("track path the app builds", "/LF/Bulk/Music/" + path + tracks[0][1],
          "/LF/Bulk/Music/MULT-0x001B00E2-000000/Music/"
          "MUS_PuncPuncPunctuation_a7.ogg")
    check("the CSV's own track numbers are kept",
          [t[2] for t in tracks], [1, 2, 4])
    check("TrackIDs are distinct", len({t[3] for t in tracks}), 3)

    conn.execute("SELECT Name,Path,Length,TrackNumber FROM Tracks"
                 " WHERE TrackNumber=1").fetchone()
    conn.execute("SELECT Path,TrackData,Name,TrackNumber FROM Tracks"
                 " WHERE TrackNumber=1").fetchone()

    # Asked for on a device nobody has added anything to, so the table has to
    # exist or the app asks a question that errors.
    check("UserTracks is empty and present",
          conn.execute("Select count(*) from UserTracks;").fetchone()[0], 0)
    conn.execute("SELECT Name,Path,TrackID FROM UserTracks ORDER BY Name;").fetchall()
    conn.execute("SELECT Name,Path,Length FROM UserTracks WHERE TrackID=1").fetchone()

    # THE JOURNAL MODE. WAL would leave the rows in a -wal file beside the
    # database, and a guest that never checkpoints opens one that looks empty.
    check("journal mode is not WAL",
          conn.execute("PRAGMA journal_mode;").fetchone()[0].lower() != "wal",
          True)
    conn.close()
    check("no -wal left beside the database",
          os.path.exists(db + "-wal"), False)

    # Rebuilding over an existing database must not stack rows up.
    musicdb.build(bulk, quiet=True)
    conn = sqlite3.connect(db)
    check("rebuild is idempotent",
          conn.execute("SELECT count(*) FROM Tracks").fetchone()[0], 3)
    conn.close()

    # An install with no music at all is not an error.
    empty = tempfile.mkdtemp(prefix="tadpole-musicdb-empty-")
    os.makedirs(os.path.join(empty, "Music"))
    check("no albums is not a failure", musicdb.build(empty, quiet=True), 0)
    check("and writes no database",
          os.path.exists(os.path.join(empty, "Music", "music.db")), False)
finally:
    import shutil
    shutil.rmtree(root, ignore_errors=True)

if fails:
    print("musicdb_test: %d FAILED" % len(fails))
    for f in fails:
        print("  - %s" % f)
    sys.exit(1)
print("musicdb_test: ok")
