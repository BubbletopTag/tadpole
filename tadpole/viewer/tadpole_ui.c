/* Tadpole — pixel-art UI chrome. See tadpole_ui.h for the coordinate contract.
 *
 * No SDL2_ttf and no zenity on this machine, so the font, the widgets and the
 * file chooser are all hand-rolled. That is not a workaround — a bitmap font
 * and 1px bevels are exactly the look being asked for, and they scale with the
 * window as pixel art instead of going blurry.
 */
#include "tadpole_ui.h"
#include "tadpole_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tadpole_port.h"

/* Long enough for any real path; the browser can walk anywhere. */
#define PATHMAX 1024

#define FONT_W 5
#define FONT_H 7
#define GLYPH_ADV 6          /* 5px cell + 1px gap */

/* Set by the Makefile; "dev" in a working copy. */
#ifndef TADPOLE_VERSION
#define TADPOLE_VERSION "dev"
#endif

/* Symbol glyphs appended past ASCII by tools/genfont.py. */
#define GL_DIAMOND  "\x7f"
#define GL_RIGHT    "\x80"
#define GL_UP       "\x81"
#define GL_DOWN     "\x82"
#define GL_LEFT     "\x83"
#define GL_SUB      "\x84"
#define GL_RADIO_0  "\x85"
#define GL_RADIO_1  "\x86"
#define GL_CHECK_0  "\x87"
#define GL_CHECK_1  "\x88"
#define GL_ROTATE   "\x89"

/* ---- theme --------------------------------------------------------------
 * Dark green, a few flat tones, 1px bevels. Deliberately few colours: pixel
 * interfaces read better with a tight palette than with gradients.
 */
#define C_VOID      0x0C1E14U     /* behind everything */
/* TWO PALETTES, ONE CODEBASE.
 *
 * Tadpole runs the LeapPad's software under qemu-arm. Glasspole runs it under
 * our own ARM emulator, which is the only option on Windows. They are the same
 * application and the same viewer, but they are NOT the same product: their
 * compatibility differs, and a user who is told "Tadpole runs this title"
 * should not be looking at a screen driven by the other one.
 *
 * So the window says which it is, and the chrome changes colour with it —
 * green for Tadpole, blue for Glasspole. Every call site below is unchanged;
 * the C_* names now read through the active palette instead of being
 * compile-time constants, so there is exactly one copy of this code and no
 * possibility of the two drifting apart.
 *
 * The blue is the green rotated towards cyan at the same lightness, so the
 * layout, contrast and bevels behave identically. */
struct palette {
	unsigned bar, bar_hi, panel, panel_hi, edge_lt, edge_dk;
	unsigned text, text_dim, accent, shadow;
};

static const struct palette pal_tadpole = {
	0x14301FU, 0x2A6642U, 0x14301FU, 0x2A6642U, 0x4E9C6BU, 0x08180FU,
	0xD8F5E4U, 0x6E9B80U, 0x8CE0A6U, 0x030806U
};

static const struct palette pal_glasspole = {
	0x142A30U, 0x2A5A66U, 0x142A30U, 0x2A5A66U, 0x4E8C9CU, 0x081418U,
	0xD8EEF5U, 0x6E8F9BU, 0x8CCFE0U, 0x030608U
};

static const struct palette *g_pal = &pal_tadpole;

#define C_BAR       (g_pal->bar)      /* menu bar */
#define C_BAR_HI    (g_pal->bar_hi)   /* hovered/open bar item */
#define C_PANEL     (g_pal->panel)    /* dropdown + dialog body */
#define C_PANEL_HI  (g_pal->panel_hi)
#define C_EDGE_LT   (g_pal->edge_lt)  /* bevel light */
#define C_EDGE_DK   (g_pal->edge_dk)  /* bevel dark */
#define C_TEXT      (g_pal->text)
#define C_TEXT_DIM  (g_pal->text_dim)
#define C_ACCENT    (g_pal->accent)
#define C_SHADOW    (g_pal->shadow)

/* WHICH PRODUCT IS THIS?
 *
 * Decided once, at startup, from what is actually going to run the guest:
 *
 *   1. TADPOLE_BRAND, if set, wins. An escape hatch for testing either look.
 *   2. On Windows it is always Glasspole. qemu-arm's user mode cannot exist
 *      there, so there is nothing else it could be.
 *   3. TADPOLE_QEMU naming a glasspole binary — that is how tadpole.sh selects
 *      the emulator, so it is the most direct statement of intent available.
 *   4. Otherwise Tadpole, which is the historical default and stays the
 *      default: a source checkout with qemu installed behaves as it always has.
 */
int ui_brand_is_glasspole(void)
{
	static int cached = -1;
	if (cached >= 0) return cached;

	const char *b = getenv("TADPOLE_BRAND");
	if (b && *b) {
		cached = (strcmp(b, "glasspole") == 0);
		return cached;
	}
#ifdef _WIN32
	cached = 1;
	return cached;
#else
	const char *q = getenv("TADPOLE_QEMU");
	cached = (q && *q && strstr(q, "glasspole") != NULL);
	return cached;
#endif
}

const char *ui_brand_name(void)
{
	return ui_brand_is_glasspole() ? "Glasspole" : "Tadpole";
}

void ui_brand_apply(void)
{
	g_pal = ui_brand_is_glasspole() ? &pal_glasspole : &pal_tadpole;
}

struct rgb { Uint8 r, g, b; };

static struct rgb unpack(unsigned v)
{
	struct rgb c;
	c.r = (Uint8)(v >> 16); c.g = (Uint8)(v >> 8); c.b = (Uint8)v;
	return c;
}

/* ---- state --------------------------------------------------------------- */

static SDL_Texture   *g_font;         /* one row of glyphs, white on alpha */
static SDL_Texture   *g_logo;
static int            g_logo_w, g_logo_h;
static Uint32        *g_logo_px;   /* kept for SDL_SetWindowIcon */
static char           g_proj[PATHMAX];
/* DESIGNATED INITIALISERS, because the positional list had silently rotted.
 *
 * `gl_hle` was added as the SECOND field of struct ui_settings and this list was
 * never updated — its comment still read "gl, gl_debug, dumpframe, dumptex,
 * shim_debug", naming five fields for what by then were six. Ten values for
 * eleven fields, so everything after gl_hle was off by one and the defaults
 * actually shipped were not the ones written here:
 *
 *     rotate           2     (not a rotation; the field holds 0/90/180/270)
 *     scale            0
 *     touch_debug      1
 *     audio_on         260
 *     audio_latency_ms 0     <- the cap on how far ahead of the speaker we run
 *
 * Anyone without a ~/.config/tadpole/ui.cfg ran with those. Naming each field
 * makes the next insertion harmless.
 *
 * GL ON by default: without it every native Brio title asserts in the stock
 * libEGL and exits. See the note in tadpole.sh.
 *
 * HLE ON by default as well, as of the first title to render a full 2D UI
 * through it. The software rasteriser is the fallback, not the reference — it
 * draws simple screens correctly and visibly mangles busy ones, and it is far
 * slower. This only decides who rasterises; nothing about the guest changes. */
static struct ui_settings g_cfg = {
	.gl               = 1,
	.gl_hle           = 1,
	.debug_level      = 1,      /* the device's own serial log, nothing more */
	.log_to_file      = 1,
	.update_check     = 1,
	.gl_dumpframe     = 0,
	.gl_dumptex       = 0,
	.rotate           = 0,
	.scale            = 2,
	.touch_debug      = 0,
	.audio_on         = 1,
	.audio_latency_ms = 260,
	.audio_pace       = 1,
	.frame_cap        = 60,
	.hle_strict       = 0,
	.msaa             = 0,
	.render_scale     = 1,
	.io_delay_us      = 0,
	.tslib            = 0,
	.boot_on_start    = 0,
	.games_dir        = "",
};
static enum ui_action g_action;
static char           g_action_path[PATHMAX];
static char           g_status[128];
static int            g_running;
static int            g_mx, g_my;     /* last mouse position, logical */

/* menus */
static int  g_open_menu = -1;         /* index into MENUS, -1 = closed */
static int  g_hot_item  = -1;

/* modal */
enum modal_kind { M_NONE = 0, M_ABOUT, M_UPDATE, M_GFX, M_AUDIO, M_PAD, M_DEBUG, M_SYSTEM,
                  M_FILES, M_MSG, M_WIZARD, M_PROGRESS, M_GAMES, M_APPS };
static enum modal_kind g_modal;

/* ---- update check state -------------------------------------------------
 * Filled by ui_update_line() from tools/check-update.py's output. The notes
 * are kept as flat lines rather than a tree: they are only ever scrolled and
 * drawn, and a release body is already line-oriented text. */
#define UP_MAXLINE 160
#define UP_MAXNOTE 220
static char g_up_status[16];
static char g_up_cur[48];
static char g_up_latest[48];
static char g_up_title[64];
static char g_up_asset[512];
static char g_up_reason[96];
static char g_up_note[UP_MAXNOTE][UP_MAXLINE];
static unsigned char g_up_head[UP_MAXNOTE];   /* 1 = a version heading */
static int  g_up_nnote;
static int  g_up_scroll;
static int  g_up_count;
static int  g_up_silent;
static char  g_msg_title[64], g_msg_body[512];
/* Non-zero when M_MSG is a yes/no rather than an acknowledgement. */
static int   g_confirm;
static int   g_sys_ready = -1;   /* -1 = not yet checked */

/* file browser */
static char  g_fb_dir[PATHMAX];
static char  g_fb_filter[16];         /* extension, "" = any */
static enum ui_action g_fb_action;    /* what to emit on choose */
/* Where to go when the browser closes. Without this, choosing a file left NO
 * modal at all and the wizard appeared to vanish mid-setup. */
static enum modal_kind g_fb_return;
/* The browser is being used to PICK A VALUE, not to trigger an action:
 * the chosen path becomes the profile photo and nothing else happens. */
static int   g_fb_pick_profile;
static char  g_fb_title[64];
struct fentry { char name[256]; int isdir; };
static struct fentry *g_fb_list;
static int   g_fb_n, g_fb_sel, g_fb_top;
#define FB_ROWS 11

/* ---- progress ------------------------------------------------------------ */

#define PROG_LINES 9
#define PROG_COLS  56
static char g_prog[PROG_LINES][PROG_COLS];
static int  g_prog_n;              /* lines written, monotonic */
static int  g_prog_running;
static int  g_prog_ok;
static int  g_prog_pct = -1;
static char g_prog_title[64];

void ui_progress_begin(const char *title)
{
	snprintf(g_prog_title, sizeof(g_prog_title), "%s", title ? title : "Working");
	memset(g_prog, 0, sizeof g_prog);
	g_prog_n = 0;
	g_prog_running = 1;
	g_prog_ok = 0;
	g_prog_pct = -1;
	g_modal = M_PROGRESS;
}

void ui_progress_line(const char *line)
{
	size_t n;
	if (!line || !*line) return;
	/* A scrolling window over the tail: the interesting part of a long install
	 * is always the most recent line. */
	snprintf(g_prog[g_prog_n % PROG_LINES], PROG_COLS, "%s", line);
	n = strlen(g_prog[g_prog_n % PROG_LINES]);
	while (n && (g_prog[g_prog_n % PROG_LINES][n-1] == '\n' ||
	             g_prog[g_prog_n % PROG_LINES][n-1] == '\r'))
		g_prog[g_prog_n % PROG_LINES][--n] = 0;
	g_prog_n++;
}

void ui_progress_done(int ok)
{
	g_prog_running = 0;
	g_prog_ok = ok;
}

void ui_progress_pct(int pct)
{
	if (pct > 100) pct = 100;
	g_prog_pct = pct;
}

int ui_progress_active(void) { return g_modal == M_PROGRESS; }

/* ---- primitives ---------------------------------------------------------- */

static void fill(SDL_Renderer *r, int x, int y, int w, int h, unsigned col)
{
	SDL_Rect rc = { x, y, w, h };
	struct rgb c = unpack(col);
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
	SDL_RenderFillRect(r, &rc);
}

/* 1px bevel: light top/left, dark bottom/right (raised) or the reverse. */
static void bevel(SDL_Renderer *r, int x, int y, int w, int h, int raised)
{
	unsigned lt = raised ? C_EDGE_LT : C_EDGE_DK;
	unsigned dk = raised ? C_EDGE_DK : C_EDGE_LT;
	fill(r, x, y, w, 1, lt);
	fill(r, x, y, 1, h, lt);
	fill(r, x, y + h - 1, w, 1, dk);
	fill(r, x + w - 1, y, 1, h, dk);
}

static void panel(SDL_Renderer *r, int x, int y, int w, int h)
{
	fill(r, x + 2, y + 2, w, h, C_SHADOW);      /* drop shadow */
	fill(r, x, y, w, h, C_PANEL);
	bevel(r, x, y, w, h, 1);
}

static int text_w(const char *s) { return (int)strlen(s) * GLYPH_ADV; }

static void text(SDL_Renderer *r, int x, int y, const char *s, unsigned col)
{
	struct rgb c = unpack(col);
	SDL_SetTextureColorMod(g_font, c.r, c.g, c.b);
	for (; *s; s++, x += GLYPH_ADV) {
		int idx = (unsigned char)*s - UI_FONT_FIRST;
		SDL_Rect src, dst;
		if (idx < 0 || idx >= UI_FONT_COUNT)
			continue;
		src.x = idx * FONT_W; src.y = 0; src.w = FONT_W; src.h = FONT_H;
		dst.x = x; dst.y = y; dst.w = FONT_W; dst.h = FONT_H;
		SDL_RenderCopy(r, g_font, &src, &dst);
	}
}

/* Same, but centred in [x, x+w). */
static void text_c(SDL_Renderer *r, int x, int w, int y, const char *s, unsigned col)
{
	text(r, x + (w - text_w(s)) / 2, y, s, col);
}

static int inside(int px, int py, int x, int y, int w, int h)
{
	return px >= x && py >= y && px < x + w && py < y + h;
}

/* mkdir -p. The directories we create live under XDG paths that are NOT
 * guaranteed to exist — ~/.local/state in particular is absent on a fresh
 * account — and a single mkdir() of a two-deep path fails with ENOENT, which
 * is how the first log file quietly went nowhere. */
static void mkdir_p(const char *path)
{
	char tmp[PATHMAX];
	char *p;
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		/* "C:/" — a drive root is not ours to create, and trying returns
		 * EACCES rather than the EEXIST a Unix "/" gives. Skip it. */
		if (p > tmp + 1 && p[-1] == ':') continue;
		*p = 0;
		tp_mkdir(tmp);
		*p = '/';
	}
	tp_mkdir(tmp);
}

/* dst = a + "/" + b, always NUL-terminated, never warns about truncation
 * because the length is checked rather than left to snprintf. */
static void path_join(char *dst, size_t n, const char *a, const char *b)
{
	size_t la = strlen(a);
	int slash = (la && a[la - 1] != '/');
	if (la >= n) la = n - 1;
	memcpy(dst, a, la);
	if (slash && la + 1 < n) dst[la++] = '/';
	{
		size_t lb = strlen(b);
		if (la + lb >= n) lb = (n > la + 1) ? n - la - 1 : 0;
		memcpy(dst + la, b, lb);
		dst[la + lb] = 0;
	}
}

/* Copy, truncating from the LEFT so the tail of a long path stays visible. */
static void path_tail(char *dst, size_t n, const char *src)
{
	size_t l = strlen(src);
	if (l < n) { memcpy(dst, src, l + 1); return; }
	memcpy(dst, "...", 3);
	memcpy(dst + 3, src + l - (n - 4), n - 4);
	dst[n - 1] = 0;
}

/* ---- setup wizard -------------------------------------------------------
 *
 * Deliberately Windows-style: a banner down the left, one idea per page, and
 * Back/Next/Finish at the bottom right. It exists because Tadpole ships with NO
 * LeapFrog code, so a first run has nothing to boot — and a wall of "missing
 * rootfs" errors is a poor way to explain that you must supply firmware from
 * hardware you own.
 *
 * Every page re-tests real state rather than remembering that it ran, so this
 * doubles as a repair tool: reopen it and it shows exactly what is missing.
 */
enum { WIZ_WELCOME = 0, WIZ_SYSTEM, WIZ_PROFILE, WIZ_GAMES, WIZ_DONE,
       WIZ_PAGES };
static int g_wiz_page;

/* ---- the profile being composed on WIZ_PROFILE --------------------------
 *
 * A freshly installed system boots to Create Profile and stops there — the
 * screen draws and nothing gets past it. Rather than leave setup blocked
 * behind a broken screen, the wizard collects the same three things and
 * writes the profile itself; the device then finds one already made.
 */
static char g_prof_name[21];
static int  g_prof_grade = 1;
static char g_prof_pic[PATHMAX];
static int  g_prof_focus;        /* the name field has the keyboard */
static int  g_prof_made;         /* one was created this session */

/* WHAT THE WIZARD DROPPED, AND WHY.
 *
 * It used to have a page of its own headed "What you need", listing qemu-arm,
 * SDL2 and "firmware tools: run ./tools/check-deps.sh". That page was written
 * when those were the user's problem. They are not any more — the AppImage
 * carries a static qemu and a Python with ubi_reader — so a whole page of the
 * setup flow existed to explain a shopping list that is now usually empty.
 *
 * It is folded into the welcome page as one line, which says "everything is
 * here" when it is, and names the missing piece when it is not. Four pages
 * instead of five, and none of them is a lecture.
 */
struct prereq { int rootfs, sysroot, games, qemu, qemu_bundled, fwtools; };

static int dir_has_entries(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int n = 0;
	if (!d) return 0;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.') { n = 1; break; }
	closedir(d);
	return n;
}

/* Is <name> anywhere on PATH? A dozen access() calls, which is cheap enough to
 * do while drawing — spawning a shell to ask `command -v` would not be. */
static int which_exists(const char *name)
{
	const char *path = getenv("PATH");
	char buf[PATHMAX];
	const char *p, *e;
	if (!path) path = "/usr/bin:/bin:/usr/local/bin";
	for (p = path; *p; p = e + (*e == ':')) {
		size_t n;
		e = strchr(p, ':');
		if (!e) e = p + strlen(p);
		n = (size_t)(e - p);
		if (!n || n + strlen(name) + 2 > sizeof(buf)) { if (!*e) break; continue; }
		memcpy(buf, p, n);
		buf[n] = '/';
		snprintf(buf + n + 1, sizeof(buf) - n - 1, "%s", name);
		if (access(buf, X_OK) == 0) return 1;
		if (!*e) break;
	}
	return 0;
}

static void prereq_check(struct prereq *p)
{
	char path[PATHMAX * 2];
	DIR *d;
	struct dirent *e;

	memset(p, 0, sizeof *p);
	/* The real layout nests deeper than one level —
	 *   rootfs/stock-4.6.0.784/1221351650/ubi_rfs
	 * — so search a couple of levels rather than assuming. Getting this wrong
	 * makes the wizard insist firmware is missing on a working install, and
	 * pop up on every launch. */
	path_join(path, sizeof(path), g_proj, "rootfs");
	if ((d = opendir(path))) {
		while ((e = readdir(d)) && !p->rootfs) {
			char lvl1[PATHMAX * 2], sub[PATHMAX * 2];
			DIR *d2;
			struct dirent *e2;
			if (e->d_name[0] == '.') continue;
			snprintf(sub, sizeof(sub), "%s/%s/ubi_rfs", path, e->d_name);
			if (dir_has_entries(sub)) { p->rootfs = 1; break; }
			snprintf(lvl1, sizeof(lvl1), "%s/%s", path, e->d_name);
			if (!(d2 = opendir(lvl1))) continue;
			while ((e2 = readdir(d2))) {
				if (e2->d_name[0] == '.') continue;
				snprintf(sub, sizeof(sub), "%s/%s/ubi_rfs", lvl1, e2->d_name);
				if (dir_has_entries(sub)) { p->rootfs = 1; break; }
			}
			closedir(d2);
		}
		closedir(d);
	}
	path_join(path, sizeof(path), g_proj, "runtime/sysroot/LF/Base");
	p->sysroot = dir_has_entries(path);
	path_join(path, sizeof(path), g_proj, "runtime/sysroot/LF/Bulk/ProgramFiles");
	p->games = dir_has_entries(path);

	/* The bundle first, then the host — the same order tools/lib-deps.sh uses,
	 * so the wizard never says "missing" about something Tadpole brought with
	 * it. TADPOLE_DEPS is set by the AppImage's AppRun; build/deps is where
	 * tools/fetch-deps.sh puts things in a source checkout. */
	{
		const char *deps = getenv("TADPOLE_DEPS");
		char cand[PATHMAX * 2];
		if (deps && *deps) {
			snprintf(cand, sizeof(cand), "%s/bin/qemu-arm", deps);
			if (access(cand, X_OK) == 0) { p->qemu = 1; p->qemu_bundled = 1; }
			snprintf(cand, sizeof(cand), "%s/python/bin/python3", deps);
			if (access(cand, X_OK) == 0) p->fwtools = 1;
		}
		if (!p->qemu) {
			snprintf(cand, sizeof(cand), "%s/build/deps/bin/qemu-arm", g_proj);
			if (access(cand, X_OK) == 0) { p->qemu = 1; p->qemu_bundled = 1; }
		}
		if (!p->fwtools) {
			snprintf(cand, sizeof(cand), "%s/build/deps/python/bin/python3", g_proj);
			if (access(cand, X_OK) == 0) p->fwtools = 1;
		}
		/* AND THE WINDOWS SPELLING OF THE SAME THING, which is not bin/python3
		 * — the embeddable distribution puts python.exe at the top of its own
		 * directory, with no bin. Without this the installer ships a Python,
		 * ubi_reader and lzallright, and the wizard's first page still says
		 * "No firmware extractor. Run ./tools/fetch-deps.sh": wrong about the
		 * dependency, and pointing at a shell script Windows cannot run
		 * either. Same failure as the qemu-arm check below, same fix.
		 *
		 * F_OK, not X_OK: executability is not a thing access() can report on
		 * Windows, and "it is there" is the whole question. */
		if (!p->fwtools) {
			snprintf(cand, sizeof(cand), "%s/build/deps/python/python.exe", g_proj);
			if (access(cand, F_OK) == 0) p->fwtools = 1;
		}
	}
	/* GLASSPOLE COUNTS AS AN EMULATOR. Without this the wizard is correct but
	 * useless on Windows: qemu-arm's user mode cannot exist there, so the
	 * check failed, and a machine that could run every Flash title was told it
	 * was missing a dependency it can never have.
	 *
	 * TADPOLE_QEMU is honoured first because that is how tadpole.sh chooses,
	 * then the build directory, with and without the .exe suffix. */
	if (!p->qemu) {
		const char *q = getenv("TADPOLE_QEMU");
		if (q && *q && access(q, X_OK) == 0) p->qemu = 1;
	}
	if (!p->qemu) {
		char cand[PATHMAX * 2];
		snprintf(cand, sizeof(cand), "%s/glasspole/build/glasspole", g_proj);
		if (access(cand, X_OK) == 0) p->qemu = 1;
		if (!p->qemu) {
			snprintf(cand, sizeof(cand), "%s/glasspole/build/glasspole.exe", g_proj);
			if (access(cand, X_OK) == 0) p->qemu = 1;
		}
	}
	if (!p->qemu)
		p->qemu = which_exists("glasspole");
	if (!p->qemu)
		p->qemu = which_exists("qemu-arm");
	if (!p->fwtools)
		p->fwtools = which_exists("ubireader_extract_files");
}

/* ---- font atlas ---------------------------------------------------------- */

static void font_build(SDL_Renderer *ren)
{
	int n = UI_FONT_COUNT, i, x, y;
	Uint32 *px = calloc((size_t)n * FONT_W * FONT_H, 4);
	if (!px) return;
	for (i = 0; i < n; i++)
		for (y = 0; y < FONT_H; y++)
			for (x = 0; x < FONT_W; x++)
				if (UI_FONT[i][y] & (1 << (FONT_W - 1 - x)))
					px[y * (n * FONT_W) + i * FONT_W + x] = 0xFFFFFFFFu;
	g_font = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
	                           SDL_TEXTUREACCESS_STATIC, n * FONT_W, FONT_H);
	if (g_font) {
		SDL_UpdateTexture(g_font, NULL, px, n * FONT_W * 4);
		SDL_SetTextureBlendMode(g_font, SDL_BLENDMODE_BLEND);
	}
	free(px);
}

/* ---- the tadpole icon ----------------------------------------------------
 * Decoded by hand rather than through SDL2_image so the viewer keeps exactly
 * one library dependency. Only the subset of PNG that tadpole.png actually
 * uses is handled: 8-bit RGBA, non-interlaced, which is what `file` reports.
 */
static unsigned be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	       ((unsigned)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return (pb <= pc) ? b : c;
}

/* zlib inflate is in libz, which SDL already links; declare the two calls we
 * need rather than pulling in the header for a single decode. */
extern int uncompress(unsigned char *dest, unsigned long *destLen,
                      const unsigned char *source, unsigned long sourceLen);

static SDL_Texture *png_texture(SDL_Renderer *ren, const char *path,
                                int *out_w, int *out_h, Uint32 **out_px)
{
	FILE *f = fopen(path, "rb");
	unsigned char *file = NULL, *idat = NULL, *raw = NULL;
	Uint32 *px = NULL;
	long len;
	unsigned pos = 8, idatn = 0, w = 0, h = 0;
	int bpp = 4, ok = 0;

	SDL_Texture *tex = NULL;

	if (!f) return NULL;
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len < 16 || !(file = malloc((size_t)len)) ||
	    fread(file, 1, (size_t)len, f) != (size_t)len) {
		fclose(f); free(file); return NULL;
	}
	fclose(f);

	while (pos + 8 <= (unsigned)len) {
		unsigned n = be32(file + pos);
		const unsigned char *ty = file + pos + 4, *dat = file + pos + 8;
		if (!memcmp(ty, "IHDR", 4)) {
			w = be32(dat); h = be32(dat + 4);
			if (dat[8] != 8 || dat[9] != 6 || dat[12] != 0) goto done;  /* RGBA8 only */
		} else if (!memcmp(ty, "IDAT", 4)) {
			unsigned char *t = realloc(idat, idatn + n);
			if (!t) goto done;
			idat = t; memcpy(idat + idatn, dat, n); idatn += n;
		} else if (!memcmp(ty, "IEND", 4)) {
			break;
		}
		pos += 12 + n;
	}
	if (!w || !h || !idat) goto done;
	{
		unsigned long need = (unsigned long)h * (w * bpp + 1);
		unsigned y, x;
		raw = malloc(need);
		if (!raw || uncompress(raw, &need, idat, idatn) != 0) goto done;
		px = malloc((size_t)w * h * 4);
		if (!px) goto done;
		for (y = 0; y < h; y++) {
			unsigned char *cur = raw + (size_t)y * (w * bpp + 1);
			int ft = *cur++;
			unsigned char *prev = (y == 0) ? NULL
			                    : raw + (size_t)(y - 1) * (w * bpp + 1) + 1;
			for (x = 0; x < w * (unsigned)bpp; x++) {
				int a = (x >= (unsigned)bpp) ? cur[x - bpp] : 0;
				int b = prev ? prev[x] : 0;
				int c = (prev && x >= (unsigned)bpp) ? prev[x - bpp] : 0;
				switch (ft) {
				case 1: cur[x] = (unsigned char)(cur[x] + a); break;
				case 2: cur[x] = (unsigned char)(cur[x] + b); break;
				case 3: cur[x] = (unsigned char)(cur[x] + ((a + b) >> 1)); break;
				case 4: cur[x] = (unsigned char)(cur[x] + paeth(a, b, c)); break;
				default: break;
				}
			}
			for (x = 0; x < w; x++)
				px[(size_t)y * w + x] =
					((Uint32)cur[x*4+3] << 24) | ((Uint32)cur[x*4] << 16) |
					((Uint32)cur[x*4+1] << 8) | cur[x*4+2];
		}
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		                        SDL_TEXTUREACCESS_STATIC, (int)w, (int)h);
		if (tex) {
			SDL_UpdateTexture(tex, NULL, px, (int)w * 4);
			SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
			if (out_w) *out_w = (int)w;
			if (out_h) *out_h = (int)h;
			if (out_px) { *out_px = px; px = NULL; }   /* caller owns it */
			ok = 1;
		}
	}
done:
	(void)ok;
	free(file); free(idat); free(raw); free(px);
	return tex;
}

/* The logo is simply the first user of the decoder above. */
static void logo_load(SDL_Renderer *ren, const char *path)
{
	g_logo = png_texture(ren, path, &g_logo_w, &g_logo_h, &g_logo_px);
}

/* ---- installed apps, for File -> Launch App -----------------------------
 *
 * This replaced a file browser filtered to .swf, which asked the user to know
 * where inside the guest filesystem a title's entry point lives — and could
 * not start a native title at all, because those need AppManager rather than
 * saplayer. The list is read from the installed packages' own meta.inf.
 */
#define AP_MAX 160
struct ap_entry {
	char pkg[64];
	char name[72];
	char version[24];
	char icon[PATHMAX];      /* absolute path to the package's own PNG */
	SDL_Texture *tex;
	int tw, th, tried;
};
static struct ap_entry g_ap[AP_MAX];
static int g_ap_n, g_ap_top, g_ap_rows = 6;

/* WHAT IS WORTH OFFERING TO LAUNCH.
 *
 * Not everything under ProgramFiles is something anyone means to start. The
 * *Widget packages are components another screen opens — KeyboardWidget is the
 * on-screen keyboard — and starting one alone gets you a crash or an empty
 * window, which reads as a broken emulator rather than as a misuse. Judged on
 * the package name because that is what LeapFrog named them by.
 */
static int ap_is_launchable(const char *pkg)
{
	size_t n = strlen(pkg);
	return !(n >= 6 && !strcmp(pkg + n - 6, "Widget"));
}

static void ap_free(void)
{
	int i;
	for (i = 0; i < g_ap_n; i++)
		if (g_ap[i].tex) { SDL_DestroyTexture(g_ap[i].tex); g_ap[i].tex = NULL; }
	g_ap_n = 0;
}

/* Copy one quoted meta.inf field into a fixed buffer.
 *
 * COPYING IMMEDIATELY IS THE POINT. The first version of this kept pointers
 * into the fgets buffer and used them only once every field had been seen — by
 * which time later lines had overwritten it. Every name came out empty while
 * the list was plainly populated: "1-15 of 104" in the corner, rows
 * highlighting under the pointer, and no text in any of them.
 */
static int meta_field(const char *line, const char *key, char *out, size_t n)
{
	const char *p = line, *q;
	size_t klen = strlen(key);

	/* ANCHORED TO THE START OF THE LINE, not found anywhere in it. meta.inf
	 * opens with MetaVersion="1.0", which contains Version=" as a substring —
	 * so a plain strstr reported every title in the library as version 1.0
	 * instead of its own. */
	while (*p == ' ' || *p == '\t') p++;
	if (strncmp(p, key, klen) != 0) return 0;
	p += klen;
	if (!(q = strchr(p, '"'))) return 0;
	if ((size_t)(q - p) >= n) return 0;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = 0;
	return out[0] != 0;
}

static void ap_reload(void)
{
	char dir[PATHMAX], meta[PATHMAX + 32], line[512], icon[128];
	DIR *d;
	struct dirent *de;

	ap_free();
	g_ap_top = 0;
	snprintf(dir, sizeof(dir), "%s/runtime/sysroot/LF/Bulk/ProgramFiles", g_proj);
	if (!(d = opendir(dir)))
		return;
	while ((de = readdir(d)) && g_ap_n < AP_MAX) {
		FILE *f;
		struct ap_entry e;
		int have_name = 0, have_so = 0;

		if (de->d_name[0] == '.' || !ap_is_launchable(de->d_name))
			continue;
		snprintf(meta, sizeof(meta), "%s/%s/meta.inf", dir, de->d_name);
		if (!(f = fopen(meta, "r")))
			continue;
		memset(&e, 0, sizeof(e));
		icon[0] = 0;
		while (fgets(line, sizeof(line), f)) {
			if (!have_name)
				have_name = meta_field(line, "Name=\"", e.name, sizeof(e.name));
			if (!have_so && strstr(line, "AppSo=\""))
				have_so = 1;
			if (!e.version[0])
				meta_field(line, "Version=\"", e.version, sizeof(e.version));
			if (!icon[0])
				meta_field(line, "Icon=\"", icon, sizeof(icon));
		}
		fclose(f);
		/* No entry point, nothing to start; no name, nothing to show. */
		if (!have_name || !have_so)
			continue;
		snprintf(e.pkg, sizeof(e.pkg), "%s", de->d_name);
		if (icon[0])
			snprintf(e.icon, sizeof(e.icon), "%s/%s/%s", dir, de->d_name, icon);
		g_ap[g_ap_n++] = e;
	}
	closedir(d);
	{
		int i, j;
		for (i = 1; i < g_ap_n; i++) {
			struct ap_entry t = g_ap[i];
			for (j = i - 1; j >= 0 && strcasecmp(g_ap[j].name, t.name) > 0; j--)
				g_ap[j + 1] = g_ap[j];
			g_ap[j + 1] = t;
		}
	}
}

/* One decode attempt per entry, however it goes: a package with a missing or
 * unreadable Icon= should cost one failed open, not one every frame. */
static void ap_icon(SDL_Renderer *r, struct ap_entry *e)
{
	if (e->tex || e->tried || !e->icon[0]) return;
	e->tried = 1;
	e->tex = png_texture(r, e->icon, &e->tw, &e->th, NULL);
}

/* ---- the game library ----------------------------------------------------
 *
 * tools/scan-games.sh reads a folder of .tar backups and writes an index plus
 * one decoded icon per title into ~/.cache/tadpole/games. This is the model
 * over that index: names, icons, and — the part that turns a list into a
 * library — whether each title is already installed.
 *
 * WHY AN INDEX AND NOT A DIRECT READ. Each backup is 20-120 MB and the icon
 * lives inside it, so building this view costs about eleven seconds for the
 * eighty-seven titles here. Doing that inside the viewer would freeze the
 * window; doing it every time the picker opens would be eleven seconds each
 * time. The scanner runs as a tool, its output goes to the progress panel, and
 * the result is cached against each archive's size and mtime.
 */
struct gentry {
	char  name[128];
	char  pid[64];
	char  ver[32];
	char  icon[32];          /* cache key, "" when the archive has no artwork */
	char  path[PATHMAX];
	long long bytes;
	int   installed;
	int   checked;
	SDL_Texture *tex;        /* loaded on first draw, freed with the UI */
	int   tw, th;
	int   tried;             /* do not re-open an icon that failed */
};
static struct gentry *g_gm;
static int  g_gm_n, g_gm_sel, g_gm_top, g_gm_rows = 8;
static char g_gm_dir[PATHMAX];     /* the folder these came from */
static int  g_gm_scanned;          /* an index has been read at least once */
static enum modal_kind g_gm_return;   /* where Close goes: wizard, or nowhere */

static void games_cache_dir(char *out, size_t n)
{
	/* XDG override, then Windows' app-data directory, then ~/.cache. The
	 * chain is environment-driven rather than #ifdef-driven: LOCALAPPDATA
	 * and USERPROFILE exist on every Windows and on no Linux, so each
	 * platform simply falls through to its own answer. (USERPROFILE also
	 * replaces the old getpwuid() fallback — pwd.h has no Windows form,
	 * and a Linux session with neither HOME nor USERPROFILE set lands on
	 * /tmp exactly as it did before.) */
	const char *x = getenv("XDG_CACHE_HOME");
	const char *la = getenv("LOCALAPPDATA");
	const char *home = getenv("HOME");
	if (!home) home = getenv("USERPROFILE");
	if (!home) home = "/tmp";
	if (x && *x)        snprintf(out, n, "%s/tadpole/games", x);
	else if (la && *la) snprintf(out, n, "%s/Tadpole/cache/games", la);
	else                snprintf(out, n, "%s/.cache/tadpole/games", home);
}

/* Installed means "a package directory with this PackageID exists in the
 * sysroot" — the same test the home screen effectively makes. */
static int game_installed(const char *pid)
{
	char p[PATHMAX * 2];
	struct stat st;
	if (!pid || !*pid) return 0;
	snprintf(p, sizeof(p), "%s/runtime/sysroot/LF/Bulk/ProgramFiles/%s",
	         g_proj, pid);
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void games_free(void)
{
	int i;
	for (i = 0; i < g_gm_n; i++)
		if (g_gm[i].tex) SDL_DestroyTexture(g_gm[i].tex);
	free(g_gm);
	g_gm = NULL; g_gm_n = 0; g_gm_sel = 0; g_gm_top = 0;
}

/* One line of index.tsv: name, package, version, bytes, icon key, path. */
static int game_parse(char *line, struct gentry *e)
{
	char *f[6], *p = line;
	int i;
	for (i = 0; i < 6; i++) {
		f[i] = p;
		if (i < 5) {
			p = strchr(p, '\t');
			if (!p) return 0;
			*p++ = 0;
		}
	}
	{
		size_t n = strlen(f[5]);
		while (n && (f[5][n-1] == '\n' || f[5][n-1] == '\r')) f[5][--n] = 0;
	}
	memset(e, 0, sizeof *e);
	snprintf(e->name, sizeof(e->name), "%s", f[0]);
	snprintf(e->pid,  sizeof(e->pid),  "%s", f[1]);
	snprintf(e->ver,  sizeof(e->ver),  "%s", f[2]);
	e->bytes = atoll(f[3]);
	snprintf(e->icon, sizeof(e->icon), "%s", f[4]);
	snprintf(e->path, sizeof(e->path), "%s", f[5]);
	e->installed = game_installed(e->pid);
	return e->name[0] && e->path[0];
}

void ui_games_reload(void)
{
	char p[PATHMAX], line[PATHMAX + 256];
	FILE *f;
	int cap = 32;

	games_free();
	games_cache_dir(p, sizeof(p));
	strncat(p, "/index.tsv", sizeof(p) - strlen(p) - 1);
	if (!(f = fopen(p, "r"))) return;
	g_gm = malloc(sizeof(*g_gm) * (size_t)cap);
	if (!g_gm) { fclose(f); return; }
	while (fgets(line, sizeof(line), f)) {
		struct gentry e;
		if (line[0] == '#') continue;
		if (!game_parse(line, &e)) continue;
		if (g_gm_n == cap) {
			struct gentry *t = realloc(g_gm, sizeof(*t) * (size_t)(cap * 2));
			if (!t) break;
			g_gm = t; cap *= 2;
		}
		g_gm[g_gm_n++] = e;
	}
	fclose(f);
	g_gm_scanned = 1;

	/* WHERE THESE CAME FROM, even on the first run of a new build. The folder
	 * is normally remembered in ui.cfg, but an index can outlive that (a
	 * settings file from before games_dir existed, or a cache shared between
	 * checkouts) and a library headed "(no folder chosen)" while listing
	 * eighty-seven games is a plain contradiction. Every entry carries its
	 * archive's full path, so the answer is already here. */
	if (!g_gm_dir[0] && g_gm_n) {
		char *slash;
		snprintf(g_gm_dir, sizeof(g_gm_dir), "%s", g_gm[0].path);
		slash = strrchr(g_gm_dir, '/');
		if (slash && slash != g_gm_dir) *slash = 0;
		else g_gm_dir[0] = 0;
	}
}

/* ---- .tpi icons ----------------------------------------------------------
 * 'TPI1', u16 w, u16 h, then w*h RGBA bytes — written by scan-games.py, which
 * does all the PNG work. Deliberately the dullest format that could work: the
 * viewer should not be in the business of decoding whatever a 2012 content
 * pipeline produced.
 */
static SDL_Texture *icon_load(SDL_Renderer *r, const char *key, int *w, int *h)
{
	char p[PATHMAX];
	unsigned char hdr[8];
	FILE *f;
	Uint32 *px;
	SDL_Texture *t = NULL;
	int iw, ih, i, n;

	games_cache_dir(p, sizeof(p));
	{
		size_t l = strlen(p);
		snprintf(p + l, sizeof(p) - l, "/i/%s.tpi", key);
	}
	if (!(f = fopen(p, "rb"))) return NULL;
	if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "TPI1", 4)) { fclose(f); return NULL; }
	iw = hdr[4] | (hdr[5] << 8);
	ih = hdr[6] | (hdr[7] << 8);
	n = iw * ih;
	if (iw <= 0 || ih <= 0 || n > 512 * 512) { fclose(f); return NULL; }
	px = malloc((size_t)n * 4);
	if (!px) { fclose(f); return NULL; }
	if (fread(px, 4, (size_t)n, f) != (size_t)n) { free(px); fclose(f); return NULL; }
	fclose(f);
	/* File order is R,G,B,A; SDL wants ARGB8888 in native order. */
	for (i = 0; i < n; i++) {
		unsigned char *b = (unsigned char *)&px[i];
		px[i] = ((Uint32)b[3] << 24) | ((Uint32)b[0] << 16) |
		        ((Uint32)b[1] << 8)  | b[2];
	}
	t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
	                      SDL_TEXTUREACCESS_STATIC, iw, ih);
	if (t) {
		SDL_UpdateTexture(t, NULL, px, iw * 4);
		SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
		*w = iw; *h = ih;
	}
	free(px);
	return t;
}

static void game_icon(SDL_Renderer *r, struct gentry *e)
{
	if (e->tex || e->tried || !e->icon[0]) return;
	e->tried = 1;                       /* one attempt, however it goes */
	e->tex = icon_load(r, e->icon, &e->tw, &e->th);
}

static int games_checked(void)
{
	int i, n = 0;
	for (i = 0; i < g_gm_n; i++) n += g_gm[i].checked ? 1 : 0;
	return n;
}

/* The checked titles, one path per line, for install-game.sh --from-list.
 * An argv would do for three games and not for thirty; a file has no limit and
 * leaves something to look at when an install goes wrong. */
static int games_write_list(char *out, size_t n)
{
	FILE *f;
	int i, wrote = 0;
	games_cache_dir(out, n);
	strncat(out, "/install-list.txt", n - strlen(out) - 1);
	if (!(f = fopen(out, "w"))) return 0;
	for (i = 0; i < g_gm_n; i++)
		if (g_gm[i].checked) { fprintf(f, "%s\n", g_gm[i].path); wrote++; }
	fclose(f);
	return wrote;
}

/* ---- settings persistence ------------------------------------------------ */

/* XDG_CONFIG_HOME, THEN $HOME/.config — the same order tadpole.sh uses.
 *
 * The script has always read
 *     ${XDG_CONFIG_HOME:-$HOME/.config}/tadpole/ui.cfg
 * and this function used to hardcode $HOME/.config. On a machine that sets
 * XDG_CONFIG_HOME they are different files: the viewer would save a setting to
 * one and tadpole.sh would go on reading the other, so the Graphics
 * checkboxes appeared to do nothing at all. */
static void cfg_path(char *out, size_t n)
{
	/* Same chain as games_cache_dir, and for the same reason. */
	const char *x = getenv("XDG_CONFIG_HOME");
	const char *la = getenv("LOCALAPPDATA");
	const char *home = getenv("HOME");
	if (!home) home = getenv("USERPROFILE");
	if (!home) home = "/tmp";
	if (x && *x)        snprintf(out, n, "%s/tadpole", x);
	else if (la && *la) snprintf(out, n, "%s/Tadpole/config", la);
	else                snprintf(out, n, "%s/.config/tadpole", home);
	mkdir_p(out);
	{
		size_t l = strlen(out);
		snprintf(out + l, n - l, "/ui.cfg");
	}
}

/* tadpole.sh reads this file too (`awk '$1=="gl"{print $2}'`), so the format
 * stays "one key, a space, one value" — no sections, no quoting. */
void ui_cfg_save(void)
{
	char p[PATHMAX];
	FILE *f;
	cfg_path(p, sizeof(p));
	if (!(f = fopen(p, "w"))) return;
	fprintf(f, "update_check %d\n", g_cfg.update_check);
	fprintf(f, "gl %d\ngl_hle %d\ndebug_level %d\nlog_to_file %d\n"
	           "gl_dumpframe %d\ngl_dumptex %d\n"
	           "rotate %d\nscale %d\ntouch_debug %d\n"
	           "audio_on %d\naudio_latency_ms %d\naudio_pace %d\n"
	           "frame_cap %d\nhle_strict %d\nmsaa %d\nrender_scale %d\n"
	           "io_delay_us %d\ntslib %d\n"
	           "boot_on_start %d\n",
	        g_cfg.gl, g_cfg.gl_hle, g_cfg.debug_level, g_cfg.log_to_file,
	        g_cfg.gl_dumpframe, g_cfg.gl_dumptex,
	        g_cfg.rotate, g_cfg.scale, g_cfg.touch_debug,
	        g_cfg.audio_on, g_cfg.audio_latency_ms, g_cfg.audio_pace,
	        g_cfg.frame_cap, g_cfg.hle_strict, g_cfg.msaa, g_cfg.render_scale,
	        g_cfg.io_delay_us,
	        g_cfg.tslib,
	        g_cfg.boot_on_start);
	/* Last, and only if set: it is the one value that can contain spaces. */
	if (g_cfg.games_dir[0])
		fprintf(f, "games_dir %s\n", g_cfg.games_dir);
	fclose(f);
}

static void cfg_load(void)
{
	char p[PATHMAX], line[PATHMAX + 64];
	FILE *f;
	cfg_path(p, sizeof(p));
	if (!(f = fopen(p, "r"))) return;
	/* LINE AT A TIME, not fscanf("%s %d").
	 *
	 * The old scanner read a word and an integer, which worked right up until
	 * a setting had a PATH for a value: games_dir made fscanf fail, and a
	 * failed fscanf ends the loop — so one folder name with a space in it
	 * silently discarded every setting that came after it. */
	while (fgets(line, sizeof(line), f)) {
		char *k = line, *v;
		size_t n;
		while (*k == ' ' || *k == '\t') k++;
		v = strchr(k, ' ');
		if (!v) continue;
		*v++ = 0;
		n = strlen(v);
		while (n && (v[n-1] == '\n' || v[n-1] == '\r' || v[n-1] == ' '))
			v[--n] = 0;
		if (!strcmp(k, "games_dir")) {
			snprintf(g_cfg.games_dir, sizeof(g_cfg.games_dir), "%s", v);
			continue;
		}
		{
			int val = atoi(v);
			if      (!strcmp(k, "gl"))               g_cfg.gl = val;
			else if (!strcmp(k, "gl_hle"))           g_cfg.gl_hle = val;
			else if (!strcmp(k, "debug_level"))      g_cfg.debug_level = val;
			else if (!strcmp(k, "log_to_file"))      g_cfg.log_to_file = val;
			else if (!strcmp(k, "update_check"))     g_cfg.update_check = val;
			else if (!strcmp(k, "gl_dumpframe"))     g_cfg.gl_dumpframe = val;
			else if (!strcmp(k, "gl_dumptex"))       g_cfg.gl_dumptex = val;
			else if (!strcmp(k, "rotate"))           g_cfg.rotate = val;
			else if (!strcmp(k, "scale"))            g_cfg.scale = val;
			else if (!strcmp(k, "touch_debug"))      g_cfg.touch_debug = val;
			else if (!strcmp(k, "audio_on"))         g_cfg.audio_on = val;
			else if (!strcmp(k, "audio_latency_ms")) g_cfg.audio_latency_ms = val;
			else if (!strcmp(k, "audio_pace"))       g_cfg.audio_pace = val;
			else if (!strcmp(k, "frame_cap"))        g_cfg.frame_cap = val;
			else if (!strcmp(k, "hle_strict"))       g_cfg.hle_strict = val;
			else if (!strcmp(k, "msaa"))             g_cfg.msaa = val;
			else if (!strcmp(k, "render_scale"))     g_cfg.render_scale = val;
			else if (!strcmp(k, "io_delay_us"))      g_cfg.io_delay_us = val;
			else if (!strcmp(k, "tslib"))            g_cfg.tslib = val;
			else if (!strcmp(k, "boot_on_start"))    g_cfg.boot_on_start = val;
			/* Older files carried these two; the debug level replaced them.
			 * Honour them once so an existing install does not silently lose
			 * the logging it was set up with. */
			else if (!strcmp(k, "gl_debug") && val)   g_cfg.debug_level = 2;
			else if (!strcmp(k, "shim_debug") && val) g_cfg.debug_level = 2;
		}
	}
	fclose(f);
}

/* ---- menu model ---------------------------------------------------------- */

struct mitem {
	const char *label;
	int         id;          /* 0 = separator */
	int         needs_run;   /* only enabled while a guest is up */
	int         needs_idle;  /* only enabled while nothing is running */
	int         needs_sys;   /* only enabled once the system files exist */
};

enum {
	IT_RUN_UI = 1, IT_SWF, IT_PKG, IT_CART, IT_FW, IT_STOP, IT_QUIT,
	IT_AUDIO, IT_GFX, IT_PAD, IT_ABOUT, IT_WIZARD, IT_ERASE, IT_UPDATE,
	IT_GAMES, IT_DEBUG, IT_SYSTEM
};

static const struct mitem FILE_ITEMS[] = {
	/* Booting needs the system files. Offering it without them produces a wall
	 * of missing-path errors in a terminal the user may not even be looking at,
	 * which is precisely what the wizard exists to prevent. */
	{ "Run System Menu",        IT_RUN_UI, 0, 1, 1 },
	{ "Launch App...",          IT_SWF,    0, 1, 1 },
	{ "",                       0,         0, 0, 0 },
	/* FIRST, and named after what it is for. Picking a .tar out of a file list
	 * is still there underneath, but it is not how anyone should have to meet
	 * their own game collection. */
	{ "Game Library...",        IT_GAMES,  0, 0, 0 },
	{ "Install .tar directly...", IT_PKG,  0, 0, 0 },
	/* A cartridge people dumped THEMSELVES, with dd on the device, is a raw
	 * FAT image and not a package — so it sat in a folder next to the .tar
	 * files being uninstallable, which is a poor reward for making a proper
	 * backup. This turns one into the other. */
	{ "Convert Cartridge Dump...", IT_CART, 0, 0, 0 },
	{ "",                       0,         0, 0, 0 },
	{ "Setup System Firmware...", IT_FW,   0, 0, 0 },
	{ "Erase System Firmware",  IT_ERASE,  0, 1, 1 },
	{ "",                       0,         0, 0, 0 },
	{ "Stop Emulation",         IT_STOP,   1, 0, 0 },
	{ "Quit",                   IT_QUIT,   0, 0, 0 },
};
static const struct mitem OPT_ITEMS[] = {
	{ "Graphics Settings...",   IT_GFX,    0, 0, 0 },
	{ "Audio Settings...",      IT_AUDIO,  0, 0, 0 },
	{ "Controller Settings...", IT_PAD,    0, 0, 0 },
	{ "Debug Settings...",      IT_DEBUG,  0, 0, 0 },
	{ "System Settings...",     IT_SYSTEM, 0, 0, 0 },
};
static const struct mitem HELP_ITEMS[] = {
	{ "Setup Wizard...",        IT_WIZARD, 0, 0, 0 },
	{ "Check for Updates...",   IT_UPDATE, 0, 0, 0 },
	{ "About Tadpole",          IT_ABOUT,  0, 0, 0 },
};

struct menu {
	const char *title;
	const struct mitem *items;
	int n;
	int x, w;                /* filled at draw time */
};
static struct menu MENUS[] = {
	{ "File",    FILE_ITEMS, (int)(sizeof FILE_ITEMS / sizeof *FILE_ITEMS), 0, 0 },
	{ "Options", OPT_ITEMS,  (int)(sizeof OPT_ITEMS  / sizeof *OPT_ITEMS),  0, 0 },
	{ "Help",    HELP_ITEMS, (int)(sizeof HELP_ITEMS / sizeof *HELP_ITEMS), 0, 0 },
};
#define NMENUS ((int)(sizeof MENUS / sizeof *MENUS))

static void menu_layout(void)
{
	int i, x = 3;
	for (i = 0; i < NMENUS; i++) {
		MENUS[i].w = text_w(MENUS[i].title) + 10;
		MENUS[i].x = x;
		x += MENUS[i].w;
	}
}

static int menu_width(const struct menu *m)
{
	int i, w = 60;
	for (i = 0; i < m->n; i++) {
		int t = text_w(m->items[i].label) + 18;
		if (t > w) w = t;
	}
	return w;
}

static int item_enabled(const struct mitem *it)
{
	if (!it->id) return 0;
	if (it->needs_run && !g_running) return 0;
	if (it->needs_idle && g_running) return 0;
	if (it->needs_sys) {
		/* Cached: this stats the filesystem, and it is asked once per item per
		 * frame while a menu is open. Invalidated whenever a tool finishes,
		 * which is the only way the answer changes. */
		if (g_sys_ready < 0) {
			struct prereq pq;
			prereq_check(&pq);
			g_sys_ready = (pq.rootfs && pq.sysroot) ? 1 : 0;
		}
		if (!g_sys_ready) return 0;
	}
	return 1;
}

/* ---- rotate button (top right) ------------------------------------------- */

#define ROT_W 48
static int rot_x(int lw) { return lw - ROT_W - 2; }

/* ---- file browser -------------------------------------------------------- */

static int fcmp(const void *a, const void *b)
{
	const struct fentry *x = a, *y = b;
	if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* dirs first */
	return strcasecmp(x->name, y->name);
}

static int has_ext(const char *name, const char *ext)
{
	size_t n, e;
	if (!ext || !*ext) return 1;
	n = strlen(name); e = strlen(ext);
	return n > e && !strcasecmp(name + n - e, ext);
}

static void fb_scan(void)
{
	DIR *d;
	struct dirent *de;
	int cap = 64;

	free(g_fb_list);
	g_fb_list = malloc(sizeof(*g_fb_list) * (size_t)cap);
	g_fb_n = 0; g_fb_sel = 0; g_fb_top = 0;
	if (!g_fb_list) return;

	if (!(d = opendir(g_fb_dir))) {
		ui_status("cannot open %s", g_fb_dir);
		return;
	}
	while ((de = readdir(d))) {
		char full[PATHMAX * 2];
		struct stat st;
		int isdir;
		if (!strcmp(de->d_name, ".")) continue;
		if (de->d_name[0] == '.' && strcmp(de->d_name, "..")) continue;
		path_join(full, sizeof(full), g_fb_dir, de->d_name);
		if (stat(full, &st) != 0) continue;
		isdir = S_ISDIR(st.st_mode);
		if (!isdir && !has_ext(de->d_name, g_fb_filter)) continue;
		if (g_fb_n == cap) {
			struct fentry *t = realloc(g_fb_list, sizeof(*t) * (size_t)(cap * 2));
			if (!t) break;
			g_fb_list = t; cap *= 2;
		}
		snprintf(g_fb_list[g_fb_n].name, sizeof(g_fb_list[0].name), "%s", de->d_name);
		g_fb_list[g_fb_n].isdir = isdir;
		g_fb_n++;
	}
	closedir(d);
	qsort(g_fb_list, (size_t)g_fb_n, sizeof(*g_fb_list), fcmp);
}

static void fb_open(const char *title, const char *start, const char *ext,
                    enum ui_action act)
{
	snprintf(g_fb_title, sizeof(g_fb_title), "%s", title);
	path_join(g_fb_dir, sizeof(g_fb_dir), start, "");
	snprintf(g_fb_filter, sizeof(g_fb_filter), "%s", ext ? ext : "");
	g_fb_action = act;
	/* Come back to whatever sent us here. Without this, choosing a file left
	 * NO modal at all and the wizard — or the library — appeared to vanish
	 * mid-task. */
	g_fb_return = (g_modal == M_WIZARD || g_modal == M_GAMES) ? g_modal : M_NONE;
	g_modal = M_FILES;
	fb_scan();
}

static void fb_enter(void)
{
	char next[PATHMAX * 2];
	if (g_fb_sel < 0 || g_fb_sel >= g_fb_n) return;
	if (g_fb_list[g_fb_sel].isdir) {
		if (!strcmp(g_fb_list[g_fb_sel].name, "..")) {
			char *slash = strrchr(g_fb_dir, '/');
			if (slash && slash != g_fb_dir) *slash = 0;
			else strcpy(g_fb_dir, "/");
		} else {
			path_join(next, sizeof(next), g_fb_dir, g_fb_list[g_fb_sel].name);
			path_join(g_fb_dir, sizeof(g_fb_dir), next, "");
		}
		fb_scan();
		return;
	}
	path_join(g_action_path, sizeof(g_action_path), g_fb_dir,
	          g_fb_list[g_fb_sel].name);
	if (g_fb_pick_profile) {
		snprintf(g_prof_pic, sizeof(g_prof_pic), "%s", g_action_path);
		g_fb_pick_profile = 0;
		g_modal = M_WIZARD;
		return;
	}
	g_action = g_fb_action;
	g_modal = M_NONE;
}

/* ---- opening the library ------------------------------------------------- */

static void games_choose_folder(void)
{
	char start[PATHMAX];
	if (g_gm_dir[0])            snprintf(start, sizeof(start), "%s", g_gm_dir);
	else if (g_cfg.games_dir[0]) snprintf(start, sizeof(start), "%s", g_cfg.games_dir);
	else                        path_join(start, sizeof(start), g_proj, "games");
	if (access(start, R_OK) != 0)
		path_join(start, sizeof(start), g_proj, "");
	/* No extension filter, so the browser offers "Use folder" — picking the
	 * folder is the whole point here, not picking a file inside it. */
	fb_open("Games folder", start, "", UI_ACT_SCAN_GAMES);
}

static void games_open(void)
{
	g_gm_return = (g_modal == M_WIZARD) ? M_WIZARD : M_NONE;
	if (!g_gm_scanned) {
		ui_games_reload();
		if (!g_gm_dir[0] && g_cfg.games_dir[0])
			snprintf(g_gm_dir, sizeof(g_gm_dir), "%s", g_cfg.games_dir);
	}
	g_modal = M_GAMES;
	/* Never open on an empty box with no hint of what to do: with nothing
	 * scanned yet, go straight to choosing the folder. */
	if (g_gm_n == 0 && !g_gm_dir[0])
		games_choose_folder();
}

/* ---- public -------------------------------------------------------------- */

/* Settings decide the window size and orientation, so they have to be readable
 * before there is a renderer to build the font with. */
void ui_preload_settings(void) { cfg_load(); }

void ui_init(SDL_Renderer *ren, const char *project_dir)
{
	char p[PATHMAX + 32];
	snprintf(g_proj, sizeof(g_proj), "%s", project_dir);
	font_build(ren);
	menu_layout();
	/* One logo per brand, beside each other at the project root. */
	path_join(p, sizeof(p), g_proj,
	          ui_brand_is_glasspole() ? "glasspole.png" : "tadpole.png");
	logo_load(ren, p);
	if (!g_logo && ui_brand_is_glasspole()) {
		/* Rather than draw nothing if the Glasspole art is missing. */
		path_join(p, sizeof(p), g_proj, "tadpole.png");
		logo_load(ren, p);
	}
	snprintf(g_status, sizeof(g_status), "idle");
	/* SDL delivers SDL_TEXTINPUT only while text input is started. Enabled
	 * once, here, rather than toggled per field: the name box is the only
	 * typing surface in the program, every other key path already returns
	 * early, and a mode that can be entered can be got stuck in. */
	SDL_StartTextInput();

	/* OPEN THE WIZARD WHEN THERE IS NOTHING TO BOOT. Without firmware the
	 * emulator can only fail, and failing with a stack of missing-file errors
	 * explains nothing to someone running it for the first time. */
	{
		struct prereq pq;
		prereq_check(&pq);
		if (!pq.rootfs || !pq.sysroot) {
			g_wiz_page = 0;
			g_modal = M_WIZARD;
			ui_status("setup needed");
		}
	}
}

void ui_shutdown(void)
{
	if (g_font) SDL_DestroyTexture(g_font);
	if (g_logo) SDL_DestroyTexture(g_logo);
	games_free();                 /* one texture per icon we ever drew */
	free(g_fb_list);
	free(g_logo_px);
	g_font = NULL; g_logo = NULL; g_fb_list = NULL; g_logo_px = NULL;
}

/* A surface over the decoded icon. The caller frees the surface; the pixels
 * stay ours, so it must be used (SDL copies it) before ui_shutdown. */
SDL_Surface *ui_icon_surface(void)
{
	if (!g_logo_px) return NULL;
	return SDL_CreateRGBSurfaceFrom(g_logo_px, g_logo_w, g_logo_h, 32,
	                                g_logo_w * 4,
	                                0x00FF0000, 0x0000FF00, 0x000000FF,
	                                0xFF000000);
}

struct ui_settings *ui_cfg(void) { return &g_cfg; }
int ui_modal(void) { return g_modal != M_NONE; }
void ui_set_running(int r) { g_running = r; }

/* Something may have installed or erased the system files. */
void ui_invalidate_prereqs(void) { g_sys_ready = -1; }

void ui_profile_get(char *name, size_t namesz, int *grade,
                    char *picture, size_t picsz)
{
	if (name && namesz) snprintf(name, namesz, "%s", g_prof_name);
	if (grade) *grade = g_prof_grade;
	if (picture && picsz) snprintf(picture, picsz, "%s", g_prof_pic);
}

void ui_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
}

enum ui_action ui_take_action(char *path, size_t n)
{
	enum ui_action a = g_action;
	g_action = UI_ACT_NONE;
	if (path && n) snprintf(path, n, "%s", g_action_path);
	return a;
}

static void msg(const char *title, const char *body)
{
	snprintf(g_msg_title, sizeof(g_msg_title), "%s", title);
	snprintf(g_msg_body, sizeof(g_msg_body), "%s", body);
	g_modal = M_MSG;
}


/* ---- update check ------------------------------------------------------- */

void ui_update_begin(int silent)
{
	g_up_status[0] = g_up_cur[0] = g_up_latest[0] = 0;
	g_up_title[0] = g_up_asset[0] = g_up_reason[0] = 0;
	g_up_nnote = g_up_scroll = g_up_count = 0;
	g_up_silent = silent;
}

/* One line of tools/check-update.py's output. Format is deliberately dull:
 * a keyword, a space, the rest of the line. */
void ui_update_line(const char *line)
{
	const char *v;
	if (!line) return;
	while (*line == ' ' || *line == '\t') line++;

	if (!strncmp(line, "status ", 7))
		snprintf(g_up_status, sizeof(g_up_status), "%s", line + 7);
	else if (!strncmp(line, "current ", 8))
		snprintf(g_up_cur, sizeof(g_up_cur), "%s", line + 8);
	else if (!strncmp(line, "latest ", 7))
		snprintf(g_up_latest, sizeof(g_up_latest), "%s", line + 7);
	else if (!strncmp(line, "title ", 6))
		snprintf(g_up_title, sizeof(g_up_title), "%s", line + 6);
	else if (!strncmp(line, "asset ", 6))
		snprintf(g_up_asset, sizeof(g_up_asset), "%s", line + 6);
	else if (!strncmp(line, "reason ", 7))
		snprintf(g_up_reason, sizeof(g_up_reason), "%s", line + 7);
	else if (!strncmp(line, "count ", 6))
		g_up_count = atoi(line + 6);
	else if (!strncmp(line, "ver ", 4) || !strncmp(line, "note ", 5)) {
		int head = line[0] == 'v';
		v = line + (head ? 4 : 5);
		if (g_up_nnote < UP_MAXNOTE) {
			/* A blank line between releases, so the list reads as sections
			 * rather than one wall of text. */
			if (head && g_up_nnote) {
				g_up_note[g_up_nnote][0] = 0;
				g_up_head[g_up_nnote] = 0;
				g_up_nnote++;
			}
			if (g_up_nnote < UP_MAXNOTE) {
				snprintf(g_up_note[g_up_nnote], UP_MAXLINE, "%s", v);
				g_up_head[g_up_nnote] = (unsigned char)head;
				g_up_nnote++;
			}
		}
	}
}

const char *ui_update_asset(void) { return g_up_asset; }
int ui_update_pending(void) { return !strcmp(g_up_status, "behind"); }

void ui_update_finish(void)
{
	/* "dev" opens the same dialog: an unreleased build still wants to see what
	 * has shipped since, and the heading already reads "You have an unreleased
	 * build" rather than claiming you are behind. */
	if (!strcmp(g_up_status, "behind") || !strcmp(g_up_status, "dev")) {
		g_modal = M_UPDATE;
		return;
	}
	/* Nothing newer. A check the user asked for still owes them an answer;
	 * the one that runs by itself at startup owes them silence. */
	if (g_up_silent)
		return;
	/* SAY IT, rather than leaving the version to speak for itself. This used
	 * to put g_up_cur in the body, so a successful check answered with a bare
	 * "08082026-0001" under a title — which reads as a fact about the build,
	 * not as the answer to the question that was asked. The body is one plain
	 * sentence instead; the version is already on the About dialog for anyone
	 * who wants it. (Kept to one short line on purpose: text() does not wrap,
	 * and this dialog is 250 logical px wide.) */
	if (!strcmp(g_up_status, "current"))
		msg("No updates", "You have the newest release.");
	else
		msg("Could not check", g_up_reason[0] ? g_up_reason :
		    "No answer from GitHub.");
}

static void activate(int id)
{
	char start[PATHMAX];
	g_open_menu = -1;
	switch (id) {
	case IT_RUN_UI: g_action = UI_ACT_RUN_UI; break;
	case IT_STOP:   g_action = UI_ACT_STOP;   break;
	case IT_QUIT:   g_action = UI_ACT_QUIT;   break;
	case IT_SWF:
		ap_reload();
		g_modal = M_APPS;
		break;
	case IT_PKG:
		if (g_cfg.games_dir[0])
			snprintf(start, sizeof(start), "%s", g_cfg.games_dir);
		else
			path_join(start, sizeof(start), g_proj, "games");
		if (access(start, R_OK) != 0)
			path_join(start, sizeof(start), g_proj, "");
		fb_open("Install Package", start, ".tar", UI_ACT_INSTALL_PKG);
		break;
	case IT_CART:
		/* Same starting folder as Install, because a dump pulled off a device
		 * over FTP lands wherever the user keeps their games — and the
		 * converted .tar is written into the games folder, so the next thing
		 * they do is find it already in the Game Library. */
		if (g_cfg.games_dir[0])
			snprintf(start, sizeof(start), "%s", g_cfg.games_dir);
		else
			path_join(start, sizeof(start), g_proj, "games");
		if (access(start, R_OK) != 0)
			path_join(start, sizeof(start), g_proj, "");
		fb_open("Cartridge dump (.bin)", start, ".bin", UI_ACT_CONVERT_CART);
		break;
	case IT_FW:
		path_join(start, sizeof(start), g_proj, "");
		fb_open("Setup System Firmware", start, ".zip", UI_ACT_SETUP_FIRMWARE);
		break;
	case IT_AUDIO: g_modal = M_AUDIO; break;
	case IT_GFX:   g_modal = M_GFX;   break;
	case IT_PAD:   g_modal = M_PAD;   break;
	case IT_DEBUG: g_modal = M_DEBUG; break;
	case IT_SYSTEM: g_modal = M_SYSTEM; break;
	case IT_ABOUT: g_modal = M_ABOUT; break;
	case IT_UPDATE: g_action = UI_ACT_CHECK_UPDATE; break;
	case IT_GAMES: games_open(); break;
	case IT_WIZARD: g_wiz_page = 0; g_modal = M_WIZARD; break;
	case IT_ERASE:
		/* Confirm first: this is the one menu item that throws work away. */
		g_confirm = IT_ERASE;
		g_fb_return = M_NONE;      /* not opened from the wizard */
		msg("Erase System Firmware",
		    "Remove the installed system files?");
		break;
	default: break;
	}
}

/* ---- dialog geometry ----------------------------------------------------
 * Dialogs are centred boxes; each has a fixed logical size so the hit tests
 * and the drawing agree without a layout pass.
 */
struct dlg { int x, y, w, h; };

static struct dlg dlg_rect(int lw, int lh, int w, int h)
{
	struct dlg d;
	d.w = w; d.h = h;
	d.x = (lw - w) / 2;
	d.y = UI_BAR_H + (lh - UI_BAR_H - h) / 2;
	if (d.y < UI_BAR_H + 2) d.y = UI_BAR_H + 2;
	return d;
}

/* A dialog that asks for a size but never wider or taller than the window.
 *
 * Rotating to portrait makes the logical space 272 wide, and the fixed sizes
 * below are up to 350 — so the file browser and the progress panel used to
 * hang off both edges of a rotated window, with their buttons off-screen. */
static struct dlg dlg_fit(int lw, int lh, int w, int h)
{
	if (w > lw - 8) w = lw - 8;
	if (h > lh - UI_BAR_H - 6) h = lh - UI_BAR_H - 6;
	if (w < 120) w = 120;
	if (h < 90)  h = 90;
	return dlg_rect(lw, lh, w, h);
}

static struct dlg cur_dlg(int lw, int lh)
{
	switch (g_modal) {
	case M_ABOUT: return dlg_fit(lw, lh, 210, 144);
	/* Room for icons and two lines per row. */
	case M_APPS:  return dlg_fit(lw, lh, 340, 232);
	/* Wide and tall: this is a changelog, and a release body wrapped
	 * into 30 columns would be unreadable. */
	case M_UPDATE: return dlg_fit(lw, lh, 400, 230);
	case M_GFX:   return dlg_fit(lw, lh, 250, 200);
	case M_AUDIO: return dlg_fit(lw, lh, 230, 122);
	case M_PAD:   return dlg_fit(lw, lh, 240, 140);
	case M_DEBUG: return dlg_fit(lw, lh, 268, 200);
	case M_SYSTEM: return dlg_fit(lw, lh, 268, 150);
	case M_FILES: return dlg_fit(lw, lh, 300, 172);
	case M_WIZARD: return dlg_fit(lw, lh, 348, 210);
	case M_PROGRESS: return dlg_fit(lw, lh, 350, 150);
	case M_MSG:   return dlg_fit(lw, lh, 250, 92);
	/* The library wants every pixel it can have: it is a list of eighty-odd
	 * names next to a picture. */
	case M_GAMES: return dlg_fit(lw, lh, 460, 260);
	default:      { struct dlg z = {0,0,0,0}; return z; }
	}
}

/* Does the library have room for the preview panel on the right? In portrait
 * it does not, and the list gets the whole width instead. */
#define GM_PANEL_W 104
#define GM_ROW_H   15
static int gm_panel(const struct dlg *d) { return d->w >= 300 ? GM_PANEL_W : 0; }
static int gm_list_w(const struct dlg *d) { return d->w - 12 - gm_panel(d); }
static int gm_list_h(const struct dlg *d) { return d->h - 26 - 22; }

/* Rows inside settings dialogs are uniform, so one helper does hit-testing
 * and drawing from the same numbers. */
#define ROW_H 14
static int row_y(const struct dlg *d, int i) { return d->y + 22 + i * ROW_H; }

/* A settings row: checkbox or a cycling value. Returns 1 if (mx,my) hits it. */
static int row_hit(const struct dlg *d, int i, int mx, int my)
{
	return inside(mx, my, d->x + 6, row_y(d, i) - 3, d->w - 12, ROW_H);
}

static void row_check(SDL_Renderer *r, const struct dlg *d, int i,
                      const char *label, int on, int hot)
{
	int y = row_y(d, i);
	if (hot) fill(r, d->x + 6, y - 3, d->w - 12, ROW_H, C_PANEL_HI);
	text(r, d->x + 10, y, on ? GL_CHECK_1 : GL_CHECK_0, on ? C_ACCENT : C_TEXT_DIM);
	text(r, d->x + 22, y, label, C_TEXT);
}

static void row_value(SDL_Renderer *r, const struct dlg *d, int i,
                      const char *label, const char *val, int hot)
{
	int y = row_y(d, i);
	if (hot) fill(r, d->x + 6, y - 3, d->w - 12, ROW_H, C_PANEL_HI);
	text(r, d->x + 10, y, label, C_TEXT);
	text(r, d->x + d->w - 12 - text_w(val), y, val, C_ACCENT);
}

/* Wizard buttons: Back / Next|Finish / Cancel, bottom right, in that order —
 * the arrangement every Windows installer has used for thirty years, because it
 * needs no explaining. */

/* Buttons for the update dialog: 0 = Download, 1 = Later. Derived from the
 * dialog rect rather than stored, so a clamped dialog on a small window still
 * has its buttons where they are drawn. */
static SDL_Rect up_btn(const struct dlg *d, int which)
{
	SDL_Rect b;
	b.w = 76; b.h = 14;
	b.y = d->y + d->h - b.h - 8;
	b.x = which == 0 ? d->x + d->w - 2 * b.w - 16 : d->x + d->w - b.w - 8;
	return b;
}

static SDL_Rect wiz_btn(const struct dlg *d, int which)   /* 0 back 1 next 2 cancel */
{
	SDL_Rect r;
	r.w = 44; r.h = 13;
	r.y = d->y + d->h - 18;
	r.x = d->x + d->w - 8 - (3 - which) * 47;
	return r;
}

/* Close button, bottom right of every dialog. */
static SDL_Rect close_rect(const struct dlg *d)
{
	SDL_Rect rc = { d->x + d->w - 48, d->y + d->h - 18, 42, 13 };
	return rc;
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_bar(SDL_Renderer *r, int lw)
{
	int i;
	char buf[32];

	fill(r, 0, 0, lw, UI_BAR_H, C_BAR);
	fill(r, 0, UI_BAR_H - 1, lw, 1, C_EDGE_DK);

	for (i = 0; i < NMENUS; i++) {
		int hot = (g_open_menu == i) ||
		          (g_open_menu < 0 && inside(g_mx, g_my, MENUS[i].x, 0, MENUS[i].w, UI_BAR_H - 1));
		if (hot) fill(r, MENUS[i].x, 0, MENUS[i].w, UI_BAR_H - 1, C_BAR_HI);
		text(r, MENUS[i].x + 5, 3, MENUS[i].title, hot ? C_ACCENT : C_TEXT);
	}

	/* Orientation button — one click per 90 degrees, so a portrait title
	 * does not mean turning your head. */
	{
		int x = rot_x(lw);
		int hot = inside(g_mx, g_my, x, 1, ROT_W, UI_BAR_H - 3);
		fill(r, x, 1, ROT_W, UI_BAR_H - 3, hot ? C_BAR_HI : C_PANEL);
		bevel(r, x, 1, ROT_W, UI_BAR_H - 3, 1);
		snprintf(buf, sizeof(buf), "ROT %d", g_cfg.rotate);
		text_c(r, x, ROT_W, 3, buf, hot ? C_ACCENT : C_TEXT);
	}

	/* status, right-aligned before the rotate button */
	{
		int sx = rot_x(lw) - 6 - text_w(g_status);
		if (sx > MENUS[NMENUS-1].x + MENUS[NMENUS-1].w + 6)
			text(r, sx, 3, g_status, g_running ? C_ACCENT : C_TEXT_DIM);
	}
}

static void draw_dropdown(SDL_Renderer *r)
{
	const struct menu *m;
	int w, h, x, y, i;

	if (g_open_menu < 0) return;
	m = &MENUS[g_open_menu];
	w = menu_width(m);
	h = m->n * 12 + 6;
	x = m->x; y = UI_BAR_H;
	panel(r, x, y, w, h);

	for (i = 0; i < m->n; i++) {
		int iy = y + 3 + i * 12;
		const struct mitem *it = &m->items[i];
		if (!it->id) {                       /* separator */
			fill(r, x + 5, iy + 5, w - 10, 1, C_EDGE_DK);
			continue;
		}
		if (g_hot_item == i && item_enabled(it))
			fill(r, x + 2, iy - 1, w - 4, 12, C_PANEL_HI);
		text(r, x + 8, iy + 1, it->label,
		     item_enabled(it) ? (g_hot_item == i ? C_ACCENT : C_TEXT) : C_TEXT_DIM);
	}
}

static const char *onoff(int v) { return v ? "ON" : "OFF"; }

static void draw_dialog(SDL_Renderer *r, int lw, int lh)
{
	struct dlg d = cur_dlg(lw, lh);
	SDL_Rect cb;
	const char *title = "";
	int i;

	if (g_modal == M_NONE) return;

	switch (g_modal) {
	case M_ABOUT: title = "About Tadpole"; break;
	case M_GFX:   title = "Graphics Settings"; break;
	case M_AUDIO: title = "Audio Settings"; break;
	case M_PAD:   title = "Controller Settings"; break;
	case M_DEBUG: title = "Debug Settings"; break;
	case M_SYSTEM: title = "System Settings"; break;
	case M_GAMES: title = "Game Library"; break;
	case M_FILES: title = g_fb_title; break;
	case M_PROGRESS: title = g_prog_title; break;
	case M_MSG:   title = g_msg_title; break;
	case M_UPDATE: title = "Update available"; break;
	case M_APPS:  title = "Launch App"; break;
	default: break;
	}

	/* dim the world behind the modal */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 8, 4, 160);
	{ SDL_Rect all = { 0, UI_BAR_H, lw, lh - UI_BAR_H }; SDL_RenderFillRect(r, &all); }
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

	panel(r, d.x, d.y, d.w, d.h);
	fill(r, d.x + 1, d.y + 1, d.w - 2, 11, C_BAR_HI);
	text(r, d.x + 6, d.y + 3, title, C_ACCENT);

	switch (g_modal) {
	case M_APPS: {
		int x = d.x + 10, y = d.y + 20, i, row = 30;
		int listh = d.h - 30 - 22, vis = listh / row;
		char line[128];

		if (vis < 1) vis = 1;
		g_ap_rows = vis;
		if (g_ap_top > g_ap_n - vis) g_ap_top = g_ap_n - vis;
		if (g_ap_top < 0) g_ap_top = 0;

		if (!g_ap_n) {
			text(r, x, y + 10, "No apps installed yet.", C_TEXT);
			text(r, x, y + 26, "File > Game Library installs your", C_TEXT_DIM);
			text(r, x, y + 36, "cartridge backups. Help > Setup", C_TEXT_DIM);
			text(r, x, y + 46, "Wizard fetches the system files.", C_TEXT_DIM);
			break;
		}
		for (i = 0; i < vis && g_ap_top + i < g_ap_n; i++) {
			struct ap_entry *e = &g_ap[g_ap_top + i];
			int yy = y + i * row;
			int hot = inside(g_mx, g_my, x - 2, yy - 2, d.w - 16, row - 2);

			if (hot) fill(r, x - 2, yy - 2, d.w - 16, row - 2, C_BAR_HI);
			ap_icon(r, e);
			if (e->tex) {
				SDL_Rect dst = { x, yy, 26, 26 };
				SDL_RenderCopy(r, e->tex, NULL, &dst);
			} else {
				fill(r, x, yy, 26, 26, C_PANEL);
				bevel(r, x, yy, 26, 26, 0);
			}
			snprintf(line, sizeof(line), "%.40s", e->name);
			text(r, x + 32, yy + 3, line, hot ? C_ACCENT : C_TEXT);
			snprintf(line, sizeof(line), "%s%s%.28s",
			         e->version[0] ? "v" : "", e->version,
			         e->version[0] ? "" : e->pkg);
			text(r, x + 32, yy + 15, line, C_TEXT_DIM);
		}
		if (g_ap_n > vis) {
			snprintf(line, sizeof(line), "%d-%d of %d", g_ap_top + 1,
			         g_ap_top + vis < g_ap_n ? g_ap_top + vis : g_ap_n, g_ap_n);
			text(r, d.x + d.w - 12 - text_w(line), d.y + d.h - 26, line, C_TEXT_DIM);
		}
		text(r, x, d.y + d.h - 26, "Install apps to see them here.", C_TEXT_DIM);
		break;
	}
	case M_UPDATE: {
		int x = d.x + 10, y = d.y + 20, i;
		int listy, listh, row = FONT_H + 2, vis;
		char line[96];

		snprintf(line, sizeof(line), "You have %s",
		         g_up_cur[0] ? g_up_cur : "an unreleased build");
		text(r, x, y, line, C_TEXT);
		snprintf(line, sizeof(line), "Newest is %s",
		         g_up_title[0] ? g_up_title : g_up_latest);
		text(r, x, y + 10, line, C_ACCENT);
		snprintf(line, sizeof(line), "%d newer release%s", g_up_count,
		         g_up_count == 1 ? "" : "s");
		text(r, d.x + d.w - 10 - text_w(line), y, line, C_TEXT_DIM);

		listy = y + 24;
		listh = d.y + d.h - 30 - listy;
		vis = listh / row;
		fill(r, x - 2, listy - 2, d.w - 16, listh + 2, C_EDGE_DK);
		bevel(r, x - 2, listy - 2, d.w - 16, listh + 2, 0);

		/* Clamp here rather than at the scroll event: the visible count
		 * depends on the dialog size, which depends on the window, which can
		 * change under a scroll position that was legal when it was set. */
		if (g_up_scroll > g_up_nnote - vis) g_up_scroll = g_up_nnote - vis;
		if (g_up_scroll < 0) g_up_scroll = 0;

		for (i = 0; i < vis && g_up_scroll + i < g_up_nnote; i++) {
			const char *t = g_up_note[g_up_scroll + i];
			int head = g_up_head[g_up_scroll + i];
			int maxc = (d.w - 24) / GLYPH_ADV;
			snprintf(line, sizeof(line), "%.*s", maxc > 90 ? 90 : maxc, t);
			/* Headings in the accent, body dimmed and indented — the font is
			 * one fixed 5x7 bitmap, so "smaller" has to be carried by weight
			 * and indent rather than by size. */
			text(r, head ? x : x + 8, listy + i * row, line,
			     head ? C_ACCENT : C_TEXT_DIM);
		}
		if (g_up_nnote > vis) {
			snprintf(line, sizeof(line), "%d/%d", g_up_scroll + 1, g_up_nnote);
			text(r, d.x + d.w - 12 - text_w(line), listy + listh - FONT_H,
			     line, C_TEXT_DIM);
		}

		{
			/* Download is disabled when the release carried no AppImage —
			 * better a greyed button than one that fails on click. */
			static const char *L[2] = { "Download", "Later" };
			int i2;
			for (i2 = 0; i2 < 2; i2++) {
				SDL_Rect b = up_btn(&d, i2);
				int on = (i2 == 1) || g_up_asset[0] != 0;
				int hot = on && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, L[i2],
				       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
		}
		break;
	}
	case M_ABOUT: {
		int lx = d.x + 10, ly = d.y + 20;
		if (g_logo) {
			SDL_Rect dst = { lx, ly, 52, 52 };
			SDL_RenderCopy(r, g_logo, NULL, &dst);
		}
		text(r, lx + 62, ly + 2,  ui_brand_name(), C_ACCENT);
		text(r, lx + 62, ly + 14, "LeapPad2 emulator", C_TEXT);
		text(r, lx + 62, ly + 26, "NXP3200 / VALENCIA", C_TEXT_DIM);
		/* WHICH BUILD THIS IS. Without it, "check for updates said I am up to
		 * date" and "which one am I actually running" had no answer anywhere
		 * in the program — and it is the first thing anyone reporting a bug
		 * is asked for. Baked in at build time; "dev" in a working copy. */
		text(r, lx + 62, ly + 38, TADPOLE_VERSION, C_ACCENT);
		text(r, lx, ly + 60, "qemu-user + guest shim +", C_TEXT_DIM);
		text(r, lx, ly + 70, "software GLES1 rasteriser", C_TEXT_DIM);
		text(r, lx, ly + 84, "A childhood preserved.", C_ACCENT);
		break;
	}
	case M_GFX: {
		char buf[32];
		row_check(r, &d, 0, "Enable OpenGL", g_cfg.gl,
		          row_hit(&d, 0, g_mx, g_my));
		row_check(r, &d, 1, "Host GPU replay (HLE)", g_cfg.gl_hle,
		          row_hit(&d, 1, g_mx, g_my));
		/* Only meaningful on the host-GPU path: the software rasteriser has
		 * no samples to average. Shown greyed rather than hidden, so the
		 * setting does not appear and disappear as HLE is toggled. */
		if (g_cfg.msaa) snprintf(buf, sizeof(buf), "%dx", g_cfg.msaa);
		else            snprintf(buf, sizeof(buf), "off");
		{
			int y = row_y(&d, 2);
			int hot = row_hit(&d, 2, g_mx, g_my);
			if (hot && g_cfg.gl_hle) fill(r, d.x + 6, y - 3, d.w - 12, ROW_H, C_PANEL_HI);
			text(r, d.x + 10, y, "Anti-aliasing",
			     g_cfg.gl_hle ? C_TEXT : C_TEXT_DIM);
			text(r, d.x + d.w - 12 - text_w(buf), y, buf,
			     g_cfg.gl_hle ? C_ACCENT : C_TEXT_DIM);
		}
		/* Render scale. Also HLE-only, and worth keeping next to AA: they are
		 * the same idea spent two different ways. */
		if (g_cfg.render_scale > 1) snprintf(buf, sizeof(buf), "%dx", g_cfg.render_scale);
		else                        snprintf(buf, sizeof(buf), "native");
		{
			int y = row_y(&d, 3);
			int hot = row_hit(&d, 3, g_mx, g_my);
			if (hot && g_cfg.gl_hle) fill(r, d.x + 6, y - 3, d.w - 12, ROW_H, C_PANEL_HI);
			text(r, d.x + 10, y, "Render scale", g_cfg.gl_hle ? C_TEXT : C_TEXT_DIM);
			text(r, d.x + d.w - 12 - text_w(buf), y, buf,
			     g_cfg.gl_hle ? C_ACCENT : C_TEXT_DIM);
		}
		row_check(r, &d, 4, "Stop if HLE falls back", g_cfg.hle_strict,
		          row_hit(&d, 4, g_mx, g_my));
		if (g_cfg.frame_cap) snprintf(buf, sizeof(buf), "%d fps", g_cfg.frame_cap);
		else                 snprintf(buf, sizeof(buf), "uncapped");
		row_value(r, &d, 5, "Frame cap", buf, row_hit(&d, 5, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d deg", g_cfg.rotate);
		row_value(r, &d, 6, "Orientation", buf, row_hit(&d, 6, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%dx", g_cfg.scale);
		row_value(r, &d, 7, "Window scale", buf, row_hit(&d, 7, g_mx, g_my));
		row_check(r, &d, 8, "Touch debug overlay", g_cfg.touch_debug,
		          row_hit(&d, 8, g_mx, g_my));
		text(r, d.x + 10, d.y + d.h - 30,
		     g_running ? "GL: reboot to apply."
		               : "GL applies at next boot.",
		     g_running ? C_ACCENT : C_TEXT_DIM);
		break;
	}
	case M_AUDIO: {
		char buf[32];
		row_check(r, &d, 0, "Enable audio", g_cfg.audio_on,
		          row_hit(&d, 0, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d ms", g_cfg.audio_latency_ms);
		row_value(r, &d, 1, "Max latency", buf, row_hit(&d, 1, g_mx, g_my));
		row_check(r, &d, 2, "Hold guest to realtime", g_cfg.audio_pace,
		          row_hit(&d, 2, g_mx, g_my));
		text(r, d.x + 10, d.y + 72, "Lower latency = tighter sync,", C_TEXT_DIM);
		text(r, d.x + 10, d.y + 82, "higher = fewer dropouts.", C_TEXT_DIM);
		break;
	}
	case M_DEBUG: {
		/* THE LEVEL IS THE POINT OF THIS PANEL. Everything below it writes
		 * files or changes behaviour; the level alone decides how much the
		 * emulator says. */
		static const char *LV[4] = { "0 - silent", "1 - normal",
		                             "2 - verbose", "3 - trace" };
		static const char *WHAT[4] = {
			"Nothing is logged.",
			"AppManager's serial log,",
			"+ shim tracing and every GL",
			"+ every guest syscall. Huge,",
		};
		static const char *WHAT2[4] = {
			"",
			"as the device prints it.",
			"stub and error.",
			"and slow. For one question.",
		};
		int lv = g_cfg.debug_level;
		char buf[32];
		if (lv < 0) lv = 0;
		if (lv > 3) lv = 3;
		row_value(r, &d, 0, "Debug level", LV[lv], row_hit(&d, 0, g_mx, g_my));
		/* The two explanation lines take a whole row of their own. They used
		 * to be drawn into row 1 at +8, which put the second line straight
		 * through the checkbox on row 2. */
		text(r, d.x + 14, row_y(&d, 1) - 2, WHAT[lv],  C_TEXT_DIM);
		text(r, d.x + 14, row_y(&d, 1) + 8, WHAT2[lv], C_TEXT_DIM);
		row_check(r, &d, 3, "Write a log file", g_cfg.log_to_file,
		          row_hit(&d, 3, g_mx, g_my));
		row_check(r, &d, 4, "Dump GL frames", g_cfg.gl_dumpframe,
		          row_hit(&d, 4, g_mx, g_my));
		row_check(r, &d, 5, "Dump GL textures", g_cfg.gl_dumptex,
		          row_hit(&d, 5, g_mx, g_my));
		row_check(r, &d, 6, "Use the device's tslib", g_cfg.tslib,
		          row_hit(&d, 6, g_mx, g_my));
		if (g_cfg.io_delay_us) snprintf(buf, sizeof(buf), "%d us", g_cfg.io_delay_us);
		else                   snprintf(buf, sizeof(buf), "off");
		row_value(r, &d, 7, "Fake NAND read delay", buf, row_hit(&d, 7, g_mx, g_my));
		text(r, d.x + 10, d.y + d.h - 30,
		     g_cfg.log_to_file ? "Log: ~/.local/state/tadpole/" : "",
		     C_TEXT_DIM);
		break;
	}
	case M_SYSTEM: {
		char buf[48];
		row_check(r, &d, 0, "Boot the system menu at startup",
		          g_cfg.boot_on_start, row_hit(&d, 0, g_mx, g_my));
		text(r, d.x + 10, row_y(&d, 1) + 2, "Games folder:", C_TEXT);
		path_tail(buf, sizeof(buf),
		          g_cfg.games_dir[0] ? g_cfg.games_dir : "(not chosen yet)");
		text(r, d.x + 14, row_y(&d, 1) + 12, buf,
		     g_cfg.games_dir[0] ? C_ACCENT : C_TEXT_DIM);
		{
			SDL_Rect b = { d.x + 10, row_y(&d, 3) + 2, 76, 13 };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
			bevel(r, b.x, b.y, b.w, b.h, 1);
			text_c(r, b.x, b.w, b.y + 3, "Game Library",
			       hot ? C_ACCENT : C_TEXT);
		}
		{
			SDL_Rect b = { d.x + 94, row_y(&d, 3) + 2, 96, 13 };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
			bevel(r, b.x, b.y, b.w, b.h, 1);
			text_c(r, b.x, b.w, b.y + 3, "Setup Wizard",
			       hot ? C_ACCENT : C_TEXT);
		}
		break;
	}
	case M_PAD: {
		static const char *rows[] = {
			"Arrows      D-pad",
			"X / Z       A / B",
			"Q / W       L / R",
			"Home        Menu",
			"Esc         Back",
			"Mouse       Stylus",
			"Ctrl+R      Rotate",
			"Ctrl+Q      Quit",
		};
		for (i = 0; i < (int)(sizeof rows / sizeof *rows); i++)
			text(r, d.x + 10, d.y + 20 + i * 10, rows[i], C_TEXT);
		text(r, d.x + 10, d.y + d.h - 30, "Remapping: not yet.", C_TEXT_DIM);
		break;
	}
	case M_PROGRESS: {
		int i2, first = (g_prog_n > PROG_LINES) ? g_prog_n - PROG_LINES : 0;
		int ly = d.y + 18;

		/* A moving bar, NOT a percentage. The steps are unpacking a zip,
		 * scanning packages and extracting a UBIFS volume, and their durations
		 * are wildly different and not knowable in advance — a fake percentage
		 * would be a lie. This says "still working" honestly, and the log lines
		 * below say what it is working on. */
		fill(r, d.x + 8, ly, d.w - 16, 7, C_VOID);
		bevel(r, d.x + 8, ly, d.w - 16, 7, 0);
		if (g_prog_running && g_prog_pct >= 0) {
			/* A MEASURED bar, not a moving one: the downloader knows the
			 * byte total before it starts, so this is the one step where a
			 * percentage is a fact rather than a guess. */
			int span = d.w - 20;
			int fillw = span * g_prog_pct / 100;
			char pc[8];
			if (fillw > 0) fill(r, d.x + 10, ly + 1, fillw, 5, C_ACCENT);
			snprintf(pc, sizeof(pc), "%d%%", g_prog_pct);
			text(r, d.x + d.w - 10 - text_w(pc), ly - 10, pc, C_ACCENT);
		} else if (g_prog_running) {
			int span = d.w - 20, wdt = 46;
			int pos = (int)((SDL_GetTicks() / 12) % (unsigned)(span + wdt)) - wdt;
			int x0 = d.x + 10 + (pos < 0 ? 0 : pos);
			int x1 = d.x + 10 + (pos + wdt > span ? span : pos + wdt);
			if (x1 > x0) fill(r, x0, ly + 1, x1 - x0, 5, C_ACCENT);
		} else {
			fill(r, d.x + 10, ly + 1, d.w - 20, 5,
			     g_prog_ok ? C_ACCENT : C_EDGE_LT);
		}

		fill(r, d.x + 8, ly + 12, d.w - 16, PROG_LINES * 9 + 4, C_VOID);
		bevel(r, d.x + 8, ly + 12, d.w - 16, PROG_LINES * 9 + 4, 0);
		for (i2 = first; i2 < g_prog_n; i2++)
			text(r, d.x + 12, ly + 16 + (i2 - first) * 9,
			     g_prog[i2 % PROG_LINES], C_TEXT_DIM);

		if (!g_prog_running)
			text(r, d.x + 10, d.y + d.h - 30,
			     g_prog_ok ? "Finished." : "Failed - see the lines above.",
			     g_prog_ok ? C_ACCENT : C_TEXT);
		break;
	}
	case M_MSG:
		text(r, d.x + 10, d.y + 24, g_msg_body, C_TEXT);
		if (g_confirm) {
			SDL_Rect b = { d.x + d.w - 96, d.y + d.h - 18, 44, 13 };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
			bevel(r, b.x, b.y, b.w, b.h, 1);
			text_c(r, b.x, b.w, b.y + 3, "Erase", hot ? C_ACCENT : C_TEXT);
			text(r, d.x + 10, d.y + 40, "Games and firmware downloads", C_TEXT_DIM);
			text(r, d.x + 10, d.y + 50, "are not touched.", C_TEXT_DIM);
		}
		break;
	case M_WIZARD: {
		struct prereq pq;
		int bx = d.x + 62, by = d.y + 20, i2;
		char welcome[48];
		const char *TITLES[WIZ_PAGES] = {
			welcome, "System files", "Who is playing?",
			"Games", "Ready"
		};
		snprintf(welcome, sizeof(welcome), "Welcome to %s", ui_brand_name());
		prereq_check(&pq);

		/* Banner down the left — the wizard's whole visual signature. */
		fill(r, d.x + 1, d.y + 12, 56, d.h - 30, C_VOID);
		if (g_logo) {
			SDL_Rect dst = { d.x + 8, d.y + 20, 40, 40 };
			SDL_RenderCopy(r, g_logo, NULL, &dst);
		}
		{
			/* CENTRED, not fixed at +6. The banner is 56px and the wordmark
			 * is now variable: TADPOLE is seven glyphs, GLASSPOLE is nine,
			 * and at GLYPH_ADV the longer one ran past the banner edge. */
			char mark[16]; size_t mi; int mw, mx;
			snprintf(mark, sizeof(mark), "%s", ui_brand_name());
			for (mi = 0; mark[mi]; mi++)
				if (mark[mi] >= 'a' && mark[mi] <= 'z') mark[mi] -= 32;
			mw = (int)strlen(mark) * GLYPH_ADV;
			mx = d.x + 1 + (56 - mw) / 2;
			if (mx < d.x + 1) mx = d.x + 1;
			text(r, mx, d.y + 66, mark, C_ACCENT);
		}
		for (i2 = 0; i2 < WIZ_PAGES; i2++)
			text(r, d.x + 8, d.y + 80 + i2 * 9,
			     i2 == g_wiz_page ? GL_SUB : " ",
			     i2 == g_wiz_page ? C_ACCENT : C_TEXT_DIM);

		text(r, bx, by, TITLES[g_wiz_page], C_ACCENT);
		fill(r, bx, by + 10, d.w - 70, 1, C_EDGE_DK);
		by += 18;

		switch (g_wiz_page) {
		case WIZ_WELCOME:
			/* Glasspole says cross-platform because it IS the reason it
			 * exists: qemu-arm's user mode is Linux-only, so Tadpole cannot
			 * follow it to Windows and Glasspole can. Tadpole keeps the plain
			 * line rather than claiming something it does not do. */
			text(r, bx, by, ui_brand_is_glasspole()
			                ? "A cross-platform LeapPad2 emulator."
			                : "A LeapPad2 emulator.", C_TEXT);
			{
				char line[64];
				snprintf(line, sizeof(line),
				         "%s contains NO LeapFrog code.", ui_brand_name());
				text(r, bx, by + 14, line, C_TEXT);
			}
			text(r, bx, by + 24, "You supply the system files and", C_TEXT_DIM);
			text(r, bx, by + 34, "games, from hardware you own.", C_TEXT_DIM);
			text(r, bx, by + 50, "Two steps, and this wizard does", C_TEXT_DIM);
			text(r, bx, by + 60, "both: system files, then games.", C_TEXT_DIM);
			/* The whole former "What you need" page, as one honest line. */
			if (pq.qemu && pq.fwtools)
				text(r, bx, by + 78,
				     pq.qemu_bundled ? GL_CHECK_1 " Everything else is built in."
				                     : GL_CHECK_1 " Your system has what it needs.",
				     C_TEXT);
			else if (!pq.qemu)
				text(r, bx, by + 78, GL_CHECK_0 " qemu-arm is missing.", C_ACCENT);
			else
				text(r, bx, by + 78, GL_CHECK_0 " No firmware extractor.", C_ACCENT);
			if (!pq.qemu || !pq.fwtools)
				text(r, bx, by + 88, "  Run ./tools/fetch-deps.sh", C_TEXT_DIM);
			break;
		case WIZ_SYSTEM:
			/* ---- the state of the install, as a checklist --------------
			 *
			 * Two lines of yes/no used to sit at the top of this page and
			 * that was the whole report. A setup wizard is judged on whether
			 * you can see where you are in it, so this is a list with ticks
			 * and a rule under it — the shape every installer has used since
			 * they were a novelty. */
			{
				static const char *NAMES[3] = { "System files",
				                                "Runtime sysroot",
				                                "Games" };
				int ok[3], i3;
				ok[0] = pq.rootfs; ok[1] = pq.sysroot; ok[2] = pq.games;
				for (i3 = 0; i3 < 3; i3++) {
					int yy = by + i3 * 11;
					text(r, bx, yy, ok[i3] ? GL_CHECK_1 : GL_CHECK_0,
					     ok[i3] ? C_ACCENT : C_TEXT_DIM);
					text(r, bx + 12, yy, NAMES[i3], ok[i3] ? C_TEXT : C_TEXT_DIM);
					text(r, bx + d.w - 132, yy, ok[i3] ? "ready" : "missing",
					     ok[i3] ? C_ACCENT : C_TEXT_DIM);
				}
				fill(r, bx, by + 35, d.w - 76, 1, C_EDGE_DK);
			}
			text(r, bx, by + 42, "From your own LFConnect downloads:", C_TEXT_DIM);
			{
				SDL_Rect b = { bx, by + 54, 76, 13 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, "Browse...", hot ? C_ACCENT : C_TEXT);
			}
			/* THE SYSROOT NEEDS ITS OWN BUTTON. Installing firmware builds it,
			 * but the two can get out of step — an interrupted install, or an
			 * Erase that took the sysroot with it — and then the page reported
			 * "Sysroot not built" with no way to act on it. */
			if (pq.rootfs && !pq.sysroot) {
				SDL_Rect b = { bx + 84, by + 54, 96, 13 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, "Build sysroot",
				       hot ? C_ACCENT : C_TEXT);
			}
			/* ---- Online System Update ----
			 *
			 * This is where someone stands when they discover they need
			 * firmware and have no idea where to get it, so this is where the
			 * answer belongs: one button, no hardware, no PC software.
			 */
			/* ---- Online System Update ----
			 *
			 * The recommended path, and drawn like it: a boxed panel with a
			 * rule above it, the button raised, and the download size stated
			 * so nobody starts 124 MB by accident. This is where someone
			 * stands when they discover they need firmware and have no idea
			 * where to get it — so this is where the answer belongs. */
			fill(r, bx, by + 74, d.w - 76, 1, C_EDGE_DK);
			text(r, bx, by + 80, "Or download them, with no device at all:",
			     C_TEXT);
			{
				SDL_Rect b = { bx, by + 92, 132, 15 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x + 1, b.y + 1, b.w, b.h, C_SHADOW);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL_HI);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 4, "Online System Update",
				       hot ? C_ACCENT : C_TEXT);
				text(r, b.x + b.w + 8, b.y + 4, "124 MB", C_TEXT_DIM);
				text(r, bx, by + 111, "from digitalcontent.leapfrog.com",
				     C_TEXT_DIM);
			}
			break;
		case WIZ_PROFILE: {
			/* A form, laid out like one: label, field, cursor. The name box
			 * is the only place in Tadpole that takes typing, so it says so
			 * — a bare rectangle that happens to accept keys is not
			 * discoverable. */
			int fy = by + 14;
			text(r, bx, by, "The LeapPad asks who is playing before", C_TEXT_DIM);
			text(r, bx, by + 10, "it will show a home screen.", C_TEXT_DIM);

			text(r, bx, fy + 8, "Name", C_TEXT);
			{
				SDL_Rect f = { bx + 40, fy + 4, 132, 14 };
				int hot = inside(g_mx, g_my, f.x, f.y, f.w, f.h);
				fill(r, f.x, f.y, f.w, f.h, C_VOID);
				bevel(r, f.x, f.y, f.w, f.h, g_prof_focus ? 1 : 0);
				text(r, f.x + 4, f.y + 4, g_prof_name, C_TEXT);
				if (g_prof_focus && (SDL_GetTicks() / 450) % 2 == 0)
					fill(r, f.x + 4 + text_w(g_prof_name), f.y + 3, 1, 8, C_ACCENT);
				if (!g_prof_name[0] && !g_prof_focus)
					text(r, f.x + 4, f.y + 4, "click and type", C_TEXT_DIM);
				(void)hot;
			}

			{
				char gb[24];
				SDL_Rect g = { bx + 40, fy + 22, 60, 14 };
				int hot = inside(g_mx, g_my, g.x, g.y, g.w, g.h);
				text(r, bx, fy + 26, "Grade", C_TEXT);
				fill(r, g.x, g.y, g.w, g.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, g.x, g.y, g.w, g.h, 1);
				if (g_prof_grade <= 0) snprintf(gb, sizeof(gb), "Pre-K");
				else                   snprintf(gb, sizeof(gb), "Grade %d", g_prof_grade);
				text_c(r, g.x, g.w, g.y + 4, gb, hot ? C_ACCENT : C_TEXT);
			}

			{
				SDL_Rect p2 = { bx + 40, fy + 40, 60, 14 };
				int hot = inside(g_mx, g_my, p2.x, p2.y, p2.w, p2.h);
				text(r, bx, fy + 44, "Photo", C_TEXT);
				fill(r, p2.x, p2.y, p2.w, p2.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, p2.x, p2.y, p2.w, p2.h, 1);
				text_c(r, p2.x, p2.w, p2.y + 4, "Choose...",
				       hot ? C_ACCENT : C_TEXT);
				if (g_prof_pic[0]) {
					char shown[26];
					path_tail(shown, sizeof(shown), g_prof_pic);
					text(r, p2.x + 66, p2.y + 4, shown, C_ACCENT);
				} else {
					text(r, p2.x + 66, p2.y + 4, "optional, .jpg", C_TEXT_DIM);
				}
			}

			{
				SDL_Rect b = { bx, fy + 60, 96, 15 };
				int on = g_prof_name[0] != 0;
				int hot = on && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x + 1, b.y + 1, b.w, b.h, C_SHADOW);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL_HI);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 4, "Create profile",
				       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
				if (g_prof_made)
					text(r, b.x + b.w + 8, b.y + 4, GL_CHECK_1 " created", C_ACCENT);
				else if (!on)
					text(r, b.x + b.w + 8, b.y + 4, "a name is needed", C_TEXT_DIM);
			}
			break;
		}
		case WIZ_GAMES:
			text(r, bx, by, pq.games ? GL_CHECK_1 " Games installed"
			                         : GL_CHECK_0 " No games yet",
			     pq.games ? C_TEXT : C_ACCENT);
			text(r, bx, by + 16, "Point Tadpole at the folder of", C_TEXT_DIM);
			text(r, bx, by + 26, ".tar backups you made from your", C_TEXT_DIM);
			text(r, bx, by + 36, "own cartridges, and pick from a", C_TEXT_DIM);
			text(r, bx, by + 46, "list of covers and names.", C_TEXT_DIM);
			{
				SDL_Rect b = { bx, by + 62, 96, 13 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, "Open Library",
				       hot ? C_ACCENT : C_TEXT);
			}
			text(r, bx, by + 80, "Later: File " GL_SUB " Game Library.", C_TEXT_DIM);
			break;
		case WIZ_DONE:
			if (pq.rootfs && pq.sysroot) {
				text(r, bx, by, "Everything needed is in place.", C_TEXT);
				text(r, bx, by + 16, "Finish, then use", C_TEXT_DIM);
				text(r, bx, by + 26, "File " GL_SUB " Run System Menu.", C_ACCENT);
				if (!pq.games) {
					text(r, bx, by + 44, "No games yet, but the system", C_TEXT_DIM);
					text(r, bx, by + 54, "menu will still start.", C_TEXT_DIM);
				}
			} else {
				text(r, bx, by, "Still missing:", C_ACCENT);
				if (!pq.rootfs)  text(r, bx, by + 12, "  firmware (system files)", C_TEXT);
				if (!pq.sysroot) text(r, bx, by + 22, "  the sysroot", C_TEXT);
				text(r, bx, by + 38, "Tadpole will not boot without", C_TEXT_DIM);
				text(r, bx, by + 48, "them. Go Back to install.", C_TEXT_DIM);
			}
			break;
		}

		/* buttons */
		{
			static const char *L[3] = { "Back", "Next", "Cancel" };
			for (i2 = 0; i2 < 3; i2++) {
				SDL_Rect b = wiz_btn(&d, i2);
				int on = !(i2 == 0 && g_wiz_page == 0);
				int hot = on && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				const char *lab = (i2 == 1 && g_wiz_page == WIZ_PAGES - 1)
				                ? "Finish" : L[i2];
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, lab,
				       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
		}
		break;
	}
	case M_GAMES: {
		int pw = gm_panel(&d), lw2 = gm_list_w(&d), lh2 = gm_list_h(&d);
		int lx = d.x + 6, ly = d.y + 26, i2;
		char buf[64];

		g_gm_rows = lh2 / GM_ROW_H;
		if (g_gm_rows < 1) g_gm_rows = 1;

		/* Where these came from, and how many there are. */
		{
			char shown[40];
			path_tail(shown, sizeof(shown), g_gm_dir[0] ? g_gm_dir : "(no folder chosen)");
			text(r, d.x + 6, d.y + 15, shown, C_TEXT_DIM);
			snprintf(buf, sizeof(buf), "%d", g_gm_n);
			text(r, d.x + d.w - 8 - text_w(buf), d.y + 15, buf, C_TEXT_DIM);
		}

		fill(r, lx, ly, lw2, lh2, C_VOID);
		bevel(r, lx, ly, lw2, lh2, 0);

		if (g_gm_n == 0) {
			/* THREE DIFFERENT NOTHINGS, and they need different advice. */
			const char *a = !g_gm_scanned ? "Choose the folder holding your"
			              : g_gm_dir[0]   ? "No game backups in that folder."
			                              : "No games found.";
			const char *b = !g_gm_scanned ? "game backups, below."
			              : g_gm_dir[0]   ? "They are .tar files made with"
			                              : "";
			const char *c = (!g_gm_scanned || !g_gm_dir[0]) ? ""
			              : "LFManager from your cartridges.";
			text(r, lx + 8, ly + 12, a, C_TEXT);
			text(r, lx + 8, ly + 24, b, C_TEXT_DIM);
			text(r, lx + 8, ly + 34, c, C_TEXT_DIM);
		}

		for (i2 = 0; i2 < g_gm_rows && g_gm_top + i2 < g_gm_n; i2++) {
			int k = g_gm_top + i2;
			struct gentry *e = &g_gm[k];
			int ry = ly + 2 + i2 * GM_ROW_H;
			int tx = lx + 4;
			char nm[64];
			unsigned col = C_TEXT;

			if (k == g_gm_sel) fill(r, lx + 1, ry - 1, lw2 - 2, GM_ROW_H, C_PANEL_HI);

			/* tick box, so a batch install is one obvious gesture */
			text(r, tx, ry + 3, e->checked ? GL_CHECK_1 : GL_CHECK_0,
			     e->checked ? C_ACCENT : C_TEXT_DIM);
			tx += 12;

			/* the icon, at 13px — small, but it is the thing people recognise */
			game_icon(r, e);
			if (e->tex) {
				SDL_Rect dst = { tx, ry, 13, 13 };
				SDL_RenderCopy(r, e->tex, NULL, &dst);
			} else {
				fill(r, tx, ry, 13, 13, C_PANEL);
				bevel(r, tx, ry, 13, 13, 0);
			}
			tx += 17;

			if (e->installed) col = C_TEXT_DIM;      /* already here */
			{
				int room = (lx + lw2 - 6 - tx) / GLYPH_ADV;
				if (room > (int)sizeof(nm) - 1) room = (int)sizeof(nm) - 1;
				if (room < 1) room = 1;
				snprintf(nm, (size_t)room + 1, "%s", e->name);
				text(r, tx, ry + 3, nm, col);
			}
			if (e->installed)
				text(r, lx + lw2 - 8 - text_w(GL_CHECK_1), ry + 3,
				     GL_CHECK_1, C_ACCENT);
		}

		/* scrollbar: with eighty-seven titles, "where am I" is a real question */
		if (g_gm_n > g_gm_rows) {
			int track = lh2 - 4;
			int knob = track * g_gm_rows / g_gm_n;
			int pos  = track * g_gm_top / g_gm_n;
			if (knob < 6) knob = 6;
			fill(r, lx + lw2 - 4, ly + 2, 3, track, C_VOID);
			fill(r, lx + lw2 - 4, ly + 2 + pos, 3, knob, C_EDGE_LT);
		}

		/* ---- the preview panel ---- */
		if (pw && g_gm_n && g_gm_sel >= 0 && g_gm_sel < g_gm_n) {
			struct gentry *e = &g_gm[g_gm_sel];
			int px = d.x + 6 + lw2 + 4, py = ly;
			int side = pw - 10;
			game_icon(r, e);
			fill(r, px, py, pw - 4, lh2, C_VOID);
			bevel(r, px, py, pw - 4, lh2, 0);
			if (e->tex) {
				/* Fit, do not stretch: these are 83x91 and 90x77 and so on,
				 * and a squashed icon looks like a decoding fault. */
				int iw = e->tw, ih = e->th, dw, dh;
				if (iw * side / (ih ? ih : 1) <= side) { dh = side; dw = iw * side / (ih ? ih : 1); }
				else                                   { dw = side; dh = ih * side / (iw ? iw : 1); }
				{
					SDL_Rect dst = { px + 2 + (side - dw) / 2, py + 4, dw, dh };
					SDL_RenderCopy(r, e->tex, NULL, &dst);
				}
			} else {
				text_c(r, px, pw - 4, py + side / 2, "no icon", C_TEXT_DIM);
			}
			{
				int ty = py + side + 8, cols = (pw - 12) / GLYPH_ADV, w2;
				const char *s = e->name;
				/* wrap the name over up to three lines, breaking on spaces */
				for (w2 = 0; w2 < 3 && *s; w2++) {
					char part[40];
					int take = cols, j;
					if ((int)strlen(s) > take) {
						for (j = take; j > 4; j--)
							if (s[j] == ' ' || s[j] == ':') { take = j; break; }
					} else {
						take = (int)strlen(s);
					}
					if (take > (int)sizeof(part) - 1) take = (int)sizeof(part) - 1;
					memcpy(part, s, (size_t)take); part[take] = 0;
					text(r, px + 4, ty + w2 * 9, part, C_TEXT);
					s += take;
					while (*s == ' ') s++;
				}
				ty += w2 * 9 + 4;
				if (e->ver[0]) {
					snprintf(buf, sizeof(buf), "v%s", e->ver);
					text(r, px + 4, ty, buf, C_TEXT_DIM); ty += 9;
				}
				snprintf(buf, sizeof(buf), "%lld MB", (e->bytes + 524288) / 1048576);
				text(r, px + 4, ty, buf, C_TEXT_DIM); ty += 9;
				if (e->installed)
					text(r, px + 4, ty, "installed", C_ACCENT);
			}
		}

		/* ---- buttons ---- */
		{
			int by = d.y + d.h - 18, i3;
			static const char *L[4] = { "Folder...", "Rescan", "", "" };
			int xs[2] = { d.x + 6, d.x + 6 + 56 };
			int ws[2] = { 52, 44 };
			for (i3 = 0; i3 < 2; i3++) {
				int hot = inside(g_mx, g_my, xs[i3], by, ws[i3], 13);
				int on = (i3 == 0) || g_gm_dir[0];
				fill(r, xs[i3], by, ws[i3], 13, hot && on ? C_BAR_HI : C_PANEL);
				bevel(r, xs[i3], by, ws[i3], 13, 1);
				text_c(r, xs[i3], ws[i3], by + 3, L[i3],
				       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
			{
				int n = games_checked();
				SDL_Rect b = { d.x + d.w - 8 - 48 - 62, by, 62, 13 };
				int hot = n && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				if (n) snprintf(buf, sizeof(buf), "Install %d", n);
				else   snprintf(buf, sizeof(buf), "Install");
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, buf,
				       !n ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
		}
		break;
	}
	case M_FILES: {
		int ly = d.y + 16, i2;
		char shown[49];
		path_tail(shown, sizeof(shown), g_fb_dir);
		text(r, d.x + 6, ly, shown, C_TEXT_DIM);

		fill(r, d.x + 6, ly + 11, d.w - 12, FB_ROWS * 11 + 2, C_VOID);
		bevel(r, d.x + 6, ly + 11, d.w - 12, FB_ROWS * 11 + 2, 0);
		for (i2 = 0; i2 < FB_ROWS && g_fb_top + i2 < g_fb_n; i2++) {
			int k = g_fb_top + i2;
			int ry = ly + 13 + i2 * 11;
			char nm[47];
			unsigned col = g_fb_list[k].isdir ? C_ACCENT : C_TEXT;
			if (k == g_fb_sel) {
				fill(r, d.x + 8, ry - 1, d.w - 16, 11, C_PANEL_HI);
				col = C_TEXT;
			}
			nm[0] = g_fb_list[k].isdir ? GL_SUB[0] : ' ';
			nm[1] = ' ';
			path_tail(nm + 2, sizeof(nm) - 2, g_fb_list[k].name);
			text(r, d.x + 10, ry, nm, col);
		}
		if (g_fb_n == 0)
			text(r, d.x + 12, ly + 15, "(nothing here)", C_TEXT_DIM);
		text(r, d.x + 6, d.y + d.h - 16,
		     *g_fb_filter ? g_fb_filter : "any file", C_TEXT_DIM);
		break;
	}
	default: break;
	}

	if (g_modal == M_WIZARD || g_modal == M_UPDATE)
		return;      /* both carry their own buttons; a generic Close would
		              * land on top of them, which it did */
	cb = close_rect(&d);
	{
		int busy = (g_modal == M_PROGRESS && g_prog_running);
		int hot = !busy && inside(g_mx, g_my, cb.x, cb.y, cb.w, cb.h);
		fill(r, cb.x, cb.y, cb.w, cb.h, hot ? C_BAR_HI : C_PANEL);
		bevel(r, cb.x, cb.y, cb.w, cb.h, 1);
		text_c(r, cb.x, cb.w, cb.y + 3,
		       g_modal == M_FILES ? "Cancel" : "Close",
		       busy ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
	}
	if (g_modal == M_FILES) {
		SDL_Rect ok = { cb.x - 46, cb.y, 42, 13 };
		int hot = inside(g_mx, g_my, ok.x, ok.y, ok.w, ok.h);
		fill(r, ok.x, ok.y, ok.w, ok.h, hot ? C_BAR_HI : C_PANEL);
		bevel(r, ok.x, ok.y, ok.w, ok.h, 1);
		text_c(r, ok.x, ok.w, ok.y + 3, "Open", hot ? C_ACCENT : C_TEXT);
		/* Some things ARE the folder: an LFC_Downloads directory of firmware
		 * packages, or a folder of game backups. A browser that can only open
		 * directories cannot express "I mean this one", so those two get a
		 * button that says it.
		 *
		 * Keyed on the ACTION, not on "there is no filter": the games chooser
		 * filters to .tar so you can see the backups sitting there and know
		 * you are in the right place, and it needs this button most of all. */
		if (g_fb_action == UI_ACT_SETUP_FIRMWARE ||
		    g_fb_action == UI_ACT_SCAN_GAMES) {
			SDL_Rect uf = { ok.x - 74, ok.y, 70, 13 };
			int h2 = inside(g_mx, g_my, uf.x, uf.y, uf.w, uf.h);
			fill(r, uf.x, uf.y, uf.w, uf.h, h2 ? C_BAR_HI : C_PANEL);
			bevel(r, uf.x, uf.y, uf.w, uf.h, 1);
			text_c(r, uf.x, uf.w, uf.y + 3, "Use folder",
			       h2 ? C_ACCENT : C_TEXT);
		}
	}
}

void ui_draw_idle(SDL_Renderer *ren, int lw, int lh)
{
	int cy = UI_BAR_H + (lh - UI_BAR_H) / 2;
	fill(ren, 0, UI_BAR_H, lw, lh - UI_BAR_H, C_VOID);
	if (g_logo) {
		SDL_Rect dst = { lw / 2 - 32, cy - 46, 64, 64 };
		SDL_RenderCopy(ren, g_logo, NULL, &dst);
	}
	{
		char mark[16]; size_t mi;
		snprintf(mark, sizeof(mark), "%s", ui_brand_name());
		for (mi = 0; mark[mi]; mi++)
			if (mark[mi] >= 'a' && mark[mi] <= 'z') mark[mi] -= 32;
		text_c(ren, 0, lw, cy + 26, mark, C_ACCENT);
	}
	text_c(ren, 0, lw, cy + 40, "File " GL_SUB " Run System Menu", C_TEXT_DIM);
}

void ui_draw(SDL_Renderer *r, int lw, int lh)
{
	draw_bar(r, lw);
	draw_dropdown(r);
	draw_dialog(r, lw, lh);
}

/* ---- scripted state, for screenshots and regression captures --------------
 *
 * The desktop here is Wayland, and a Wayland compositor will not route
 * XTEST-synthesised clicks into an XWayland client — so "open the menu and
 * screenshot it" cannot be driven from outside the process. This puts the UI
 * into a named state directly instead, which is also a far more repeatable way
 * to capture a panel than aiming a synthetic pointer at it.
 *
 * Format: "<state>" or "<state>@<x>,<y>" to place the cursor for hover.
 */
void ui_debug_state(const char *spec)
{
	char name[32];
	const char *at = strchr(spec, '@');
	size_t n = at ? (size_t)(at - spec) : strlen(spec);

	if (n >= sizeof(name)) n = sizeof(name) - 1;
	memcpy(name, spec, n);
	name[n] = 0;
	if (at) sscanf(at + 1, "%d,%d", &g_mx, &g_my);

	g_open_menu = -1;
	g_modal = M_NONE;
	if      (!strcmp(name, "idle"))    ;
	else if (!strcmp(name, "file"))    g_open_menu = 0;
	else if (!strcmp(name, "options")) g_open_menu = 1;
	else if (!strcmp(name, "help"))    g_open_menu = 2;
	else if (!strcmp(name, "gfx"))     g_modal = M_GFX;
	else if (!strcmp(name, "audio"))   g_modal = M_AUDIO;
	else if (!strcmp(name, "pad"))     g_modal = M_PAD;
	else if (!strcmp(name, "debug"))   g_modal = M_DEBUG;
	else if (!strcmp(name, "system"))  g_modal = M_SYSTEM;
	else if (!strcmp(name, "about"))   g_modal = M_ABOUT;
	/* `apps@x,y` renders the launcher with the pointer at x,y, so a row can be
	 * shown highlighted without anyone touching a mouse. */
	else if (!strcmp(name, "apps"))    { ap_reload(); g_modal = M_APPS; }
	else if (!strcmp(name, "games")) {
		ui_games_reload();
		if (!g_gm_dir[0] && g_cfg.games_dir[0])
			snprintf(g_gm_dir, sizeof(g_gm_dir), "%s", g_cfg.games_dir);
		/* Tick a couple, so a screenshot shows what selection looks like. */
		if (g_gm_n > 2) { g_gm[1].checked = 1; g_gm[2].checked = 1; }
		g_gm_sel = (g_gm_n > 1) ? 1 : 0;
		g_modal = M_GAMES;
	}
	else if (!strcmp(name, "files"))   activate(IT_SWF);
	else if (!strcmp(name, "pkg"))     activate(IT_PKG);
	else if (!strcmp(name, "running")) { g_running = 1; ui_status("running"); }
	else if (!strcmp(name, "progress")) {
		ui_progress_begin("Setup System Firmware");
		ui_progress_line("==> 70 package(s) to inspect");
		ui_progress_line("==> Firmware-Base version 4.6.0.784");
		ui_progress_line("  root filesystem: C4G-E1M-W4K-erootfs.ubi");
		ui_progress_line("  kernel: kernel.bin");
		ui_progress_line("==> extracting the root filesystem");
		ui_progress_line("    (a 53 MB volume - takes a minute or two)");
	}
	else if (!strncmp(name, "wiz", 3)) {
		g_modal = M_WIZARD;
		g_wiz_page = (name[3] >= '0' && name[3] <= '9') ? name[3] - '0' : 0;
	}
	/* hovering an item only makes sense once the menu is open */
	if (g_open_menu >= 0 && at) {
		const struct menu *m = &MENUS[g_open_menu];
		int w = menu_width(m), i;
		for (i = 0; i < m->n; i++)
			if (inside(g_mx, g_my, m->x, UI_BAR_H + 3 + i * 12, w, 12))
				g_hot_item = i;
	}
}

/* ---- events -------------------------------------------------------------- */

static void cycle_rotate(void)
{
	g_cfg.rotate = (g_cfg.rotate + 90) % 360;
	g_action = UI_ACT_RELAYOUT;
	ui_cfg_save();
}

static int dialog_click(int lw, int lh, int mx, int my)
{
	struct dlg d = cur_dlg(lw, lh);
	SDL_Rect cb = close_rect(&d);

	if (g_modal == M_APPS && g_ap_n) {
		int x = d.x + 10, y = d.y + 20, i, row = 30;
		int listh = d.h - 30 - 22, vis = listh / row;
		for (i = 0; i < vis && g_ap_top + i < g_ap_n; i++) {
			if (!inside(mx, my, x - 2, y + i * row - 2, d.w - 16, row - 2))
				continue;
			snprintf(g_action_path, sizeof(g_action_path), "%s",
			         g_ap[g_ap_top + i].pkg);
			g_action = UI_ACT_RUN_APP;
			g_modal = M_NONE;
			return 1;
		}
	}

	if (g_modal == M_UPDATE) {
		SDL_Rect b0 = up_btn(&d, 0), b1 = up_btn(&d, 1);
		if (g_up_asset[0] && inside(mx, my, b0.x, b0.y, b0.w, b0.h)) {
			g_action = UI_ACT_DO_UPDATE;
			g_modal = M_NONE;
			return 1;
		}
		if (inside(mx, my, b1.x, b1.y, b1.w, b1.h)) {
			g_modal = M_NONE;
			return 1;
		}
	}

	if (g_modal == M_WIZARD) {
		struct prereq pq;
		int i;
		char start[PATHMAX];

		prereq_check(&pq);
		for (i = 0; i < 3; i++) {
			SDL_Rect b = wiz_btn(&d, i);
			if (!inside(mx, my, b.x, b.y, b.w, b.h)) continue;
			if (i == 0 && g_wiz_page > 0) g_wiz_page--;
			else if (i == 1) {
				if (g_wiz_page < WIZ_PAGES - 1) g_wiz_page++;
				else { g_modal = M_NONE; g_wiz_page = 0; }
			} else if (i == 2) { g_modal = M_NONE; g_wiz_page = 0; }
			return 1;
		}
		/* the per-page Browse buttons */
		if (g_wiz_page == WIZ_SYSTEM && pq.rootfs && !pq.sysroot &&
		    inside(mx, my, d.x + 62 + 84, d.y + 92, 96, 13)) {
			g_action = UI_ACT_BUILD_SYSROOT;
			return 1;
		}
		if (g_wiz_page == WIZ_SYSTEM &&
		    inside(mx, my, d.x + 62, d.y + 92, 76, 13)) {
			path_join(start, sizeof(start), g_proj, "sources");
			if (access(start, R_OK) != 0)
				path_join(start, sizeof(start), g_proj, "");
			/* A directory OR a package: install-firmware.sh takes either, and a
			 * user with a loose .lfp should not have to know the difference. */
			fb_open("Firmware folder or package", start, "",
			        UI_ACT_SETUP_FIRMWARE);
			return 1;
		}
		if (g_wiz_page == WIZ_PROFILE) {
			int fy = d.y + 38 + 14;
			int bx2 = d.x + 62;
			if (inside(mx, my, bx2 + 40, fy + 4, 132, 14)) {
				g_prof_focus = 1;
				return 1;
			}
			g_prof_focus = 0;
			if (inside(mx, my, bx2 + 40, fy + 22, 60, 14)) {
				g_prof_grade = (g_prof_grade + 1) % 6;   /* Pre-K .. 5 */
				return 1;
			}
			if (inside(mx, my, bx2 + 40, fy + 40, 60, 14)) {
				const char *home = getenv("HOME");
				fb_open("Profile photo (.jpg)", home ? home : "/", ".jpg",
				        UI_ACT_NONE);
				g_fb_action = UI_ACT_NONE;   /* the path is kept, not acted on */
				g_fb_return = M_WIZARD;
				g_fb_pick_profile = 1;
				return 1;
			}
			if (g_prof_name[0] && inside(mx, my, bx2, fy + 60, 96, 15)) {
				g_action = UI_ACT_MAKE_PROFILE;
				g_fb_return = M_WIZARD;
				g_prof_made = 1;
				return 1;
			}
			return 1;
		}
		if (g_wiz_page == WIZ_SYSTEM &&
		    inside(mx, my, d.x + 62, d.y + 130, 132, 15)) {
			g_action = UI_ACT_ONLINE_UPDATE;
			g_fb_return = M_WIZARD;      /* back to setup when it finishes */
			return 1;
		}
		if (g_wiz_page == WIZ_GAMES &&
		    inside(mx, my, d.x + 62, d.y + 100, 96, 13)) {
			games_open();          /* remembers that it must come back here */
			return 1;
		}
		return 1;
	}
	if (g_modal == M_MSG && g_confirm) {
		SDL_Rect y = { d.x + d.w - 96, d.y + d.h - 18, 44, 13 };
		if (inside(mx, my, y.x, y.y, y.w, y.h)) {
			g_action = UI_ACT_ERASE_FW;
			g_confirm = 0;
			g_modal = M_NONE;
			return 1;
		}
	}
	if (inside(mx, my, cb.x, cb.y, cb.w, cb.h)) {
		if (g_modal == M_PROGRESS && g_prog_running)
			return 1;                 /* cannot dismiss mid-install */
		g_confirm = 0;
		/* Back to whatever we came from, so a Cancel or a finished install
		 * returns the user to setup — or to the library — rather than to an
		 * empty screen. */
		if ((g_modal == M_FILES || g_modal == M_PROGRESS || g_modal == M_MSG) &&
		    (g_fb_return == M_WIZARD || g_fb_return == M_GAMES)) {
			g_modal = g_fb_return;
			g_fb_return = M_NONE;
		} else if (g_modal == M_GAMES) {
			g_modal = g_gm_return;
			g_gm_return = M_NONE;
		} else {
			g_modal = M_NONE;
		}
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_GAMES) {
		int pw = gm_panel(&d), lw2 = gm_list_w(&d), lh2 = gm_list_h(&d);
		int lx = d.x + 6, ly = d.y + 26, i;
		int by = d.y + d.h - 18;
		(void)pw;

		if (inside(mx, my, d.x + 6, by, 52, 13)) {      /* Folder... */
			games_choose_folder();
			return 1;
		}
		if (inside(mx, my, d.x + 62, by, 44, 13)) {     /* Rescan */
			if (g_gm_dir[0]) {
				snprintf(g_action_path, sizeof(g_action_path), "%s", g_gm_dir);
				g_action = UI_ACT_SCAN_GAMES;
				g_fb_return = M_GAMES;   /* the progress panel comes back here */
			}
			return 1;
		}
		{                                               /* Install N */
			SDL_Rect b = { d.x + d.w - 8 - 48 - 62, by, 62, 13 };
			if (inside(mx, my, b.x, b.y, b.w, b.h)) {
				if (games_checked() &&
				    games_write_list(g_action_path, sizeof(g_action_path))) {
					g_action = UI_ACT_INSTALL_GAMES;
					g_fb_return = M_GAMES;
				}
				return 1;
			}
		}
		for (i = 0; i < g_gm_rows && g_gm_top + i < g_gm_n; i++) {
			if (inside(mx, my, lx + 1, ly + 1 + i * GM_ROW_H, lw2 - 2, GM_ROW_H)) {
				int k = g_gm_top + i;
				/* One click does both jobs: it selects the row, so the panel
				 * on the right describes it, and it ticks it, so Install knows
				 * what to install. Anything subtler needs explaining. */
				g_gm_sel = k;
				g_gm[k].checked = !g_gm[k].checked;
				return 1;
			}
		}
		(void)lh2;
		return 1;    /* inside the dialog, but not on anything */
	}
	if (g_modal == M_FILES) {
		SDL_Rect ok = { cb.x - 46, cb.y, 42, 13 };
		int ly = d.y + 16, i;
		if (g_fb_action == UI_ACT_SETUP_FIRMWARE ||
		    g_fb_action == UI_ACT_SCAN_GAMES) {
			SDL_Rect uf = { ok.x - 74, ok.y, 70, 13 };
			if (inside(mx, my, uf.x, uf.y, uf.w, uf.h)) {
				path_join(g_action_path, sizeof(g_action_path), g_fb_dir, "");
				g_action = g_fb_action;
				if (g_fb_action == UI_ACT_SCAN_GAMES) {
					/* Remember it now, not when the scan finishes: the folder
					 * is what the user chose, and it should still be their
					 * folder even if the scan finds nothing in it. */
					snprintf(g_gm_dir, sizeof(g_gm_dir), "%s", g_action_path);
					snprintf(g_cfg.games_dir, sizeof(g_cfg.games_dir), "%s",
					         g_action_path);
					ui_cfg_save();
					g_fb_return = M_GAMES;
				}
				g_modal = M_NONE;      /* the progress panel takes over */
				return 1;
			}
		}
		if (inside(mx, my, ok.x, ok.y, ok.w, ok.h)) { fb_enter(); return 1; }
		for (i = 0; i < FB_ROWS && g_fb_top + i < g_fb_n; i++) {
			if (inside(mx, my, d.x + 8, ly + 12 + i * 11, d.w - 16, 11)) {
				int k = g_fb_top + i;
				if (k == g_fb_sel) fb_enter();      /* second click opens */
				else g_fb_sel = k;
				return 1;
			}
		}
		return 1;
	}
	if (g_modal == M_GFX) {
		if (row_hit(&d, 0, mx, my)) {
			g_cfg.gl = !g_cfg.gl;
			if (g_running)
				ui_status("GL %s on reboot", g_cfg.gl ? "on" : "off");
		}
		else if (row_hit(&d, 1, mx, my)) {
			g_cfg.gl_hle = !g_cfg.gl_hle;
			if (g_running)
				ui_status("HLE %s on reboot", g_cfg.gl_hle ? "on" : "off");
		}
		else if (row_hit(&d, 2, mx, my) && g_cfg.gl_hle) {
			static const int s[] = { 0, 2, 4, 8 };
			int i, k = 0;
			for (i = 0; i < 4; i++)
				if (s[i] == g_cfg.msaa) k = (i + 1) % 4;
			g_cfg.msaa = s[k];
			/* The viewer notices the change and rebuilds the render target on
			 * the next frame, so this takes effect while you watch — which is
			 * the only way to judge whether it was worth having. */
		}
		else if (row_hit(&d, 3, mx, my) && g_cfg.gl_hle) {
			/* Up to 8x, which is 3840x2176 — 4K for a 480x272 panel. The
			 * driver's own ceiling is asked for at build time and the
			 * request clamped to it, so an ambitious setting costs frames
			 * rather than producing a black screen. */
			static const int steps[] = { 1, 2, 3, 4, 6, 8 };
			int i, k = 0;
			for (i = 0; i < 6; i++)
				if (steps[i] == g_cfg.render_scale) k = (i + 1) % 6;
			g_cfg.render_scale = steps[k];
		}
		else if (row_hit(&d, 4, mx, my)) g_cfg.hle_strict = !g_cfg.hle_strict;
		else if (row_hit(&d, 5, mx, my)) {
			static const int hz[] = { 60, 30, 0 };   /* 0 = uncapped */
			int i, k = 0;
			for (i = 0; i < 3; i++)
				if (hz[i] == g_cfg.frame_cap) k = (i + 1) % 3;
			g_cfg.frame_cap = hz[k];
		}
		else if (row_hit(&d, 6, mx, my)) cycle_rotate();
		else if (row_hit(&d, 7, mx, my)) {
			g_cfg.scale = g_cfg.scale % 4 + 1;
			g_action = UI_ACT_RELAYOUT;
		}
		else if (row_hit(&d, 8, mx, my)) g_cfg.touch_debug = !g_cfg.touch_debug;
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_AUDIO) {
		if (row_hit(&d, 0, mx, my)) g_cfg.audio_on = !g_cfg.audio_on;
		else if (row_hit(&d, 1, mx, my)) {
			static const int steps[] = { 80, 120, 180, 260, 400, 600 };
			int i, k = 0;
			for (i = 0; i < 6; i++)
				if (steps[i] == g_cfg.audio_latency_ms) k = (i + 1) % 6;
			g_cfg.audio_latency_ms = steps[k];
		}
		else if (row_hit(&d, 2, mx, my)) g_cfg.audio_pace = !g_cfg.audio_pace;
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_DEBUG) {
		if (row_hit(&d, 0, mx, my))
			g_cfg.debug_level = (g_cfg.debug_level + 1) % 4;
		else if (row_hit(&d, 3, mx, my)) g_cfg.log_to_file = !g_cfg.log_to_file;
		else if (row_hit(&d, 4, mx, my)) g_cfg.gl_dumpframe = !g_cfg.gl_dumpframe;
		else if (row_hit(&d, 5, mx, my)) g_cfg.gl_dumptex = !g_cfg.gl_dumptex;
		else if (row_hit(&d, 6, mx, my)) g_cfg.tslib = !g_cfg.tslib;
		else if (row_hit(&d, 7, mx, my)) {
			static const int us[] = { 0, 200, 1000, 5000 };
			int i, k = 0;
			for (i = 0; i < 4; i++)
				if (us[i] == g_cfg.io_delay_us) k = (i + 1) % 4;
			g_cfg.io_delay_us = us[k];
		}
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_SYSTEM) {
		if (row_hit(&d, 0, mx, my)) g_cfg.boot_on_start = !g_cfg.boot_on_start;
		else if (inside(mx, my, d.x + 10, row_y(&d, 3) + 2, 76, 13)) {
			ui_cfg_save();
			games_open();
			return 1;
		}
		else if (inside(mx, my, d.x + 94, row_y(&d, 3) + 2, 96, 13)) {
			ui_cfg_save();
			g_wiz_page = 0;
			g_modal = M_WIZARD;
			return 1;
		}
		ui_cfg_save();
		return 1;
	}
	return 1;    /* modal swallows everything */
}

int ui_event(const SDL_Event *e, int lw, int lh)
{
	switch (e->type) {
	case SDL_MOUSEMOTION:
		g_mx = e->motion.x; g_my = e->motion.y;
		if (g_open_menu >= 0) {
			const struct menu *m = &MENUS[g_open_menu];
			int w = menu_width(m), i;
			g_hot_item = -1;
			for (i = 0; i < m->n; i++)
				if (inside(g_mx, g_my, m->x, UI_BAR_H + 3 + i * 12, w, 12))
					g_hot_item = i;
			/* slide between menus while one is open, like a real menu bar */
			for (i = 0; i < NMENUS; i++)
				if (inside(g_mx, g_my, MENUS[i].x, 0, MENUS[i].w, UI_BAR_H))
					g_open_menu = i;
			return 1;
		}
		if (g_modal != M_NONE) return 1;
		return g_my < UI_BAR_H;

	case SDL_MOUSEBUTTONDOWN: {
		int mx = e->button.x, my = e->button.y, i;
		g_mx = mx; g_my = my;

		if (g_modal != M_NONE)
			return dialog_click(lw, lh, mx, my);

		if (g_open_menu >= 0) {
			const struct menu *m = &MENUS[g_open_menu];
			int w = menu_width(m);
			for (i = 0; i < m->n; i++) {
				if (inside(mx, my, m->x, UI_BAR_H + 3 + i * 12, w, 12)) {
					if (item_enabled(&m->items[i]))
						activate(m->items[i].id);
					return 1;
				}
			}
			g_open_menu = -1;               /* click-away closes */
			if (my >= UI_BAR_H) return 1;
		}
		if (my < UI_BAR_H) {
			if (inside(mx, my, rot_x(lw), 1, ROT_W, UI_BAR_H - 3)) {
				cycle_rotate();
				return 1;
			}
			for (i = 0; i < NMENUS; i++)
				if (inside(mx, my, MENUS[i].x, 0, MENUS[i].w, UI_BAR_H)) {
					g_open_menu = (g_open_menu == i) ? -1 : i;
					g_hot_item = -1;
					return 1;
				}
			return 1;                       /* bar swallows stray clicks */
		}
		return 0;
	}

	case SDL_MOUSEBUTTONUP:
		if (g_modal != M_NONE) return 1;
		if (g_open_menu >= 0) return 1;
		return e->button.y < UI_BAR_H;

	case SDL_MOUSEWHEEL:
		if (g_modal == M_FILES) {
			g_fb_top -= e->wheel.y;
			if (g_fb_top > g_fb_n - FB_ROWS) g_fb_top = g_fb_n - FB_ROWS;
			if (g_fb_top < 0) g_fb_top = 0;
			return 1;
		}
		if (g_modal == M_GAMES) {
			g_gm_top -= e->wheel.y * 2;
			if (g_gm_top > g_gm_n - g_gm_rows) g_gm_top = g_gm_n - g_gm_rows;
			if (g_gm_top < 0) g_gm_top = 0;
			return 1;
		}
		if (g_modal == M_APPS) {
			g_ap_top -= e->wheel.y * 2;
			if (g_ap_top > g_ap_n - g_ap_rows) g_ap_top = g_ap_n - g_ap_rows;
			if (g_ap_top < 0) g_ap_top = 0;
			return 1;
		}
		if (g_modal == M_UPDATE) {
			/* Only the lower bound is clamped here. How many lines fit depends
			 * on the dialog height, which depends on the window; the draw
			 * clamps against the real visible count each frame. */
			g_up_scroll -= e->wheel.y * 3;
			if (g_up_scroll < 0) g_up_scroll = 0;
			return 1;
		}
		return g_modal != M_NONE;

	case SDL_TEXTINPUT:
		/* THE ONLY PLACE IN TADPOLE THAT TAKES TYPING. Everything else is
		 * pointer-driven, so text input is enabled all the time and simply
		 * ignored unless the name field has focus — no mode to get stuck in. */
		if (g_modal == M_WIZARD && g_wiz_page == WIZ_PROFILE && g_prof_focus) {
			size_t n = strlen(g_prof_name);
			const char *t = e->text.text;
			for (; *t && n < sizeof(g_prof_name) - 1; t++)
				if ((unsigned char)*t >= 0x20 && (unsigned char)*t < 0x7F)
					g_prof_name[n++] = *t;
			g_prof_name[n] = 0;
			g_prof_made = 0;      /* edited since it was written */
			return 1;
		}
		return g_modal != M_NONE;

	case SDL_KEYDOWN:
		if (g_modal == M_WIZARD && g_wiz_page == WIZ_PROFILE && g_prof_focus) {
			size_t n = strlen(g_prof_name);
			if (e->key.keysym.sym == SDLK_BACKSPACE) {
				if (n) g_prof_name[n - 1] = 0;
				g_prof_made = 0;
				return 1;
			}
			if (e->key.keysym.sym == SDLK_RETURN ||
			    e->key.keysym.sym == SDLK_TAB) {
				g_prof_focus = 0;
				return 1;
			}
			if (e->key.keysym.sym == SDLK_ESCAPE) {
				g_prof_focus = 0;
				return 1;
			}
		}
		if (g_modal == M_GAMES) {
			switch (e->key.keysym.sym) {
			case SDLK_ESCAPE:
				g_modal = g_gm_return; g_gm_return = M_NONE; return 1;
			case SDLK_SPACE:
				if (g_gm_sel >= 0 && g_gm_sel < g_gm_n)
					g_gm[g_gm_sel].checked = !g_gm[g_gm_sel].checked;
				return 1;
			case SDLK_RETURN:
				/* Enter installs whatever is ticked, and if nothing is, the
				 * one under the cursor — pressing Return on a highlighted game
				 * should not quietly do nothing. */
				if (!games_checked() && g_gm_sel >= 0 && g_gm_sel < g_gm_n)
					g_gm[g_gm_sel].checked = 1;
				if (games_checked() &&
				    games_write_list(g_action_path, sizeof(g_action_path))) {
					g_action = UI_ACT_INSTALL_GAMES;
					g_fb_return = M_GAMES;
				}
				return 1;
			case SDLK_UP:       if (g_gm_sel > 0) g_gm_sel--; break;
			case SDLK_DOWN:     if (g_gm_sel < g_gm_n - 1) g_gm_sel++; break;
			case SDLK_PAGEUP:   g_gm_sel -= g_gm_rows; break;
			case SDLK_PAGEDOWN: g_gm_sel += g_gm_rows; break;
			case SDLK_HOME:     g_gm_sel = 0; break;
			case SDLK_END:      g_gm_sel = g_gm_n - 1; break;
			default: return 1;
			}
			if (g_gm_sel < 0) g_gm_sel = 0;
			if (g_gm_sel > g_gm_n - 1) g_gm_sel = g_gm_n - 1;
			if (g_gm_sel < g_gm_top) g_gm_top = g_gm_sel;
			if (g_gm_sel >= g_gm_top + g_gm_rows)
				g_gm_top = g_gm_sel - g_gm_rows + 1;
			return 1;
		}
		if (g_modal == M_FILES) {
			switch (e->key.keysym.sym) {
			case SDLK_ESCAPE: g_modal = M_NONE; return 1;
			case SDLK_RETURN: fb_enter(); return 1;
			case SDLK_UP:     if (g_fb_sel > 0) g_fb_sel--; break;
			case SDLK_DOWN:   if (g_fb_sel < g_fb_n - 1) g_fb_sel++; break;
			case SDLK_PAGEUP:   g_fb_sel -= FB_ROWS; break;
			case SDLK_PAGEDOWN: g_fb_sel += FB_ROWS; break;
			case SDLK_HOME:   g_fb_sel = 0; break;
			case SDLK_END:    g_fb_sel = g_fb_n - 1; break;
			default: return 1;
			}
			if (g_fb_sel < 0) g_fb_sel = 0;
			if (g_fb_sel > g_fb_n - 1) g_fb_sel = g_fb_n - 1;
			if (g_fb_sel < g_fb_top) g_fb_top = g_fb_sel;
			if (g_fb_sel >= g_fb_top + FB_ROWS) g_fb_top = g_fb_sel - FB_ROWS + 1;
			return 1;
		}
		if (g_modal != M_NONE) {
			if (e->key.keysym.sym == SDLK_ESCAPE ||
			    e->key.keysym.sym == SDLK_RETURN) {
				if (g_modal == M_PROGRESS && g_prog_running)
					return 1;
				if (g_modal == M_PROGRESS && g_fb_return == M_WIZARD) {
					g_modal = M_WIZARD;
					g_fb_return = M_NONE;
				} else {
					g_modal = M_NONE;
				}
				ui_cfg_save();
			}
			return 1;
		}
		if (g_open_menu >= 0) {
			if (e->key.keysym.sym == SDLK_ESCAPE) { g_open_menu = -1; return 1; }
			return 1;
		}
		return 0;

	case SDL_KEYUP:
		return g_modal != M_NONE || g_open_menu >= 0;
	}
	return 0;
}

/* Referenced by the About text; kept out of the way of the rest. */
static void unused_keep(void) { (void)onoff; (void)msg; (void)GL_DIAMOND; }
void ui_unused_ref(void);
void ui_unused_ref(void) { unused_keep(); }
