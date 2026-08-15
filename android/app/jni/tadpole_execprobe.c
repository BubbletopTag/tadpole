/* A whole executable, shipped inside the APK as lib/<abi>/libtadpoleexec.so.
 *
 * IT IS NOT A LIBRARY. It is a PIE executable with a name that lies, and the
 * lie is load-bearing.
 *
 * Android will not let an app execute a file it wrote. Measured, on an API 35
 * image, with the app's own private directory:
 *
 *     probe: exec from app files   DENIED (execve refused — SELinux/noexec)
 *
 * That is the untrusted_app SELinux domain losing exec permission on
 * app_data_file, which happened in Android 10 and applies to every app on the
 * device regardless of its target API. It matters here more than it does to
 * almost any other program, because Tadpole's entire model is running binaries
 * the user supplied: qemu-arm, the guest's uClibc executables out of a real
 * device's firmware, the shim libraries. None of those can be downloaded to
 * the app's data directory and run.
 *
 * The one path that still works is the APK's own native library directory. It
 * is populated by the package installer, not by the app, so it lives in a
 * different SELinux type, and files there are executable. Every terminal
 * emulator and every "run a real Linux binary" app on Android does this. The
 * cost is that anything you want to execute must be inside the APK at install
 * time, must be named lib*.so, and must be re-shipped to change — which for
 * qemu-arm is fine and for the user's own firmware is not.
 *
 * This program exists to prove that the path works, rather than to assert it.
 * It prints one line and exits 42.
 */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	(void)argc;
	printf("tadpole exec probe: running as pid %d from %s, %zu-bit\n",
	       (int)getpid(), argv[0], sizeof(void *) * 8);
	fflush(stdout);
	return 42;
}
