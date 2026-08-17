/* Glasspole — the path rules, driven directly.
 *
 * WHY THIS IS A HOST TEST AND NOT AN ARM ONE. The other files in this
 * directory are guest programs: the emulator's job is to run them, and the
 * oracle is qemu-arm sitting next to it. Path resolution has no such oracle,
 * because the interesting half only misbehaves on Windows — where there is no
 * qemu-arm to diff against and, on this machine, no way to execute the binary
 * at all. So the rules are written to be platform-independent (see path.h) and
 * proved here, on Linux, with the exact strings out of the log that prompted
 * them.
 *
 * THE LOG. A user installed to E:\MiscGames\romstuff\Glasspole and got:
 *
 *   [tadpole] fcreate /MiscGames/romstuff/Glasspole/runtime/sysroot/...   (fine)
 *   [0x5] CreateHandle: No framebuffer allocation available
 *   <ASSERT>: [0x0] Unsupported destination PixelFormat used 0
 *
 * Every path in that log has had its drive letter removed by the viewer, which
 * is deliberate — see drel() in tadpole_view.c. The sysroot paths survive it
 * because the guest's current directory is in the install tree, so the missing
 * drive is filled back in as E:. The RUNTIME directory does not survive it: it
 * lives under %LOCALAPPDATA% on C:, and the same rewrite sends the guest to
 * E:\Users\... instead. Nothing under TADPOLE_DIR then opens — no fb0.bin, no
 * state.bin, no event nodes — and "No framebuffer allocation available" is the
 * guest saying so in its own words.
 *
 * Build and run:  see glasspole/CMakeLists.txt (target gp-path-test), or
 *   c++ -I../src -o /tmp/t path_test.cpp ../src/path.cpp ../src/host_posix.c -lpthread
 */
#include "path.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int g_fail;

static void eq(const char *what, const std::string &got, const std::string &want)
{
    if (got == want) {
        printf("  ok    %s\n", what);
        return;
    }
    printf("  FAIL  %s\n           got  \"%s\"\n           want \"%s\"\n",
           what, got.c_str(), want.c_str());
    g_fail++;
}

static void yes(const char *what, int cond)
{
    if (cond) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s\n", what);
    g_fail++;
}

/* gp_path_anchor as a string, so the failures read like the thing they model:
 * "" means it refused, which for a path that already names its volume is the
 * whole point. */
static std::string anchor(const char *p, char drive)
{
    char out[1024];
    return gp_path_anchor(p, drive, out, sizeof out) ? std::string(out)
                                                     : std::string();
}

/* ---- 1. how a path is classified ---------------------------------------- */
static void test_kind(void)
{
    printf("path kind\n");
    yes("a guest path is rooted",
        gp_path_kind("/LF/Base/bin/AppManager") == GP_PATH_ROOTED);
    yes("a relative path is relative",
        gp_path_kind("Data/main.waf") == GP_PATH_RELATIVE);
    yes("the empty path is relative (and never the sysroot)",
        gp_path_kind("") == GP_PATH_RELATIVE);
    /* The one that was missing. An is-absolute test that only knows the
     * leading slash reads "C:\Users\bob" as a FILENAME. */
    yes("a backslash drive path names its own volume",
        gp_path_kind("C:\\Users\\bob\\AppData") == GP_PATH_HOST);
    yes("a forward-slash drive path names its own volume",
        gp_path_kind("C:/Users/bob/AppData") == GP_PATH_HOST);
    yes("a lowercase drive letter counts",
        gp_path_kind("e:/MiscGames") == GP_PATH_HOST);
    yes("a UNC path names its own volume",
        gp_path_kind("\\\\nas\\share\\Tadpole") == GP_PATH_HOST);
    yes("a single leading backslash is just a filename",
        gp_path_kind("\\weird") == GP_PATH_RELATIVE);
}

/* ---- 2. anchoring, and above all refusing to anchor ---------------------- */
static void test_anchor(void)
{
    printf("anchoring a driveless path\n");

    /* The install tree, as the viewer spells it for the guest. Anchoring it on
     * the drive glasspole.exe was loaded from puts it back where it came from,
     * whatever directory the process happens to be standing in. */
    eq("the install tree is anchored on the image drive",
       anchor("/MiscGames/romstuff/Glasspole/runtime/sysroot/LF/Base/bin/AppManager", 'E'),
       "E:/MiscGames/romstuff/Glasspole/runtime/sysroot/LF/Base/bin/AppManager");

    /* THE BUG, as one line. This is what the viewer used to send for
     * TADPOLE_DIR on an E: install: %LOCALAPPDATA% with the C: filed off. The
     * only drive available to fill it back in is the install's, so the guest
     * goes looking on E: for a directory that exists on C:. */
    eq("a driveless %LOCALAPPDATA% lands on the install drive — the bug",
       anchor("/Users/bob/AppData/Local/Tadpole/run/fb0.bin", 'E'),
       "E:/Users/bob/AppData/Local/Tadpole/run/fb0.bin");

    /* THE FIX, as one line. Keep the drive and there is nothing to guess. */
    eq("a path that names C: is never re-homed onto E:",
       anchor("C:/Users/bob/AppData/Local/Tadpole/run/fb0.bin", 'E'),
       "");

    eq("a relative path still means \"from here\"",
       anchor("runtime/sysroot", 'E'), "");
    eq("a UNC in forward-slash spelling is left alone",
       anchor("//nas/share/Tadpole/run", 'E'), "");
    eq("an empty path is left alone", anchor("", 'E'), "");

    {   /* No room, no rewrite — and no half-written buffer either. */
        char small[8];
        yes("a path too long for the buffer is refused",
            gp_path_anchor("/aaaaaaaaaaaaaaaa", 'E', small, sizeof small) == 0);
    }
}

/* ---- 3. the sysroot rule ------------------------------------------------- */
static void test_host_path(void)
{
    /* A real sysroot on disk, because the rule's whole question is "does the
     * sysroot have this file". Answering it from a mock would test the mock. */
    char tmpl[] = "/tmp/gp-path-XXXXXX";
    const char *root = mkdtemp(tmpl);
    std::string sr, deep;
    char buf[1024];

    if (!root) { printf("  FAIL  could not make a temporary sysroot\n"); g_fail++; return; }
    sr = root;
    snprintf(buf, sizeof buf, "%s/LF", root);            gp_mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/LF/Base", root);       gp_mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/LF/Base/there", root);
    {
        gp_file *f = 0;
        if (gp_open(buf, GP_O_WRONLY | GP_O_CREAT | GP_O_TRUNC, 0644, &f) == 0)
            gp_close(f);
    }

    printf("guest path -> host path\n");

    eq("the sysroot wins when it has the file",
       gp_host_path(sr, "/", "/LF/Base/there"), sr + "/LF/Base/there");
    eq("and the literal path is used when it does not",
       gp_host_path(sr, "/", "/LF/Base/absent"), "/LF/Base/absent");
    eq("the empty path stays empty", gp_host_path(sr, "/", ""), "");
    eq("a relative path joins the cwd first",
       gp_host_path(sr, "/LF/Base", "there"), sr + "/LF/Base/there");

    /* THE BUG, at the layer that would have hidden the fix. Even once the
     * viewer keeps the drive letter, this rule had to stop reading "C:" as the
     * first component of a filename — otherwise the C: path arrives and is
     * immediately turned into /C:/Users/... against the guest's cwd. */
    eq("a host path on another volume is passed through untouched",
       gp_host_path(sr, "/LF/Bulk/ProgramFiles/pkg",
                    "C:/Users/bob/AppData/Local/Tadpole/run/fb0.bin"),
       "C:/Users/bob/AppData/Local/Tadpole/run/fb0.bin");
    eq("...in backslash spelling too",
       gp_host_path(sr, "/", "C:\\Users\\bob\\AppData\\Local\\Tadpole\\run\\ev0"),
       "C:\\Users\\bob\\AppData\\Local\\Tadpole\\run\\ev0");
    eq("...and the sysroot is not prepended to it",
       gp_host_path(sr, "/", "E:/MiscGames/romstuff/Glasspole/runtime/sysroot/LF/Base/there"),
       "E:/MiscGames/romstuff/Glasspole/runtime/sysroot/LF/Base/there");

    /* Housekeeping. Leaving temporary sysroots behind is how a test suite
     * becomes the reason a disk fills up. */
    snprintf(buf, sizeof buf, "%s/LF/Base/there", root); gp_unlink(buf);
    snprintf(buf, sizeof buf, "%s/LF/Base", root);       gp_rmdir(buf);
    snprintf(buf, sizeof buf, "%s/LF", root);            gp_rmdir(buf);
    gp_rmdir(root);
}

int main(void)
{
    test_kind();
    test_anchor();
    test_host_path();
    if (g_fail) {
        printf("\n%d failure%s\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("\nall path rules hold\n");
    return 0;
}
