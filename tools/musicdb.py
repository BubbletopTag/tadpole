#!/usr/bin/env python3
# Tadpole — build LF/Bulk/Music/music.db, without which the Music app is empty
# however much music is installed.
#
#   tools/musicdb.py [runtime/sysroot/LF/Bulk]
#
# THE APP DOES NOT SCAN THE FILESYSTEM. App.so in the Explorer Music Player
# package (MULT-0x001B00B9-000000) embeds SQLite 3.7.11 and asks it questions.
# The strings are in the binary, and they are the whole specification:
#
#   SELECT IconPath,Path,AlbumID FROM Albums ORDER BY Name;
#   SELECT CoverPathLarge,CoverPath,CoverPathLargeLEX FROM Albums WHERE AlbumID=
#   SELECT CreditsPath,CoverPath,Name From Albums Where AlbumID=
#   SELECT Name,Path,TrackNumber,TrackID,TrackData FROM Tracks Where AlbumID=
#   SELECT Name,Path,Length,TrackNumber FROM Tracks WHERE TrackNumber=
#   SELECT Name,Path,TrackID FROM UserTracks ORDER BY Name;
#   Select count(*) from UserTracks;
#
# So an empty music.db is an empty Music app, and five .ogg files sitting in
# LF/Bulk/Music make no difference at all. On real hardware LFConnect writes
# this database from a computer, which is exactly what the app's own narration
# says — "ask your parent to connect to a computer to add music". Nothing in
# the firmware ever writes it, so a Tadpole install has to.
#
# THE SOURCE DATA IS ALREADY INSTALLED, in the separate MusicInfo package that
# ships beside every album. AlbumInfo.csv is one album header row followed by
# one row per track:
#
#   ,Art/icon.png,Art/coverLarge.png,Art/cover.png,LeapFrog Learning Songs
#   1,"Punc, Punc, Punctuation",Music/MUS_PuncPuncPunctuation_a7.ogg,59,
#
# An empty first field marks the album; otherwise the first field is the track
# number. Fields may be quoted because a track name may hold a comma, and two
# of these do. The lines end CRLF.

import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

# THE PATHS ARE RELATIVE, AND THIS WAS MEASURED RATHER THAN ASSUMED. The app
# prepends "/LF/Bulk/Music/" itself and then concatenates Albums.Path and the
# item's own path with NO separator of its own. Writing guest-absolute paths
# into the database therefore produces this, straight out of TADPOLE_STRACE=1:
#
#   open /LF/Bulk/Music//LF/Bulk/Music/MULT-0x001B00E2-000000/LF/Bulk/Music/
#        MULT-0x001B00E2-000000/Art/icon.png = -2
#   CBlitBuffer::createFromPng unable to open png ...
#
# which is the prefix three times over: the app's own, then Path, then
# IconPath. So the album row is found and drawn and its art is not, and an
# album whose art and tracks all fail to open is an album the user cannot tell
# from a missing one.
#
# Albums.Path is therefore the album package directory WITH A TRAILING SLASH,
# and every other path is stored exactly as AlbumInfo.csv writes it:
#
#   "/LF/Bulk/Music/" + "MULT-0x001B00E2-000000/" + "Art/icon.png"
#   "/LF/Bulk/Music/" + "MULT-0x001B00E2-000000/" + "Music/MUS_ABC_a7.ogg"
#
# which is the one arrangement that makes both the art and the audio resolve
# with the concatenation the app actually performs.


def say(msg=""):
    print(msg, flush=True)


# ---- the CSV ----------------------------------------------------------------

def fields(line):
    """Comma-separated with quoted fields — two track names contain commas."""
    out, cur, quoted = [], [], False
    i = 0
    while i < len(line):
        c = line[i]
        if quoted:
            if c == '"':
                if i + 1 < len(line) and line[i + 1] == '"':
                    cur.append('"')
                    i += 1
                else:
                    quoted = False
            else:
                cur.append(c)
        elif c == '"':
            quoted = True
        elif c == ",":
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur))
    return out


def clean(rel):
    """A path as the CSV writes it, kept relative to the album directory.

    An absolute path is passed through untouched — the app would mangle it,
    but inventing a relative form for one is guessing at data this has never
    been given.
    """
    return rel.strip()


def sibling(info, music_dir):
    """The package holding the audio for the CSV in `info`.

    A MusicInfo package describes a Music package with the same product id and
    a different trailing field — MULT-0x001B00E2-777777 describes
    MULT-0x001B00E2-000000. Falls back to the CSV's own directory, which is
    what a self-contained album would look like.
    """
    name = os.path.basename(info)
    cut = name.rfind("-")
    if cut > 0:
        stem = name[:cut + 1]
        for f in sorted(os.listdir(music_dir)):
            p = os.path.join(music_dir, f)
            if p == info or not os.path.isdir(p):
                continue
            if f.startswith(stem):
                return p
    return info


def parse(csv_path, info, music_dir):
    audio = sibling(info, music_dir)
    # THE TRAILING SLASH IS load-bearing: the app concatenates this directly
    # onto the item path without inserting one.
    base = os.path.basename(audio) + "/"

    album = {"name": "", "icon": "", "cover": "", "cover_large": "",
             "path": base, "tracks": []}
    with open(csv_path, "r", errors="replace") as fh:
        text = fh.read()
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if not line.strip():
            continue
        f = fields(line)
        while len(f) < 5:
            f.append("")
        if not f[0].strip():                      # the album header row
            album["icon"] = clean(f[1])
            album["cover_large"] = clean(f[2])
            album["cover"] = clean(f[3])
            album["name"] = f[4].strip()
            continue
        try:
            number = int(f[0].strip())
        except ValueError:
            continue
        rel = f[2].strip()
        try:
            length = int(f[3].strip())
        except ValueError:
            length = 0
        # ONLY TRACKS WHOSE AUDIO IS ACTUALLY THERE: an album that lists a song
        # the install did not bring is a row that plays silence.
        if not os.path.isfile(os.path.join(audio, rel.replace("/", os.sep))):
            continue
        album["tracks"].append({"number": number, "name": f[1].strip(),
                                "path": clean(rel), "length": length})
    if not album["name"]:
        album["name"] = os.path.basename(audio)
    return album


# ---- the database -----------------------------------------------------------

SCHEMA = (
    "CREATE TABLE Albums (AlbumID INTEGER PRIMARY KEY, Name TEXT,"
    " IconPath TEXT, Path TEXT, CoverPath TEXT, CoverPathLarge TEXT,"
    " CoverPathLargeLEX TEXT, CreditsPath TEXT);",
    "CREATE TABLE Tracks (TrackID INTEGER PRIMARY KEY, AlbumID INTEGER,"
    " Name TEXT, Path TEXT, TrackNumber INTEGER, Length INTEGER,"
    " TrackData TEXT);",
    # Selected with count(*) even when nobody has added anything, so it has to
    # exist or the app asks a question that errors.
    "CREATE TABLE UserTracks (TrackID INTEGER PRIMARY KEY, Name TEXT,"
    " Path TEXT, Length INTEGER);",
)


def build(bulk, quiet=False):
    """Rebuild the database from whatever is installed. Never raises."""
    def out(msg):
        if not quiet:
            say(msg)

    music_dir = os.path.join(bulk, "Music")
    if not os.path.isdir(music_dir):
        out("    music            no LF/Bulk/Music, nothing to do")
        return 0

    albums = []
    for name in sorted(os.listdir(music_dir)):
        pkg = os.path.join(music_dir, name)
        if not os.path.isdir(pkg):
            continue
        csv_path = os.path.join(pkg, "AlbumInfo.csv")
        if not os.path.isfile(csv_path):
            continue
        try:
            album = parse(csv_path, pkg, music_dir)
        except OSError as e:
            out("    music            cannot read %s: %s" % (name, e))
            continue
        if album["tracks"]:
            albums.append(album)

    if not albums:
        out("    music            no album info found, database left alone")
        return 0

    db = os.path.join(music_dir, "music.db")
    for stale in (db, db + "-journal", db + "-wal", db + "-shm"):
        # A STALE JOURNAL BESIDE THE DATABASE makes SQLite roll the new one
        # back to nothing on first open, which is this exact symptom again.
        try:
            os.remove(stale)
        except OSError:
            pass

    n_tracks = 0
    try:
        conn = sqlite3.connect(db)
        try:
            # NOT write-ahead logging. WAL leaves the content in a -wal file
            # beside the database; the guest's SQLite is 3.7.11 and a guest
            # that never checkpoints would open a database that looks empty,
            # which is the symptom this whole file exists to fix. DELETE
            # journalling keeps everything in the one file.
            conn.execute("PRAGMA journal_mode=DELETE;")
            for stmt in SCHEMA:
                conn.execute(stmt)
            album_id, track_id = 1, 1
            for a in albums:
                conn.execute(
                    "INSERT INTO Albums (AlbumID,Name,IconPath,Path,CoverPath,"
                    "CoverPathLarge,CoverPathLargeLEX,CreditsPath)"
                    " VALUES (?,?,?,?,?,?,?,?)",
                    (album_id, a["name"], a["icon"], a["path"], a["cover"],
                     a["cover_large"], a["cover_large"], ""))
                for t in sorted(a["tracks"], key=lambda t: t["number"]):
                    conn.execute(
                        "INSERT INTO Tracks (TrackID,AlbumID,Name,Path,"
                        "TrackNumber,Length,TrackData) VALUES (?,?,?,?,?,?,?)",
                        (track_id, album_id, t["name"], t["path"],
                         t["number"], t["length"], ""))
                    track_id += 1
                    n_tracks += 1
                out("    music            %s (%d tracks)"
                    % (a["name"], len(a["tracks"])))
                album_id += 1
            conn.commit()
        finally:
            conn.close()
    except sqlite3.Error as e:
        out("    music            could not build the database: %s" % e)
        return 0

    try:
        os.chmod(db, 0o644)
    except OSError:
        pass
    out("    music            %d album(s), %d track(s)" % (len(albums), n_tracks))
    return len(albums)


def main(argv):
    bulk = argv[1] if len(argv) > 1 else os.path.join(
        PROJ, "runtime", "sysroot", "LF", "Bulk")
    if not os.path.isdir(bulk):
        say("musicdb: no such directory: %s" % bulk)
        return 1
    say("==> music database")
    build(bulk)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
