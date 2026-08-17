/* Glasspole — path anchoring and the sysroot rule. See path.h for why the
 * anchoring half exists at all.
 *
 * Moved here out of cpu.cpp so that it can be exercised without dynarmic:
 * tests/path_test.cpp links this file against the POSIX host backend and drives
 * it with the exact strings out of a user's log. A rule this load-bearing that
 * could only be tested by booting a title was a rule nobody tested.
 */
#include "path.h"
#include "host.h"

#include <string.h>

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int gp_path_kind(const char *p)
{
    if (!p || !p[0])
        return GP_PATH_RELATIVE;
    /* A drive letter is unambiguous: no path this project produces has a colon
     * in its second byte for any other reason. */
    if (is_alpha(p[0]) && p[1] == ':')
        return GP_PATH_HOST;
    /* "\\server\share". A single leading backslash is NOT rooted here — on
     * Linux it is an ordinary (if strange) filename, and on Windows nothing
     * hands us one, because every host path that reaches the guest has been
     * through the viewer's separator conversion first. */
    if (p[0] == '\\' && p[1] == '\\')
        return GP_PATH_HOST;
    if (p[0] == '/')
        return GP_PATH_ROOTED;
    return GP_PATH_RELATIVE;
}

int gp_path_anchor(const char *p, char drive, char *out, size_t cap)
{
    size_t n;

    if (!p || !out || !drive)
        return 0;
    if (gp_path_kind(p) != GP_PATH_ROOTED)
        return 0;
    /* "//server/share" — a UNC that has been through the viewer's backslash
     * conversion. Prefixing a drive to it would turn a server name into a
     * directory name, and the failure would be an unhelpful ENOENT. Leave it;
     * Win32 resolves it correctly as it stands. */
    if (p[1] == '/')
        return 0;

    n = strlen(p);
    if (n + 3 > cap)          /* letter, colon, NUL */
        return 0;
    out[0] = drive;
    out[1] = ':';
    memcpy(out + 2, p, n + 1);
    return 1;
}

std::string gp_host_path(const std::string &sysroot, const std::string &cwd,
                         const std::string &guest)
{
    /* AN EMPTY PATH IS NOT THE SYSROOT. It used to return it, which made
     * stat("") succeed — because the sysroot is a directory and it certainly
     * exists — where every real system answers ENOENT.
     *
     * That one line cost most of a day. BaseUtils::FileExists("") came back
     * true, so CAppManager::LoadNewApp believed a Leapster view frame was
     * present, wrapped the home screen in it, and asserted in LightningJSON
     * shortly after. Identical syscalls to qemu, identical files, and a
     * different answer to a question about a path that was not there. */
    if (guest.empty()) return std::string();

    /* A PATH THAT NAMES ITS OWN VOLUME IS ALREADY THE ANSWER, and neither the
     * cwd nor the sysroot has anything to add to it.
     *
     * This branch is what makes it safe for the host to hand the guest a path
     * on a second drive. TADPOLE_DIR is the one that does: the install can be
     * anywhere, but the runtime directory lives under %LOCALAPPDATA%, so on an
     * E: install the guest is told about a C: path and must not have it
     * mangled. Without this it fell into the relative branch below — "C:" does
     * not begin with a slash, so the rule read it as a filename — and every
     * framebuffer open became
     *
     *   /C:/Users/<name>/AppData/Local/Tadpole/run/fb0.bin
     *
     * which is the classic shape of the bug: an is-absolute test that only
     * knows about the leading slash calls "C:\x" relative. */
    if (gp_path_kind(guest.c_str()) == GP_PATH_HOST) return guest;

    /* Relative paths resolve against the guest's cwd, not against the sysroot
     * root. Getting this wrong is invisible until a title chdirs.
     *
     * JOINING IS ALL THAT HAPPENS HERE. The result goes back through exactly
     * the same sysroot rule as any absolute path below, and it has to, because
     * THE CWD IS NOT ALWAYS A GUEST PATH: the shim prepends the sysroot to
     * every chdir itself (qemu-user does not translate chdir at all), so what
     * arrives is often an absolute HOST path. This branch used to prepend the
     * sysroot unconditionally and with no fallback, which produced
     *
     *   open Data/misc.waf -> /tmp/sr/tmp/sr/LF/Bulk/ProgramFiles/<pkg>/Data/misc.waf = -2
     *
     * — the sysroot twice — for EVERY relative open a title makes after
     * entering its own package, which is nearly all of them. Measured on Clam
     * Prix: all six of its Data .waf opens failed, waf_open asserted on a null
     * archive, and the title exited 127 with a screen it had never drawn.
     *
     * RECURSING RATHER THAN TRACKING A SECOND CWD. The first fix for this kept
     * the resolved host directory alongside the guest one and used it here.
     * That works only when SYS_chdir was the thing that set it, and it needs
     * two strings to stay in step forever. Handing the joined path back to the
     * sysroot-first/literal-second rule needs no extra state and is right for
     * either kind of cwd, because "does the sysroot have it" is the question
     * that distinguishes them. It also covers open, stat, access, mkdir,
     * unlink, rename and opendir in one place instead of at each call. */
    if (gp_path_kind(guest.c_str()) == GP_PATH_RELATIVE) {
        std::string base = cwd;
        if (base.empty() || base.back() != '/') base += '/';
        return gp_host_path(sysroot, cwd, base + guest);
    }

    /* qemu-user's -L semantics, and they matter more than they look. The
     * sysroot wins when it has the file, and otherwise the path is used as it
     * stands — which is what lets an absolute HOST path through.
     *
     * Tadpole depends on exactly this. LD_LIBRARY_PATH points at
     * runtime/shimlibs on the host, and TADPOLE_DIR is /tmp/tadpole; neither
     * exists inside the rootfs, so both have to fall through. Prepending the
     * sysroot unconditionally would hide the shim from the guest's own linker
     * and the failure would look like the shim simply not working. */
    std::string in_root = sysroot + guest;
    struct gp_statbuf st;
    if (gp_stat(in_root.c_str(), &st) == 0) return in_root;
    return guest;
}
