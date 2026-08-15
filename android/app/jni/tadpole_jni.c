/* Tadpole for Android — getting the viewer's output off the device.
 *
 * THIS IS THE FILE THAT MAKES THE PORT WORKABLE, and it is worth being blunt
 * about why. Tadpole's entire debugging story is printf. tadpole_view.c,
 * tadpole_hle.c and the shim all report what they did by writing lines to
 * stdout and stderr, and every harness in tools/ — probe-launch.sh,
 * compat-sweep.sh, the crash triage — works by grepping that output. The
 * HANDOVER's own reproducers are `grep -a "guest exited with status 139"
 * run.log`.
 *
 * On Android an app process's stdout and stderr are connected to /dev/null.
 * Not a pipe nobody reads — /dev/null. Every diagnostic line the viewer has
 * ever printed vanishes silently, and a port that does not fix this first is a
 * port debugged by screenshot.
 *
 * ### How it is fixed
 *
 * pipe(), dup2 both ends of it over fds 1 and 2, and run a thread that reads
 * the other end and hands each line to __android_log_write. After that,
 *
 *     adb logcat -s tadpole
 *
 * is `run.log`, and every existing grep works unchanged.
 *
 * Two details that are not obvious and cost time if you get them wrong:
 *
 * - setvbuf(_IOLBF) on both streams. Bionic sees a pipe rather than a terminal
 *   and picks full buffering, 4 KB at a time. Without this, output arrives in
 *   4 KB gulps and — much worse — anything printed before a crash is still in
 *   the buffer when the process dies, so the last thing the viewer said before
 *   it fell over is exactly the part you never see.
 *
 * - logcat drops any single message over ~4000 bytes, so long lines are split.
 *
 * ### And the runtime directory
 *
 * nativeSetenv exists because the viewer already has the hook this port needs
 * and it is an environment variable, not a flag: TADPOLE_DIR overrides the
 * compiled-in /tmp/tadpole. Android has no writable /tmp — the directory is
 * absent, not merely unwritable — so the FIFOs and the shared arena have to
 * live in getFilesDir(). SDL2 2.32 has no nativeSetenv of its own (it has
 * nativeSetupJNI, nativeRunMain and a dozen others, but not this), so it is
 * four lines here.
 */
#include <jni.h>
#include "tadpole_ui.h"   /* ui_cfg(), for nativeRotate below */
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "tadpole"

static int   g_pipe[2] = { -1, -1 };
static pthread_t g_thread;

static void emit(int prio, char *s, size_t n)
{
	/* logcat silently drops a message past about 4000 bytes. Cut at 3500
	 * rather than lose the line. */
	while (n > 3500) {
		char save = s[3500];
		s[3500] = 0;
		__android_log_write(prio, TAG, s);
		s[3500] = save;
		s += 3500;
		n -= 3500;
	}
	if (n) {
		s[n] = 0;
		__android_log_write(prio, TAG, s);
	}
}

static void *pump(void *unused)
{
	static char buf[8192];
	size_t used = 0;
	ssize_t got;

	(void)unused;
	while ((got = read(g_pipe[0], buf + used, sizeof(buf) - used - 1)) > 0) {
		char *start = buf, *nl;
		used += (size_t)got;
		buf[used] = 0;
		while ((nl = memchr(start, '\n', (size_t)(buf + used - start))) != NULL) {
			*nl = 0;
			emit(ANDROID_LOG_INFO, start, (size_t)(nl - start));
			start = nl + 1;
		}
		/* Keep the tail — a partial line — and shuffle it down. If a single
		 * line ever fills the whole buffer, flush it rather than deadlock. */
		used = (size_t)(buf + used - start);
		if (used) memmove(buf, start, used);
		if (used >= sizeof(buf) - 1) {
			emit(ANDROID_LOG_INFO, buf, used);
			used = 0;
		}
	}
	return NULL;
}

/* Runs when the dynamic linker maps libmain.so, which is before SDLActivity
 * calls SDL_main — so the redirection is in place for the viewer's very first
 * line, including anything printed by its own constructors. */
__attribute__((constructor))
static void tadpole_log_redirect(void)
{
	if (g_pipe[0] != -1) return;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	if (pipe(g_pipe) != 0) {
		__android_log_write(ANDROID_LOG_ERROR, TAG,
		                    "pipe() failed: stdout stays on /dev/null");
		return;
	}
	dup2(g_pipe[1], STDOUT_FILENO);
	dup2(g_pipe[1], STDERR_FILENO);

	if (pthread_create(&g_thread, NULL, pump, NULL) != 0) {
		__android_log_write(ANDROID_LOG_ERROR, TAG, "log pump thread failed");
		return;
	}
	pthread_detach(g_thread);
	__android_log_write(ANDROID_LOG_INFO, TAG,
	                    "stdout/stderr redirected to logcat tag \"" TAG "\"");
}

/* WHICH WAY THE VIEWER THINKS IT IS POINTING, so the ACTIVITY can turn to
 * match. The ROT chip in the menu bar has always rotated the picture inside the
 * window; on a phone the window is the whole screen, so rotating the picture
 * without rotating the screen leaves a portrait title letterboxed inside a
 * landscape display with black down both sides.
 *
 * READ, NOT PUSHED, and that is the whole reason this is three lines. Making
 * the viewer notify us would mean a callback in tadpole_ui.c, which is shared
 * with the desktop build and would then carry an Android-shaped hook for no
 * desktop reason. ui_cfg() is already public — it is how the viewer's own
 * settings are read — and the value it returns is a plain int written by the
 * UI thread and read by the UI thread's poller. The Java side asks four times
 * a second and turns the activity when the answer changes.
 *
 * Safe to call at any point after libmain.so is mapped: g_cfg is a static with
 * a compile-time initialiser, so it holds the default rotation before main()
 * runs and the saved one afterwards.
 */
JNIEXPORT jint JNICALL
Java_org_tadpole_view_TadpoleActivity_nativeRotate(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return (jint)ui_cfg()->rotate;
}

JNIEXPORT void JNICALL
Java_org_tadpole_view_TadpoleActivity_nativeSetenv(JNIEnv *env, jclass cls,
                                                   jstring jname, jstring jval)
{
	const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
	const char *val  = (*env)->GetStringUTFChars(env, jval, NULL);
	(void)cls;
	setenv(name, val, 1);
	__android_log_print(ANDROID_LOG_INFO, TAG, "setenv %s=%s", name, val);
	(*env)->ReleaseStringUTFChars(env, jname, name);
	(*env)->ReleaseStringUTFChars(env, jval, val);
}
