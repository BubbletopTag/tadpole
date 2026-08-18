package org.tadpole.view;

import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.IOException;
import java.io.PrintStream;

/**
 * The tools, for Android.
 *
 * <p>WHY THIS EXISTS. Everything under tools/ is a shell script with a Python
 * twin — the .sh ones are Linux's entry points and the .py ones are what
 * Windows runs, because Windows has no bash. Android has neither, and cannot
 * acquire either: an app may not execute a file it wrote, so shipping a Python
 * runtime and calling it would not help even if one were small enough to ship.
 * The viewer's honest refusal was
 *
 *     tools/check-update.py needs a shell or Python, and Android has neither
 *
 * which is accurate and leaves the front end with buttons that cannot work.
 * This is the third spelling of the same tools: Java, in the app's own process,
 * where none of those restrictions apply.
 *
 * <p>THE CONTRACT IS THE ONE THE VIEWER ALREADY HAS, deliberately. On every
 * other platform spawn_script() forks a process and hands the caller a read fd
 * carrying its stdout and stderr; tool_drain() reads that a line at a time into
 * the progress panel and tool_reap() collects the exit status. So this hands
 * back a pipe and an exit status too, and not one line of tadpole_ui.c or of
 * the progress panel had to learn that Android is different. A tool here is a
 * thread that writes to a PrintStream and returns a boolean.
 *
 * <p>ONE AT A TIME, which is what the viewer already enforces — tool_run()
 * refuses to start a second while g_tool is set — but it is enforced here as
 * well rather than assumed, because the cost of being wrong is two threads
 * writing to one pipe and a progress panel that interleaves two installs.
 */
public final class TadpoleTools {
    private static final String TAG = "tadpole";

    /** -1 running, 0 finished and failed, 1 finished and succeeded, 2 idle. */
    static final int RUNNING = -1, FAILED = 0, OK = 1, IDLE = 2;

    private static volatile int state = IDLE;
    private static Thread worker;

    private TadpoleTools() {}

    /** Handed over by the activity — see Tools.proj(). */
    public static void setProjectDir(String dir) { Tools.setProjectDir(dir); }

    /**
     * Start a tool. Returns the read end of a pipe carrying its output, or -1
     * if the tool is unknown or one is already running.
     *
     * <p>The pipe is made HERE and not in C, because ParcelFileDescriptor is
     * the public way to get one whose write end a Java stream can own; the
     * alternative is adoptFd on a descriptor made in C, which is not public
     * API. detachFd() hands the read end over as a plain integer that the
     * viewer's existing read loop treats like any other.
     */
    public static synchronized int start(String script, String[] argv) {
        if (state == RUNNING) {
            Log.w(TAG, "tools: " + script + " refused, one is already running");
            return -1;
        }
        final Tool tool = Tools.forScript(script);
        if (tool == null) {
            Log.w(TAG, "tools: no Android implementation of " + script);
            return -1;
        }

        final ParcelFileDescriptor[] pipe;
        try {
            pipe = ParcelFileDescriptor.createPipe();
        } catch (IOException e) {
            Log.e(TAG, "tools: could not make a pipe", e);
            return -1;
        }

        final PrintStream out = new PrintStream(
                new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1]), true);
        final String[] args = argv == null ? new String[0] : argv;

        state = RUNNING;
        worker = new Thread(new Runnable() {
            @Override public void run() {
                boolean ok = false;
                try {
                    ok = tool.run(args, out);
                } catch (Throwable t) {
                    /* REPORTED DOWN THE PIPE, not just to logcat. An exception
                     * here is the tool failing, and the person who pressed the
                     * button is looking at the progress panel — sending it only
                     * to logcat is how a crash becomes "it just stopped". */
                    out.println("  " + t.getClass().getSimpleName() + ": " + t.getMessage());
                    Log.e(TAG, "tools: " + tool.getClass().getSimpleName() + " threw", t);
                } finally {
                    /* Closing the write end is what ends the viewer's read
                     * loop, so it has to happen whatever went wrong. */
                    out.close();
                    state = ok ? OK : FAILED;
                }
            }
        }, "tadpole-tool");
        worker.start();

        return pipe[0].detachFd();
    }

    /**
     * -1 while it runs, then 0 or 1 once. Latches back to IDLE on the read that
     * reports completion, so the viewer's tool_reap() — which polls every frame
     * and expects to see a finish exactly once — behaves as it does with
     * waitpid.
     */
    public static synchronized int poll() {
        int s = state;
        if (s == OK || s == FAILED) {
            state = IDLE;
            worker = null;
            return s;
        }
        return s == RUNNING ? RUNNING : IDLE;
    }

    /** What a tool is: argv in, lines out, did it work. */
    interface Tool {
        boolean run(String[] argv, PrintStream out) throws Exception;
    }
}
