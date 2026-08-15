package org.tadpole.view;

import org.libsdl.app.SDLActivity;

import android.os.Bundle;
import android.util.Log;

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
 */
public class TadpoleActivity extends SDLActivity {
    static final String TAG = "tadpole";

    public static native void nativeSetenv(String name, String value);

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
        } catch (Throwable t) {
            Log.e(TAG, "could not set TADPOLE_DIR", t);
        }

        super.onCreate(s);
    }
}
