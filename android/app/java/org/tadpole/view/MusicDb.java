package org.tadpole.view;

import android.database.sqlite.SQLiteDatabase;

import java.io.File;
import java.io.IOException;
import java.io.PrintStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Builds LF/Bulk/Music/music.db, without which the Music app is empty however
 * much music is installed.
 *
 * <p>THE APP DOES NOT SCAN THE FILESYSTEM. App.so in the Explorer Music Player
 * package embeds SQLite and asks it questions — the strings are right there in
 * the binary:
 *
 * <pre>
 *   SELECT IconPath,Path,AlbumID FROM Albums ORDER BY Name;
 *   SELECT Name,Path,TrackNumber,TrackID,TrackData FROM Tracks Where AlbumID=
 *   SELECT CoverPathLarge,CoverPath,CoverPathLargeLEX FROM Albums WHERE AlbumID=
 *   SELECT CreditsPath,CoverPath,Name From Albums Where AlbumID=
 *   Select count(*) from UserTracks;
 * </pre>
 *
 * So an empty music.db is an empty Music app, and five .ogg files sitting in
 * LF/Bulk/Music make no difference at all. On real hardware LFConnect writes
 * this database from a computer, which is exactly what the app's own narration
 * says — "ask your parent to connect to a computer to add music".
 *
 * <p>THE SOURCE DATA IS ALREADY INSTALLED, in the separate MusicInfo package
 * that ships beside every album. AlbumInfo.csv is one album header row followed
 * by one row per track:
 *
 * <pre>
 *   ,Art/icon.png,Art/coverLarge.png,Art/cover.png,LeapFrog Learning Songs
 *   1,"Punc, Punc, Punctuation",Music/MUS_PuncPuncPunctuation_a7.ogg,59,
 * </pre>
 *
 * An empty first field marks the album; otherwise the first field is the track
 * number. Fields may be quoted because a track name may hold a comma, and two
 * of these do.
 *
 * <p>THE SCHEMA IS COPIED OFF A REAL DEVICE, and the first version of this
 * class shows why that was not optional. That version derived the tables from
 * the twelve queries App.so issues — every column they name was present, the
 * database read back correctly, and the Music app ABORTED on it: SIGABRT
 * through libstdc++'s unwinder, an uncaught exception, with no album ever
 * drawn. What it wanted was {@code Albums.PackageID}, a column that appears in
 * no query in the binary, so no amount of reading strings could have produced
 * it. Two more the same: {@code Tracks.Length} is TEXT holding "59", and
 * {@code CoverPathLargeLex} is spelled with a lowercase "ex" and is NULL.
 *
 * <p>The paths were wrong too, and in a way that looked reasonable. A real
 * database keeps {@code Albums.Path} as the package directory with a trailing
 * slash — "MULT-0x001B00E2-000000/" — and {@code IconPath} and the covers
 * exactly as AlbumInfo.csv spells them, "Art/icon.png". This used to write
 * guest-absolute paths on the reasoning that the app selects IconPath and Path
 * as separate columns and uses each directly. It does not.
 *
 */
final class MusicDb {

    private MusicDb() {}

    /** Rebuilds the database from whatever is installed. Never throws. */
    static void build(File bulk, PrintStream out) {
        File musicDir = new File(bulk, "Music");
        File[] pkgs = musicDir.listFiles();
        if (pkgs == null) return;

        List<Album> albums = new ArrayList<Album>();
        for (File p : pkgs) {
            if (!p.isDirectory()) continue;
            File csv = new File(p, "AlbumInfo.csv");
            if (!csv.isFile()) continue;
            try {
                Album a = parse(csv, p, musicDir);
                if (a != null && !a.tracks.isEmpty()) albums.add(a);
            } catch (Exception e) {
                out.println("  music: cannot read " + p.getName() + ": " + e.getMessage());
            }
        }

        if (albums.isEmpty()) {
            out.println("  music: no album info found, database left empty");
            return;
        }

        File db = new File(musicDir, "music.db");
        db.delete();
        /* And the journal beside it: a stale -journal from an older build makes
         * SQLite roll the new database back to nothing on first open. */
        new File(musicDir, "music.db-journal").delete();

        SQLiteDatabase d = null;
        try {
            d = SQLiteDatabase.openOrCreateDatabase(db, null);
            /* NOT write-ahead logging. WAL leaves the content in a -wal file
             * beside the database, and the guest's SQLite is from 2013 and will
             * not read one — it would open a database that looks empty, which
             * is the exact symptom this whole class exists to fix.
             *
             * AND IT IS A QUERY, NOT AN execSQL. execSQL refuses any statement
             * that returns a row, and PRAGMA journal_mode returns the mode it
             * ended up in. Measured: as execSQL it threw on the very first
             * call, the catch below swallowed it, and music.db was left holding
             * nothing but the android_metadata table Android creates itself. */
            android.database.Cursor jc = d.rawQuery("PRAGMA journal_mode=DELETE;", null);
            if (jc != null) jc.close();

            /* THESE THREE STATEMENTS ARE COPIED OFF A REAL DEVICE, not derived
             * from the queries App.so issues, and the difference mattered.
             * Deriving them produced a database with every column those queries
             * name — and the Music app aborted on it, through libstdc++'s
             * unwinder, with an uncaught exception. What it wanted was
             * Albums.PackageID: a column NO query in the binary mentions, so no
             * amount of reading the strings would ever have produced it.
             *
             * Two more that reading could not have given: Tracks.Length is
             * TEXT and holds "59", not an integer; and CoverPathLargeLex is
             * spelled with a lowercase "ex" and is NULL rather than empty. */
            d.execSQL("CREATE TABLE Albums ( AlbumID INTEGER PRIMARY KEY,"
                    + " CoverPath TEXT, CoverPathLarge TEXT, CoverPathLargeLex TEXT,"
                    + " CreditsPath TEXT, IconPath TEXT, Name TEXT, PackageID TEXT,"
                    + " Path TEXT)");
            d.execSQL("CREATE TABLE Tracks ( AlbumID INTEGER, Length TEXT, Name TEXT,"
                    + " Path TEXT, TrackData TEXT, TrackID INTEGER PRIMARY KEY,"
                    + " TrackNumber INTEGER)");
            /* Selected with count(*) even when nobody has added anything, so it
             * has to exist or the app asks a question that errors. */
            d.execSQL("CREATE TABLE UserTracks ( Album TEXT, Artist TEXT, Length INTEGER,"
                    + " Name TEXT, Path TEXT, Size INTEGER, TrackID INTEGER PRIMARY KEY,"
                    + " TrackNumber INTEGER)");

            int albumId = 1, trackId = 1, nTracks = 0;
            for (Album a : albums) {
                /* PATHS ARE RELATIVE AND Path IS THE PACKAGE DIRECTORY WITH A
                 * TRAILING SLASH — "MULT-0x001B00E2-000000/" — with IconPath
                 * and the covers left exactly as the CSV spells them. The
                 * guest-absolute form this used to write was a guess, and it
                 * was wrong. */
                d.execSQL("INSERT INTO Albums (AlbumID,CoverPath,CoverPathLarge,"
                        + "CoverPathLargeLex,CreditsPath,IconPath,Name,PackageID,Path)"
                        + " VALUES (?,?,?,?,?,?,?,?,?)",
                        new Object[] { albumId, a.cover, a.coverLarge, null,
                                       "", a.icon, a.name, a.pkg, a.path });
                for (Track t : a.tracks) {
                    d.execSQL("INSERT INTO Tracks (AlbumID,Length,Name,Path,TrackData,"
                            + "TrackID,TrackNumber) VALUES (?,?,?,?,?,?,?)",
                            new Object[] { albumId, t.length, t.name, t.path,
                                           t.data, trackId++, t.number });
                    nTracks++;
                }
                out.println("  music: " + a.name.trim() + " (" + a.tracks.size() + " tracks)");
                albumId++;
            }
            out.println("  music: " + albums.size() + " album(s), " + nTracks + " track(s)");
        } catch (Throwable e) {
            /* NAMED, not swallowed. The first version of this printed here and
             * the line scrolled past inside a long install; the database it
             * left behind looked plausible from the outside — right name,
             * right place, non-zero length. */
            out.println("  music: could not build the database: " + e);
            android.util.Log.w("tadpole", "music.db: " + e, e);
        } finally {
            if (d != null) try { d.close(); } catch (Throwable e) { /* ignore */ }
        }
    }

    /* ---- the CSV ---------------------------------------------------------- */

    private static final class Album {
        String name = "", icon = "", cover = "", coverLarge = "", path = "", pkg = "";
        final List<Track> tracks = new ArrayList<Track>();
    }
    private static final class Track {
        String name = "", path = "", length = "", data = "";
        int number;
    }

    /**
     * `info` is the MusicInfo package holding the CSV; the audio lives in the
     * sibling package with the same product id and a different last field —
     * MULT-0x001B00E2-777777 describes MULT-0x001B00E2-000000. Falls back to
     * the CSV's own directory when there is no such sibling, which is what a
     * self-contained album would look like.
     */
    private static Album parse(File csv, File info, File musicDir) throws IOException {
        File audio = sibling(info, musicDir);
        Album a = new Album();
        a.pkg  = audio.getName();
        a.path = a.pkg + "/";

        /* SPLIT ON \n AND KEEP THE \r, and do not trim the fields. That is not
         * sloppiness, it is fidelity: the database a real device carries has
         * Name = "LeapFrog Learning Songs\r" and TrackData = "\r", because
         * whatever wrote it took the CSV's fields verbatim and the file has DOS
         * line endings. Reproducing that exactly costs nothing and removes a
         * whole class of question about whether a difference matters. */
        for (String line : Tools.readAll(csv).split("\n")) {
            if (line.trim().isEmpty()) continue;
            List<String> f = fields(line);
            while (f.size() < 5) f.add("");
            if (f.get(0).trim().isEmpty()) {          /* the album header row */
                a.icon       = f.get(1);
                a.coverLarge = f.get(2);
                a.cover      = f.get(3);
                a.name       = f.get(4);
                continue;
            }
            Track t = new Track();
            try { t.number = Integer.parseInt(f.get(0).trim()); }
            catch (NumberFormatException e) { continue; }
            t.name   = f.get(1);
            t.path   = f.get(2);
            t.length = f.get(3).trim();      /* TEXT on the device: "59" */
            t.data   = f.get(4);             /* "\r", and that is what it holds */
            /* Only tracks whose audio is actually there: an album that lists a
             * song the install did not bring is a row that plays silence. */
            if (new File(audio, t.path.trim()).isFile()) a.tracks.add(t);
        }
        if (a.name.isEmpty()) a.name = a.pkg;
        return a;
    }

    private static File sibling(File info, File musicDir) {
        String n = info.getName();
        int last = n.lastIndexOf('-');
        if (last > 0) {
            String stem = n.substring(0, last + 1);
            File[] all = musicDir.listFiles();
            if (all != null) for (File f : all) {
                if (f.equals(info) || !f.isDirectory()) continue;
                if (f.getName().startsWith(stem)) return f;
            }
        }
        return info;
    }

    /** Comma-separated, with quoted fields — two track names contain commas. */
    private static List<String> fields(String line) {
        List<String> out = new ArrayList<String>();
        StringBuilder cur = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; }
                    else quoted = false;
                } else cur.append(c);
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                out.add(cur.toString());
                cur.setLength(0);
            } else {
                cur.append(c);
            }
        }
        out.add(cur.toString());
        return out;
    }
}
