package org.tadpole.view;

import org.libsdl.app.SDLActivity;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Environment;
import android.provider.Settings;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/* Tadpole's viewer, wrapped in the activity SDL needs.
 *
 * ### The two things that are not boilerplate
 *
 * 1. getLibraries() names libmain.so, which is the viewer. SDLActivity dlopens
 *    each name in order and then calls SDL_main by JNI — so the viewer's own
 *    `int main(int, char **)` becomes SDL_main with nothing changed in the
 *    viewer at all, because SDL_main.h does `#define main SDL_main` and
 *    tadpole_view.c includes <SDL2/SDL.h>. Checked, not assumed:
 *
 *        llvm-nm -D --defined-only libmain.so | grep SDL_main
 *        00007870 T SDL_main
 *
 * 2. TADPOLE_DIR. The viewer's compiled-in runtime directory is /tmp/tadpole,
 *    and Android has no /tmp — the directory is absent, not merely unwritable.
 *    Everything the viewer puts there (the shared-memory arena it maps, the
 *    named FIFOs it makes with mkfifo) has to move to getFilesDir(), which is
 *    /data/data/org.tadpole.view/files and is the only place a non-root app
 *    process may create a named pipe at all.
 *
 *    The viewer already reads TADPOLE_DIR — it was added so the Windows build
 *    could use LOCALAPPDATA — so this needs no change to the viewer either. It
 *    does need a way to set an environment variable in the native process
 *    before main runs, which SDL2 2.32 does not expose, hence nativeSetenv in
 *    android/app/jni/tadpole_jni.c.
 *
 * Setting it in onCreate is deliberately BEFORE super.onCreate(), which is what
 * loads the libraries and eventually calls SDL_main. Do it after and the viewer
 * has already read the variable that was not there.
 *
 * ### And two that are about the screen
 *
 * 3. Fullscreen. See goFullscreen().
 * 4. The ROT chip turns the phone. See the rotation watcher at the bottom.
 */
public class TadpoleActivity extends SDLActivity {
    static final String TAG = "tadpole";

    public static native void nativeSetenv(String name, String value);
    public static native void nativeProbe(String filesDir, String libDir);
    /* The viewer's current display rotation, 0/90/180/270. See the note above
     * the implementation in tadpole_jni.c for why this is polled rather than
     * pushed. */
    public static native int nativeRotate();

    @Override protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override protected void onCreate(Bundle s) {
        Log.i(TAG, "TadpoleActivity.onCreate abi=" + android.os.Build.CPU_ABI
                 + " sdk=" + android.os.Build.VERSION.SDK_INT);

        /* libmain.so has to be mapped before its nativeSetenv can be called,
         * and SDLActivity only loads it inside super.onCreate(). So load it
         * here first — System.loadLibrary is idempotent, SDL's later load of
         * the same name is a no-op. */
        try {
            System.loadLibrary("SDL2");
            System.loadLibrary("main");
            String dir = getFilesDir().getAbsolutePath();
            nativeSetenv("TADPOLE_DIR", dir);
            Log.i(TAG, "TADPOLE_DIR=" + dir);

            /* NO SOFT KEYBOARD, AND IT IS THE VIEWER'S OWN DOING.
             *
             * tadpole_ui.c calls SDL_StartTextInput() once, at init, on
             * purpose — its comment says the name field is "the only place in
             * Tadpole that takes typing", so input is left enabled all the
             * time and ignored unless that field has focus, which means there
             * is no mode to get stuck in. On a desktop that costs nothing.
             *
             * On Android it summons the IME and leaves it there, over the
             * bottom half of the screen, permanently. The emulator never
             * showed it because nothing was ever typed into it.
             *
             * SDL reads its hints from the environment when they are not set
             * in code, so this turns the screen keyboard off the same way
             * TADPOLE_DIR is set, and without a line of change in the viewer.
             *
             * WHAT IT COSTS, stated plainly: the Setup Wizard's profile-name
             * field can no longer be filled in on a device with no hardware
             * keyboard. Raising the IME only while that field has focus needs
             * a conditional SDL_StartTextInput in tadpole_ui.c — a shared
             * file — so it waits until this branch is being merged rather than
             * being smuggled in here. Half a screen of keyboard over
             * everything is the worse of the two. */
            nativeSetenv("SDL_ENABLE_SCREEN_KEYBOARD", "0");

            /* THE PROJECT DIRECTORY, which on a desktop is the checkout and on
             * Android has to be somewhere the app can actually write.
             *
             * g_proj is the anchor for nearly everything the front end looks
             * for: the logo, runtime/sysroot, the games folder, the installed
             * app list, micromod caches. find_project_dir() derives it from
             * argv[0] by walking up looking for tadpole.sh, which under an
             * Android app finds nothing and settles on "." — the filesystem
             * root, where none of those exist. That is why the wizard's panel
             * had no Tadpole in it.
             *
             * TADPOLE_PROJECT is checked before any of that guessing and wins
             * outright, so pointing it at the app's own directory gives the
             * viewer a project it can both read and write. Everything else
             * then falls into place underneath it. */
            nativeSetenv("TADPOLE_PROJECT", dir);
            extractAssets(dir);
            nativeProbe(dir, getApplicationInfo().nativeLibraryDir);
        } catch (Throwable t) {
            Log.e(TAG, "could not set TADPOLE_DIR", t);
        }

        super.onCreate(s);

        /* INTO THE CUTOUT, NOT AROUND IT. Without this the system leaves the
         * notch's strip black and hands the app the rectangle below it, so a
         * "fullscreen" app on a phone with a camera hole is fullscreen minus a
         * band. SHORT_EDGES lets the surface run the whole way and is only
         * meaningful once the bars are hidden, which is the next call. */
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        goFullscreen();
        askForStorage();
    }

    /* ---- the assets the viewer reads as files ------------------------------
     *
     * ONCE, AND ONLY IF ABSENT OR STALE. Copying two PNGs on every launch is
     * cheap but it is also the kind of thing that quietly overwrites a file
     * someone has replaced on purpose, so a size comparison stands in for a
     * proper version check: the assets are fixed at build time, so a file that
     * is already the right length is already the right file.
     */
    private void extractAssets(String destDir) {
        String[] names = { "tadpole.png", "glasspole.png" };
        for (String name : names) {
            File out = new File(destDir, name);
            InputStream in = null;
            OutputStream os = null;
            try {
                in = getAssets().open(name);
                if (out.exists() && out.length() == in.available()) {
                    in.close();
                    continue;
                }
                os = new FileOutputStream(out);
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                Log.i(TAG, "unpacked asset " + name + " -> " + out);
            } catch (Throwable t) {
                Log.e(TAG, "could not unpack asset " + name, t);
            } finally {
                try { if (in != null) in.close(); } catch (Throwable ignored) {}
                try { if (os != null) os.close(); } catch (Throwable ignored) {}
            }
        }
    }

    /* ---- storage -----------------------------------------------------------
     *
     * WHY ALL-FILES ACCESS AND NOT THE DOCUMENT PICKER. Tadpole has its own
     * file browser — it walks directories itself and shows them in its own
     * chrome — and the firmware and the game backups are ordinary files the
     * user already has, in Downloads or on a card. The Storage Access
     * Framework would hand back opaque content:// URIs that nothing in the
     * viewer can open, and adopting it means replacing that browser with the
     * system picker. MANAGE_EXTERNAL_STORAGE keeps real paths working, which
     * is what the existing code is written against.
     *
     * It cannot be granted by a dialog: from API 30 the user has to be sent to
     * a Settings page and toggle it there. Below 30 it is an ordinary runtime
     * permission. Both are asked for once and the app runs either way — with
     * no access the browser simply shows nothing outside the app's own folder,
     * which is exactly what it did before this and is not a crash.
     */
    private static final int REQ_STORAGE = 4711;

    private void askForStorage() {
        try {
            if (Build.VERSION.SDK_INT >= 30) {
                if (Environment.isExternalStorageManager()) {
                    Log.i(TAG, "storage: all-files access already granted");
                    return;
                }
                Log.i(TAG, "storage: asking for all-files access");
                Intent i = new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } else {
                String p = android.Manifest.permission.READ_EXTERNAL_STORAGE;
                if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED)
                    requestPermissions(new String[] { p }, REQ_STORAGE);
            }
        } catch (Throwable t) {
            /* A device with no Settings activity for this, or a ROM that has
             * removed it. Not fatal: see the note above. */
            Log.e(TAG, "storage: could not ask", t);
        }
    }

    /* ---- fullscreen --------------------------------------------------------
     *
     * The manifest's theme takes the title bar away; this takes the status and
     * navigation bars. Both are needed and neither does the other's job.
     *
     * TWO IMPLEMENTATIONS, AND THE OLD ONE IS NOT DEAD CODE. The
     * SYSTEM_UI_FLAG_* route was deprecated in API 30 and, for an app whose
     * target SDK is 35, is a NO-OP on Android 15 and later — the platform
     * enforces edge-to-edge and ignores the flags entirely. So API 30+ has to
     * go through WindowInsetsController. But minSdk here is 26, and on Android
     * 8 to 10 WindowInsetsController does not exist at all. There is no single
     * call that covers 26 through 36; there are two, and a version check.
     *
     * BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE is the modern spelling of what
     * IMMERSIVE_STICKY meant: a swipe from the edge shows the bars briefly and
     * they go away again on their own, rather than staying up and permanently
     * shrinking the picture. Anything else and the first accidental swipe costs
     * the user their screen for the rest of the session.
     */
    private void goFullscreen() {
        Window w = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(
                  View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    /* The bars come back whenever the window loses focus — a notification
     * shade, a permission dialog, the recents switcher — and do not leave
     * again by themselves. Re-asserting on focus is what makes it stick. */
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) goFullscreen();
    }

    /* ---- the ROT chip turns the phone --------------------------------------
     *
     * ROT has always rotated the picture INSIDE the window, which is the right
     * thing on a desktop where the window is a rectangle on a larger screen. On
     * a phone the window IS the screen, so rotating a portrait title to read
     * upright left it letterboxed in a landscape display with a black band down
     * each side — the picture was correct and most of the screen was wasted.
     *
     * POLLED, AT FOUR HERTZ, ON THE UI THREAD. The alternative is a callback
     * out of tadpole_ui.c, and that file is shared with the desktop build,
     * which would then carry an Android-shaped hook for no reason of its own.
     * ui_cfg() is already public and the rotation is a plain int; reading it is
     * cheaper than the machinery required to be told about it. Polling on the
     * UI thread also means setRequestedOrientation() is called from the only
     * thread allowed to call it, without a single line of thread handling.
     *
     * WHICH WAY ROUND. 0 and 180 leave the logical space landscape, 90 and 270
     * make it portrait, so the device has to match or the letterboxing is
     * simply the other way up. Mapping the two landscape cases to LANDSCAPE and
     * REVERSE_LANDSCAPE (and likewise for portrait) rather than collapsing them
     * means one press of ROT walks through all four ways of holding the phone,
     * which is what someone pressing it four times expects.
     */
    private static final int POLL_MS = 250;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private int lastRot = Integer.MIN_VALUE;

    private final Runnable rotWatch = new Runnable() {
        @Override public void run() {
            try {
                int rot  = nativeRotate();
                int want = orientationFor(rot);
                if (rot != lastRot) {
                    Log.i(TAG, "ROT " + rot + " -> activity orientation " + want);
                    lastRot = rot;
                }
                /* COMPARED AGAINST THE ACTIVITY, NOT AGAINST THE LAST VALUE WE
                 * SENT, and that is the whole fix. See the note below. */
                if (getRequestedOrientation() != want) {
                    setRequestedOrientation(want);
                    /* A rotation rebuilds the window's insets, and on some
                     * builds the bars come back with them. */
                    goFullscreen();
                }
            } catch (Throwable t) {
                /* libmain.so not mapped yet, or unloaded on the way out.
                 * Neither is worth a log line four times a second. */
            }
            ui.postDelayed(this, POLL_MS);
        }
    };

    /* WHY THIS RE-ASSERTS RATHER THAN FIRING ONCE.
     *
     * SDL sets the orientation itself, from native, when it creates the window
     * — SDLActivity.setOrientationBis(). With a resizable window and no
     * SDL_HINT_ORIENTATIONS it picks SCREEN_ORIENTATION_FULL_USER, which hands
     * the decision to the user's rotation lock. The viewer's window IS
     * resizable, so that is what happens, and it happens AFTER onResume():
     *
     *     17:14:35.816  tadpole  ROT 0 -> activity orientation 0
     *     17:14:35.975  SDL      setOrientation() requestedOrientation=13
     *                            width=960 height=570 resizable=true hint=
     *
     * 13 is FULL_USER. Ours was correct and was overwritten 160 ms later, and a
     * watcher that only acted when the viewer's rotation CHANGED never spoke
     * again — the app sat in whichever way the phone happened to be held.
     *
     * Setting SDL_HINT_ORIENTATIONS instead would mean deciding the allowed
     * orientations before the window exists, which is exactly what a control
     * the user presses at run time cannot do. So the watcher compares what the
     * activity is actually asking for against what ROT says it should be, and
     * corrects it. That is self-healing: it survives SDL doing this again on
     * any later window recreation, and it costs one getter call four times a
     * second.
     */
    private static int orientationFor(int rot) {
        switch (((rot % 360) + 360) % 360) {
            case 90:  return ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
            case 180: return ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE;
            case 270: return ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT;
            default:  return ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
        }
    }

    @Override protected void onResume() {
        super.onResume();
        ui.removeCallbacks(rotWatch);
        ui.post(rotWatch);
        goFullscreen();
    }

    @Override protected void onPause() {
        ui.removeCallbacks(rotWatch);
        super.onPause();
    }
}
