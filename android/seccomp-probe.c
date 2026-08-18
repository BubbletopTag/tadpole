/* Which syscalls does an Android app's seccomp filter refuse?
 *
 * zygote installs a seccomp-bpf filter on every app process, and a filter is
 * INHERITED ACROSS execve — so a guest the app spawns runs under it too, and
 * dies with SIGSYS (logcat: "signo 31") having printed nothing. This says
 * exactly which numbers are refused, instead of finding them one crash at a
 * time.
 *
 * Build and run — it must be a CHILD OF THE APP to inherit the filter, so the
 * way to run it is to put it where the app is about to exec something:
 *
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi26-clang \
 *       -O1 -o seccomp-probe android/seccomp-probe.c
 *
 *   # give it an interpreter the app's launch path will accept: PT_INTERP is
 *   # rewritten to a relative path (see android_relinterp in tadpole_view.c),
 *   # so the linker has to be reachable from the sysroot.
 *   adb shell 'S=/data/user/0/org.tadpole.view/files/runtime/sysroot; \
 *              mkdir -p $S/system/bin && ln -sf /system/bin/linker $S/system/bin/linker'
 *   # ...rewrite this binary's PT_INTERP to "system/bin/linker", drop it in as
 *   # $S/LF/Base/bin/AppManager, and press File -> Run System Menu.
 *
 * Output goes to stdout, which the viewer already pipes into its guest log
 * (~/.local/state/tadpole/tadpole.log under the app's files directory).
 *
 * The answer on the dev tablet (API 27, armeabi-v7a) is in
 * android/NOTES-arm32.md under "What the filter blocks": thirteen of the
 * forty-nine tried, and NOT simply "the legacy ones" — readlink(85) and
 * open(5) are allowed while stat(106) is not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

static sigjmp_buf jb;
static volatile int trapped;
static void on_sigsys(int s) { (void)s; trapped = 1; siglongjmp(jb, 1); }

struct ent { int nr; const char *name; long a, b, c, d, e; };

static struct ent T[] = {
  {  5, "open",        0,0,0,0,0 },
  {  8, "creat",       0,0,0,0,0 },
  {  9, "link",        0,0,0,0,0 },
  { 10, "unlink",      0,0,0,0,0 },
  { 12, "chdir",       0,0,0,0,0 },
  { 14, "mknod",       0,0,0,0,0 },
  { 15, "chmod",       0,0,0,0,0 },
  { 19, "lseek",      -1,0,0,0,0 },
  { 33, "access",      0,0,0,0,0 },
  { 38, "rename",      0,0,0,0,0 },
  { 39, "mkdir",       0,0,0,0,0 },
  { 40, "rmdir",       0,0,0,0,0 },
  { 41, "dup",        -1,0,0,0,0 },
  { 42, "pipe",        0,0,0,0,0 },
  { 48, "signal",      0,0,0,0,0 },
  { 54, "ioctl",      -1,0,0,0,0 },
  { 55, "fcntl",      -1,0,0,0,0 },
  { 64, "getppid",     0,0,0,0,0 },
  { 67, "sigaction",   0,0,0,0,0 },
  { 82, "select_old",  0,1,1,1,1 },
  { 83, "symlink",     0,0,0,0,0 },
  { 85, "readlink",    0,0,0,0,0 },
  {102, "socketcall",  0,0,0,0,0 },
  {106, "stat",        0,0,0,0,0 },
  {107, "lstat",       0,0,0,0,0 },
  {108, "fstat",      -1,0,0,0,0 },
  {114, "wait4",      -1,0,0,0,0 },
  {116, "sysinfo",     0,0,0,0,0 },
  {117, "ipc",         0,0,0,0,0 },
  {122, "uname",       0,0,0,0,0 },
  {126, "sigprocmask", 0,0,0,0,0 },
  {140, "_llseek",    -1,0,0,0,0 },
  {141, "getdents",   -1,0,0,0,0 },
  {142, "_newselect",  0,0,0,0,1 },
  {143, "flock",      -1,0,0,0,0 },
  {148, "fdatasync",  -1,0,0,0,0 },
  {168, "poll",        0,0,0,0,0 },
  {183, "getcwd",      0,0,0,0,0 },
  {195, "stat64",      0,0,0,0,0 },
  {196, "lstat64",     0,0,0,0,0 },
  {197, "fstat64",    -1,0,0,0,0 },
  {217, "getdents64", -1,0,0,0,0 },
  {221, "fcntl64",    -1,0,0,0,0 },
  {266, "statfs64",    0,0,0,0,0 },
  {267, "fstatfs64",  -1,0,0,0,0 },
  {322, "openat",     -1,0,0,0,0 },
  {327, "fstatat64",  -1,0,0,0,0 },
  {332, "readlinkat", -1,0,0,0,0 },
  {334, "faccessat",  -1,0,0,0,0 },
  {  0, NULL,          0,0,0,0,0 }
};

int main(int argc, char **argv)
{
    struct sigaction sa;
    FILE *out = stdout;
    int i;

    if (argc > 1) { out = fopen(argv[1], "w"); if (!out) out = stdout; }

    sa.sa_handler = on_sigsys;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSYS, &sa, NULL);

    fprintf(out, "seccomp probe: uid=%d pid=%d\n", (int)getuid(), (int)getpid());
    for (i = 0; T[i].name; i++) {
        long r;
        trapped = 0;
        if (sigsetjmp(jb, 1) == 0) {
            errno = 0;
            r = syscall(T[i].nr, T[i].a, T[i].b, T[i].c, T[i].d, T[i].e);
            fprintf(out, "%-14s %3d  allowed (ret=%ld errno=%d)\n",
                    T[i].name, T[i].nr, r, errno);
        } else {
            fprintf(out, "%-14s %3d  BLOCKED (SIGSYS)\n", T[i].name, T[i].nr);
        }
        fflush(out);
    }
    fprintf(out, "probe done\n");
    fflush(out);
    return 0;
}
