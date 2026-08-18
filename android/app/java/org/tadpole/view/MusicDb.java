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
 * <p>ONE THING HERE IS INFERRED RATHER THAN MEASURED, and it is flagged so the
 * next person does not mistake it for established: the CSV's paths are relative
 * ("Art/icon.png"), the album's files live in the sibling Music package rather
 * than beside the CSV, and no capture of a real music.db exists to say which
 * form the database stores. This writes GUEST-ABSOLUTE paths —
 * /LF/Bulk/Music/&lt;album package&gt;/Art/icon.png — on the reasoning that the
 * app selects IconPath and Path as separate columns and uses each directly. If
 * the album appears but its art or tracks do not, that inference is where to
 * look first, and the fix is one join away.
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
             * is the exact symptom this whole class exists to fix. DELETE
             * journalling keeps everything in the one file. */
            d.execSQL("PRAGMA journal_mode=DELETE;");
            d.execSQL("CREATE TABLE Albums (AlbumID INTEGER PRIMARY KEY, Name TEXT,"
                    + " IconPath TEXT, Path TEXT, CoverPath TEXT, CoverPathLarge TEXT,"
                    + " CoverPathLargeLEX TEXT, CreditsPath TEXT);");
            d.execSQL("CREATE TABLE Tracks (TrackID INTEGER PRIMARY KEY, AlbumID INTEGER,"
                    + " Name TEXT, Path TEXT, TrackNumber INTEGER, Length INTEGER,"
                    + " TrackData TEXT);");
            /* Selected with count(*) even when nobody has added anything, so it
             * has to exist or the app asks a question that errors. */
            d.execSQL("CREATE TABLE UserTracks (TrackID INTEGER PRIMARY KEY, Name TEXT,"
                    + " Path TEXT, Length INTEGER);");

            int albumId = 1, trackId = 1, nTracks = 0;
            for (Album a : albums) {
                d.execSQL("INSERT INTO Albums (AlbumID,Name,IconPath,Path,CoverPath,"
                        + "CoverPathLarge,CoverPathLargeLEX,CreditsPath)"
                        + " VALUES (?,?,?,?,?,?,?,?)",
                        new Object[] { albumId, a.name, a.icon, a.path, a.cover,
                                       a.coverLarge, a.coverLarge, "" });
                for (Track t : a.tracks) {
                    d.execSQL("INSERT INTO Tracks (TrackID,AlbumID,Name,Path,TrackNumber,"
                            + "Length,TrackData) VALUES (?,?,?,?,?,?,?)",
                            new Object[] { trackId++, albumId, t.name, t.path,
                                           t.number, t.length, "" });
                    nTracks++;
                }
                out.println("  music: " + a.name + " (" + a.tracks.size() + " tracks)");
                albumId++;
            }
            out.println("  music: " + albums.size() + " album(s), " + nTracks + " track(s)");
        } catch (Throwable e) {
            out.println("  music: could not build the database: " + e);
        } finally {
            if (d != null) try { d.close(); } catch (Throwable e) { /* ignore */ }
        }
    }

    /* ---- the CSV ---------------------------------------------------------- */

    private static final class Album {
        String name = "", icon = "", cover = "", coverLarge = "", path = "";
        final List<Track> tracks = new ArrayList<Track>();
    }
    private static final class Track {
        String name = "", path = "";
        int number, length;
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
        String base = "/LF/Bulk/Music/" + audio.getName();

        Album a = new Album();
        a.path = base;
        for (String line : Tools.readAll(csv).split("\r?\n")) {
            if (line.trim().isEmpty()) continue;
            List<String> f = fields(line);
            while (f.size() < 5) f.add("");
            if (f.get(0).trim().isEmpty()) {          /* the album header row */
                a.icon       = join(base, f.get(1));
                a.coverLarge = join(base, f.get(2));
                a.cover      = join(base, f.get(3));
                a.name       = f.get(4).trim();
                continue;
            }
            Track t = new Track();
            try { t.number = Integer.parseInt(f.get(0).trim()); } catch (NumberFormatException e) { continue; }
            t.name = f.get(1).trim();
            t.path = join(base, f.get(2));
            try { t.length = Integer.parseInt(f.get(3).trim()); } catch (NumberFormatException e) { t.length = 0; }
            /* Only tracks whose audio is actually there: an album that lists a
             * song the install did not bring is a row that plays silence. */
            if (new File(audio, f.get(2).trim()).isFile()) a.tracks.add(t);
        }
        if (a.name.isEmpty()) a.name = audio.getName();
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

    private static String join(String base, String rel) {
        rel = rel.trim();
        if (rel.isEmpty()) return "";
        if (rel.startsWith("/")) return rel;
        return base + "/" + rel;
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
