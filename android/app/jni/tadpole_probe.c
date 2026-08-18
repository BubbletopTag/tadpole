/* Tadpole for Android — measuring what the platform will actually let us do.
 *
 * Every one of these is a mechanism Tadpole depends on today, and every answer
 * here is measured on the device rather than read off a compatibility table.
 * Android's documentation is written for app developers and is vague in exactly
 * the places an emulator cares about, and the rules changed several times
 * (SELinux domains in 5.0, W^X in 10, exec-from-data in 10, scoped storage in
 * 11), so what a given ROM actually permits is a question with a local answer.
 *
 * Runs at startup and prints to logcat under "tadpole". Results are quoted in
 * android/NOTES-arm32.md and android/NOTES-shim.md.
 *
 * The questions, in the order they decide things:
 *
 *  1. Is there a writable /tmp?          Tadpole's arena and FIFOs live there.
 *  2. Does mkfifo work in the app dir?   The audio and event channels are FIFOs.
 *  3. Does a MAP_SHARED file mapping?    The framebuffer arena is one.
 *  4. Does memfd_create work?            The modern way to do the same thing.
 *  5. Can we make memory executable?     Glasspole is a JIT. Without this it
 *                                        cannot exist on this platform at all.
 *  6. Can we exec a binary we wrote?     qemu-arm, the guest's own binaries.
 *  7. Can we exec from the APK's lib/?   The known way round (6).
 *  8. Does fork() work?                  gp_fork(), VideoDaemon, the guest.
 *  9. Is there a 32-bit userspace?       Decides the whole port.
 */
#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TAG "tadpole"
#define P(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, "probe: " fmt, ##__VA_ARGS__)

static void probe_tmp(void)
{
	struct stat st;
	if (stat("/tmp", &st) != 0) {
		P("/tmp                  ABSENT (%s) — not merely unwritable", strerror(errno));
		return;
	}
	P("/tmp                  exists, writable=%s", access("/tmp", W_OK) == 0 ? "yes" : "NO");
}

static void probe_fifo(const char *dir)
{
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/probe.fifo", dir);
	unlink(path);
	if (mkfifo(path, 0600) != 0) {
		P("mkfifo in app dir     FAILED (%s)", strerror(errno));
		return;
	}
	/* O_RDWR on a FIFO never blocks, which is the trick the viewer already
	 * uses so that a reader with no writer does not hang. */
	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		P("mkfifo in app dir     made, but open FAILED (%s)", strerror(errno));
	} else {
		char buf[8] = { 0 };
		int ok = (write(fd, "tadpole", 7) == 7 && read(fd, buf, 7) == 7);
		P("mkfifo in app dir     OK, round trip %s", ok ? "OK" : "FAILED");
		close(fd);
	}
	unlink(path);
}

static void probe_shared_map(const char *dir)
{
	char path[512];
	int fd;
	void *p;

	snprintf(path, sizeof(path), "%s/probe.arena", dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { P("MAP_SHARED file       open FAILED (%s)", strerror(errno)); return; }
	if (ftruncate(fd, 65536) != 0) { P("MAP_SHARED file       ftruncate FAILED"); close(fd); return; }
	p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) P("MAP_SHARED file       mmap FAILED (%s)", strerror(errno));
	else { *(volatile int *)p = 1; P("MAP_SHARED file       OK (the framebuffer arena's mechanism)"); munmap(p, 65536); }
	close(fd);
	unlink(path);
}

static void probe_memfd(void)
{
	int fd = (int)syscall(__NR_memfd_create, "tadpole", 0);
	void *a, *b;

	if (fd < 0) { P("memfd_create          FAILED (%s)", strerror(errno)); return; }
	if (ftruncate(fd, 65536) != 0) { P("memfd_create          OK, ftruncate FAILED"); close(fd); return; }
	a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	b = mmap(NULL, 65536, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	P("memfd_create          OK; RW map %s, RX map %s%s%s",
	  a == MAP_FAILED ? "FAILED" : "OK",
	  b == MAP_FAILED ? "FAILED" : "OK",
	  b == MAP_FAILED ? " (" : "",
	  b == MAP_FAILED ? strerror(errno) : "");
	P("                      ^ the dual mapping is how a JIT writes code it "
	  "may then execute without ever holding W+X");
	if (a != MAP_FAILED) munmap(a, 65536);
	if (b != MAP_FAILED) munmap(b, 65536);
	close(fd);
}

/* The two ways a JIT might get executable memory, both tried, because Android
 * 10 made the first one conditional and the docs do not say on what. */
static void probe_exec_memory(void)
{
	void *p;

	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) P("anon RWX mmap         FAILED (%s)", strerror(errno));
	else { P("anon RWX mmap         OK — W^X is NOT enforced on anonymous memory here"); munmap(p, 4096); }

	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { P("anon RW->RX mprotect  initial mmap FAILED"); return; }
	if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0)
		P("anon RW->RX mprotect  FAILED (%s) — a JIT cannot work this way", strerror(errno));
	else
		P("anon RW->RX mprotect  OK (Glasspole's code cache can live here)");
	munmap(p, 4096);
}

/* Copy a known-good binary somewhere and try to run it from there. This is the
 * rule that decides whether a qemu-arm or a guest binary can be shipped as data
 * — downloaded firmware, a cartridge backup — or has to be smuggled in as a
 * fake .so inside the APK. */
static void probe_exec_from(const char *label, const char *dst)
{
	char buf[65536];
	int in, outfd;
	ssize_t n;
	pid_t pid;
	int st = 0;

	in = open("/system/bin/true", O_RDONLY);
	if (in < 0) in = open("/system/bin/sh", O_RDONLY);
	if (in < 0) { P("exec from %-11s could not read a binary to copy", label); return; }

	unlink(dst);
	outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0700);
	if (outfd < 0) { P("exec from %-11s cannot create there (%s)", label, strerror(errno)); close(in); return; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(outfd, buf, (size_t)n) != n) break;
	close(in); close(outfd);
	chmod(dst, 0700);

	/* DID execve FAIL, OR DID THE PROGRAM RUN AND EXIT 127? Those are not the
	 * same thing, and this probe answered "SELinux refused it" for both — for
	 * two years, in every launch log, and in NOTES-arm32.md's Option A, which
	 * cites it.
	 *
	 * The copied binary is /system/bin/true, which on Android is a SYMLINK TO
	 * TOYBOX. Copy it to probe.bin, exec it, and toybox looks at argv[0], does
	 * not recognise "probe.bin" as one of its applets, prints
	 *
	 *     toybox: Unknown command probe.bin
	 *
	 * — a line sitting in the middle of the probe output the whole time — and
	 * exits 127. The exec had already succeeded.
	 *
	 * So the child reports the failure itself, down a close-on-exec pipe: if
	 * execve works the pipe closes empty, and if it does not the errno arrives.
	 * Nothing the program chooses to exit with can be mistaken for either. */
	{
		int pfd[2];
		int err = 0;
		ssize_t got;

		if (pipe(pfd) != 0) {
			P("exec from %-11s could not make a pipe", label);
			unlink(dst);
			return;
		}
		fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

		pid = fork();
		if (pid == 0) {
			char *const av[] = { (char *)dst, NULL };
			close(pfd[0]);
			execv(dst, av);
			err = errno;
			(void)!write(pfd[1], &err, sizeof err);
			_exit(127);
		}
		close(pfd[1]);
		got = read(pfd[0], &err, sizeof err);
		close(pfd[0]);
		waitpid(pid, &st, 0);

		if (got == (ssize_t)sizeof err)
			P("exec from %-11s DENIED (execve: %s)", label, strerror(err));
		else
			P("exec from %-11s OK — execve succeeded (program exit %d)",
			  label, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	}
	unlink(dst);
}

/* The APK's own lib dir, which the package installer populated and the app
 * cannot write to. This is the only place an app may execute a file from. */
static void probe_exec_packaged(const char *path)
{
	pid_t pid;
	int st = 0;

	if (access(path, F_OK) != 0) {
		P("exec from APK lib     %s is not packaged (%s)", path, strerror(errno));
		return;
	}
	pid = fork();
	if (pid == 0) {
		char *const av[] = { (char *)path, NULL };
		execv(path, av);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 42)
		P("exec from APK lib     OK — a packaged binary CAN be executed");
	else if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
		P("exec from APK lib     DENIED (execve refused)");
	else
		P("exec from APK lib     ran but exited %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void probe_fork(void)
{
	pid_t pid = fork();
	int st = 0;
	if (pid < 0) { P("fork()                FAILED (%s)", strerror(errno)); return; }
	if (pid == 0) _exit(42);
	waitpid(pid, &st, 0);
	P("fork()                %s", (WIFEXITED(st) && WEXITSTATUS(st) == 42) ? "OK" : "child did not run");
}

static void probe_arm32(void)
{
	P("/system/bin/linker    %s", access("/system/bin/linker", F_OK) == 0
	  ? "PRESENT — a 32-bit userspace exists, ARM32 guest binaries CAN be loaded"
	  : "ABSENT — 64-bit-only ROM, nothing 32-bit can execute here at all");
	P("/system/bin/linker64  %s", access("/system/bin/linker64", F_OK) == 0 ? "present" : "absent");
	P("this process          %zu-bit", sizeof(void *) * 8);
}

JNIEXPORT void JNICALL
Java_org_tadpole_view_TadpoleActivity_nativeProbe(JNIEnv *env, jclass cls, jstring jdir,
                                                  jstring jlibdir)
{
	const char *dir    = (*env)->GetStringUTFChars(env, jdir, NULL);
	const char *libdir = (*env)->GetStringUTFChars(env, jlibdir, NULL);
	char path[512];

	(void)cls;
	P("---- Tadpole platform probe ----");
	probe_arm32();
	probe_tmp();
	probe_fifo(dir);
	probe_shared_map(dir);
	probe_memfd();
	probe_exec_memory();
	probe_fork();

	snprintf(path, sizeof(path), "%s/probe.bin", dir);
	probe_exec_from("app files", path);
	/* The lib dir is read-only to the app, so this one cannot be a copy —
	 * libtadpoleexec.so is a real PIE executable packaged into the APK by
	 * android/build-apk.sh. See tadpole_execprobe.c. */
	snprintf(path, sizeof(path), "%s/libtadpoleexec.so", libdir);
	probe_exec_packaged(path);
	P("---- end probe ----");

	(*env)->ReleaseStringUTFChars(env, jdir, dir);
	(*env)->ReleaseStringUTFChars(env, jlibdir, libdir);
}
