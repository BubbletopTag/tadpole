/* Glasspole — what a path is anchored to, which is not a question Linux asks.
 *
 * On Linux "absolute" is one bit: the leading slash, and there is exactly one
 * root. Windows has one root PER VOLUME, and a path that begins with a
 * separator but names no drive — `\Users\bob`, or `/Users/bob`, which Win32
 * treats identically — is resolved against whichever drive the process happens
 * to be sitting on. That is not a detail. It is the difference between a file
 * being found and a file being looked for on the wrong disk, and nothing in
 * the failure says which disk was searched.
 *
 * Tadpole walked into this deliberately. The viewer spells every path it hands
 * the guest drive-relative (`C:\x\y` -> `/x/y`) because a drive letter cannot
 * survive a colon-separated LD_LIBRARY_PATH, and because a backslash is not a
 * separator to uClibc. That rewrite is sound for the paths inside the install
 * tree — they all live on one volume, and the guest is started with its
 * current directory in that tree. It is NOT sound for a path on a DIFFERENT
 * volume, because the spelling has room for exactly one drive and the run
 * needs two: the install, and the per-user runtime directory under
 * %LOCALAPPDATA%. On a C: install the two coincide and the bug is invisible.
 * Install to E: and the guest goes looking for E:\Users\<name>\AppData\...,
 * which does not exist, and every framebuffer, state and event node under
 * TADPOLE_DIR silently fails to open. See gp_path_anchor below.
 *
 * These rules are deliberately IDENTICAL on both platforms, so that the Linux
 * build — the one with an oracle next to it — tests the Windows behaviour.
 */
#ifndef GP_PATH_H
#define GP_PATH_H

#include <stddef.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/* How a path finds its way to a file. */
enum {
    /* Nothing in the path says where to start: resolve it against a cwd. */
    GP_PATH_RELATIVE = 0,
    /* Rooted, but on no named volume: "/LF/Base/bin/AppManager". This is what
     * a guest path looks like, and also what the viewer's drive-relative
     * rewrite produces. On Windows it needs an anchor before it means
     * anything; on Linux it is already complete. */
    GP_PATH_ROOTED   = 1,
    /* Names its own volume and therefore needs nothing from us: "C:\Users\bob",
     * "C:/Users/bob", or a UNC "\\server\share". The guest never writes one of
     * these; the HOST does, and it must be passed through untouched. */
    GP_PATH_HOST     = 2
};

int gp_path_kind(const char *p);

/* Give a rooted-but-driveless path the drive it is missing, so that it stops
 * depending on where the process happens to be standing.
 *
 * Writes "<drive>:<p>" into out and returns 1 when it applies; returns 0 and
 * touches nothing otherwise — which is the important half. A path that ALREADY
 * names its volume is never re-homed, so a runtime directory on C: keeps its C:
 * even when the program itself was started from E:. That refusal is the fix;
 * the anchoring is only what makes the remaining driveless paths stop caring
 * about the current directory.
 *
 * Refused for: relative paths (they mean "from here" and still do), paths that
 * already carry a drive or a UNC prefix, a leading "//" (a UNC in the forward-
 * slash spelling the viewer produces — anchoring it would corrupt a server
 * name), and anything too long for out.
 */
int gp_path_anchor(const char *p, char drive, char *out, size_t cap);

#ifdef __cplusplus
}   /* extern "C" */

/* Guest path -> host path: qemu-user's -L semantics, and the one place the
 * sysroot exists. See the long note on the implementation.
 *
 * Free rather than a Machine method so that it can be tested without dynarmic;
 * Machine::HostPath is a one-line call to this.
 */
std::string gp_host_path(const std::string &sysroot, const std::string &cwd,
                         const std::string &guest);
#endif

#endif /* GP_PATH_H */
