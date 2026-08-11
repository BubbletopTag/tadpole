/* Tadpole — the double-click.
 *
 * Windows users get one thing to click and a window, instead of a PowerShell
 * incantation. The whole job: find the project root relative to this exe,
 * point the viewer at it, start the viewer. Everything else — including
 * "there is no firmware yet" — is the viewer's setup wizard's job, which
 * already exists and re-tests real state on every page. The one failure this
 * exe owns is "the viewer is not here", which gets a message box, not
 * silence.
 *
 * The root is found by walking up from the exe looking for tadpole.sh, so
 * the same binary works from the repo root (where a copy of it should live)
 * and from tadpole/viewer/build (where it is born). TADPOLE_PROJECT is set
 * explicitly because the viewer's argv[0] derivation strips three path
 * components — right for viewer/tadpole-view on Linux, one short for
 * viewer/build/tadpole-view.exe here.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* WHICH BUILD THIS IS — CARRIED, NOT PRINTED.
 *
 * tadpole.exe is a GUI-subsystem program with no console to print to, so there
 * is nothing useful for it to *say*; what matters is that the string is IN it.
 * The release scripts verify an artifact by reading the version back out of
 * the finished binary with `strings`, and until now this was one of two
 * executables in the installer that contained no version to find — so it could
 * not be checked, only assumed, which is the state that let a "dev" build ship.
 *
 * `volatile` and the exported name keep the linker from discarding a constant
 * that no code path reads. That is the entire point of it: it is written for
 * the build to inspect, not for the program to use. */
#ifndef TADPOLE_VERSION
#define TADPOLE_VERSION "dev"
#endif
volatile const char tadpole_build_version[] = TADPOLE_VERSION;

static void fail(const WCHAR *what)
{
    MessageBoxW(NULL, what, L"Tadpole", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static void dirname_inplace(WCHAR *p)
{
    WCHAR *s = p + lstrlenW(p);
    while (s > p && *s != L'\\' && *s != L'/') s--;
    *s = 0;
}

static int exists(const WCHAR *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
    WCHAR root[MAX_PATH], probe[MAX_PATH + 32], viewer[MAX_PATH + 64];
    WCHAR line[MAX_PATH + 256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int up;

    (void)hi; (void)hp; (void)show;

    GetModuleFileNameW(NULL, root, MAX_PATH);
    dirname_inplace(root);

    /* Walk up to the tree that has tadpole.sh at its root. */
    for (up = 0; up < 5; up++) {
        wsprintfW(probe, L"%s\\tadpole.sh", root);
        if (exists(probe))
            break;
        dirname_inplace(root);
        if (!root[0])
            fail(L"This tadpole.exe is not inside a Tadpole tree —\n"
                 L"no tadpole.sh found above it.");
    }

    wsprintfW(viewer, L"%s\\tadpole\\viewer\\build\\tadpole-view.exe", root);
    if (!exists(viewer)) {
        wsprintfW(viewer, L"%s\\tadpole-view.exe", root);
        if (!exists(viewer))
            fail(L"The viewer is not built yet.\n\n"
                 L"From an MSYS2 MINGW64 shell:\n"
                 L"  cd tadpole/viewer\n"
                 L"  cmake -S . -B build -G Ninja && ninja -C build\n\n"
                 L"(docs/windows-viewer-build.md has the full steps.)");
    }

    SetEnvironmentVariableW(L"TADPOLE_PROJECT", root);

    /* Anything given to the launcher goes straight to the viewer, so a
     * shortcut with --boot lands on the system menu instead of the front
     * end, and -s 3 opens bigger. */
    wsprintfW(line, L"\"%s\" %s", viewer, cmd ? cmd : L"");

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(viewer, line, NULL, NULL, FALSE, 0, NULL, root,
                        &si, &pi))
        fail(L"The viewer would not start.");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
