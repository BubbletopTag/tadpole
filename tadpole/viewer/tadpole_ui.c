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
#include <pwd.h>

/* Long enough for any real path; the browser can walk anywhere. */
#define PATHMAX 1024

#define FONT_W 5
#define FONT_H 7
#define GLYPH_ADV 6          /* 5px cell + 1px gap */

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
#define C_BAR       0x14301FU     /* menu bar */
#define C_BAR_HI    0x2A6642U     /* hovered/open bar item */
#define C_PANEL     0x14301FU     /* dropdown + dialog body */
#define C_PANEL_HI  0x2A6642U
#define C_EDGE_LT   0x4E9C6BU     /* bevel light */
#define C_EDGE_DK   0x08180FU     /* bevel dark */
#define C_TEXT      0xD8F5E4U
#define C_TEXT_DIM  0x6E9B80U
#define C_ACCENT    0x8CE0A6U
#define C_SHADOW    0x030806U

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
	.gl_debug         = 0,
	.gl_dumpframe     = 0,
	.gl_dumptex       = 0,
	.shim_debug       = 0,
	.rotate           = 0,
	.scale            = 2,
	.touch_debug      = 0,
	.audio_on         = 1,
	.audio_latency_ms = 260,
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
enum modal_kind { M_NONE = 0, M_ABOUT, M_GFX, M_AUDIO, M_PAD, M_FILES, M_MSG,
                  M_WIZARD, M_PROGRESS };
static enum modal_kind g_modal;
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
static char g_prog_title[64];

void ui_progress_begin(const char *title)
{
	snprintf(g_prog_title, sizeof(g_prog_title), "%s", title ? title : "Working");
	memset(g_prog, 0, sizeof g_prog);
	g_prog_n = 0;
	g_prog_running = 1;
	g_prog_ok = 0;
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
enum { WIZ_WELCOME = 0, WIZ_LEGAL, WIZ_SYSTEM, WIZ_GAMES, WIZ_DONE, WIZ_PAGES };
static int g_wiz_page;

struct prereq { int rootfs, sysroot, games, qemu; };

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
	p->qemu = access("/usr/bin/qemu-arm", X_OK) == 0 ||
	          access("/usr/local/bin/qemu-arm", X_OK) == 0;
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

static void logo_load(SDL_Renderer *ren, const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char *file = NULL, *idat = NULL, *raw = NULL;
	Uint32 *px = NULL;
	long len;
	unsigned pos = 8, idatn = 0, w = 0, h = 0;
	int bpp = 4, ok = 0;

	if (!f) return;
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len < 16 || !(file = malloc((size_t)len)) ||
	    fread(file, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(file); return; }
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
		g_logo = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		                           SDL_TEXTUREACCESS_STATIC, (int)w, (int)h);
		if (g_logo) {
			SDL_UpdateTexture(g_logo, NULL, px, (int)w * 4);
			SDL_SetTextureBlendMode(g_logo, SDL_BLENDMODE_BLEND);
			g_logo_w = (int)w; g_logo_h = (int)h;
			g_logo_px = px;
			px = NULL;                 /* owned by g_logo_px now */
			ok = 1;
		}
	}
done:
	(void)ok;
	free(file); free(idat); free(raw); free(px);
}

/* ---- settings persistence ------------------------------------------------ */

static void cfg_path(char *out, size_t n)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : "/tmp";
	}
	snprintf(out, n, "%s/.config/tadpole", home);
	mkdir(out, 0755);
	snprintf(out, n, "%s/.config/tadpole/ui.cfg", home);
}

void ui_cfg_save(void)
{
	char p[PATHMAX];
	FILE *f;
	cfg_path(p, sizeof(p));
	if (!(f = fopen(p, "w"))) return;
	fprintf(f, "gl %d\ngl_hle %d\ngl_debug %d\ngl_dumpframe %d\ngl_dumptex %d\n"
	           "shim_debug %d\nrotate %d\nscale %d\ntouch_debug %d\n"
	           "audio_on %d\naudio_latency_ms %d\n",
	        g_cfg.gl, g_cfg.gl_hle, g_cfg.gl_debug, g_cfg.gl_dumpframe, g_cfg.gl_dumptex,
	        g_cfg.shim_debug, g_cfg.rotate, g_cfg.scale, g_cfg.touch_debug,
	        g_cfg.audio_on, g_cfg.audio_latency_ms);
	fclose(f);
}

static void cfg_load(void)
{
	char p[PATHMAX], k[64];
	int v;
	FILE *f;
	cfg_path(p, sizeof(p));
	if (!(f = fopen(p, "r"))) return;
	while (fscanf(f, "%63s %d", k, &v) == 2) {
		if      (!strcmp(k, "gl"))               g_cfg.gl = v;
		else if (!strcmp(k, "gl_hle"))           g_cfg.gl_hle = v;
		else if (!strcmp(k, "gl_debug"))         g_cfg.gl_debug = v;
		else if (!strcmp(k, "gl_dumpframe"))     g_cfg.gl_dumpframe = v;
		else if (!strcmp(k, "gl_dumptex"))       g_cfg.gl_dumptex = v;
		else if (!strcmp(k, "shim_debug"))       g_cfg.shim_debug = v;
		else if (!strcmp(k, "rotate"))           g_cfg.rotate = v;
		else if (!strcmp(k, "scale"))            g_cfg.scale = v;
		else if (!strcmp(k, "touch_debug"))      g_cfg.touch_debug = v;
		else if (!strcmp(k, "audio_on"))         g_cfg.audio_on = v;
		else if (!strcmp(k, "audio_latency_ms")) g_cfg.audio_latency_ms = v;
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
	IT_RUN_UI = 1, IT_SWF, IT_PKG, IT_FW, IT_STOP, IT_QUIT,
	IT_AUDIO, IT_GFX, IT_PAD, IT_ABOUT, IT_WIZARD, IT_ERASE
};

static const struct mitem FILE_ITEMS[] = {
	/* Booting needs the system files. Offering it without them produces a wall
	 * of missing-path errors in a terminal the user may not even be looking at,
	 * which is precisely what the wizard exists to prevent. */
	{ "Run System Menu",        IT_RUN_UI, 0, 1, 1 },
	{ "Launch .swf...",         IT_SWF,    0, 1, 1 },
	{ "",                       0,         0, 0, 0 },
	{ "Install Package...",     IT_PKG,    0, 0, 0 },
	{ "Setup System Firmware...", IT_FW,   0, 0, 0 },
	{ "Erase System Firmware",  IT_ERASE,  0, 1, 1 },
	{ "",                       0,         0, 0, 0 },
	{ "Stop Emulation",         IT_STOP,   1, 0, 0 },
	{ "Quit",                   IT_QUIT,   0, 0, 0 },
};
static const struct mitem OPT_ITEMS[] = {
	{ "Audio Settings...",      IT_AUDIO,  0, 0, 0 },
	{ "Graphics Settings...",   IT_GFX,    0, 0, 0 },
	{ "Controller Settings...", IT_PAD,    0, 0, 0 },
};
static const struct mitem HELP_ITEMS[] = {
	{ "Setup Wizard...",        IT_WIZARD, 0, 0, 0 },
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
	g_fb_return = (g_modal == M_WIZARD) ? M_WIZARD : M_NONE;
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
	g_action = g_fb_action;
	g_modal = M_NONE;
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
	path_join(p, sizeof(p), g_proj, "tadpole.png");
	logo_load(ren, p);
	snprintf(g_status, sizeof(g_status), "idle");

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

static void activate(int id)
{
	char start[PATHMAX];
	g_open_menu = -1;
	switch (id) {
	case IT_RUN_UI: g_action = UI_ACT_RUN_UI; break;
	case IT_STOP:   g_action = UI_ACT_STOP;   break;
	case IT_QUIT:   g_action = UI_ACT_QUIT;   break;
	case IT_SWF:
		path_join(start, sizeof(start), g_proj, "runtime/sysroot/LF");
		fb_open("Launch .swf", start, ".swf", UI_ACT_RUN_SWF);
		break;
	case IT_PKG:
		path_join(start, sizeof(start), g_proj, "games");
		if (access(start, R_OK) != 0)
			path_join(start, sizeof(start), g_proj, "");
		fb_open("Install Package", start, ".tar", UI_ACT_INSTALL_PKG);
		break;
	case IT_FW:
		path_join(start, sizeof(start), g_proj, "");
		fb_open("Setup System Firmware", start, ".zip", UI_ACT_SETUP_FIRMWARE);
		break;
	case IT_AUDIO: g_modal = M_AUDIO; break;
	case IT_GFX:   g_modal = M_GFX;   break;
	case IT_PAD:   g_modal = M_PAD;   break;
	case IT_ABOUT: g_modal = M_ABOUT; break;
	case IT_WIZARD: g_wiz_page = 0; g_modal = M_WIZARD; break;
	case IT_ERASE:
		/* Confirm first: this is the one menu item that throws work away. */
		g_confirm = IT_ERASE;
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

static struct dlg cur_dlg(int lw, int lh)
{
	switch (g_modal) {
	case M_ABOUT: return dlg_rect(lw, lh, 210, 132);
	case M_GFX:   return dlg_rect(lw, lh, 250, 164);
	case M_AUDIO: return dlg_rect(lw, lh, 230, 108);
	case M_PAD:   return dlg_rect(lw, lh, 240, 140);
	case M_FILES: return dlg_rect(lw, lh, 300, 172);
	case M_WIZARD: return dlg_rect(lw, lh, 340, 196);
	case M_PROGRESS: return dlg_rect(lw, lh, 350, 150);
	case M_MSG:   return dlg_rect(lw, lh, 250, 92);
	default:      { struct dlg z = {0,0,0,0}; return z; }
	}
}

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
	case M_FILES: title = g_fb_title; break;
	case M_PROGRESS: title = g_prog_title; break;
	case M_MSG:   title = g_msg_title; break;
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
	case M_ABOUT: {
		int lx = d.x + 10, ly = d.y + 20;
		if (g_logo) {
			SDL_Rect dst = { lx, ly, 52, 52 };
			SDL_RenderCopy(r, g_logo, NULL, &dst);
		}
		text(r, lx + 62, ly + 2,  "Tadpole", C_ACCENT);
		text(r, lx + 62, ly + 14, "LeapPad2 emulator", C_TEXT);
		text(r, lx + 62, ly + 26, "NXP3200 / VALENCIA", C_TEXT_DIM);
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
		row_check(r, &d, 2, "GL debug logging", g_cfg.gl_debug,
		          row_hit(&d, 2, g_mx, g_my));
		row_check(r, &d, 3, "Dump GL frames", g_cfg.gl_dumpframe,
		          row_hit(&d, 3, g_mx, g_my));
		row_check(r, &d, 4, "Dump GL textures", g_cfg.gl_dumptex,
		          row_hit(&d, 4, g_mx, g_my));
		row_check(r, &d, 5, "Touch debug overlay", g_cfg.touch_debug,
		          row_hit(&d, 5, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d deg", g_cfg.rotate);
		row_value(r, &d, 6, "Orientation", buf, row_hit(&d, 6, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%dx", g_cfg.scale);
		row_value(r, &d, 7, "Window scale", buf, row_hit(&d, 7, g_mx, g_my));
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
		text(r, d.x + 10, d.y + 58, "Lower = tighter sync,", C_TEXT_DIM);
		text(r, d.x + 10, d.y + 68, "higher = fewer dropouts.", C_TEXT_DIM);
		break;
	}
	case M_PAD: {
		static const char *rows[] = {
			"Arrows      D-pad",
			"Z / X       A / B",
			"A / S       L / R",
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
		if (g_prog_running) {
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
		static const char *TITLES[WIZ_PAGES] = {
			"Welcome to Tadpole", "What you need", "System files",
			"Games", "Ready"
		};
		prereq_check(&pq);

		/* Banner down the left — the wizard's whole visual signature. */
		fill(r, d.x + 1, d.y + 12, 56, d.h - 30, C_VOID);
		if (g_logo) {
			SDL_Rect dst = { d.x + 8, d.y + 20, 40, 40 };
			SDL_RenderCopy(r, g_logo, NULL, &dst);
		}
		text(r, d.x + 6, d.y + 66, "TADPOLE", C_ACCENT);
		for (i2 = 0; i2 < WIZ_PAGES; i2++)
			text(r, d.x + 8, d.y + 80 + i2 * 9,
			     i2 == g_wiz_page ? GL_SUB : " ",
			     i2 == g_wiz_page ? C_ACCENT : C_TEXT_DIM);

		text(r, bx, by, TITLES[g_wiz_page], C_ACCENT);
		fill(r, bx, by + 10, d.w - 70, 1, C_EDGE_DK);
		by += 18;

		switch (g_wiz_page) {
		case WIZ_WELCOME:
			text(r, bx, by,      "A LeapPad2 emulator.", C_TEXT);
			text(r, bx, by + 12, "Tadpole contains NO LeapFrog code.", C_TEXT);
			text(r, bx, by + 24, "You supply the system files and", C_TEXT_DIM);
			text(r, bx, by + 34, "games, from hardware you own.", C_TEXT_DIM);
			text(r, bx, by + 50, "This wizard checks what is missing", C_TEXT_DIM);
			text(r, bx, by + 60, "and helps you install it.", C_TEXT_DIM);
			break;
		case WIZ_LEGAL:
			text(r, bx, by,      "Dependencies:", C_ACCENT);
			text(r, bx, by + 11, pq.qemu ? GL_CHECK_1 " qemu-arm" : GL_CHECK_0 " qemu-arm  MISSING",
			     pq.qemu ? C_TEXT : C_ACCENT);
			text(r, bx, by + 21, GL_CHECK_1 " SDL2  (already running)", C_TEXT);
			/* ubi_reader needs lzallright, cryptography and zstandard, and it
			 * imports them lazily — so "is ubi_reader installed" is not the
			 * question. tools/check-deps.sh tests the whole set at once. */
			text(r, bx, by + 31, "  firmware tools: run", C_TEXT_DIM);
			text(r, bx, by + 41, "  ./tools/check-deps.sh", C_ACCENT);
			/* Glyphs are 7px tall, so rows need 10 and a SECTION break needs
			 * more. This heading was 6px below the line above it and the two
			 * overlapped into an unreadable smear. */
			text(r, bx, by + 60, "System files and games:", C_ACCENT);
			text(r, bx, by + 72, "Tadpole ships none. It uses the", C_TEXT_DIM);
			text(r, bx, by + 82, "files from your own device --", C_TEXT_DIM);
			text(r, bx, by + 92, "see README.md. No warranty.", C_TEXT_DIM);
			break;
		case WIZ_SYSTEM:
			text(r, bx, by, pq.rootfs ? GL_CHECK_1 " Firmware installed"
			                          : GL_CHECK_0 " Firmware NOT installed",
			     pq.rootfs ? C_TEXT : C_ACCENT);
			text(r, bx, by + 11, pq.sysroot ? GL_CHECK_1 " Sysroot built"
			                                : GL_CHECK_0 " Sysroot not built",
			     pq.sysroot ? C_TEXT : C_ACCENT);
			text(r, bx, by + 27, "Point this at your LFConnect", C_TEXT_DIM);
			text(r, bx, by + 37, "downloads folder (it holds the", C_TEXT_DIM);
			text(r, bx, by + 47, ".lf2 / .lfp packages).", C_TEXT_DIM);
			{
				SDL_Rect b = { bx, by + 60, 76, 13 };
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
				SDL_Rect b = { bx + 84, by + 60, 96, 13 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, "Build sysroot",
				       hot ? C_ACCENT : C_TEXT);
			}
			break;
		case WIZ_GAMES:
			text(r, bx, by, pq.games ? GL_CHECK_1 " Games installed"
			                         : GL_CHECK_0 " No games yet",
			     pq.games ? C_TEXT : C_ACCENT);
			text(r, bx, by + 16, "Games are .tar backups made with", C_TEXT_DIM);
			text(r, bx, by + 26, "LFManager from cartridges you own.", C_TEXT_DIM);
			text(r, bx, by + 42, "You can add more later from", C_TEXT_DIM);
			text(r, bx, by + 52, "File " GL_SUB " Install Package.", C_TEXT_DIM);
			{
				SDL_Rect b = { bx, by + 66, 76, 13 };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				fill(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL);
				bevel(r, b.x, b.y, b.w, b.h, 1);
				text_c(r, b.x, b.w, b.y + 3, "Browse...", hot ? C_ACCENT : C_TEXT);
			}
			break;
		case WIZ_DONE:
			if (pq.rootfs && pq.sysroot) {
				text(r, bx, by, "Everything needed is in place.", C_TEXT);
				text(r, bx, by + 16, "Finish, then use", C_TEXT_DIM);
				text(r, bx, by + 26, "File " GL_SUB " Run System Menu.", C_ACCENT);
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

	if (g_modal == M_WIZARD)
		return;                      /* it has its own Back/Next/Cancel */
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
		/* Firmware can be a DIRECTORY (an LFC_Downloads folder) as well as an
		 * archive, and install-firmware.sh takes either — but a browser that
		 * only ever opens directories cannot express "I mean this one". No
		 * filter means we are picking firmware, so offer it. */
		if (!*g_fb_filter) {
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
	text_c(ren, 0, lw, cy + 26, "TADPOLE", C_ACCENT);
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
	else if (!strcmp(name, "about"))   g_modal = M_ABOUT;
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
		    inside(mx, my, d.x + 62 + 84, d.y + 98, 96, 13)) {
			g_action = UI_ACT_BUILD_SYSROOT;
			return 1;
		}
		if (g_wiz_page == WIZ_SYSTEM &&
		    inside(mx, my, d.x + 62, d.y + 98, 76, 13)) {
			path_join(start, sizeof(start), g_proj, "sources");
			if (access(start, R_OK) != 0)
				path_join(start, sizeof(start), g_proj, "");
			/* A directory OR a package: install-firmware.sh takes either, and a
			 * user with a loose .lfp should not have to know the difference. */
			fb_open("Firmware folder or package", start, "",
			        UI_ACT_SETUP_FIRMWARE);
			return 1;
		}
		if (g_wiz_page == WIZ_GAMES &&
		    inside(mx, my, d.x + 62, d.y + 104, 76, 13)) {
			path_join(start, sizeof(start), g_proj, "games");
			if (access(start, R_OK) != 0)
				path_join(start, sizeof(start), g_proj, "");
			fb_open("Game .tar backup", start, ".tar", UI_ACT_INSTALL_PKG);
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
		/* Back to the wizard if that is where we came from, so a Cancel or a
		 * finished install returns the user to setup rather than an empty
		 * screen. */
		if ((g_modal == M_FILES || g_modal == M_PROGRESS) &&
		    g_fb_return == M_WIZARD) {
			g_modal = M_WIZARD;
			g_fb_return = M_NONE;
		} else {
			g_modal = M_NONE;
		}
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_FILES) {
		SDL_Rect ok = { cb.x - 46, cb.y, 42, 13 };
		int ly = d.y + 16, i;
		if (!*g_fb_filter) {
			SDL_Rect uf = { ok.x - 74, ok.y, 70, 13 };
			if (inside(mx, my, uf.x, uf.y, uf.w, uf.h)) {
				path_join(g_action_path, sizeof(g_action_path), g_fb_dir, "");
				g_action = g_fb_action;
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
		else if (row_hit(&d, 2, mx, my)) g_cfg.gl_debug = !g_cfg.gl_debug;
		else if (row_hit(&d, 3, mx, my)) g_cfg.gl_dumpframe = !g_cfg.gl_dumpframe;
		else if (row_hit(&d, 4, mx, my)) g_cfg.gl_dumptex = !g_cfg.gl_dumptex;
		else if (row_hit(&d, 5, mx, my)) g_cfg.touch_debug = !g_cfg.touch_debug;
		else if (row_hit(&d, 6, mx, my)) cycle_rotate();
		else if (row_hit(&d, 7, mx, my)) {
			g_cfg.scale = g_cfg.scale % 4 + 1;
			g_action = UI_ACT_RELAYOUT;
		}
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
		return g_modal != M_NONE;

	case SDL_KEYDOWN:
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
