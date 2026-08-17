/* Tadpole — does a saved setting really reach the disk?
 *
 *     make cfg-selftest && ./viewer/cfg-selftest
 *
 * WHY THIS IS NOT A UI TEST. The Graphics panel now says "Saving..." and then
 * "Saved!" when you change a row. A cue like that is worse than no cue at all
 * if it is a timer dressed up as a result — it would go on saying "Saved!"
 * over a read-only config directory, a full disk, or any of the other ways a
 * write actually fails, and the user would trust it. So ui_cfg_save() reports
 * whether the write SUCCEEDED, the panel shows "Saved!" only on a 1, and this
 * pins down that the 1 and the 0 both mean what they say.
 *
 * Nothing here draws: tadpole_ui.c needs SDL2 and zlib to link but no viewer
 * symbol at all, and the config path never calls into either.
 */
#include "tadpole_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Not in tadpole_ui.h: it exists for this test and for the panel that draws it,
 * and nothing else in the viewer should be asking. */
const char *ui_save_cue_text(Uint32 age, int ok);

static int fails;

/* Both NULL, or both the same text. */
static int eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

static void ok(const char *what, int cond)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) fails++;
}

static char g_root[512];

static void cfgdir(char *out, size_t n)  { snprintf(out, n, "%s/tadpole", g_root); }
static void cfgfile(char *out, size_t n) { snprintf(out, n, "%s/tadpole/ui.cfg", g_root); }

/* The whole file, or NULL. */
static char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (!buf) { fclose(f); return 0; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
	buf[n] = 0;
	fclose(f);
	if (len) *len = n;
	return buf;
}

static int has_line(const char *hay, const char *needle)
{
	return hay && strstr(hay, needle) != 0;
}

/* Is there any ui.cfg.* left lying around? A temp file that outlives the save
 * is litter, and one that outlives a FAILED save is litter the next reader
 * could mistake for the real thing. */
static int temp_litter(void)
{
	char dir[600], path[700];
	struct stat st;
	cfgdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/ui.cfg.tmp", dir);
	return stat(path, &st) == 0;
}

int main(void)
{
	char file[700], dir[600], *txt;
	int rc;

	snprintf(g_root, sizeof(g_root), "/tmp/tadpole-cfg-selftest-%d", (int)getpid());
	setenv("XDG_CONFIG_HOME", g_root, 1);
	/* cfg_path() prefers XDG_CONFIG_HOME, but HOME is the fallback and a stray
	 * one would send this at the developer's real config. */
	setenv("HOME", g_root, 1);
	unsetenv("LOCALAPPDATA");
	mkdir(g_root, 0700);

	cfgfile(file, sizeof(file));
	cfgdir(dir, sizeof(dir));

	printf("a save that works says so, and the value is really there\n");
	ui_cfg()->frame_cap = 30;
	rc = ui_cfg_save();
	ok("ui_cfg_save() -> 1", rc == 1);
	txt = slurp(file, 0);
	ok("ui.cfg exists", txt != 0);
	ok("frame_cap 30 is on disk", has_line(txt, "frame_cap 30\n"));
	/* The last key the writer emits. If it is present the file is not a
	 * half-written one that happened to get far enough to include the row we
	 * asked about. */
	ok("the file is complete", has_line(txt, "connect_nag "));
	ok("no temp file left behind", !temp_litter());
	free(txt);

	printf("uncapped is a value, not an absence\n");
	ui_cfg()->frame_cap = 0;
	rc = ui_cfg_save();
	ok("ui_cfg_save() -> 1", rc == 1);
	txt = slurp(file, 0);
	ok("frame_cap 0 is on disk", has_line(txt, "frame_cap 0\n"));
	free(txt);

	printf("a save that cannot work says THAT, and loses nothing\n");
	ui_cfg()->frame_cap = 30;
	ok("ui_cfg_save() -> 1", ui_cfg_save() == 1);
	{
		char before_len_txt[1];
		long before_len = 0, after_len = 0;
		char *before = slurp(file, &before_len);
		(void)before_len_txt;
		/* Read-only directory: neither the temp file nor a direct rewrite can
		 * be created. Skipped under root, which ignores the mode bits. */
		if (geteuid() == 0) {
			printf("  skip (running as root; mode bits do not apply)\n");
			free(before);
		} else {
			chmod(dir, 0500);
			ui_cfg()->frame_cap = 0;
			rc = ui_cfg_save();
			chmod(dir, 0700);
			ok("ui_cfg_save() -> 0", rc == 0);
			txt = slurp(file, &after_len);
			ok("the previous file is intact",
			   txt && before && after_len == before_len &&
			   memcmp(txt, before, (size_t)after_len) == 0);
			ok("it still says frame_cap 30", has_line(txt, "frame_cap 30\n"));
			ok("no temp file left behind", !temp_litter());
			free(txt);
			free(before);
		}
	}

	/* THE CUE ITSELF. What Graphics says after a save is a pure function of
	 * how long ago the save was and whether it worked, split out of the
	 * drawing precisely so it can be checked without a renderer — the viewer
	 * cannot be driven headlessly and synthetic clicks do not reach it under
	 * XWayland, so a pixel test here would be a test of the harness. */
	printf("the cue reports the save rather than performing one\n");
	ok("a save just now is still 'Saving...'",
	   eq(ui_save_cue_text(0, 1), "Saving..."));
	ok("and still is at 99ms",
	   eq(ui_save_cue_text(99, 1), "Saving..."));
	ok("a good save then says so",
	   eq(ui_save_cue_text(100, 1), "Saved!"));
	ok("and keeps saying so for a moment",
	   eq(ui_save_cue_text(1499, 1), "Saved!"));
	ok("then goes away", ui_save_cue_text(1500, 1) == 0);
	/* THE ONE THAT MATTERS: a failed write must never reach "Saved!", at any
	 * age. That is the whole reason the cue reports a result. */
	ok("a failed save NEVER says Saved!",
	   !eq(ui_save_cue_text(100, 0), "Saved!") &&
	   !eq(ui_save_cue_text(1000, 0), "Saved!") &&
	   !eq(ui_save_cue_text(7999, 0), "Saved!"));
	ok("it says the opposite", eq(ui_save_cue_text(100, 0), "NOT saved!"));
	ok("and stays up far longer than a success",
	   eq(ui_save_cue_text(7999, 0), "NOT saved!"));
	ok("before it too goes away", ui_save_cue_text(8001, 0) == 0);

	/* Tidy up: /tmp is shared and this ran under a predictable name. */
	unlink(file);
	rmdir(dir);
	rmdir(g_root);

	printf("\n");
	if (fails) { printf("FAILED (%d)\n", fails); return 1; }
	printf("all good\n");
	return 0;
}
