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
 * Dark blue, a few flat tones, 1px bevels. Deliberately few colours: pixel
 * interfaces read better with a tight palette than with gradients.
 */
/* TWO PALETTES, ONE CODEBASE — AND THE COLOUR IS NOT THE PRODUCT NAME.
 *
 * These started as one thing. Green meant "Tadpole, running the guest under
 * qemu-arm"; blue meant "Glasspole, running it under our own ARM JIT", which
 * was the only option on Windows. One boolean picked the wordmark, the logo
 * and the palette together, so the colour on screen was a promise about which
 * engine was behind the picture.
 *
 * Glasspole is now the default engine everywhere (see tools/lib-deps.sh), and
 * that promise would have repainted the whole application on people who never
 * asked for a different product. So the two questions are separated:
 *
 *   what it is CALLED   Tadpole. Always, on every engine. The one exception is
 *                       the Windows installer's own build, which ships under
 *                       the Glasspole name and has to keep answering to it.
 *   what COLOUR it is   blue, on both. It is the house colour now, not a
 *                       status light.
 *
 * The blue is the green rotated towards cyan at the same lightness, so the
 * layout, contrast and bevels behave identically — which is what makes this a
 * palette swap and not a redesign. The green is kept and reachable with
 * TADPOLE_THEME=green: it is the look every screenshot in the docs was taken
 * in, and deleting it would make those a lie with no way back.
 *
 * DEEPER THAN THEY WERE, now that the panels are translucent. Flat opaque
 * chrome wants a mid tone or it reads as a hole; glass wants a dark one,
 * because the light in it comes from what is BEHIND it and a pale sheet has
 * nowhere left to brighten. Only the structural tones moved — bar, panel,
 * their highlights and the bevel edges. Text, dim text and the accent are
 * untouched, so every one of them gained contrast against the darker ground
 * rather than losing it.
 *
 * C_VOID JOINS THE STRUCT, which was a small bug fix when the chrome was flat
 * and is not optional now that it is glass. It was the one colour left as a
 * compile-time constant, so blue chrome drew on a dark GREEN backdrop — and
 * the backdrop is no longer just what shows around the panels: it is the light
 * the glass gathers and blurs, so a green void tints every frosted surface on
 * the screen from behind. Both palettes carry their own.
 *
 * The blue void is the green one taken through the same rotation as the rest
 * of the pair — G scaled by ~0.86, B taking the green's old G — so the two
 * looks are lit identically and only the hue differs. */
#define C_VOID      (g_pal->void_bg)  /* behind everything */
struct palette {
	unsigned void_bg;
	unsigned bar, bar_hi, panel, panel_hi, edge_lt, edge_dk;
	/* text_dimmest is a THIRD step down, for a caption that should be
	 * readable when looked at and invisible when not — the package ID beside
	 * an app's name is the only user so far. C_TEXT_DIM already carries real
	 * information elsewhere (a version, a hint, a disabled menu item), and
	 * reusing it here made the ID compete with the name it captions. */
	unsigned text, text_dim, text_dimmest, accent, shadow;
};

static const struct palette pal_green = {
	0x07150DU,
	0x0E2416U, 0x1D4A2FU, 0x0E2416U, 0x1D4A2FU, 0x3C7A53U, 0x05110AU,
	0xD8F5E4U, 0x6E9B80U, 0x47665AU, 0x8CE0A6U, 0x020604U
};

static const struct palette pal_blue = {
	0x071216U,
	0x0E1F25U, 0x1C4451U, 0x0E1F25U, 0x1C4451U, 0x3A6C7CU, 0x050E12U,
	0xD8EEF5U, 0x6E8F9BU, 0x47606AU, 0x8CCFE0U, 0x020506U
};

static const struct palette *g_pal = &pal_blue;

#define C_BAR       (g_pal->bar)      /* menu bar */
#define C_BAR_HI    (g_pal->bar_hi)   /* hovered/open bar item */
#define C_PANEL     (g_pal->panel)    /* dropdown + dialog body */
#define C_PANEL_HI  (g_pal->panel_hi)
#define C_EDGE_LT   (g_pal->edge_lt)  /* bevel light */
#define C_EDGE_DK   (g_pal->edge_dk)  /* bevel dark */
#define C_TEXT      (g_pal->text)
#define C_TEXT_DIM  (g_pal->text_dim)
#define C_DIMMEST   (g_pal->text_dimmest)
#define C_ACCENT    (g_pal->accent)
#define C_SHADOW    (g_pal->shadow)

/* WHICH PRODUCT IS THIS?
 *
 * IT IS NO LONGER A QUESTION ABOUT THE ENGINE. This used to answer "Glasspole"
 * whenever TADPOLE_QEMU named a glasspole binary, back when choosing that
 * binary was an unusual thing to do on purpose. tadpole.sh now sets that
 * variable for everyone, on every run, so the same test would rename the
 * application out from under every user on this platform. Which engine is
 * running is reported by ui_engine_name(), where a fact about the engine
 * belongs; it does not decide what the program is called.
 *
 *   1. TADPOLE_BRAND, if set, wins. An escape hatch for capturing either look
 *      without a Windows machine to do it on.
 *   2. On Windows it is Glasspole, because that is the name the installer
 *      writes into Programs\Glasspole and the Start menu. A window titled
 *      Tadpole above a Start menu entry called Glasspole is worse than either
 *      name on its own.
 *   3. Everywhere else, Tadpole. On every engine, including glasspole.
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
#else
	cached = 0;
#endif
	return cached;
}

const char *ui_brand_name(void)
{
	return ui_brand_is_glasspole() ? "Glasspole" : "Tadpole";
}

/* WHAT IS ACTUALLY BEHIND THE PICTURE, for the About box.
 *
 * tadpole.sh exports TADPOLE_QEMU as the engine it launched, so this is the
 * binary the guest's code is really executing on rather than a guess. Anything
 * unrecognised is named as itself: a bring-your-own build is a legitimate
 * thing to be running and deserves a truthful answer, not "qemu-user".
 */
const char *ui_engine_name(void)
{
	const char *e = getenv("TADPOLE_QEMU"), *b;
	if (!e || !*e) {
#ifdef _WIN32
		/* Nothing else can be running: the viewer builds the glasspole
		 * command line itself here, and qemu-user has no Windows port. */
		return "Glasspole JIT";
#else
		/* tadpole-view run by hand rather than through tadpole.sh. */
		return "an ARM engine";
#endif
	}
	b = strrchr(e, '/');
#ifdef _WIN32
	{
		const char *bs = strrchr(e, '\\');
		if (bs && (!b || bs > b)) b = bs;
	}
#endif
	b = b ? b + 1 : e;
	if (strstr(b, "glasspole")) return "Glasspole JIT";
	if (strstr(b, "qemu"))      return "qemu-user";
	return b;
}

/* The palette is the house style, not a report on the engine — see the note
 * above the two of them. TADPOLE_THEME=green brings back the original. */
void ui_brand_apply(void)
{
	const char *t = getenv("TADPOLE_THEME");
	g_pal = (t && strcmp(t, "green") == 0) ? &pal_green : &pal_blue;
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
/* ANDROID GETS THE CHEAP CHROME AND NOBODY ELSE DOES. See the note above
 * `frost` in tadpole_ui.h for what it costs; the split is by platform rather
 * than by "is this a touchscreen" because it is about the GPU on the other end
 * of the readback, not about how the thing is held. A desktop that wants the
 * cheap look ticks the box, and every capture in shots/ is unchanged. */
#ifdef __ANDROID__
#define FROST_DEFAULT 0
#else
#define FROST_DEFAULT 1
#endif

static struct ui_settings g_cfg = {
	.gl               = 1,
	.gl_hle           = 1,
	.debug_level      = 1,      /* the device's own serial log, nothing more */
	.log_to_file      = 1,
	.update_check     = 1,
	.gl_dumpframe     = 0,
	.gl_dumptex       = 0,
	.rotate           = 0,
	/* ON. The panel's software is not all one way up — the LeapPad UI is
	 * portrait, titles are landscape — and until now the only way to see
	 * either of them upright was to notice and press Ctrl+R. */
	.auto_rotate      = 1,
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
	.fast_boot        = 1,
	.pad_on           = 1,
	.pad_size         = 100,
	.pad_opacity      = 70,
	.pad_left         = 1,
	.touch_ui         = 1,
	.frost            = FROST_DEFAULT,
	.games_dir        = "",
};
static enum ui_action g_action;
static char           g_action_path[PATHMAX];
/* A SECOND FIELD, for the one action that needs two. Installing micromods
 * takes the title AND which of its slots were ticked; everything else here
 * has always been "one path", and widening ui_take_action() for a single
 * caller would touch every one of them. */
static char           g_action_arg[256];
static char           g_status[128];
static int            g_running;
static int            g_mx, g_my;     /* last mouse position, logical */

/* menus */
static int  g_open_menu = -1;         /* index into MENUS, -1 = closed */
static int  g_hot_item  = -1;

/* modal */
enum modal_kind { M_NONE = 0, M_ABOUT, M_UPDATE, M_GFX, M_AUDIO, M_PAD, M_DEBUG, M_SYSTEM,
                  M_FILES, M_MSG, M_WIZARD, M_PROGRESS, M_GAMES, M_APPS,
                  M_MICROMODS };
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
/* HOW MANY ROWS THE BROWSER SHOWS, and it is no longer a constant: the row
 * height changes with the touch setting, so the count that fits changes with
 * it. Written down at draw time — the same trick g_gm_rows and g_mm_rows
 * already use — because the paging keys and the wheel clamp run outside the
 * draw and have no dialog rect to work it out from. */
#define FB_ROWS (g_fb_rows)
static int g_fb_rows = 11;

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

/* WRAP, DO NOT TRUNCATE — the truncated half is where the reason lives.
 *
 * This used to snprintf into PROG_COLS and drop the rest, which is fine for the
 * "==> extracting the root filesystem" lines it was written for and useless for
 * the one line that matters. A Windows user reported Didj setup failing with
 * nothing on screen but
 *
 *     C:/Users/tadpole/AppData/Local/Programs/Glasspole/build
 *
 * which is exactly 55 characters — PROG_COLS - 1. The real line was Python's
 * own "…python.exe: can't open file '…\tools\install-didj.py': [Errno 2] No
 * such file or directory", naming both the fault and the file, and every word
 * of that was thrown away here. The same applies to a certificate failure:
 * netssl.explain() writes a full sentence saying the certificate could not be
 * verified and whether a CA bundle was found, and the user sees a path prefix.
 *
 * Breaking on a space where there is one keeps prose readable; paths and URLs
 * have none, so a hard split is the fallback rather than the rule. */
/* FOLD TO ASCII ON THE WAY IN, because the bitmap font is ASCII plus a handful
 * of our own glyphs and has nothing for U+2014. The tools are written in the
 * same prose style as the comments here, so 45 of their message strings contain
 * an em-dash, and every one of them arrived as three bytes of garbage — on the
 * Windows console the same line reads "LF\Base <?> install the system firmware
 * first". Folding here fixes all fifteen tools at once, and any tool added
 * later, which hand-editing their strings would not.
 *
 * -> bytes written. Only punctuation that has an honest ASCII equivalent is
 * translated; anything else becomes '?' rather than silently vanishing, so a
 * message that loses something says that it did. */
static size_t ascii_fold(char *dst, size_t cap, const char *src)
{
	static const struct { const char *utf8, *ascii; } MAP[] = {
		{ "\xE2\x80\x94", "-" },   { "\xE2\x80\x93", "-" },     /* em/en dash */
		{ "\xE2\x80\xA6", "..." },                              /* ellipsis   */
		{ "\xE2\x80\x98", "'" },   { "\xE2\x80\x99", "'" },     /* quotes     */
		{ "\xE2\x80\x9C", "\"" },  { "\xE2\x80\x9D", "\"" },
		{ "\xC2\xA0",     " " },                                /* nbsp       */
		{ "\xE2\x86\x92", "->" },                               /* arrow      */
	};
	size_t o = 0, k;

	while (*src && o + 1 < cap) {
		const unsigned char c = (unsigned char)*src;
		if (c < 0x80) { dst[o++] = *src++; continue; }
		for (k = 0; k < sizeof(MAP) / sizeof(MAP[0]); k++) {
			size_t l = strlen(MAP[k].utf8);
			if (!strncmp(src, MAP[k].utf8, l)) {
				size_t a = strlen(MAP[k].ascii);
				if (o + a + 1 >= cap) { src += l; break; }
				memcpy(dst + o, MAP[k].ascii, a);
				o += a; src += l;
				break;
			}
		}
		if (k < sizeof(MAP) / sizeof(MAP[0])) continue;
		/* Unknown: consume the whole UTF-8 sequence so its continuation bytes
		 * do not each become their own '?'. */
		dst[o++] = '?';
		src++;
		while (((unsigned char)*src & 0xC0) == 0x80) src++;
	}
	dst[o] = 0;
	return o;
}

void ui_progress_line(const char *line)
{
	char buf[512];
	const char *p;
	size_t n;

	if (!line || !*line) return;

	n = ascii_fold(buf, sizeof(buf), line);
	while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
	if (!n) return;

	for (p = buf; *p; ) {
		size_t left = strlen(p), take = PROG_COLS - 1, i;
		if (left > take) {
			/* Last space that still fits, if one exists past the margin —
			 * a break at column 3 of a 55-wide box wastes the line. */
			size_t brk = 0;
			for (i = 0; i < take; i++)
				if (p[i] == ' ') brk = i;
			if (brk > take / 4) take = brk;
		} else {
			take = left;
		}
		memcpy(g_prog[g_prog_n % PROG_LINES], p, take);
		g_prog[g_prog_n % PROG_LINES][take] = 0;
		g_prog_n++;
		p += take;
		while (*p == ' ') p++;         /* the break itself is not content */
	}
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

/* ---- animation -----------------------------------------------------------
 *
 * Enough motion to say where a panel came from, and no more. The viewer
 * already redraws continuously — the progress marquee and the profile
 * cursor have always been time-driven — so this needs no timer, no
 * invalidation and no state machine: everything is a pure function of "how
 * long ago did this appear".
 *
 * WHAT DECIDES WHEN SOMETHING APPEARED. Not the twenty-odd places that
 * assign g_modal; the draw notices the value changed since last frame and
 * stamps the clock. Every path that opens a dialog — a menu item, the
 * wizard's Next, an alert raised by the frame pump — is covered without
 * touching any of them.
 *
 * ANIMATIONS ARE OFF FOR --ui-shot. A capture renders a single frame, so an
 * animated one would catch whatever fraction of the entrance that frame
 * happened to land on, and every regression capture would differ from the
 * last for no reason. ui_anim_disable() pins everything settled.
 */
static int    g_anim_off;
static Uint32 g_modal_at, g_menu_at, g_wiz_at;

void ui_anim_disable(void) { g_anim_off = 1; }

/* Defined here rather than near the launcher so it sits with the other debug
 * hooks; the statics it reads are declared further down. */
void ui_debug_apps(int *n, int *top, int *sel, int *rows);

/* 0 at `since`, 1 once `dur` has passed, eased so it decelerates into place. */
static float anim_t(Uint32 since, Uint32 dur)
{
	Uint32 now;
	float t, inv;

	if (g_anim_off || !since || !dur) return 1.0f;
	now = SDL_GetTicks();
	if (now <= since) return 0.0f;
	if (now - since >= dur) return 1.0f;
	t = (float)(now - since) / (float)dur;
	inv = 1.0f - t;
	return 1.0f - inv * inv * inv;        /* ease-out cubic */
}

/* Notices a changed value and restamps the clock. */
static void anim_watch(int cur, int *seen, Uint32 *stamp)
{
	if (cur != *seen) { *seen = cur; *stamp = SDL_GetTicks(); }
}

/* ---- primitives ---------------------------------------------------------- */

/* EVERYTHING BELOW IS DRAWN THROUGH THIS. A fade has to reach the panel body,
 * its rim, its shadow, the chips inside it and the text on them, or the parts
 * that ignored it pop in while the rest fades and the whole thing looks
 * broken. One multiplier applied in the four primitives covers all of it. */
static Uint8 g_alpha = 255;

static Uint8 amul(Uint8 a)
{
	return g_alpha == 255 ? a : (Uint8)((int)a * g_alpha / 255);
}

static void fill(SDL_Renderer *r, int x, int y, int w, int h, unsigned col)
{
	SDL_Rect rc = { x, y, w, h };
	struct rgb c = unpack(col);
	SDL_SetRenderDrawBlendMode(r, g_alpha == 255 ? SDL_BLENDMODE_NONE
	                                             : SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, amul(255));
	SDL_RenderFillRect(r, &rc);
}

/* ---- rounded, softly antialiased shapes ----------------------------------
 *
 * The whole chrome used to be flat fills and 1px light/dark bevels — sharp
 * rectangles, on purpose, because that is what a pixel-art interface is. This
 * keeps that palette and that font but trades the hard rectangle for a
 * rounded, translucent one: every corner below is a small polygon fan rather
 * than a rect, with a one-pixel alpha feather doing the antialiasing SDL's
 * flat SDL_RenderFillRect never had to worry about.
 *
 * RR_SEG points per 90-degree corner, computed once as a unit quarter-circle
 * rather than called through cosf/sinf per frame — this file has never linked
 * libm and a rounded rectangle is not a reason to start on every platform
 * this cross-builds for. */
#define RR_SEG      8
#define RR_MAX_PTS  (4 * (RR_SEG + 1))
#define RR_FEATHER  1.0f

static const float RR_CX[RR_SEG + 1] = {
	1.000000f, 0.980785f, 0.923880f, 0.831470f, 0.707107f,
	0.555570f, 0.382683f, 0.195090f, 0.000000f
};
static const float RR_CY[RR_SEG + 1] = {
	0.000000f, 0.195090f, 0.382683f, 0.555570f, 0.707107f,
	0.831470f, 0.923880f, 0.980785f, 1.000000f
};

/* Traces a w x h rect clockwise from its top edge, rounding each corner by
 * its own radius — 0 leaves that corner square. Used both for a uniformly
 * rounded chip and for the dialog header strip, which rounds only the top
 * two so it sits flush with the panel content below it. Writes at most
 * RR_MAX_PTS points into `pts` and returns how many it used. */
static int rr_build(SDL_FPoint *pts, float x, float y, float w, float h,
                    float rtl, float rtr, float rbr, float rbl)
{
	int n = 0, k;

	if (rtr > 0) {
		float cx = x + w - rtr, cy = y + rtr;
		for (k = 0; k <= RR_SEG; k++) {
			pts[n].x = cx + rtr * RR_CY[k]; pts[n].y = cy - rtr * RR_CX[k]; n++;
		}
	} else { pts[n].x = x + w; pts[n].y = y; n++; }

	if (rbr > 0) {
		float cx = x + w - rbr, cy = y + h - rbr;
		for (k = 0; k <= RR_SEG; k++) {
			pts[n].x = cx + rbr * RR_CX[k]; pts[n].y = cy + rbr * RR_CY[k]; n++;
		}
	} else { pts[n].x = x + w; pts[n].y = y + h; n++; }

	if (rbl > 0) {
		float cx = x + rbl, cy = y + h - rbl;
		for (k = 0; k <= RR_SEG; k++) {
			pts[n].x = cx - rbl * RR_CY[k]; pts[n].y = cy + rbl * RR_CX[k]; n++;
		}
	} else { pts[n].x = x; pts[n].y = y + h; n++; }

	if (rtl > 0) {
		float cx = x + rtl, cy = y + rtl;
		for (k = 0; k <= RR_SEG; k++) {
			pts[n].x = cx - rtl * RR_CX[k]; pts[n].y = cy - rtl * RR_CY[k]; n++;
		}
	} else { pts[n].x = x; pts[n].y = y; n++; }

	return n;
}

/* The workhorse: a filled rounded rect, flat-coloured when `tex` is NULL or
 * sampling `tex` (stretched to x,y,w,h and modulated by the vertex colour)
 * when it is not — that second form is the frosted-glass fill, see
 * glass_capture() below. The colour runs top-to-bottom between two
 * colour/alpha pairs, which is what lets one call draw a flat chip, a glass
 * body with a gentle vertical falloff, or a highlight that fades to nothing.
 *
 * Either way the edge gets a genuine antialiased feather: a second, larger
 * outline fades to zero alpha, and a triangle strip connects it to the solid
 * one. */
static void rr_geom(SDL_Renderer *r, SDL_Texture *tex, const SDL_FRect *uv,
                    float x, float y, float w, float h, float rtl, float rtr,
                    float rbr, float rbl, unsigned ctop, Uint8 atop,
                    unsigned cbot, Uint8 abot)
{
	SDL_FPoint in[RR_MAX_PTS], out[RR_MAX_PTS];
	SDL_Vertex verts[1 + RR_MAX_PTS * 2];
	int idx[RR_MAX_PTS * 9];
	int n, i, ni = 0;
	struct rgb ct = unpack(ctop), cb = unpack(cbot);
	/* Which rectangle of POSITION space maps to the texture's 0..1 — the
	 * shape's own bounds unless the caller says otherwise. The glass needs
	 * otherwise: its texture is a capture of the whole screen, and the panel
	 * has to sample its own window onto it rather than stretching the entire
	 * screen across itself. */
	float ux = uv ? uv->x : x, uy = uv ? uv->y : y;
	float uw = uv ? uv->w : w, uh = uv ? uv->h : h;

	if (w <= 0 || h <= 0 || (atop == 0 && abot == 0)) return;
	if (uw <= 0) uw = 1;
	if (uh <= 0) uh = 1;
	memset(verts, 0, sizeof(verts));

	n = rr_build(in, x, y, w, h, rtl, rtr, rbr, rbl);
	rr_build(out, x - RR_FEATHER, y - RR_FEATHER, w + 2 * RR_FEATHER, h + 2 * RR_FEATHER,
	        rtl > 0 ? rtl + RR_FEATHER : 0, rtr > 0 ? rtr + RR_FEATHER : 0,
	        rbr > 0 ? rbr + RR_FEATHER : 0, rbl > 0 ? rbl + RR_FEATHER : 0);

#define RR_MIX(dst, py, a_scale) do {                                        \
		float t_ = ((py) - y) / h;                                           \
		if (t_ < 0) t_ = 0; else if (t_ > 1) t_ = 1;                         \
		(dst).r = (Uint8)(ct.r + (cb.r - ct.r) * t_);                        \
		(dst).g = (Uint8)(ct.g + (cb.g - ct.g) * t_);                        \
		(dst).b = (Uint8)(ct.b + (cb.b - ct.b) * t_);                        \
		(dst).a = amul((Uint8)((atop + (abot - atop) * t_) * (a_scale)));    \
	} while (0)

	verts[0].position.x = x + w / 2; verts[0].position.y = y + h / 2;
	RR_MIX(verts[0].color, y + h / 2, 1);
	verts[0].tex_coord.x = (x + w / 2 - ux) / uw;
	verts[0].tex_coord.y = (y + h / 2 - uy) / uh;
	for (i = 0; i < n; i++) {
		verts[1 + i].position = in[i];
		RR_MIX(verts[1 + i].color, in[i].y, 1);
		verts[1 + i].tex_coord.x = (in[i].x - ux) / uw;
		verts[1 + i].tex_coord.y = (in[i].y - uy) / uh;
		verts[1 + n + i].position = out[i];
		RR_MIX(verts[1 + n + i].color, out[i].y, 0);
		verts[1 + n + i].tex_coord.x = (out[i].x - ux) / uw;
		verts[1 + n + i].tex_coord.y = (out[i].y - uy) / uh;
	}
#undef RR_MIX
	for (i = 0; i < n; i++) {
		int j = (i + 1) % n;
		idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + j;
	}
	for (i = 0; i < n; i++) {
		int j = (i + 1) % n;
		idx[ni++] = 1 + i;     idx[ni++] = 1 + n + i; idx[ni++] = 1 + j;
		idx[ni++] = 1 + j;     idx[ni++] = 1 + n + i; idx[ni++] = 1 + n + j;
	}
	/* Untextured geometry takes the RENDERER's blend mode; textured geometry
	 * takes the TEXTURE's. Setting this one is therefore right for the flat
	 * shapes and simply ignored for the glass, which the caller has already
	 * put into whichever mode it wants. */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(r, tex, verts, 1 + 2 * n, idx, ni);
}

static void rr_fill_ex(SDL_Renderer *r, SDL_Texture *tex, float x, float y,
                       float w, float h, float rtl, float rtr, float rbr,
                       float rbl, unsigned col, Uint8 alpha)
{
	rr_geom(r, tex, NULL, x, y, w, h, rtl, rtr, rbr, rbl, col, alpha, col, alpha);
}

/* A thin rounded outline: antialiased on the outer edge, a hard inner edge
 * against whatever fill it sits on top of (always the same tone at this
 * thinness, so the seam is not visible in practice).
 *
 * The colour runs top-to-bottom like rr_geom's. A rim of ONE brightness all
 * the way round is what a drawn rectangle looks like; a rim that is bright
 * along the top and fades towards the bottom is what a lit sheet looks like,
 * and it is most of the difference between "rounded box" and "glass".
 *
 * TOP AND BOTTOM RADII ARE SEPARATE because the dropdown's are: it hangs off
 * the menu bar with square top corners, and a rim that rounds where the body
 * does not is the one place the two shapes visibly disagree. */
static void rr_stroke_grad(SDL_Renderer *r, float x, float y, float w, float h,
                           float rtop, float rbot, unsigned ctop, Uint8 atop,
                           unsigned cbot, Uint8 abot, float thick)
{
	SDL_FPoint outer[RR_MAX_PTS], inner[RR_MAX_PTS], feather[RR_MAX_PTS];
	SDL_Vertex verts[RR_MAX_PTS * 3];
	int idx[RR_MAX_PTS * 12];
	int n, i, ni = 0, nv = 0;
	struct rgb ct = unpack(ctop), cb = unpack(cbot);
	float itop, ibot, ftop, fbot;

	if (w <= 0 || h <= 0 || (atop == 0 && abot == 0)) return;
	if (thick * 2 >= w) thick = w / 2 - 0.5f;
	if (thick * 2 >= h) thick = h / 2 - 0.5f;
	if (thick < 0.5f) thick = 0.5f;
	itop = rtop - thick; if (itop < 0) itop = 0;
	ibot = rbot - thick; if (ibot < 0) ibot = 0;
	/* A square corner stays square as the outline grows outward — feathering
	 * it into a rounded one puts a soft notch on a hard corner. */
	ftop = rtop > 0 ? rtop + RR_FEATHER : 0;
	fbot = rbot > 0 ? rbot + RR_FEATHER : 0;
	memset(verts, 0, sizeof(verts));

	n = rr_build(outer, x, y, w, h, rtop, rtop, rbot, rbot);
	rr_build(inner, x + thick, y + thick, w - 2 * thick, h - 2 * thick,
	        itop, itop, ibot, ibot);
	rr_build(feather, x - RR_FEATHER, y - RR_FEATHER, w + 2 * RR_FEATHER,
	        h + 2 * RR_FEATHER, ftop, ftop, fbot, fbot);

#define RS_MIX(dst, py, a_scale) do {                                        \
		float t_ = ((py) - y) / h;                                           \
		if (t_ < 0) t_ = 0; else if (t_ > 1) t_ = 1;                         \
		(dst).r = (Uint8)(ct.r + (cb.r - ct.r) * t_);                        \
		(dst).g = (Uint8)(ct.g + (cb.g - ct.g) * t_);                        \
		(dst).b = (Uint8)(ct.b + (cb.b - ct.b) * t_);                        \
		(dst).a = amul((Uint8)((atop + (abot - atop) * t_) * (a_scale)));    \
	} while (0)

	for (i = 0; i < n; i++) {
		verts[nv].position = feather[i]; RS_MIX(verts[nv].color, feather[i].y, 0); nv++;
		verts[nv].position = outer[i];   RS_MIX(verts[nv].color, outer[i].y, 1);   nv++;
		verts[nv].position = inner[i];   RS_MIX(verts[nv].color, inner[i].y, 1);   nv++;
	}
#undef RS_MIX
	for (i = 0; i < n; i++) {
		int j = (i + 1) % n;
		int f0 = 3*i, o0 = 3*i+1, in0 = 3*i+2, f1 = 3*j, o1 = 3*j+1, in1 = 3*j+2;
		idx[ni++] = f0;  idx[ni++] = o0;  idx[ni++] = f1;
		idx[ni++] = f1;  idx[ni++] = o0;  idx[ni++] = o1;
		idx[ni++] = o0;  idx[ni++] = in0; idx[ni++] = o1;
		idx[ni++] = o1;  idx[ni++] = in0; idx[ni++] = in1;
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(r, NULL, verts, nv, idx, ni);
}

/* A soft ELLIPTICAL falloff: one triangle fan with the alpha in the vertices,
 * so the GPU interpolates between the centre and the rim. Both directions are
 * useful and they are the same shape:
 *
 *   a_mid > a_rim   a glow — light pooling outward from a point
 *   a_mid < a_rim   a vignette — the edges of the picture falling away
 *
 * ITS OWN, FINER CIRCLE TABLE. The rounded rects get eight points per corner,
 * which is ample for a 13px button, and reusing that here gave the vignette
 * 36 rim points across the whole window — the iso-alpha contours came out as
 * a visible polygon and the flat chords read as faint streaks across the
 * backdrop. Sixty-four is smooth at that size. Still no libm: the quarter
 * turn is a table, as RR_CX/RR_CY is.
 *
 * SEPARATE X AND Y RADII because the window is not square. A circular
 * vignette on a 480x272 panel darkens the top and bottom edges long before
 * it touches the sides, which is exactly the horizontal banding it was
 * supposed to avoid; an ellipse of the frame's own proportions reaches all
 * four corners at once.
 *
 * An earlier glow stacked a dozen concentric circles at a low alpha instead.
 * Do not go back to it: every circle contributes its own hard edge and the
 * result is a set of visible rings — a bullseye, not a glow. */
#define RAD_SEG 16                      /* per quadrant; 64 round the ellipse */
static const float RAD_CX[RAD_SEG + 1] = {
	1.000000f, 0.995185f, 0.980785f, 0.956940f, 0.923880f,
	0.881921f, 0.831470f, 0.773010f, 0.707107f, 0.634393f,
	0.555570f, 0.471397f, 0.382683f, 0.290285f, 0.195090f,
	0.098017f, 0.000000f
};
static const float RAD_CY[RAD_SEG + 1] = {
	0.000000f, 0.098017f, 0.195090f, 0.290285f, 0.382683f,
	0.471397f, 0.555570f, 0.634393f, 0.707107f, 0.773010f,
	0.831470f, 0.881921f, 0.923880f, 0.956940f, 0.980785f,
	0.995185f, 1.000000f
};

static void radial(SDL_Renderer *r, int cx, int cy, float rx, float ry,
                   unsigned col, Uint8 a_mid, Uint8 a_rim)
{
	SDL_Vertex v[1 + 4 * RAD_SEG];
	int idx[4 * RAD_SEG * 3];
	int n = 4 * RAD_SEG, i, ni = 0;
	struct rgb c = unpack(col);

	if (rx <= 0 || ry <= 0 || (a_mid == 0 && a_rim == 0)) return;
	memset(v, 0, sizeof(v));

	v[0].position.x = (float)cx; v[0].position.y = (float)cy;
	v[0].color.r = c.r; v[0].color.g = c.g; v[0].color.b = c.b;
	v[0].color.a = amul(a_mid);

	/* One quarter-turn table, four quadrants, by the rotation identities —
	 * cos(90+f) = -sin f, sin(90+f) = cos f, and so on round. Written out
	 * rather than folded into sign tricks because a mirrored quadrant that
	 * walks the table the wrong way makes a lopsided ellipse, and that is
	 * a tedious thing to see and a worse one to debug. */
	for (i = 0; i < n; i++) {
		int q = i / RAD_SEG, j = i % RAD_SEG;
		float ux, uy;
		switch (q) {
		default:
		case 0: ux =  RAD_CX[j]; uy =  RAD_CY[j]; break;
		case 1: ux = -RAD_CY[j]; uy =  RAD_CX[j]; break;
		case 2: ux = -RAD_CX[j]; uy = -RAD_CY[j]; break;
		case 3: ux =  RAD_CY[j]; uy = -RAD_CX[j]; break;
		}
		v[1 + i].position.x = cx + rx * ux;
		v[1 + i].position.y = cy + ry * uy;
		v[1 + i].color.r = c.r; v[1 + i].color.g = c.g; v[1 + i].color.b = c.b;
		v[1 + i].color.a = amul(a_rim);
	}
	for (i = 0; i < n; i++) {
		idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + ((i + 1) % n);
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(r, NULL, v, 1 + n, idx, ni);
}

static void glow(SDL_Renderer *r, int cx, int cy, int radius, unsigned col,
                 Uint8 alpha)
{
	radial(r, cx, cy, (float)radius, (float)radius, col, alpha, 0);
}

/* How much to round a rect this small: enough to read as "rounded" on a
 * button or a list row without eating so much of a small chip that it turns
 * into a lozenge. */
static float rr_radius(int w, int h)
{
	float rad = 4.0f;
	float m = (float)(w < h ? w : h) / 2.0f;
	return rad < m ? rad : m;
}

static float rr_radius_panel(int w, int h)
{
	float rad = 10.0f;
	float m = (float)(w < h ? w : h) / 2.0f;
	return rad < m ? rad : m;
}

/* Flat rounded fill — the rounded replacement for a bare `fill()` call where
 * a highlight or a box benefits from matching the rest of the chrome. */
static void rfill(SDL_Renderer *r, int x, int y, int w, int h, unsigned col,
                  Uint8 alpha)
{
	float rad = rr_radius(w, h);
	rr_fill_ex(r, NULL, (float)x, (float)y, (float)w, (float)h,
	          rad, rad, rad, rad, col, alpha);
}

/* A button or a sunken box. This is what every `fill()` + `bevel()` pair in
 * the dialogs collapsed into — same two arguments bevel() ever needed (the
 * rect and which way it faces), now drawing the body and the edge together.
 *
 * `raised` still means what it did, but it is no longer a choice of which two
 * sides get the light. Both states carry a vertical gradient and a graded rim;
 * they differ in DIRECTION, which is what the old light-top-left/dark-bottom-
 * right bevel was really encoding:
 *
 *   raised  body brightens upward, rim bright along the top   — a button
 *   sunken  body darkens upward, rim dark along the top       — a well
 *
 * A flat fill with a single-tone outline was the first version and it read as
 * a sticker: correct shape, no light on it. */
static void chip(SDL_Renderer *r, int x, int y, int w, int h, unsigned col,
                 int raised)
{
	float rad = rr_radius(w, h);
	float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;

	if (raised) {
		rr_geom(r, NULL, NULL, fx, fy, fw, fh, rad, rad, rad, rad,
		       col, 176, col, 202);
		rr_stroke_grad(r, fx, fy, fw, fh, rad, rad,
		              C_EDGE_LT, 185, C_EDGE_LT, 96, 1.0f);
	} else {
		rr_geom(r, NULL, NULL, fx, fy, fw, fh, rad, rad, rad, rad,
		       col, 208, col, 176);
		rr_stroke_grad(r, fx, fy, fw, fh, rad, rad,
		              C_EDGE_DK, 165, C_EDGE_LT, 78, 1.0f);
	}
}

/* ---- frosted glass --------------------------------------------------------
 *
 * TADPOLE_FROST=0/1 OVERRIDES THE SETTING, for the same reason
 * TADPOLE_TOUCH_UI does: the before and after of this cannot be captured
 * otherwise, because reaching the tick box means opening a dialog and the
 * dialog is the thing being measured.
 *
 * Captures whatever is already drawn and hands back a small, softly blurred
 * copy of it: a few passes of "redraw at half size with linear filtering",
 * which is a cheap, GPU-only stand-in for a real gaussian blur and is plenty
 * convincing at this UI's scale. NULL on any failure (no render-target
 * support, an unreadable target, mid-resize, ...) — callers fall back to a
 * flat tint rather than depending on it.
 *
 * THE WHOLE TARGET, NOT THE PANEL'S RECTANGLE, AND THAT IS DELIBERATE.
 * Everything this file draws is in the renderer's LOGICAL space, but
 * SDL_RenderReadPixels does NOT interpret its rect that way: measured on
 * SDL 2.32, a logical size of 100x100 on a 200x200 window (scale 2) and a
 * read of rect (0,0,50,50) returns a 50x50 block of OUTPUT pixels — the rect
 * goes through unscaled. So a logical rect both names the wrong region and
 * disagrees with the buffer you sized for it: asking for a 340x232 panel at
 * 2x filled a quarter of the allocation and left the rest uninitialised,
 * which showed up on screen as a hard-edged bright rectangle across half the
 * panel.
 *
 * Passing NULL sidesteps the entire question — it means "the whole render
 * target" and nothing else — and the panels then pick their own region out of
 * it with texture coordinates. One capture also does for every panel in the
 * frame.
 */
static int m_frost(void)
{
	const char *e = getenv("TADPOLE_FROST");
	if (e) return atoi(e) != 0;
	return g_cfg.frost;
}

static SDL_Texture *glass_blur(SDL_Renderer *r)
{
	unsigned char *px;
	SDL_Texture *cur, *prev_target;
	int w = 0, h = 0, steps;

	SDL_GetRendererOutputSize(r, &w, &h);
	if (w <= 0 || h <= 0) return NULL;

	px = malloc((size_t)w * h * 4);
	if (!px) return NULL;
	if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) != 0) {
		free(px);
		return NULL;
	}
	cur = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
	if (!cur) { free(px); return NULL; }
	SDL_UpdateTexture(cur, NULL, px, w * 4);
	free(px);

	/* Restore whatever was being drawn into rather than assuming the window:
	 * this runs in the middle of somebody else's frame. */
	prev_target = SDL_GetRenderTarget(r);
	/* HOW BLURRED, AND WHY THE STEP COUNT IS WHAT SETS IT.
	 *
	 * Each pass halves the capture with linear filtering, so the radius
	 * doubles per step and the result is 1/2^steps of the screen whatever the
	 * window size — the strength is scale-invariant as long as the chain runs
	 * to completion. That is why the floor below is as low as it is: it exists
	 * only to stop a degenerate texture, not to end the chain, and a floor
	 * high enough to end it early would quietly make the blur weaker on small
	 * windows than on large ones.
	 *
	 * Four steps looked like a smeared photograph of the backdrop — the logo
	 * still read as a logo. Seven is a wash you take for frosted glass rather
	 * than for a picture of what is behind it. */
#define GLASS_STEPS 7
	for (steps = 0; steps < GLASS_STEPS && w > 4 && h > 4; steps++) {
		int nw = w / 2, nh = h / 2;
		SDL_Texture *next = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
		                                      SDL_TEXTUREACCESS_TARGET, nw, nh);
		if (!next) break;
		SDL_SetTextureScaleMode(cur, SDL_ScaleModeLinear);
		SDL_SetTextureBlendMode(cur, SDL_BLENDMODE_NONE);
		if (SDL_SetRenderTarget(r, next) != 0) {
			SDL_DestroyTexture(next);
			break;
		}
		SDL_RenderCopy(r, cur, NULL, NULL);
		SDL_SetRenderTarget(r, prev_target);
		SDL_DestroyTexture(cur);
		cur = next;
		w = nw; h = nh;
	}
	SDL_SetTextureScaleMode(cur, SDL_ScaleModeLinear);
	return cur;
}

/* ONE CAPTURE, REUSED FOR A WHILE. A readback is a GPU-to-CPU stall and the
 * downsample chain is four render-target switches; doing that every frame for
 * a dialog that is sitting still is pure waste. What is behind a modal barely
 * moves — the emulator is paused while one is up (see ui_modal) — so the
 * capture is kept for a tenth of a second.
 *
 * Always taken BEFORE any panel is drawn in the frame, so it never contains
 * last frame's glass reflected back into this one.
 */
static SDL_Texture *g_glass;
static Uint32       g_glass_at;

static void glass_free(void)
{
	if (g_glass) SDL_DestroyTexture(g_glass);
	g_glass = NULL;
	g_glass_at = 0;
}

static SDL_Texture *glass_capture(SDL_Renderer *r)
{
	Uint32 now = SDL_GetTicks();

	/* OFF MEANS THE READBACK NEVER HAPPENS. Not "capture it and draw it at
	 * zero alpha" — the expensive half of the frost is the capture, and a
	 * setting that still paid for it would save nothing. The cached texture
	 * goes too rather than sitting in VRAM: at a tablet's output size it is
	 * several megabytes of a picture nothing will ask for again. */
	if (!m_frost()) {
		glass_free();
		return NULL;
	}

	if (g_glass && now - g_glass_at < 100)
		return g_glass;

	glass_free();
	g_glass = glass_blur(r);
	g_glass_at = now;
	return g_glass;
}

/* The logical rectangle the captured texture covers, which is what the panels
 * map their own position onto. The capture is the whole render target, so in
 * logical terms that is the whole logical screen. */
static SDL_FRect glass_uv(SDL_Renderer *r)
{
	SDL_FRect uv = { 0, 0, 0, 0 };
	int lw = 0, lh = 0;

	SDL_RenderGetLogicalSize(r, &lw, &lh);
	if (lw <= 0 || lh <= 0)          /* no logical size set: output IS logical */
		SDL_GetRendererOutputSize(r, &lw, &lh);
	uv.w = (float)lw;
	uv.h = (float)lh;
	return uv;
}

/* A dialog or dropdown's outer frame: a soft shadow, a tinted body, the live
 * blurred backdrop bleeding through it, and a bright rounded rim.
 *
 * WHY THE BLUR IS *ADDED* RATHER THAN USED AS THE BODY. The obvious build —
 * fill the panel with the blurred backdrop and tint it — produces a black
 * panel here, and it is worth writing down why, because it looks like a bug
 * in the blur and is not. Frosted glass in a desktop UI reads as glass
 * because there is a bright, busy desktop behind it. Behind this one is
 * C_VOID: an almost-black field with a small logo on it. Blurring
 * near-black gives near-black, and compositing that as the body can only
 * ever darken what is underneath, so the panel came out looking like a hole
 * cut in the window.
 *
 * So the body is a theme-coloured gradient — the floor the panel can never
 * fall below — and the backdrop is ADDED on top of it. Additive is the whole
 * trick: darkness behind contributes nothing and the tint survives, while
 * anything bright behind (a running game, the logo's glow) bleeds through as
 * light. It is also the physically sensible one: glass adds the light that
 * passes through it to the light it reflects.
 *
 * AND ADDITIVE IS ALSO WHY THE FIRST VERSION WAS UNREADABLE OVER A GAME.
 * Added light is unbounded: the panel's brightness is the backdrop's, times a
 * gain, plus a floor. Tuned against the idle void — near-black, so the added
 * term is near-zero — a body at alpha 202 and a gain of 150 looked right.
 * Over a title's box art the same numbers put the top of the File menu at
 * 1.0:1 against its own disabled text, measured over a white backdrop with
 * TADPOLE_SHOT_BG: the greyed items were not dim, they were GONE.
 *
 * The fix is both halves of that expression, because neither alone is enough.
 * The body went nearly opaque, so the panel has a dark floor no backdrop can
 * lift; and the gain came down by well over half, so bright content bleeds
 * through as a glow rather than erasing what it is behind. The frost is
 * quieter over the void than it was and that is the price — legibility over
 * the bright case is worth more than glass over the dark one, and the dark
 * one never needed the help.
 *
 * `rtop` rounds the top corners independently of the bottom ones: a dialog
 * floats and rounds all four, a dropdown hangs off the menu bar and wants its
 * top edge welded to it.
 *
 * `blur` may be NULL, and is owned by glass_capture() — never destroyed here.
 */
static void panel_glass(SDL_Renderer *r, int x, int y, int w, int h,
                        SDL_Texture *blur, float rtop)
{
	float rad = rr_radius_panel(w, h);
	/* Asked directly rather than inferred from `blur != NULL`: a capture can
	 * also come back NULL from a renderer without render-target support or
	 * from a mid-resize frame, and those want the full shadow they have always
	 * had, not the cheap one. This is a setting, not a failure. */
	int frosted = m_frost();
	int i;

	if (rtop > rad) rtop = rad;
	if (rtop < 0) rtop = 0;

	/* Widening rings rather than one hard offset rectangle: a shadow with an
	 * edge is a second border, not a shadow. Six thin ones instead of three
	 * fat ones — same total darkness, spread over twice the distance, and
	 * with each step small enough that the banding between rings disappears.
	 * The offset grows faster than the spread so the light stays overhead.
	 *
	 * THE SHADOW COMES DOWN WITH THE FROST, because it is the other half of
	 * what a panel costs and it is not a rounded corner. Each ring covers the
	 * whole panel and then some, so six of them is six full-panel passes of
	 * alpha blending before a single pixel of the panel itself is drawn — on
	 * a 250x200 dialog at a tablet's output scale that is a couple of million
	 * blended pixels per frame, spent on something nobody is looking at.
	 *
	 * Two rings at the same outer reach rather than a tighter shadow: dropping
	 * the spread would have been the cheaper edit and the wrong one, since a
	 * shadow that hugs the panel is the "second border" the six were written
	 * to avoid. The alpha doubles to make up part of the four rings that are
	 * gone; the rest stays lost, and a slightly lighter shadow is the price. */
	for (i = 6; i >= 1; i -= frosted ? 1 : 3) {
		float grow = (float)i * 2.2f;
		float gtop = rtop > 0 ? rtop + grow : 0;
		rr_fill_ex(r, NULL, x - grow, y - grow + 1.0f + grow * 0.55f,
		          w + 2 * grow, h + 2 * grow,
		          gtop, gtop, rad + grow, rad + grow,
		          C_SHADOW, (Uint8)(13 * (7 - i) * (frosted ? 1 : 2)));
	}

	rr_geom(r, NULL, NULL, (float)x, (float)y, (float)w, (float)h,
	       rtop, rtop, rad, rad, C_PANEL_HI, 246, C_PANEL, 252);

	if (blur) {
		SDL_FRect uv = glass_uv(r);
		SDL_SetTextureBlendMode(blur, SDL_BLENDMODE_ADD);
		SDL_SetTextureAlphaMod(blur, 255);
		rr_geom(r, blur, &uv, (float)x, (float)y, (float)w, (float)h,
		       rtop, rtop, rad, rad, 0x9FB4A8U, 34, 0x6F8478U, 22);
	}

	/* The light catching the top of the sheet, fading out before halfway. */
	rr_geom(r, NULL, NULL, (float)x + 1, (float)y + 1, (float)w - 2, (float)h * 0.45f,
	       rtop > 1 ? rtop - 1 : 0, rtop > 1 ? rtop - 1 : 0, 0, 0,
	       C_EDGE_LT, 46, C_EDGE_LT, 0);

	/* The rim, brightest along the top and falling to almost nothing at the
	 * bottom. A rim of one brightness all the way round is a drawn outline;
	 * this is an edge with a light above it, and it is the single change that
	 * did most for making the panels read as sheets of something. */
	rr_stroke_grad(r, (float)x, (float)y, (float)w, (float)h, rtop, rad,
	              C_EDGE_LT, 190, C_EDGE_LT, 70, 1.2f);
}

static void panel(SDL_Renderer *r, int x, int y, int w, int h, float rtop)
{
	panel_glass(r, x, y, w, h, glass_capture(r), rtop);
}

/* ---- touch metrics -------------------------------------------------------
 *
 * See the note above ui_bar_h() in tadpole_ui.h for why only the heights move
 * and the font does not. The pairs are written out rather than derived from
 * one multiplier because they are not one multiplier: the bar gains 9 and a
 * settings row gains 10, since a row has a tick box beside its label and wants
 * a little more air than a title in a bar does. A single factor would have
 * been tidier to read and worse to look at.
 *
 * 23 AND 24 ARE NOT ARBITRARY. A fingertip is about 9mm; on the panel-sized
 * logical space of a typical fullscreen touch device that lands between 20 and
 * 26 logical pixels, and below about 20 the rows start being missed.
 */
/* TADPOLE_TOUCH_UI OVERRIDES THE SETTING, so either look can be captured
 * without going through a dialog to reach it — which is how the before and
 * after of this change were compared at all. */
static int m_touch(void)
{
	const char *e = getenv("TADPOLE_TOUCH_UI");
	if (e) return atoi(e) != 0;
	return g_cfg.touch_ui;
}

int ui_bar_h(void)      { return m_touch() ? 22 : 13; }
int ui_row_h(void)      { return m_touch() ? 24 : 14; }
int ui_menu_row_h(void) { return m_touch() ? 23 : 12; }
int ui_btn_h(void)      { return m_touch() ? 22 : 13; }
int ui_list_row_h(void) { return m_touch() ? 22 : 11; }
int ui_touch_ui(void)   { return m_touch(); }

/* A settings row, and where the first one starts: below the dialog's title,
 * which it has to clear whatever the row height turned out to be. Up here with
 * the other metrics because dlg_body_h() sizes a dialog from them, and that is
 * a long way above the rows themselves. */
#define ROW_H (ui_row_h())
static int row_top(void) { return ui_touch_ui() ? 26 : 22; }

/* Where the 7px font sits inside a box of height h, so a label is centred
 * whatever the box turned out to be. Every one of these used to be written by
 * hand as "+ 3", which is right at 13 and wrong at 22. */
static int text_dy(int h) { return (h - FONT_H) / 2; }

static int text_w(const char *s) { return (int)strlen(s) * GLYPH_ADV; }

static void text(SDL_Renderer *r, int x, int y, const char *s, unsigned col)
{
	struct rgb c = unpack(col);
	SDL_SetTextureColorMod(g_font, c.r, c.g, c.b);
	SDL_SetTextureAlphaMod(g_font, g_alpha);
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

/* ---- hover, and the fact that a touchscreen has none ---------------------
 *
 * Some sixty highlights in this file ask inside(g_mx, g_my, ...). Under a
 * mouse that means "the pointer is over this", and it is continuously true or
 * continuously false. Under a finger it means nothing of the kind: g_mx and
 * g_my are only written when something is being touched, so the moment the
 * finger lifts, the last place it was stays behind — and whatever is under
 * that point keeps its highlight until the next tap moves it somewhere else.
 *
 * A row lit up that nobody is pointing at is worse than no highlight at all.
 * It is a claim about where the next press will land, and it is wrong.
 *
 * The fix is one line rather than sixty: when a TOUCH lifts, the pointer is
 * moved off the screen entirely, because that is where it now is. Every hover
 * test then answers no on its own, without knowing anything about touch. While
 * the finger is down the highlight follows it, which is press feedback and is
 * the one thing a touchscreen genuinely wants from these.
 *
 * A real mouse button release does not do this: the mouse is still there.
 */
#define POINTER_GONE (-30000)

static void pointer_left(void) { g_mx = g_my = POINTER_GONE; }

/* ---- lent to the overlay controls ----------------------------------------
 *
 * viewer/tadpole_pad.c draws the on-screen D-pad and Home button. They are
 * over the guest's picture rather than up in the bar, but they are the same
 * interface and have to be made of the same material — so they borrow the
 * shapes rather than growing a second, subtly different idea of what a corner
 * radius, a rim or a glow is. That divergence is the whole reason these are
 * exported instead of copied: two sets of rounded-rect routines drift, and the
 * drift shows up as one control that does not look like the others.
 *
 * The palette goes with them for the same reason. TADPOLE_THEME=green has to
 * repaint the D-pad too, and it cannot if the D-pad has its own colours.
 */
void ui_rr_fill(SDL_Renderer *r, float x, float y, float w, float h,
                float radius, unsigned col, Uint8 alpha)
{
	rr_fill_ex(r, NULL, x, y, w, h, radius, radius, radius, radius, col, alpha);
}

void ui_rr_grad(SDL_Renderer *r, float x, float y, float w, float h,
                float radius, unsigned ctop, Uint8 atop,
                unsigned cbot, Uint8 abot)
{
	rr_geom(r, NULL, NULL, x, y, w, h, radius, radius, radius, radius,
	        ctop, atop, cbot, abot);
}

void ui_rr_stroke(SDL_Renderer *r, float x, float y, float w, float h,
                  float radius, unsigned ctop, Uint8 atop,
                  unsigned cbot, Uint8 abot, float thick)
{
	rr_stroke_grad(r, x, y, w, h, radius, radius, ctop, atop, cbot, abot, thick);
}

void ui_glow(SDL_Renderer *r, int cx, int cy, int radius, unsigned col,
             Uint8 alpha)
{
	glow(r, cx, cy, radius, col, alpha);
}

void ui_text_at(SDL_Renderer *r, int x, int y, const char *s, unsigned col)
{
	text(r, x, y, s, col);
}

int ui_text_w(const char *s) { return text_w(s); }

void ui_colors(struct ui_colors *out)
{
	out->accent   = C_ACCENT;
	out->text     = C_TEXT;
	out->text_dim = C_TEXT_DIM;
	out->panel    = C_PANEL;
	out->panel_hi = C_PANEL_HI;
	out->edge     = C_EDGE_LT;
	out->shadow   = C_SHADOW;
	out->bg       = C_VOID;
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
/* WIZ_DIDJ SITS RIGHT AFTER WIZ_SYSTEM because it depends on it: the
 * compatibility files are installed INTO LF/Base, so there has to be a LF/Base
 * first. It is also entirely optional — most people have no Didj dumps — so the
 * page says so and Next skips past it without doing anything. */
enum { WIZ_WELCOME = 0, WIZ_SYSTEM, WIZ_DIDJ, WIZ_PROFILE, WIZ_GAMES, WIZ_DONE,
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
/* didj: the compatibility files are in LF/Base. didj_overlay: the controller
 * overlay is staged. Both are needed before a Didj game will install, and they
 * are reported separately because they come from two different downloads and a
 * user who has one and not the other should be told which. */
struct prereq { int rootfs, sysroot, games, qemu, qemu_bundled, fwtools,
                didj, didj_overlay; };

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

	/* THE SAME TWO QUESTIONS install-didj.sh ASKS, and deliberately the same
	 * answers: "is Didj support installed" means AppManager can find the
	 * patches, not that some stamp file was written. A user who followed the
	 * community guide by hand is then correctly shown as already set up. */
	path_join(path, sizeof(path), g_proj, "runtime/sysroot/LF/Base/DidjPatches");
	p->didj = dir_has_entries(path);
	path_join(path, sizeof(path), g_proj, "runtime/didj/overlay");
	p->didj_overlay = dir_has_entries(path);

	/* The bundle first, then the host — the same order tools/lib-deps.sh uses,
	 * so the wizard never says "missing" about something Tadpole brought with
	 * it. TADPOLE_DEPS is set by the AppImage's AppRun; build/deps is where
	 * tools/fetch-deps.sh puts things in a source checkout. */
	{
		const char *deps = getenv("TADPOLE_DEPS");
		char cand[PATHMAX * 2];
		if (deps && *deps) {
			/* Glasspole first, matching tad_qemu(): a bundle carrying it is
			 * self-contained, and saying otherwise would send an AppImage
			 * user to install qemu-user for an engine it never runs. */
			snprintf(cand, sizeof(cand), "%s/bin/glasspole", deps);
			if (access(cand, X_OK) == 0) { p->qemu = 1; p->qemu_bundled = 1; }
			if (!p->qemu) {
				snprintf(cand, sizeof(cand), "%s/bin/qemu-arm", deps);
				if (access(cand, X_OK) == 0) { p->qemu = 1; p->qemu_bundled = 1; }
			}
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
 * one library dependency. Only the subset of PNG the files we open actually
 * use is handled: 8 bits a channel, non-interlaced, colour type 6 (RGBA) or
 * 2 (RGB).
 *
 * RGB WAS ADDED FOR THE BOOT LOGOS, not for the icon. LeapFrog's own screens
 * are a mix — Valencia-Boot-logoCW.png is RGBA, Madrid-Boot-logo.png beside it
 * is RGB — and a reader that silently returns NULL for half of them would make
 * "this system has no boot logo" depend on which of the two a firmware
 * shipped. Everything downstream of the row unpacking is already per-channel,
 * so it costs an opacity and a stride.
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

SDL_Texture *ui_png_texture(SDL_Renderer *ren, const char *path,
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
			/* 8 bits a channel, not interlaced, and a colour type we unpack. */
			if (dat[8] != 8 || dat[12] != 0) goto done;
			if      (dat[9] == 6) bpp = 4;   /* RGBA */
			else if (dat[9] == 2) bpp = 3;   /* RGB  */
			else goto done;
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
					/* RGB has no alpha channel to read; it is opaque. */
					((Uint32)(bpp == 4 ? cur[x*bpp+3] : 255) << 24) |
					((Uint32)cur[x*bpp] << 16) |
					((Uint32)cur[x*bpp+1] << 8) | cur[x*bpp+2];
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
	g_logo = ui_png_texture(ren, path, &g_logo_w, &g_logo_h, &g_logo_px);
}

/* ---- installed apps, for File -> Launch App -----------------------------
 *
 * This replaced a file browser filtered to .swf, which asked the user to know
 * where inside the guest filesystem a title's entry point lives — and could
 * not start a native title at all, because those need AppManager rather than
 * saplayer. The list is read from the installed packages' own meta.inf.
 */
/* GROWN ON DEMAND, NOT CAPPED AT 160.
 *
 * This was a fixed `g_ap[160]` and the reload loop stopped filling it at the
 * ceiling. A real install here has 454 launchable packages, so the launcher
 * listed the first 160 alphabetically and there was no amount of scrolling
 * that reached the rest — the list simply ended at "1-6 of 160". Same growth
 * pattern as the game library, which never had a ceiling. */
struct ap_entry {
	char pkg[64];
	char name[72];
	char version[24];
	char icon[PATHMAX];      /* absolute path to the package's own PNG */
	SDL_Texture *tex;
	int tw, th, tried;
};
static struct ap_entry *g_ap;
static int g_ap_n, g_ap_cap, g_ap_top, g_ap_sel, g_ap_rows = 6;
/* The selection the view has already been scrolled to. -1 so the first draw
 * centres on whatever is selected; see the note in the M_APPS draw. */
static int g_ap_seen = -1;

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
	free(g_ap);
	g_ap = NULL;
	g_ap_n = g_ap_cap = 0;
}

/* -> 0 if the list could not grow, in which case the caller stops adding
 * rather than writing past the end. */
static int ap_push(const struct ap_entry *e)
{
	if (g_ap_n == g_ap_cap) {
		int cap = g_ap_cap ? g_ap_cap * 2 : 64;
		struct ap_entry *t = realloc(g_ap, sizeof(*t) * (size_t)cap);
		if (!t) return 0;
		g_ap = t;
		g_ap_cap = cap;
	}
	g_ap[g_ap_n++] = *e;
	return 1;
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
	g_ap_sel = 0;
	snprintf(dir, sizeof(dir), "%s/runtime/sysroot/LF/Bulk/ProgramFiles", g_proj);
	if (!(d = opendir(dir)))
		return;
	while ((de = readdir(d))) {
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
		if (!ap_push(&e))
			break;
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

/* The initial of entry i, so --selftest-apps can assert that a type-to-jump
 * landed on the right letter without knowing anything about the library it is
 * run against. 0 if the index is out of range. */
char ui_debug_app_initial(int i)
{
	return (i >= 0 && i < g_ap_n) ? g_ap[i].name[0] : 0;
}

/* ---- micromods -----------------------------------------------------------
 *
 * LeapFrog's bonus content: alternate music, expansion tracks, dress-up items.
 * A child earned badges, a parent connected the device to LFConnect, and the
 * points bought them. A device that was never connected has none, and there
 * is no LFConnect to talk to now.
 *
 * A micromod carries NO CONTENT. It is a directory under LF/Bulk/Downloads
 * holding a ~260-byte meta.inf, a checksum and sometimes a preview image; the
 * material it "delivers" already shipped inside the game. SpongeBob's
 * KART_TRACK_EXPANSION is a meta.inf and an icon, while the four tracks it
 * unlocks sit in the base package's own Data/Sound/. The package is a flag
 * saying the child earned it.
 *
 * So this screen reads what is installed for one title and can write the rest.
 * The writing is tools/micromods.py's job, not this file's — the viewer asks
 * for it the same way it asks for a firmware install.
 *
 * ONE TITLE AT A TIME, on purpose. Nothing on the device enumerates every
 * micromod that exists; that list only ever lived in LeapFrog's catalogue.
 * But a game only cares about its own, and filters by ProductID when it reads
 * the folder — so "the micromods for this game" is a question with an answer,
 * where "all micromods" is not.
 */
#define MM_MAX 64
/* ONE ROW PER MICROMOD, NOT PER PACKAGE. The same unlock ships once for every
 * device family a title was built for — LPAD-...-000001 and MULT-...-000001
 * are the same bonus item packaged for different hardware — so listing the
 * files gave Ni Hao Kai-lan "60" when what it has is fifteen slots across four
 * families. The slot is the micromod; the families are which builds can see
 * it. */
struct mm_entry {
	char slot[8];            /* 000001, 100000, ... — the identity */
	char name[72];
	unsigned char installed; /* sitting in Downloads already */
	unsigned char avail;     /* LeapFrog still serves it — a scan found it */
	unsigned char dep;       /* the CartridgeData stub, not a micromod */
	unsigned char pick;      /* ticked, to be installed */
};
static struct mm_entry g_mm[MM_MAX];
static int g_mm_n, g_mm_top, g_mm_rows = 6;
static char g_mm_product[16];    /* the 0x........ this screen is about */
static char g_mm_family[8];      /* ...and which build of it — see below */
static char g_mm_title[72];      /* the game's name, for the heading */

/* Pull one quoted field out of a meta.inf. Same shape as the launcher's
 * meta_field(), kept separate because that one lives with the app list and
 * this runs over a different directory. */
static int mm_field(const char *line, const char *key, char *out, size_t n)
{
	const char *p = line, *q;
	size_t klen = strlen(key);
	while (*p == ' ' || *p == '\t') p++;
	if (strncmp(p, key, klen) != 0) return 0;
	p += klen;
	if (!(q = strchr(p, '"'))) return 0;
	if ((size_t)(q - p) >= n) return 0;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = 0;
	return out[0] != 0;
}

/* A PRODUCTID DOES NOT IDENTIFY A GAME, so this takes the family as well.
 *
 * 0x00180025 is SpongeBob SquarePants: The Clam Prix as LPAD and MULT — and
 * the French "M. Crayon sauve Bourgribouille" as LST3. Two unrelated games,
 * one ProductID. Filtering the Downloads folder on the ProductID alone put
 * Clam Prix's kart tracks and background music on Mr Pencil's Micromods
 * screen, which is nonsense in the way that is easy to miss: the packages
 * really do share an ID, and only the family tells them apart.
 *
 * Twelve of the 258 ProductIDs installed here differ in title across their
 * families. Most are harmless — "Toy Story 3" against "Disney-Pixar Toy Story
 * 3" — but two are genuinely different products, so the pair is the identity
 * and the ProductID on its own is not. */
static void mm_reload(const char *product, const char *family)
{
	char dir[PATHMAX], meta[PATHMAX + 32], line[512];
	DIR *d;
	struct dirent *de;

	g_mm_n = 0;
	g_mm_top = 0;
	snprintf(g_mm_product, sizeof(g_mm_product), "%s", product ? product : "");
	snprintf(g_mm_family, sizeof(g_mm_family), "%s", family ? family : "");
	if (!g_mm_product[0]) return;
	snprintf(dir, sizeof(dir), "%s/runtime/sysroot/LF/Bulk/Downloads", g_proj);
	if (!(d = opendir(dir))) return;
	while ((de = readdir(d)) && g_mm_n < MM_MAX) {
		FILE *f;
		struct mm_entry e;
		char type[32], prod[32];
		int is_mm = 0, mine = 0;

		if (de->d_name[0] == '.') continue;
		/* The ProductID is the middle field of the directory's own name, so
		 * the ones that cannot belong to this title are skipped without
		 * opening anything. */
		if (!strstr(de->d_name, g_mm_product)) continue;
		snprintf(meta, sizeof(meta), "%s/%s/meta.inf", dir, de->d_name);
		if (!(f = fopen(meta, "r"))) continue;
		memset(&e, 0, sizeof(e));
		type[0] = prod[0] = 0;
		while (fgets(line, sizeof(line), f)) {
			if (!type[0] && mm_field(line, "Type=\"", type, sizeof(type)))
				is_mm = !strcmp(type, "MicroDownload");
			if (!e.name[0]) mm_field(line, "Name=\"", e.name, sizeof(e.name));
			/* ProductID is written unquoted, so it is read off the line
			 * rather than through mm_field(). */
			if (!strncmp(line, "ProductID=", 10)) {
				snprintf(prod, sizeof(prod), "%s", line + 10);
				prod[strcspn(prod, " \t\r\n")] = 0;
				mine = !strcasecmp(prod, g_mm_product);
			}
		}
		fclose(f);
		if (!is_mm || !mine) continue;
		{
			/* Split FAM-PRODUCT-SLOT. Anything shaped differently is left
			 * alone rather than guessed at. */
			const char *a = strchr(de->d_name, '-');
			const char *b = a ? strchr(a + 1, '-') : NULL;
			char fam[16];
			int k, found = -1;
			if (!a || !b || (size_t)(a - de->d_name) >= sizeof(fam)) continue;
			memcpy(fam, de->d_name, (size_t)(a - de->d_name));
			fam[a - de->d_name] = 0;
			/* THE FAMILY IS NOT PART OF "IS THIS INSTALLED", and filtering
			 * on it here said "on server" about packages sitting in the
			 * folder. LTM::CMicroDownloads::get matches a package to the
			 * running title on its ProductID and never looks at the family
			 * — which is exactly why a MULT package works for the LST3 and
			 * LPAD builds, and why LeapFrog serving only MULT is not a
			 * problem. So any family's copy of a slot counts as installed.
			 *
			 * g_mm_family still names the build in the heading, because a
			 * ProductID can be two different games and the user should see
			 * which one this screen is about. */
			(void)fam;
			snprintf(e.slot, sizeof(e.slot), "%.7s", b + 1);
			e.installed = 1;

			for (k = 0; k < g_mm_n; k++)
				if (!strcmp(g_mm[k].slot, e.slot)) { found = k; break; }
			if (found < 0) {
				if (g_mm_n >= MM_MAX) continue;
				g_mm[g_mm_n++] = e;
			} else if (e.name[0]) {
				/* A real package's name beats the placeholder a slot sweep
				 * writes. Only reachable if a family somehow appears twice,
				 * which it should not — kept because losing a real name to a
				 * placeholder would be silent and confusing. */
				struct mm_entry *g = &g_mm[found];
				if (!g->name[0] || !strncmp(g->name, "MICROMOD_", 9))
					snprintf(g->name, sizeof(g->name), "%s", e.name);
			}
		}
	}
	closedir(d);

	/* WHAT LEAPFROG STILL HAS, folded in beside what is already here.
	 *
	 * tools/micromods.py --scan writes runtime/micromods-cache/<ProductID>/
	 * index.tsv — one line of `slot, mod|dep, name, has-preview` per package
	 * the server answered for. Reading that is why this screen can list a
	 * title's real micromods by name before any of them are installed; the
	 * device itself enumerates nothing, and the title's own binary names them
	 * in a form that differs per game engine.
	 *
	 * The index is not authoritative about what is INSTALLED — the loop above
	 * is — so a slot already on disk keeps its row and merely gains a note
	 * that it is also downloadable. */
	{
		char idx[PATHMAX + 64];
		FILE *f;
		snprintf(idx, sizeof(idx), "%s/runtime/micromods-cache/%s/index.tsv",
		         g_proj, g_mm_product);
		if ((f = fopen(idx, "r"))) {
			while (fgets(line, sizeof(line), f) && g_mm_n < MM_MAX) {
				char *slot = line, *kind, *name, *nl;
				int k, found = -1;
				if ((nl = strchr(line, '\n'))) *nl = 0;
				if (!(kind = strchr(slot, '\t'))) continue;
				*kind++ = 0;
				if (!(name = strchr(kind, '\t'))) continue;
				*name++ = 0;
				if ((nl = strchr(name, '\t'))) *nl = 0;
				for (k = 0; k < g_mm_n; k++)
					if (!strcmp(g_mm[k].slot, slot)) { found = k; break; }
				if (found < 0) {
					struct mm_entry e;
					memset(&e, 0, sizeof(e));
					snprintf(e.slot, sizeof(e.slot), "%.7s", slot);
					snprintf(e.name, sizeof(e.name), "%s", name);
					e.avail = 1;
					e.dep = !strcmp(kind, "dep");
					g_mm[g_mm_n++] = e;
				} else {
					g_mm[found].avail = 1;
					if (!g_mm[found].name[0])
						snprintf(g_mm[found].name, sizeof(g_mm[found].name),
						         "%s", name);
				}
			}
			fclose(f);
		}
	}
	{
		int i, j;
		for (i = 1; i < g_mm_n; i++) {
			struct mm_entry t = g_mm[i];
			for (j = i - 1; j >= 0 && strcmp(g_mm[j].slot, t.slot) > 0; j--)
				g_mm[j + 1] = g_mm[j];
			g_mm[j + 1] = t;
		}
	}
}

/* Re-read after a scan or an install has changed what is on disk. The screen
 * keeps whichever title it was already about. */
void ui_micromods_reload(void)
{
	if (g_mm_product[0])
		mm_reload(g_mm_product, g_mm_family[0] ? g_mm_family : NULL);
}

/* The ticked slots, comma-separated, for --install --only. -> 0 if none. */
int ui_micromods_picked(char *out, size_t n)
{
	int i, k = 0;
	if (n) out[0] = 0;
	for (i = 0; i < g_mm_n; i++) {
		if (!g_mm[i].pick || g_mm[i].installed) continue;
		if (k && strlen(out) + 1 < n) strncat(out, ",", n - strlen(out) - 1);
		strncat(out, g_mm[i].slot, n - strlen(out) - 1);
		k++;
	}
	return k;
}

void ui_debug_apps(int *n, int *top, int *sel, int *rows)
{
	if (n)    *n    = g_ap_n;
	if (top)  *top  = g_ap_top;
	if (sel)  *sel  = g_ap_sel;
	if (rows) *rows = g_ap_rows;
}

/* One decode attempt per entry, however it goes: a package with a missing or
 * unreadable Icon= should cost one failed open, not one every frame. */
static void ap_icon(SDL_Renderer *r, struct ap_entry *e)
{
	if (e->tex || e->tried || !e->icon[0]) return;
	e->tried = 1;
	e->tex = ui_png_texture(r, e->icon, &e->tw, &e->th, NULL);
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
	           "rotate %d\nauto_rotate %d\nscale %d\ntouch_debug %d\n"
	           "audio_on %d\naudio_latency_ms %d\naudio_pace %d\n"
	           "frame_cap %d\nhle_strict %d\nmsaa %d\nrender_scale %d\n"
	           "io_delay_us %d\ntslib %d\n"
	           "boot_on_start %d\nfast_boot %d\n"
	           "pad_on %d\npad_size %d\npad_opacity %d\npad_left %d\n"
	           "touch_ui %d\nfrost %d\n",
	        g_cfg.gl, g_cfg.gl_hle, g_cfg.debug_level, g_cfg.log_to_file,
	        g_cfg.gl_dumpframe, g_cfg.gl_dumptex,
	        g_cfg.rotate, g_cfg.auto_rotate, g_cfg.scale, g_cfg.touch_debug,
	        g_cfg.audio_on, g_cfg.audio_latency_ms, g_cfg.audio_pace,
	        g_cfg.frame_cap, g_cfg.hle_strict, g_cfg.msaa, g_cfg.render_scale,
	        g_cfg.io_delay_us,
	        g_cfg.tslib,
	        g_cfg.boot_on_start, g_cfg.fast_boot,
	        g_cfg.pad_on, g_cfg.pad_size, g_cfg.pad_opacity, g_cfg.pad_left,
	        g_cfg.touch_ui, g_cfg.frost);
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
			else if (!strcmp(k, "auto_rotate"))      g_cfg.auto_rotate = val;
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
			else if (!strcmp(k, "fast_boot"))        g_cfg.fast_boot = val;
			else if (!strcmp(k, "pad_on"))          g_cfg.pad_on = val;
			else if (!strcmp(k, "pad_size"))        g_cfg.pad_size = val;
			else if (!strcmp(k, "pad_opacity"))     g_cfg.pad_opacity = val;
			else if (!strcmp(k, "pad_left"))        g_cfg.pad_left = val;
			else if (!strcmp(k, "touch_ui"))        g_cfg.touch_ui = val;
			else if (!strcmp(k, "frost"))           g_cfg.frost = val;
			/* Older files carried these two; the debug level replaced them.
			 * Honour them once so an existing install does not silently lose
			 * the logging it was set up with. */
			else if (!strcmp(k, "gl_debug") && val)   g_cfg.debug_level = 2;
			else if (!strcmp(k, "shim_debug") && val) g_cfg.debug_level = 2;
		}
	}
	fclose(f);
	/* WHATEVER THE FILE SAID. Every install that ever unticked host GPU replay
	 * has `gl_hle 0` written down, and honouring it now would hand those users
	 * the deprecated software rasteriser for ever — silently, since the row
	 * that used to explain it is greyed. The setting is kept in the struct and
	 * still written back, so nothing else has to change and a future release
	 * could give the choice back; it is simply not read as an off switch.
	 * TADPOLE_GL_SOFTWARE=1 is the deliberate way to the old path. */
	g_cfg.gl_hle = 1;
	/* AND THE ROW THAT USED TO TURN THIS ON IS GONE. An install that ticked
	 * "Stop if HLE falls back" has `hle_strict 1` written down, and honouring it
	 * now would abort the title on a fallback — the behaviour this release
	 * replaced with a dialog — with nothing in the UI to untick. The env var
	 * TADPOLE_HLE_STRICT is the way to ask for it, and guest_setenv() leaves an
	 * environment that already has it alone. */
	g_cfg.hle_strict = 0;
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
	/* WIDER TITLES WHEN THEY HAVE TO BE PRESSED. "File" is four glyphs, 24
	 * pixels of text; with the old 10 pixels of padding the whole target was
	 * 34 wide and 13 tall, which is a comfortable thing to click at and a
	 * fiddly thing to hit with a thumb. The bar gained its height from
	 * ui_bar_h(); this is the other half of the same target. */
	int pad = ui_touch_ui() ? 20 : 10;
	int i, x = ui_touch_ui() ? 4 : 3;
	for (i = 0; i < NMENUS; i++) {
		MENUS[i].w = text_w(MENUS[i].title) + pad;
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

/* WHERE THE Nth DROPDOWN ITEM IS. This was "UI_BAR_H + 3 + i * 12" written out
 * at four call sites — the draw and three hit tests — with the row height as a
 * bare 12 in each. They agreed only by everyone remembering to change all
 * four, which is not a property of the code, it is a habit of the author.
 *
 * A SEPARATOR IS A RULE, NOT A ROW, so it is not given a row's height. At the
 * old 12 that distinction was not worth drawing; at a touch-sized 23 a
 * full-height separator is a fingertip of nothing in the middle of the menu,
 * and the File menu has three of them. Summing the heights rather than
 * multiplying one of them is what lets the two differ. */
static int menu_item_h(const struct mitem *it)
{
	if (it->id) return ui_menu_row_h();
	return ui_touch_ui() ? 9 : 12;
}

static int menu_item_y(const struct menu *m, int i)
{
	int k, y = UI_BAR_H + 3;
	for (k = 0; k < i && k < m->n; k++)
		y += menu_item_h(&m->items[k]);
	return y;
}

static int menu_height(const struct menu *m)
{
	int k, h = 6;
	for (k = 0; k < m->n; k++)
		h += menu_item_h(&m->items[k]);
	return h;
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

/* WHERE A FILE CHOOSER SHOULD OPEN, which on Android is not where the files
 * this program owns live.
 *
 * Everywhere else <project>/games is exactly right: it sits beside the viewer
 * and the user put their backups there themselves. On Android the project
 * directory is the app's PRIVATE data directory — /data/user/0/<pkg>/files —
 * which no file manager will show, which MTP does not export, and which a
 * person plugging the tablet into a computer cannot write to at all. Opening
 * there offers a folder nobody can put anything into, which reads as the
 * chooser being broken.
 *
 * Shared storage is the answer, and it is spelled two ways depending on how
 * old the platform is, so both are tried. Nothing is created here: a chooser
 * that makes a directory as a side effect of being opened is a surprise, and
 * the user is about to pick one anyway.
 */
static void default_browse_dir(char *out, size_t n)
{
#ifdef __ANDROID__
	static const char *shared[] = {
		"/storage/emulated/0", "/sdcard", "/storage/self/primary"
	};
	size_t i;
	for (i = 0; i < sizeof(shared) / sizeof(shared[0]); i++)
		if (access(shared[i], R_OK) == 0) {
			snprintf(out, n, "%s", shared[i]);
			return;
		}
#endif
	path_join(out, n, g_proj, "games");
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
	else                        default_browse_dir(start, sizeof(start));
	if (access(start, R_OK) != 0)
		default_browse_dir(start, sizeof(start));
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
	glass_free();                 /* the cached backdrop behind the panels */
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

/* Valid until the next action is taken. Only UI_ACT_MICROMODS_INSTALL sets
 * it; everything else leaves whatever was there, so read it in the same
 * breath as the action it belongs to. */
const char *ui_action_arg(void)
{
	return g_action_arg;
}

static void msg(const char *title, const char *body)
{
	snprintf(g_msg_title, sizeof(g_msg_title), "%s", title);
	snprintf(g_msg_body, sizeof(g_msg_body), "%s", body);
	g_modal = M_MSG;
}

/* The same dialog, for the render loop rather than a menu item. It has to be
 * possible to raise one of these from outside this file: the host-GPU replay
 * dying is noticed by the frame pump, not by anything the user clicked.
 *
 * Deliberately does NOT steal an open menu or a modal already on screen —
 * whatever the user is doing wins, and the caller is expected to try again. */
int ui_alert(const char *title, const char *body)
{
	if (g_modal != M_NONE) return 0;
	g_confirm = 0;
	g_open_menu = -1;
	msg(title, body);
	return 1;
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
	/* AN UNRELEASED BUILD IS NOT BEHIND, AND MUST NOT BE NAGGED.
	 *
	 * "dev" used to open this dialog too, on the reasoning that someone running
	 * a working copy still wants to see what has shipped since. In practice it
	 * is backwards: a working copy is almost always NEWER than the newest
	 * release — it is the code the next release will be cut from — so the
	 * program spent every launch offering to "update" a developer to something
	 * older than what they had just compiled, with a Download button that would
	 * have replaced their build with a published one.
	 *
	 * It is still a distinct state and still reported as one: check-update.py
	 * prints `status dev`, Help -> About says "dev (unreleased)", and asking
	 * for the check by hand gets the answer below. What it no longer does is
	 * interrupt.
	 *
	 * Only a genuine `behind` raises the prompt now — and since tools/
	 * release.sh will not publish an asset whose binary does not carry the
	 * release version, `dev` in a released build is no longer reachable. */
	if (!strcmp(g_up_status, "behind")) {
		g_modal = M_UPDATE;
		return;
	}
	/* Nothing newer. A check the user asked for still owes them an answer;
	 * the one that runs by itself at startup owes them silence. */
	if (g_up_silent)
		return;
	/* Asked for, and running a working copy: say so plainly rather than
	 * falling through to "Could not check", which would blame the network for
	 * an answer that arrived perfectly well. */
	if (!strcmp(g_up_status, "dev")) {
		msg("Unreleased build", "This build is not a release.");
		return;
	}
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
			default_browse_dir(start, sizeof(start));
		if (access(start, R_OK) != 0)
			default_browse_dir(start, sizeof(start));
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
			default_browse_dir(start, sizeof(start));
		if (access(start, R_OK) != 0)
			default_browse_dir(start, sizeof(start));
		fb_open("Cartridge dump (.bin)", start, ".bin", UI_ACT_CONVERT_CART);
		break;
	case IT_FW:
		/* The downloads land in shared storage on Android for the same reason
		 * the backups do — this is the folder a browser saved them into. */
		default_browse_dir(start, sizeof(start));
		if (access(start, R_OK) != 0)
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

/* HOW TALL A SETTINGS DIALOG HAS TO BE, given what is in it.
 *
 * These used to be plain numbers — 200, 122, 164 — measured once by eye at a
 * row height of 14. A touch-sized row is 24, and the plain numbers did not
 * know that: the Graphics dialog quietly pushed its last two rows and its
 * footnote out through its own floor and drew them over the Close button.
 *
 * `rows` is how many settings rows the body draws; `extra` is everything under
 * them that is not one — a note, a separator, a block of key bindings.
 */
static int dlg_body_h(int rows, int extra)
{
	return row_top() + rows * ROW_H + extra + 6 + ui_btn_h() + 5;
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

/* THE ANIMATED RECT, used by the draw AND the hit test.
 *
 * A modal rises the last few pixels into place as it fades in. Both the
 * drawing and dialog_click() read their geometry from here, so the two agree
 * on every frame of that: a click during the entrance lands on the panel
 * where it currently looks, not where it is about to be. Offsetting only the
 * draw would have made the panel briefly lie about its own buttons. */
static struct dlg cur_dlg_settled(int lw, int lh);

static struct dlg cur_dlg(int lw, int lh)
{
	struct dlg d = cur_dlg_settled(lw, lh);
	d.y += (int)((1.0f - anim_t(g_modal_at, 150)) * 10.0f);
	return d;
}

static struct dlg cur_dlg_settled(int lw, int lh)
{
	switch (g_modal) {
	case M_ABOUT: return dlg_fit(lw, lh, 210, 144);
	/* Room for icons and two lines per row. */
	/* As wide as the library, and as tall as a 272px panel allows. It is a
	 * list of four hundred things; the extra row and the room for a package
	 * ID beside each name are worth more here than a tidy small box. */
	case M_APPS:  return dlg_fit(lw, lh, 420, 252);
	case M_MICROMODS: return dlg_fit(lw, lh, 380, 214);
	/* Wide and tall: this is a changelog, and a release body wrapped
	 * into 30 columns would be unreadable. */
	case M_UPDATE: return dlg_fit(lw, lh, 400, 230);
	/* NINE rows: the eight this had, plus "Turn with the app" from main. */
	case M_GFX:   return dlg_fit(lw, lh, 250, dlg_body_h(9, 0));
	case M_AUDIO: return dlg_fit(lw, lh, 230, dlg_body_h(3, 0));
	/* Four settings, a note about A and B, then the key bindings — which are
	 * one column at a mouse-sized row height and two at a touch-sized one, so
	 * they still fit under rows that are ten pixels taller each. */
	case M_PAD:   return dlg_fit(lw, lh, 240,
	                             dlg_body_h(5, ui_touch_ui() ? 60 : 100));
	case M_DEBUG: return dlg_fit(lw, lh, 268, dlg_body_h(8, 0));
	/* Three settings and the frost, the games folder and its path, then a row
	 * of two buttons sitting where a seventh row would be.
	 *
	 * THE FROST WENT HERE AND NOT IN GRAPHICS SETTINGS, which is where a
	 * performance knob belongs and where there is no room: M_GFX already asks
	 * for nine touch-sized rows, which is 275 logical pixels in a window that
	 * can offer 266, so its last row is drawn over its own Close button. A
	 * tenth row there would have hidden the setting behind the bug. This is
	 * also the dialog the other "what the chrome looks like" switch lives in,
	 * which is the one someone turning things off will already have open. */
	case M_SYSTEM: return dlg_fit(lw, lh, 268, dlg_body_h(7, 6));
	case M_FILES: return dlg_fit(lw, lh, 300, 172);
	case M_WIZARD: return dlg_fit(lw, lh, 348, 210);
	case M_PROGRESS: return dlg_fit(lw, lh, 350, 150);
	/* Tall enough for a wrapped body plus the erase confirmation's two extra
	 * lines beneath it — see the M_MSG draw. */
	case M_MSG:   return dlg_fit(lw, lh, 250, 116);
	/* The library wants every pixel it can have: it is a list of eighty-odd
	 * names next to a picture. */
	case M_GAMES: return dlg_fit(lw, lh, 460, 260);
	default:      { struct dlg z = {0,0,0,0}; return z; }
	}
}

/* HOW FAR THE BOTTOM ROW OF BUTTONS SITS FROM THE DIALOG'S FLOOR. One number,
 * because four different helpers below each used to write their own and a
 * taller button pushed each of them a different distance off the edge. */
static int btn_row_y(const struct dlg *d)
{
	return d->y + d->h - ui_btn_h() - 5;
}

/* ---- app launcher geometry ----------------------------------------------
 * ONE SET OF NUMBERS FOR THE DRAW AND THE HIT TEST. Both used to compute the
 * row height and visible count inline, in two places, from the same three
 * magic numbers — which is how the clickable width came to disagree with the
 * drawn width once a scrollbar appeared beside it.
 */
#define AP_ROW_H 28
/* THE LIST STARTS DIRECTLY UNDER THE TITLE STRIP. An earlier version kept a
 * header line of its own for the hint and the count, which cost eleven pixels
 * at the top and pushed the well down onto the footer — the "Enter launches"
 * line ended up drawn along the well's bottom border in both orientations.
 * The count moved into the title strip, which had room going spare, and the
 * hint moved down beside the footer. */
static int ap_list_y(const struct dlg *d) { return d->y + 18; }
static int ap_list_h(const struct dlg *d) { return d->h - 18 - 26; }
static int ap_rows_fit(const struct dlg *d)
{
	int v = ap_list_h(d) / AP_ROW_H;
	return v < 1 ? 1 : v;
}
/* The scrollbar takes its width out of the rows, so a click near the right
 * edge lands on the row rather than in a gap. */
static int ap_row_w(const struct dlg *d, int vis)
{
	/* Wide enough to clear the scrollbar AND the gap either side of it, so
	 * the row highlight stops short of the bar rather than running under it. */
	return d->w - 16 - (g_ap_n > vis ? 10 : 0);
}

/* Does the library have room for the preview panel on the right? In portrait
 * it does not, and the list gets the whole width instead. */
#define GM_PANEL_W 104
/* Tall enough to put a finger on, which for a row with an icon in it is not
 * much more than it already was. */
#define GM_ROW_H   (ui_touch_ui() ? 24 : 15)
static int gm_panel(const struct dlg *d) { return d->w >= 300 ? GM_PANEL_W : 0; }

/* THE MICROMODS BUTTON, WHEN THERE IS ROOM FOR IT. -> 0 when there is not.
 *
 * It sits between the Rescan/Folder pair on the left and Install/Close on the
 * right, and in portrait the dialog clamps to 264 logical pixels, at which
 * width those two groups already meet. Drawn unconditionally it printed
 * "MicroInstall 2" over the top of the install button — and, because its hit
 * test ran first, ate the clicks meant for it. Games stopped installing.
 *
 * Both the draw and the hit test come through here so they cannot disagree
 * again, and the dialog degrades the way it already does elsewhere: the
 * preview panel disappears under 300px too. */
static int gm_micro_rect(const struct dlg *d, SDL_Rect *out)
{
	int x = d->x + 110, w = 76;
	int install_x = d->x + d->w - 8 - 48 - 62;
	if (x + w + 6 > install_x) return 0;
	out->x = x; out->y = btn_row_y(d); out->w = w; out->h = ui_btn_h();
	return 1;
}
static int gm_list_w(const struct dlg *d) { return d->w - 12 - gm_panel(d); }
/* Stops above the footer, which is a button tall — it used to be told 22,
 * which is a button tall only at the old height. */
static int gm_list_h(const struct dlg *d) { return d->h - 26 - ui_btn_h() - 9; }

/* ---- scrolling a list with a finger --------------------------------------
 *
 * EVERY LIST IN THIS FILE SCROLLED BY WHEEL AND BY KEYBOARD, and a touchscreen
 * has neither. Four hundred titles in the launcher, three hundred in the
 * library, a directory of any size in the browser: without this, a finger can
 * reach the first seven of each and nothing else. That is not a rough edge,
 * it is the interface not working.
 *
 * ONE DESCRIPTION OF "A LIST", filled in per modal, so the drag does not need
 * to know which of the five it is dragging. Each of them already keeps its own
 * first-visible index and its own row height; this collects the two, plus the
 * rectangle the drag counts inside, and hands back a pointer so the drag can
 * move the real thing.
 */
struct list_view {
	SDL_Rect body;      /* where a press starts a drag */
	int      row_h;
	int     *top;       /* the list's own first-visible index */
	int      n;         /* rows in total */
	int      vis;       /* rows on screen */
};

static int list_view(const struct dlg *d, struct list_view *v);

/* TAP OR DRAG, decided by how far it moved. Below this a press inside a list
 * is still a press and selects the row under it; above it, the press was the
 * beginning of a scroll and must not also choose something. Four pixels is
 * about the wobble of a finger that believes it is holding still. */
#define DRAG_SLOP 4

static struct {
	int    active;      /* a press landed inside a list body */
	int    x0, y0;      /* and where it landed, for the tap that may follow */
	int    last;        /* the last y seen, so motion is a delta */
	int    moved;       /* the furthest it has been from the start */
	int    rem;         /* pixels not yet worth a whole row */
	float  vel;         /* pixels per frame, for the flick */
} g_drag;

static void list_clamp(struct list_view *v)
{
	if (*v->top > v->n - v->vis) *v->top = v->n - v->vis;
	if (*v->top < 0) *v->top = 0;
}

/* Move a list by a pixel delta, keeping the remainder. Without the remainder a
 * slow drag never accumulates a whole row and the list simply does not move,
 * which reads as the list being stuck rather than as the drag being gentle. */
static void list_scroll_px(struct list_view *v, int dy)
{
	int rows;
	g_drag.rem += dy;
	rows = g_drag.rem / v->row_h;
	if (!rows) return;
	g_drag.rem -= rows * v->row_h;
	*v->top -= rows;                 /* content follows the finger */
	list_clamp(v);
}

/* The flick. Called once a frame from ui_draw, because that is the only thing
 * here that happens every frame. */
static void list_fling(int lw, int lh)
{
	struct dlg d;
	struct list_view v;
	int dy;

	if (g_drag.active || g_modal == M_NONE) return;
	if (g_drag.vel > -0.6f && g_drag.vel < 0.6f) { g_drag.vel = 0; return; }
	d = cur_dlg(lw, lh);
	if (!list_view(&d, &v)) { g_drag.vel = 0; return; }
	dy = (int)g_drag.vel;
	list_scroll_px(&v, dy);
	/* A list that has run out of room stops dead rather than coasting against
	 * its own end for another half second. */
	if (*v.top == 0 || *v.top == v.n - v.vis) { g_drag.vel = 0; return; }
	g_drag.vel *= 0.90f;
}

/* Rows inside settings dialogs are uniform, so one helper does hit-testing
 * and drawing from the same numbers. */
static int row_y(const struct dlg *d, int i) { return d->y + row_top() + i * ROW_H; }

/* A settings row: checkbox or a cycling value. Returns 1 if (mx,my) hits it. */
static int row_hit(const struct dlg *d, int i, int mx, int my)
{
	/* The box is centred on the label rather than hung three pixels above it:
	 * "- 3" is the right offset at a row height of 14 and at no other. */
	return inside(mx, my, d->x + 6, row_y(d, i) - text_dy(ROW_H), d->w - 12, ROW_H);
}

static void row_check(SDL_Renderer *r, const struct dlg *d, int i,
                      const char *label, int on, int hot)
{
	int y = row_y(d, i);
	if (hot) rfill(r, d->x + 6, y - text_dy(ROW_H), d->w - 12, ROW_H, C_PANEL_HI, 178);
	text(r, d->x + 10, y, on ? GL_CHECK_1 : GL_CHECK_0, on ? C_ACCENT : C_TEXT_DIM);
	text(r, d->x + 22, y, label, C_TEXT);
}

/* A setting that is no longer a choice: ticked, dimmed, and inert. Shown rather
 * than removed because it has been in this dialog for a long time and its
 * absence would read as "the feature went away" — the opposite of what
 * happened. Dimmed and inert IS the explanation; a trailing "- always on" was
 * tried and only made the row longer. */
static void row_check_locked(SDL_Renderer *r, const struct dlg *d, int i,
                             const char *label)
{
	int y = row_y(d, i);
	text(r, d->x + 10, y, GL_CHECK_1, C_TEXT_DIM);
	text(r, d->x + 22, y, label, C_TEXT_DIM);
}

static void row_value(SDL_Renderer *r, const struct dlg *d, int i,
                      const char *label, const char *val, int hot)
{
	int y = row_y(d, i);
	if (hot) rfill(r, d->x + 6, y - text_dy(ROW_H), d->w - 12, ROW_H, C_PANEL_HI, 178);
	text(r, d->x + 10, y, label, C_TEXT);
	text(r, d->x + d->w - 12 - text_w(val), y, val, C_ACCENT);
}

/* Wizard buttons: Back / Next|Finish / Cancel, bottom right, in that order —
 * the arrangement every Windows installer has used for thirty years, because it
 * needs no explaining. */

/* A button, drawn: the chip and its label, with the label centred vertically
 * whatever the height turned out to be.
 *
 * THIS PATTERN APPEARED SEVENTEEN TIMES, always as a chip() and a text_c() with
 * the vertical offset written out by hand as "+ 3" — correct at a height of 13
 * and at no other height, which is the entire problem with making buttons
 * bigger. Returns whether it is hot, since every caller wanted to know. */
static int button(SDL_Renderer *r, SDL_Rect b, const char *label)
{
	int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
	chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
	text_c(r, b.x, b.w, b.y + text_dy(b.h), label, hot ? C_ACCENT : C_TEXT);
	return hot;
}

/* Buttons for the update dialog: 0 = Download, 1 = Later. Derived from the
 * dialog rect rather than stored, so a clamped dialog on a small window still
 * has its buttons where they are drawn. */
static SDL_Rect up_btn(const struct dlg *d, int which)
{
	SDL_Rect b;
	b.w = 76; b.h = ui_btn_h();
	b.y = btn_row_y(d);
	b.x = which == 0 ? d->x + d->w - 2 * b.w - 16 : d->x + d->w - b.w - 8;
	return b;
}

static SDL_Rect wiz_btn(const struct dlg *d, int which)   /* 0 back 1 next 2 cancel */
{
	SDL_Rect r;
	r.w = 44; r.h = ui_btn_h();
	r.y = btn_row_y(d);
	r.x = d->x + d->w - 8 - (3 - which) * 47;
	return r;
}

/* Close button, bottom right of every dialog. */
static SDL_Rect close_rect(const struct dlg *d)
{
	SDL_Rect rc;
	rc.w = 42; rc.h = ui_btn_h();
	rc.x = d->x + d->w - 48;
	rc.y = btn_row_y(d);
	return rc;
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_bar(SDL_Renderer *r, int lw)
{
	int i;
	char buf[32];

	/* The bar is lit from above like everything else: a gradient body and a
	 * hairline of light along the very top edge, closed off underneath by the
	 * dark rule that was always there.
	 *
	 * EVERY PIXEL OF THIS STAYS ABOVE UI_BAR_H. A soft shadow falling from the
	 * bar onto the picture below would look better and is not ours to draw —
	 * everything from UI_BAR_H down belongs to the guest, and an emulator that
	 * dims the top rows of the display it is emulating is lying about what the
	 * hardware drew. The tempting version of this cost three rows.
	 *
	 * Drawn 1px oversize left, right and top so the antialiased feather falls
	 * outside the window rather than leaving a dim seam along its edges. */
	rr_geom(r, NULL, NULL, -1.0f, -1.0f, (float)lw + 2.0f, (float)UI_BAR_H,
	       0, 0, 0, 0, C_BAR_HI, 255, C_BAR, 255);
	fill(r, 0, 0, lw, 1, C_EDGE_LT);
	fill(r, 0, UI_BAR_H - 1, lw, 1, C_EDGE_DK);

	for (i = 0; i < NMENUS; i++) {
		int hot = (g_open_menu == i) ||
		          (g_open_menu < 0 && inside(g_mx, g_my, MENUS[i].x, 0,
		                                    MENUS[i].w, UI_BAR_H - 1));
		if (hot) rfill(r, MENUS[i].x, 0, MENUS[i].w, UI_BAR_H - 1, C_BAR_HI, 178);
		/* Centred in the bar's full height, which comes out at exactly the 3
		 * this was written as before the height became a variable. */
		text_c(r, MENUS[i].x, MENUS[i].w, text_dy(UI_BAR_H), MENUS[i].title,
		       hot ? C_ACCENT : C_TEXT);
	}

	/* Orientation button — one press per 90 degrees, so a portrait title
	 * does not mean turning your head. */
	{
		int x = rot_x(lw);
		int bh = UI_BAR_H - 3;
		int hot = inside(g_mx, g_my, x, 1, ROT_W, bh);
		chip(r, x, 1, ROT_W, bh, hot ? C_BAR_HI : C_PANEL, 1);
		snprintf(buf, sizeof(buf), "ROT %d", g_cfg.rotate);
		text_c(r, x, ROT_W, 1 + text_dy(bh), buf, hot ? C_ACCENT : C_TEXT);
	}

	/* status, right-aligned before the rotate button */
	{
		int sx = rot_x(lw) - 6 - text_w(g_status);
		if (sx > MENUS[NMENUS-1].x + MENUS[NMENUS-1].w + 6)
			text(r, sx, text_dy(UI_BAR_H), g_status,
			     g_running ? C_ACCENT : C_TEXT_DIM);
	}
}

static void draw_dropdown(SDL_Renderer *r)
{
	static int seen = -1;
	const struct menu *m;
	int w, h, x, y, i;
	float t;

	anim_watch(g_open_menu, &seen, &g_menu_at);
	if (g_open_menu < 0) return;
	m = &MENUS[g_open_menu];
	w = menu_width(m);
	h = menu_height(m);
	x = m->x; y = UI_BAR_H;

	/* Drops out from under the bar: short, because a menu is something you
	 * are already reaching for and anything slower is in the way. */
	t = anim_t(g_menu_at, 110);
	g_alpha = (Uint8)(255.0f * t);
	y -= (int)((1.0f - t) * 5.0f);

	/* SQUARE ALONG THE TOP. A dropdown is not a floating sheet — it is the
	 * bar's own title continued downwards, and rounding the two corners that
	 * meet the bar detaches it from the thing it belongs to. The bottom two
	 * stay rounded, which is the edge that really is floating. */
	panel(r, x, y, w, h, 0.0f);

	for (i = 0; i < m->n; i++) {
		const struct mitem *it = &m->items[i];
		int ih = menu_item_h(it);
		/* The entrance slides the whole sheet up by a few pixels, so rows are
		 * placed relative to the sheet's own top — menu_item_y() answers in
		 * settled screen coordinates, which is what the hit tests want. */
		int iy = y + (menu_item_y(m, i) - UI_BAR_H);
		if (!it->id) {                       /* separator */
			fill(r, x + 5, iy + ih / 2, w - 10, 1, C_EDGE_DK);
			continue;
		}
		if (g_hot_item == i && item_enabled(it))
			rfill(r, x + 2, iy, w - 4, ih, C_PANEL_HI, 178);
		text(r, x + 8, iy + text_dy(ih), it->label,
		     item_enabled(it) ? (g_hot_item == i ? C_ACCENT : C_TEXT) : C_TEXT_DIM);
	}
	g_alpha = 255;
}

static const char *onoff(int v) { return v ? "ON" : "OFF"; }

/* The body. Wrapped by draw_dialog() below, which owns the entrance fade —
 * this function has an early return in it, and a fade that has to be undone
 * on the way out is exactly the kind of thing that leaks through one branch
 * and tints the rest of the frame. */
static void draw_dialog_body(SDL_Renderer *r, int lw, int lh)
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
	case M_MICROMODS: title = "Micromods"; break;
	default: break;
	}

	/* CAPTURE BEFORE DIMMING, or the glass is made of the dimmed picture and
	 * there is nothing left in it to see. The dim belongs to the world AROUND
	 * the panel; the panel itself wants the backdrop as it really is. */
	{
		SDL_Texture *blur = glass_capture(r);

		/* Lighter than it was: the panel now has a shadow and a lit rim to
		 * separate it from the backdrop, so the dim no longer has to do that
		 * job on its own — and a heavy dim leaves the glass nothing to pick
		 * up. */
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		/* Comes up with the panel. Drawn straight rather than through fill(),
		 * so it takes the entrance curve by hand. */
		SDL_SetRenderDrawColor(r, 0, 8, 4,
		                       (Uint8)(104 * anim_t(g_modal_at, 150)));
		{ SDL_Rect all = { 0, UI_BAR_H, lw, lh - UI_BAR_H }; SDL_RenderFillRect(r, &all); }
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

		panel_glass(r, d.x, d.y, d.w, d.h, blur,
		            rr_radius_panel(d.w, d.h));
	}
	/* Rounded to match the panel's own top corners — a square strip inset by
	 * one pixel hangs visibly outside a 10px radius. */
	{
		float hrad = rr_radius_panel(d.w, d.h) - 1.0f;
		rr_geom(r, NULL, NULL, d.x + 1.0f, d.y + 1.0f, d.w - 2.0f, 11.0f,
		       hrad, hrad, 0, 0, C_BAR_HI, 208, C_BAR_HI, 172);
	}
	text(r, d.x + 6, d.y + 3, title, C_ACCENT);

	switch (g_modal) {
	case M_APPS: {
		int x   = d.x + 8;
		int y   = ap_list_y(&d);
		int vis = ap_rows_fit(&d);
		int rw  = ap_row_w(&d, vis);
		int i;
		char line[128];

		g_ap_rows = vis;
		/* Keep the selected row on screen. Arrow keys move the selection and
		 * the view follows it, which is the half of "scrolling" this list
		 * never had — the wheel worked, and nothing else did.
		 *
		 * ONLY WHEN THE SELECTION HAS ACTUALLY MOVED. This used to run on
		 * every frame, and every frame it dragged the view back to wherever
		 * the selection was. That was invisible for as long as the only ways
		 * to scroll were the wheel and the arrow keys: the arrows move the
		 * selection anyway, and the wheel's own clamp kept the two together by
		 * accident. A finger does neither — it scrolls the VIEW and leaves the
		 * selection where it was — so the next frame snapped straight back and
		 * the list could not be dragged at all. Caught by --selftest-apps,
		 * which now drags and then draws. */
		if (g_ap_sel < 0) g_ap_sel = 0;
		if (g_ap_sel > g_ap_n - 1) g_ap_sel = g_ap_n - 1;
		if (g_ap_sel != g_ap_seen) {
			if (g_ap_sel < g_ap_top) g_ap_top = g_ap_sel;
			if (g_ap_sel >= g_ap_top + vis) g_ap_top = g_ap_sel - vis + 1;
			g_ap_seen = g_ap_sel;
		}
		if (g_ap_top > g_ap_n - vis) g_ap_top = g_ap_n - vis;
		if (g_ap_top < 0) g_ap_top = 0;

		if (!g_ap_n) {
			text(r, x + 2, y + 14, "No apps installed yet.", C_TEXT);
			text(r, x + 2, y + 32, "File " GL_SUB " Game Library installs your", C_TEXT_DIM);
			text(r, x + 2, y + 42, "cartridge backups.", C_TEXT_DIM);
			text(r, x + 2, y + 58, "Help " GL_SUB " Setup Wizard fetches the", C_TEXT_DIM);
			text(r, x + 2, y + 68, "system files they need.", C_TEXT_DIM);
			break;
		}

		/* WHERE YOU ARE, ON THE TITLE STRIP. The range rather than the bare
		 * total: the total never changes and stops being information after
		 * the first glance, while "196-202 of 462" is the answer to the
		 * question a long list actually raises. */
		snprintf(line, sizeof(line), "%d-%d of %d", g_ap_top + 1,
		         g_ap_top + vis < g_ap_n ? g_ap_top + vis : g_ap_n, g_ap_n);
		text(r, d.x + d.w - 8 - text_w(line), d.y + 3, line, C_TEXT_DIM);

		/* The well the rows sit in, so the list reads as one object rather
		 * than as text floating on the panel. */
		chip(r, x - 4, y - 4, d.w - 16, vis * AP_ROW_H + 6, C_VOID, 0);

		for (i = 0; i < vis && g_ap_top + i < g_ap_n; i++) {
			struct ap_entry *e = &g_ap[g_ap_top + i];
			int k   = g_ap_top + i;
			int yy  = y + i * AP_ROW_H;
			int hot = inside(g_mx, g_my, x - 2, yy, rw, AP_ROW_H - 2);
			int sel = k == g_ap_sel;
			int tx  = x + 32;

			/* Hover and selection are different things and have to look
			 * different: the pointer is wherever it happens to be, the
			 * selection is where Enter will act. The accent bar down the left
			 * is the one that means "this one". */
			if (hot || sel)
				rfill(r, x - 2, yy, rw, AP_ROW_H - 2,
				      hot ? C_BAR_HI : C_PANEL_HI, hot ? 178 : 190);
			if (sel)
				rfill(r, x - 2, yy, 2, AP_ROW_H - 2, C_ACCENT, 235);

			/* EVERY ROW GETS A TILE, drawn before the artwork rather than
			 * instead of it. Package icons are not all the same size and not
			 * all present; without something behind them the rows with art
			 * and the rows without looked like two different lists. */
			chip(r, x + 3, yy + 2, 22, 22, C_PANEL, 0);
			ap_icon(r, e);
			if (e->tex) {
				SDL_Rect dst = { x + 4, yy + 3, 20, 20 };
				SDL_SetTextureAlphaMod(e->tex, g_alpha);
				SDL_RenderCopy(r, e->tex, NULL, &dst);
			}

			/* The package ID, right-aligned and dim. Four hundred titles
			 * include several genuine duplicate NAMES — two "Wheel Works",
			 * two "Ben 10: Ultimate Alien" — and without this the list offers
			 * you the same row twice with no way to tell which is which. */
			{
				int idw = text_w(e->pkg);
				int room = rw - 40;
				if (idw + text_w(e->name) + 24 < room)
					text(r, x - 2 + rw - 6 - idw, yy + 12, e->pkg, C_DIMMEST);
			}
			snprintf(line, sizeof(line), "%.34s", e->name);
			text(r, tx, yy + 3, line, sel || hot ? C_ACCENT : C_TEXT);
			if (e->version[0]) {
				snprintf(line, sizeof(line), "v%.20s", e->version);
				text(r, tx, yy + 13, line, C_TEXT_DIM);
			}
		}

		/* Scrollbar, for the same reason the game library has one: with four
		 * hundred packages, "where am I and how much is left" is a real
		 * question that six visible rows cannot answer. */
		if (g_ap_n > vis) {
			int track = vis * AP_ROW_H - 2;
			int knob  = track * vis / g_ap_n;
			int pos   = track * g_ap_top / g_ap_n;
			int sx    = d.x + d.w - 18;   /* inside the well, clear of the rows */
			if (knob < 10) knob = 10;
			if (pos > track - knob) pos = track - knob;
			if (pos < 0) pos = 0;
			rfill(r, sx, y, 4, track, C_SHADOW, 170);
			rfill(r, sx, y + pos, 4, knob, C_EDGE_LT, 210);
		}

		text(r, x + 2, d.y + d.h - 26,
		     "Type a letter to jump " GL_DIAMOND " Enter launches", C_TEXT_DIM);
		break;
	}
	case M_MICROMODS: {
		/* +29, not +26: at +26 the header line and the well's rounded top
		 * edge abutted exactly and the count was sliced along its baseline —
		 * the same fault the app launcher had, for the same reason. The
		 * count moved onto the title strip, which has room going spare. */
		int x = d.x + 8, y = d.y + 29, i;
		int rh = ui_list_row_h();
		int vis = (d.h - 29 - 40) / rh;
		char line[128];

		if (vis < 1) vis = 1;
		g_mm_rows = vis;
		if (g_mm_top > g_mm_n - vis) g_mm_top = g_mm_n - vis;
		if (g_mm_top < 0) g_mm_top = 0;

		{
			/* Say what the number counts. "60" was the file count, and it
			 * read as sixty pieces of bonus content when the title has
			 * fifteen slots built for four device families. The
			 * CartridgeData stub is in the list but is not one of them, so
			 * it is not counted either. */
			int nm = 0, ni = 0, q;
			for (q = 0; q < g_mm_n; q++) {
				if (g_mm[q].dep) continue;
				nm++;
				if (g_mm[q].installed) ni++;
			}
			if (ni && ni < nm)
				snprintf(line, sizeof(line), "%d of %d installed", ni, nm);
			else
				snprintf(line, sizeof(line), "%d micromods", nm);
			text(r, d.x + d.w - 8 - text_w(line), d.y + 3, line, C_TEXT_DIM);
		}
		text(r, x, d.y + 16, g_mm_title, C_ACCENT);
		if (g_mm_family[0]) {
			/* Which build, because the same ProductID can be two different
			 * games on different hardware. */
			snprintf(line, sizeof(line), "%s build", g_mm_family);
			text(r, d.x + d.w - 8 - text_w(line), d.y + 16, line, C_DIMMEST);
		}

		if (!g_mm_n) {
			text(r, x, y + 14, "None installed for this game.", C_TEXT);
			text(r, x, y + 32, "Bonus content was earned on the device and", C_TEXT_DIM);
			text(r, x, y + 42, "redeemed through LFConnect. LeapFrog still", C_TEXT_DIM);
			text(r, x, y + 52, "serves the packages, so Scan asks what this", C_TEXT_DIM);
			text(r, x, y + 62, "game has and lists it here.", C_TEXT_DIM);
			break;
		}
		chip(r, x - 4, y - 4, d.w - 16, vis * rh + 6, C_VOID, 0);
		for (i = 0; i < vis && g_mm_top + i < g_mm_n; i++) {
			struct mm_entry *e = &g_mm[g_mm_top + i];
			int yy = y + i * rh + text_dy(rh) - 2;
			int can = e->avail && !e->installed && !e->dep;
			/* A TICK BOX ONLY WHERE TICKING MEANS SOMETHING. An installed slot
			 * has nothing to fetch, and the CartridgeData stub is not a
			 * micromod — it goes in alongside them because they name it in
			 * Depends, not because anyone chooses it. */
			if (can) {
				chip(r, x + 1, yy - 1, 9, 9,
				     e->pick ? C_ACCENT : C_PANEL, 1);
				if (e->pick) text(r, x + 3, yy, "x", C_VOID);
			}
			/* The slot IS the micromod, so it leads. */
			text(r, x + 14, yy, e->slot, C_TEXT_DIM);
			{
				/* CLIPPED TO WHERE THE STATUS BEGINS. Bubble Guppies names
				 * a micromod "New clickable decoration for Save the Puppy
				 * Park", which is 20 characters past the column the status
				 * is right-aligned in, so the two drew straight through
				 * each other and the row read "...Save the Pupinstalledark".
				 * Nothing was wrong with either string; there was simply no
				 * rule about what happens when they meet. */
				const char *tag = e->dep ? "cartridge"
				                : e->installed ? "installed"
				                : e->avail ? "on server" : "";
				int tagx = d.x + d.w - 16 - text_w(tag);
				int room = (tag[0] ? tagx - 6 : d.x + d.w - 10) - (x + 60);
				char nm[96];
				const char *full = e->name[0] ? e->name : "(unnamed)";
				int fit = room / GLYPH_ADV;
				if (fit < 1) fit = 1;
				if (fit > (int)sizeof(nm) - 1) fit = (int)sizeof(nm) - 1;
				snprintf(nm, (size_t)fit + 1, "%s", full);
				/* An ellipsis rather than a hard cut, so a clipped name
				 * looks clipped instead of looking like the name. */
				if ((int)strlen(full) > fit && fit >= 2)
					nm[fit - 1] = nm[fit - 2] = '.';
				text(r, x + 60, yy, nm,
				     e->installed ? C_TEXT : can ? C_TEXT : C_TEXT_DIM);
				if (tag[0])
					text(r, tagx, yy, tag,
					     e->installed ? C_DIMMEST : C_TEXT_DIM);
			}
		}
		if (g_mm_n > vis) {
			snprintf(line, sizeof(line), "%d-%d of %d", g_mm_top + 1,
			         g_mm_top + vis < g_mm_n ? g_mm_top + vis : g_mm_n, g_mm_n);
			text(r, d.x + d.w - 8 - text_w(line), d.y + d.h - 26, line, C_TEXT_DIM);
		}
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
		chip(r, x - 2, listy - 2, d.w - 16, listh + 2, C_EDGE_DK, 0);

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
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), L[i2],
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
		 * is asked for. Baked in at build time; "dev" in a working copy.
		 *
		 * AND A BARE "dev" IS NOT AN ANSWER. It reads as a version — people
		 * reported it as one, as "my copy says DEV" — when what it means is
		 * that this binary was built without being told which release it is.
		 * Releases cannot say it any more (tools/release.sh verifies the
		 * stamp is in the binary before it will publish), so anyone seeing
		 * this is running a working copy and is better served by being told
		 * so in words. 16 characters at 6px fits the 210px dialog. */
		text(r, lx + 62, ly + 38,
		     strcmp(TADPOLE_VERSION, "dev") ? TADPOLE_VERSION
		                                    : "dev (unreleased)", C_ACCENT);
		/* NAMED, NOT ASSUMED. This line read "qemu-user" whether or not
		 * qemu was anywhere near the machine, which was survivable while
		 * qemu ran every guest and is simply false now that glasspole
		 * does. It is also the first thing a bug report needs, and the
		 * only place in the program that answers it. */
		{
			char line[40];
			snprintf(line, sizeof(line), "%s + guest shim +", ui_engine_name());
			text(r, lx, ly + 60, line, C_TEXT_DIM);
		}
		text(r, lx, ly + 70, "software GLES1 rasteriser", C_TEXT_DIM);
		text(r, lx, ly + 84, "A childhood preserved.", C_ACCENT);
		break;
	}
	case M_GFX: {
		char buf[32];
		row_check(r, &d, 0, "Enable OpenGL", g_cfg.gl,
		          row_hit(&d, 0, g_mx, g_my));
		/* NOT A CHOICE ANY MORE. The software rasteriser is deprecated: it
		 * samples one texture unit and ignores the blend factors, so it draws
		 * busy titles wrongly rather than slowly, and two rendering bugs this
		 * month were invisible on it. Turning replay off from here would be
		 * choosing that quietly, which is the thing being removed. It stays
		 * visible, ticked and greyed; TADPOLE_GL_SOFTWARE=1 is the way to the
		 * old path for anyone deliberately comparing the two. */
		row_check_locked(r, &d, 1, "Host GPU replay (HLE)");
		/* Host-GPU only, which since the deprecation means always: these two
		 * used to grey out when replay was off, and replay can no longer be
		 * off. */
		if (g_cfg.msaa) snprintf(buf, sizeof(buf), "%dx", g_cfg.msaa);
		else            snprintf(buf, sizeof(buf), "off");
		row_value(r, &d, 2, "Anti-aliasing", buf, row_hit(&d, 2, g_mx, g_my));
		/* Render scale. Also HLE-only, and worth keeping next to AA: they are
		 * the same idea spent two different ways. */
		if (g_cfg.render_scale > 1) snprintf(buf, sizeof(buf), "%dx", g_cfg.render_scale);
		else                        snprintf(buf, sizeof(buf), "native");
		row_value(r, &d, 3, "Render scale", buf, row_hit(&d, 3, g_mx, g_my));
		/* "Stop if HLE falls back" used to live here. It described a choice
		 * that no longer exists for a user: replay dying now raises a dialog
		 * either way, and the setting only chose between that and killing the
		 * title outright. TADPOLE_HLE_STRICT=1 still does the latter, which is
		 * a debugging tool and belongs in the environment, not in a menu. */
		if (g_cfg.frame_cap) snprintf(buf, sizeof(buf), "%d fps", g_cfg.frame_cap);
		else                 snprintf(buf, sizeof(buf), "uncapped");
		row_value(r, &d, 4, "Frame cap", buf, row_hit(&d, 4, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d deg", g_cfg.rotate);
		row_value(r, &d, 5, "Orientation", buf, row_hit(&d, 5, g_mx, g_my));
		/* Directly under the orientation it overrides, because that is the
		 * row someone is looking at when they wonder why the window turned. */
		row_check(r, &d, 6, "Turn with the app", g_cfg.auto_rotate,
		          row_hit(&d, 6, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%dx", g_cfg.scale);
		/* Rows 7 and 8, not 6 and 7: "Turn with the app" was added above them
		 * on main while the touch metrics were being written here. */
		row_value(r, &d, 7, "Window scale", buf, row_hit(&d, 7, g_mx, g_my));
		row_check(r, &d, 8, "Touch debug overlay", g_cfg.touch_debug,
		          row_hit(&d, 8, g_mx, g_my));
		/* On the button row, to the LEFT of Close. It used to be a fixed 30
		 * pixels off the floor, which is above the button at a height of 13
		 * and straight through it at 22. */
		text(r, d.x + 10, btn_row_y(&d) + text_dy(ui_btn_h()),
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
		/* Named for what it does rather than for what it skips. "Play the boot
		 * animation" would have been the same switch the other way up, and the
		 * wrong way up: the default is the fast one, and a default should read
		 * as the plain choice rather than as a feature being withheld. */
		row_check(r, &d, 1, "Fast Boot - skip the logo and video",
		          g_cfg.fast_boot, row_hit(&d, 1, g_mx, g_my));
		/* The switch for everything ui_bar_h() and its neighbours decide. Off
		 * restores the original metrics exactly, for anyone driving this with
		 * a mouse who would rather have the rows back. */
		row_check(r, &d, 2, "Touch-sized menus and buttons",
		          g_cfg.touch_ui, row_hit(&d, 2, g_mx, g_my));
		/* Directly under the other switch for how the chrome looks, and named
		 * for the thing on screen rather than for the technique: nobody goes
		 * looking for "backdrop downsample chain". What it costs is said in
		 * the status line when it is turned off, which is the moment anyone
		 * cares. */
		row_check(r, &d, 3, "Frosted glass behind panels",
		          g_cfg.frost, row_hit(&d, 3, g_mx, g_my));
		text(r, d.x + 10, row_y(&d, 4) + 2, "Games folder:", C_TEXT);
		path_tail(buf, sizeof(buf),
		          g_cfg.games_dir[0] ? g_cfg.games_dir : "(not chosen yet)");
		text(r, d.x + 14, row_y(&d, 4) + 12, buf,
		     g_cfg.games_dir[0] ? C_ACCENT : C_TEXT_DIM);
		{
			SDL_Rect b = { d.x + 10, row_y(&d, 6) + 2, 76, ui_btn_h() };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
			text_c(r, b.x, b.w, b.y + text_dy(b.h), "Game Library",
			       hot ? C_ACCENT : C_TEXT);
		}
		{
			SDL_Rect b = { d.x + 94, row_y(&d, 6) + 2, 96, ui_btn_h() };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
			text_c(r, b.x, b.w, b.y + text_dy(b.h), "Setup Wizard",
			       hot ? C_ACCENT : C_TEXT);
		}
		break;
	}
	case M_PAD: {
		/* THE ON-SCREEN CONTROLS COME FIRST, and the keyboard list that used to
		 * be the whole dialog is underneath. This is the build for a
		 * touchscreen: the keys are the fallback here, not the subject. */
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
		char buf[16];
		int ky;

		row_check(r, &d, 0, "On-screen D-pad and Home",
		          g_cfg.pad_on, row_hit(&d, 0, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d%%", g_cfg.pad_size);
		row_value(r, &d, 1, "Size", buf, row_hit(&d, 1, g_mx, g_my));
		snprintf(buf, sizeof(buf), "%d%%", g_cfg.pad_opacity);
		row_value(r, &d, 2, "Opacity", buf, row_hit(&d, 2, g_mx, g_my));
		row_value(r, &d, 3, "D-pad corner", g_cfg.pad_left ? "LEFT" : "RIGHT",
		          row_hit(&d, 3, g_mx, g_my));
		/* A and B are missing from this list on purpose — see the note at the
		 * top of tadpole_pad.h. Saying so here is cheaper than answering it. */
		text(r, d.x + 10, row_y(&d, 4) + 1,
		     "A and B: the title draws its own.", C_DIMMEST);

		ky = row_y(&d, 5) + 4;
		fill(r, d.x + 8, ky - 4, d.w - 16, 1, C_EDGE_DK);
		text(r, d.x + 10, ky + 2, "KEYBOARD", C_TEXT_DIM);
		/* TWO COLUMNS WHEN THE ROWS ARE TALL. Eight bindings down one side is
		 * eighty pixels the touch-sized rows above have already spent, and
		 * this list is reference material — the last thing in the dialog that
		 * should push a setting off the bottom. At 240 wide two columns of
		 * seventeen glyphs fit with room to spare. */
		{
			int n = (int)(sizeof rows / sizeof *rows);
			int cols = ui_touch_ui() ? 2 : 1;
			int per = (n + cols - 1) / cols;
			for (i = 0; i < n; i++)
				text(r, d.x + 10 + (i / per) * (d.w / 2 - 6),
				     ky + 14 + (i % per) * 10, rows[i], C_TEXT);
			text(r, d.x + 10, ky + 18 + per * 10,
			     "Remapping: not yet.", C_DIMMEST);
		}
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
		chip(r, d.x + 8, ly, d.w - 16, 7, C_VOID, 0);
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

		chip(r, d.x + 8, ly + 12, d.w - 16, PROG_LINES * 9 + 4, C_VOID, 0);
		for (i2 = first; i2 < g_prog_n; i2++)
			text(r, d.x + 12, ly + 16 + (i2 - first) * 9,
			     g_prog[i2 % PROG_LINES], C_TEXT_DIM);

		if (!g_prog_running)
			text(r, d.x + 10, d.y + d.h - 30,
			     g_prog_ok ? "Finished." : "Failed - see the lines above.",
			     g_prog_ok ? C_ACCENT : C_TEXT);
		break;
	}
	case M_MSG: {
		/* WRAPPED, because the body is not always ours to keep short. Two of
		 * the four callers pass a fixed sentence, but "Could not check" passes
		 * netssl's reason — which is a full explanation of a certificate
		 * failure and the single most useful line the update check produces.
		 * Drawn unwrapped it ran off the panel and took the half naming the
		 * cause with it, the same way the progress panel used to truncate. */
		int ly = d.y + 24, cols = (d.w - 20) / GLYPH_ADV, nl = 0;
		const char *p = g_msg_body;
		while (*p && nl < 4) {
			int take = (int)strlen(p), brk;
			if (take > cols) {
				take = cols;
				for (brk = take; brk > cols / 4; brk--)
					if (p[brk] == ' ') { take = brk; break; }
			}
			{
				char line[128];
				int n = take < (int)sizeof(line) - 1 ? take : (int)sizeof(line) - 1;
				memcpy(line, p, (size_t)n);
				line[n] = 0;
				text(r, d.x + 10, ly, line, C_TEXT);
			}
			ly += 10; nl++;
			p += take;
			while (*p == ' ') p++;
		}
		if (g_confirm) {
			SDL_Rect b = { d.x + d.w - 96, btn_row_y(&d), 44, ui_btn_h() };
			int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
			chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
			text_c(r, b.x, b.w, b.y + text_dy(b.h), "Erase", hot ? C_ACCENT : C_TEXT);
			/* Below the body, wherever it ended, rather than at a fixed offset
			 * that a two-line body would collide with. */
			text(r, d.x + 10, ly + 6, "Games and firmware downloads", C_TEXT_DIM);
			text(r, d.x + 10, ly + 16, "are not touched.", C_TEXT_DIM);
		}
		break;
	}
	case M_WIZARD: {
		struct prereq pq;
		int bx = d.x + 62, by = d.y + 20, i2;
		Uint8 wiz_alpha;
		char welcome[48];
		const char *TITLES[WIZ_PAGES] = {
			welcome, "System files", "Didj support", "Who is playing?",
			"Games", "Ready"
		};
		snprintf(welcome, sizeof(welcome), "Welcome to %s", ui_brand_name());
		prereq_check(&pq);

		/* Banner down the left — the wizard's whole visual signature. Its
		 * bottom-left corner follows the panel's, so the sidebar sits inside
		 * the glass rather than cutting across it; the two right-hand corners
		 * stay square because that edge is a seam against the content, not an
		 * outside edge. A faint glow behind the mark picks up the same light
		 * the idle backdrop has. */
		{
			float brad = rr_radius_panel(d.w, d.h) - 1.0f;
			/* Both stops stay in the green (or the blue). Fading to C_SHADOW
			 * was tried and reads as a black slab bolted to the side of the
			 * panel; letting the glass through towards the bottom keeps it a
			 * dark column of the same material. */
			rr_geom(r, NULL, NULL, d.x + 1.0f, (float)d.y + 12.0f, 56.0f,
			       (float)d.h - 30.0f, 0, 0, 0, brad,
			       C_VOID, 232, C_VOID, 172);
			glow(r, d.x + 29, d.y + 44, 46, C_ACCENT, 34);
		}
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

		/* PAGE TURN. Only the page's own content moves — the sidebar, the
		 * step markers, the heading rule and the Back/Next row are the parts
		 * that stay put between pages, and sliding them too would turn a step
		 * forward into the whole dialog twitching.
		 *
		 * Nested inside the entrance fade rather than replacing it: opening
		 * the wizard fades the panel in AND deals its first page, and one
		 * multiplied by the other is what makes that read as a single motion.
		 * `wiz_alpha` is restored right after the switch. */
		{
			static int wseen = -1;
			anim_watch(g_wiz_page, &wseen, &g_wiz_at);
		}
		{
			float wt = anim_t(g_wiz_at, 130);
			wiz_alpha = g_alpha;
			g_alpha = (Uint8)(wiz_alpha * wt);
			bx += (int)((1.0f - wt) * 9.0f);
		}

		switch (g_wiz_page) {
		case WIZ_WELCOME:
			/* The Windows build says cross-platform because being able to
			 * exist there is the reason its engine was written: qemu-user
			 * is Linux-only. This build says the plain line — not because
			 * the engine underneath differs any more (it does not), but
			 * because "cross-platform" is a claim about where you can get
			 * it, and the thing you can get here runs on Linux. */
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
				/* NOT "qemu-arm is missing" any more: glasspole satisfies
				 * this check and is what a default install runs, so naming
				 * one of the two engines would send people after the one
				 * they do not need. */
				text(r, bx, by + 78, GL_CHECK_0 " No ARM engine installed.", C_ACCENT);
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
				SDL_Rect b = { bx, by + 54, 76, ui_btn_h() };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), "Browse...", hot ? C_ACCENT : C_TEXT);
			}
			/* THE SYSROOT NEEDS ITS OWN BUTTON. Installing firmware builds it,
			 * but the two can get out of step — an interrupted install, or an
			 * Erase that took the sysroot with it — and then the page reported
			 * "Sysroot not built" with no way to act on it. */
			if (pq.rootfs && !pq.sysroot) {
				SDL_Rect b = { bx + 84, by + 54, 96, ui_btn_h() };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), "Build sysroot",
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
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL_HI, 1);
				text_c(r, b.x, b.w, b.y + 4, "Online System Update",
				       hot ? C_ACCENT : C_TEXT);
				text(r, b.x + b.w + 8, b.y + 4, "124 MB", C_TEXT_DIM);
				text(r, bx, by + 111, "from digitalcontent.leapfrog.com",
				     C_TEXT_DIM);
			}
			break;
		/* ---- Didj games (optional) ----
		 *
		 * The Didj is a 2008 LeapFrog handheld two generations older than the
		 * LeapPad2. Its games run — the firmware already carries the
		 * compatibility layer and loads it on every boot — but the DATA that
		 * layer needs is LeapFrog's, so Tadpole cannot ship it and this page is
		 * where a user supplies it.
		 *
		 * SHAPED LIKE THE SYSTEM PAGE ON PURPOSE: same tick list, same rule,
		 * same Browse buttons. Two files, reported separately, because they are
		 * two different downloads and "which one am I missing" is the only
		 * question this page has to answer.
		 *
		 * Nothing here is required. A user with no Didj dumps presses Next. */
		/* LAYOUT MIRRORS WIZ_SYSTEM ON PURPOSE — same tick list at the top, same
		 * rule under it, same 76px "Browse..." buttons — because it is the same
		 * kind of page and the two sit next to each other in the flow.
		 *
		 * ASCII ONLY IN UI STRINGS. The bitmap font in tadpole_font.h has 96
		 * glyphs, ASCII 32..127, and text() draws anything outside that as the
		 * fallback arrow. An em-dash in a label therefore renders as a stray
		 * "▶" — which is exactly what the first draft of this page did, three
		 * times. Nothing else in this file uses one; hyphens throughout.
		 *
		 * BUTTON LABELS STAY SHORT for the same reason they are "Browse..." on
		 * the system page: text_c centres inside a fixed box and does not clip,
		 * so a label wider than its button spills out both sides. At GLYPH_ADV
		 * of 6px, "Choose ControlOverlay.zip" is 150px and did precisely that in
		 * a 132px box. The filename belongs in the line above, where there is
		 * room for it. */
		case WIZ_DIDJ:
			text(r, bx, by, "Optional. Only for Didj game dumps.", C_TEXT_DIM);
			{
				static const char *NAMES[2] = { "Didj compatibility files",
				                                "Controller overlay" };
				int ok[2], i3;
				ok[0] = pq.didj; ok[1] = pq.didj_overlay;
				for (i3 = 0; i3 < 2; i3++) {
					int yy = by + 14 + i3 * 11;
					text(r, bx, yy, ok[i3] ? GL_CHECK_1 : GL_CHECK_0,
					     ok[i3] ? C_ACCENT : C_TEXT_DIM);
					text(r, bx + 12, yy, NAMES[i3], ok[i3] ? C_TEXT : C_TEXT_DIM);
					text(r, bx + d.w - 132, yy, ok[i3] ? "ready" : "missing",
					     ok[i3] ? C_ACCENT : C_TEXT_DIM);
				}
				fill(r, bx, by + 38, d.w - 76, 1, C_EDGE_DK);
			}
			/* THE ORDER MATTERS AND THE PAGE SAYS SO. The compatibility files are
			 * installed INTO LF/Base, so without system files there is nowhere to
			 * put them, and the button would fail with an error about a path
			 * rather than about the step that was skipped. */
			if (!pq.rootfs || !pq.sysroot) {
				text(r, bx, by + 46, "Install the system files first.", C_TEXT);
				text(r, bx, by + 58, "These are added to them.", C_TEXT_DIM);
				break;
			}
			/* TWO WAYS TO GET EACH PIECE, side by side: fetch it, or point at one
			 * already on disk. Browse comes first because it is the path that
			 * always works — Download depends on a source being published, and
			 * for the compatibility files that question is still open. */
			text(r, bx, by + 46, "1. DIDJ.zip", C_TEXT);
			text(r, bx + 12, by + 57, "the Leapster Explorer's Didj files",
			     C_TEXT_DIM);
			{
				SDL_Rect b = { bx + 12, by + 69, 76, ui_btn_h() };
				SDL_Rect g = { bx + 96, by + 69, 76, ui_btn_h() };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				int hotg = inside(g_mx, g_my, g.x, g.y, g.w, g.h);
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), "Browse...", hot ? C_ACCENT : C_TEXT);
				chip(r, g.x, g.y, g.w, g.h, hotg ? C_BAR_HI : C_PANEL, 1);
				text_c(r, g.x, g.w, g.y + text_dy(g.h), "Download", hotg ? C_ACCENT : C_TEXT);
				if (pq.didj)
					text(r, g.x + g.w + 10, g.y + text_dy(g.h), "installed", C_ACCENT);
			}
			text(r, bx, by + 90, "2. ControlOverlay.zip", C_TEXT);
			text(r, bx + 12, by + 101, "on-screen buttons the LeapPad2 lacks",
			     C_TEXT_DIM);
			{
				SDL_Rect b = { bx + 12, by + 113, 76, ui_btn_h() };
				SDL_Rect g = { bx + 96, by + 113, 76, ui_btn_h() };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				int hotg = inside(g_mx, g_my, g.x, g.y, g.w, g.h);
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), "Browse...", hot ? C_ACCENT : C_TEXT);
				chip(r, g.x, g.y, g.w, g.h, hotg ? C_BAR_HI : C_PANEL, 1);
				text_c(r, g.x, g.w, g.y + text_dy(g.h), "Download", hotg ? C_ACCENT : C_TEXT);
				if (pq.didj_overlay)
					text(r, g.x + g.w + 10, g.y + text_dy(g.h), "installed", C_ACCENT);
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
				chip(r, f.x, f.y, f.w, f.h, C_VOID, g_prof_focus ? 1 : 0);
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
				chip(r, g.x, g.y, g.w, g.h, hot ? C_BAR_HI : C_PANEL, 1);
				if (g_prof_grade <= 0) snprintf(gb, sizeof(gb), "Pre-K");
				else                   snprintf(gb, sizeof(gb), "Grade %d", g_prof_grade);
				text_c(r, g.x, g.w, g.y + 4, gb, hot ? C_ACCENT : C_TEXT);
			}

			{
				SDL_Rect p2 = { bx + 40, fy + 40, 60, 14 };
				int hot = inside(g_mx, g_my, p2.x, p2.y, p2.w, p2.h);
				text(r, bx, fy + 44, "Photo", C_TEXT);
				chip(r, p2.x, p2.y, p2.w, p2.h, hot ? C_BAR_HI : C_PANEL, 1);
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
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL_HI, 1);
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
				SDL_Rect b = { bx, by + 62, 96, ui_btn_h() };
				int hot = inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), "Open Library",
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
		g_alpha = wiz_alpha;      /* the page turn ends here; the frame's own
		                           * entrance fade carries on below */

		/* buttons */
		{
			static const char *L[3] = { "Back", "Next", "Cancel" };
			for (i2 = 0; i2 < 3; i2++) {
				SDL_Rect b = wiz_btn(&d, i2);
				int on = !(i2 == 0 && g_wiz_page == 0);
				int hot = on && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				const char *lab = (i2 == 1 && g_wiz_page == WIZ_PAGES - 1)
				                ? "Finish" : L[i2];
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), lab,
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

		/* Where these came from, and how many there are.
		 *
		 * THE PATH IS TRUNCATED TO THE ROOM LEFT BY THE COUNT, not to a fixed
		 * 40 characters. In portrait the dialog is 264 logical pixels wide,
		 * 40 glyphs is 240 of them, and the count was drawn straight over the
		 * tail of the path — the header read "…/leappad-emu/games473". */
		{
			char shown[40];
			int room;
			snprintf(buf, sizeof(buf), "%d", g_gm_n);
			room = (d.w - 14 - 6 - text_w(buf)) / GLYPH_ADV;
			if (room < 8) room = 8;
			if (room > (int)sizeof(shown) - 1) room = (int)sizeof(shown) - 1;
			path_tail(shown, (size_t)room + 1,
			          g_gm_dir[0] ? g_gm_dir : "(no folder chosen)");
			text(r, d.x + 6, d.y + 15, shown, C_TEXT_DIM);
			text(r, d.x + d.w - 8 - text_w(buf), d.y + 15, buf, C_TEXT_DIM);
		}

		chip(r, lx, ly, lw2, lh2, C_VOID, 0);

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

			if (k == g_gm_sel) rfill(r, lx + 1, ry - 1, lw2 - 2, GM_ROW_H, C_PANEL_HI, 178);

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
				chip(r, tx, ry, 13, 13, C_PANEL, 0);
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
			chip(r, px, py, pw - 4, lh2, C_VOID, 0);
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
			int by = btn_row_y(&d), i3;
			static const char *L[4] = { "Folder...", "Rescan", "", "" };
			int xs[2] = { d.x + 6, d.x + 6 + 56 };
			int ws[2] = { 52, 44 };
			/* MICROMODS BELONGS WITH THE LIBRARY, not with the launcher.
			 * The launcher is a list of things to START; this is a question
			 * about a title you OWN, which is what the library is a list of.
			 * It reads the selected row's PackageID, so it works whether or
			 * not that title is installed yet. */
			{
				SDL_Rect mb;
				if (gm_micro_rect(&d, &mb)) {
					int on = g_gm_n > 0;
					int hot = on && inside(g_mx, g_my, mb.x, mb.y, mb.w, mb.h);
					chip(r, mb.x, mb.y, mb.w, mb.h, hot ? C_BAR_HI : C_PANEL, 1);
					text_c(r, mb.x, mb.w, mb.y + text_dy(mb.h), "Micromods",
					       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
				}
			}
			for (i3 = 0; i3 < 2; i3++) {
				int hot = inside(g_mx, g_my, xs[i3], by, ws[i3], ui_btn_h());
				int on = (i3 == 0) || g_gm_dir[0];
				chip(r, xs[i3], by, ws[i3], ui_btn_h(),
				     hot && on ? C_BAR_HI : C_PANEL, 1);
				text_c(r, xs[i3], ws[i3], by + text_dy(ui_btn_h()), L[i3],
				       !on ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
			{
				int n = games_checked();
				SDL_Rect b = { d.x + d.w - 8 - 48 - 62, by, 62, ui_btn_h() };
				int hot = n && inside(g_mx, g_my, b.x, b.y, b.w, b.h);
				if (n) snprintf(buf, sizeof(buf), "Install %d", n);
				else   snprintf(buf, sizeof(buf), "Install");
				chip(r, b.x, b.y, b.w, b.h, hot ? C_BAR_HI : C_PANEL, 1);
				text_c(r, b.x, b.w, b.y + text_dy(b.h), buf,
				       !n ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
			}
		}
		break;
	}
	case M_FILES: {
		int ly = d.y + 16, i2;
		int rh = ui_list_row_h();
		char shown[49];
		path_tail(shown, sizeof(shown), g_fb_dir);
		text(r, d.x + 6, ly, shown, C_TEXT_DIM);

		g_fb_rows = (d.y + d.h - 20 - (ly + 13)) / rh;
		if (g_fb_rows < 1) g_fb_rows = 1;
		if (g_fb_top > g_fb_n - FB_ROWS) g_fb_top = g_fb_n - FB_ROWS;
		if (g_fb_top < 0) g_fb_top = 0;

		chip(r, d.x + 6, ly + 11, d.w - 12, FB_ROWS * rh + 2, C_VOID, 0);
		for (i2 = 0; i2 < FB_ROWS && g_fb_top + i2 < g_fb_n; i2++) {
			int k = g_fb_top + i2;
			int ry = ly + 13 + i2 * rh + text_dy(rh) - 2;
			char nm[47];
			unsigned col = g_fb_list[k].isdir ? C_ACCENT : C_TEXT;
			if (k == g_fb_sel) {
				rfill(r, d.x + 8, ly + 12 + i2 * rh, d.w - 16, rh,
				      C_PANEL_HI, 178);
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
		chip(r, cb.x, cb.y, cb.w, cb.h, hot ? C_BAR_HI : C_PANEL, 1);
		text_c(r, cb.x, cb.w, cb.y + text_dy(cb.h),
		       g_modal == M_FILES ? "Cancel" : "Close",
		       busy ? C_TEXT_DIM : hot ? C_ACCENT : C_TEXT);
	}
	/* TWO ACTIONS, IN THE ORDER THEY ARE USED. Scan asks LeapFrog what this
	 * title's bonus content is and lists it by name; Install fetches the
	 * ticked rows. The child still turns each one on in the game's own menu,
	 * because that choice is stored per profile in the save data. */
	if (g_modal == M_MICROMODS) {
		SDL_Rect in = { cb.x - 62, cb.y, 58, ui_btn_h() };
		SDL_Rect sc = { cb.x - 62 - 62, cb.y, 58, ui_btn_h() };
		char picked[256];
		int npick = ui_micromods_picked(picked, sizeof(picked));
		int hot = npick && inside(g_mx, g_my, in.x, in.y, in.w, in.h);
		int hot2 = inside(g_mx, g_my, sc.x, sc.y, sc.w, sc.h);
		chip(r, sc.x, sc.y, sc.w, sc.h, hot2 ? C_BAR_HI : C_PANEL, 1);
		text_c(r, sc.x, sc.w, sc.y + text_dy(sc.h), "Scan", hot2 ? C_ACCENT : C_TEXT);
		chip(r, in.x, in.y, in.w, in.h, hot ? C_BAR_HI : C_PANEL, 1);
		if (npick) {
			char lab[24];
			snprintf(lab, sizeof(lab), "Install %d", npick);
			text_c(r, in.x, in.w, in.y + text_dy(in.h), lab, hot ? C_ACCENT : C_TEXT);
		} else {
			text_c(r, in.x, in.w, in.y + text_dy(in.h), "Install", C_TEXT_DIM);
		}
	}
	if (g_modal == M_FILES) {
		SDL_Rect ok = { cb.x - 46, cb.y, 42, ui_btn_h() };
		int hot = inside(g_mx, g_my, ok.x, ok.y, ok.w, ok.h);
		chip(r, ok.x, ok.y, ok.w, ok.h, hot ? C_BAR_HI : C_PANEL, 1);
		text_c(r, ok.x, ok.w, ok.y + text_dy(ok.h), "Open", hot ? C_ACCENT : C_TEXT);
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
			SDL_Rect uf = { ok.x - 74, ok.y, 70, ui_btn_h() };
			int h2 = inside(g_mx, g_my, uf.x, uf.y, uf.w, uf.h);
			chip(r, uf.x, uf.y, uf.w, uf.h, h2 ? C_BAR_HI : C_PANEL, 1);
			text_c(r, uf.x, uf.w, uf.y + text_dy(uf.h), "Use folder",
			       h2 ? C_ACCENT : C_TEXT);
		}
	}
}

void ui_draw_idle(SDL_Renderer *ren, int lw, int lh)
{
	int ih = lh - UI_BAR_H;
	int cy = UI_BAR_H + ih / 2;

	/* NOT A FLAT VOID ANY MORE. This was one fill of C_VOID, which is very
	 * nearly black — fine as a backdrop for a picture, but it is also what
	 * the glass panels are made of, and frosted glass over nothing looks like
	 * a hole. The bar is drawn after this and paints over the overspill at
	 * the top.
	 *
	 * NO LAMP BEHIND THE LOGO. There were two glow fans centred on the mark,
	 * and what they actually looked like was a torch pointed at the middle of
	 * the window: a bright blob with the logo sitting in it, drawing the eye
	 * to empty space rather than to the mark. Depth here comes from the
	 * gradient and from the edges falling away instead — a vignette frames
	 * the picture without ever becoming a thing you look at, and it still
	 * gives the glass a tonal range to pick up.
	 *
	 * The gradient runs light-at-the-top to dark-at-the-bottom, which is the
	 * direction every other lit thing in this UI agrees on: panel bodies,
	 * their rims, and the chips all brighten upward. */
	rr_geom(ren, NULL, NULL, -2.0f, (float)UI_BAR_H - 2.0f, (float)lw + 4.0f,
	       (float)ih + 4.0f, 0, 0, 0, 0,
	       C_BAR, 255, C_VOID, 255);
	/* An ellipse through the four corners: with rx = w/2 and ry = h/2 scaled
	 * by sqrt(2), (w/2)^2/rx^2 + (h/2)^2/ry^2 = 1 lands the rim exactly on
	 * the corners, so the picture darkens evenly towards its own frame
	 * instead of towards a circle drawn inside it.
	 *
	 * AND IT GOES WHEN THE FROST DOES, because half of why it is here is to
	 * give the glass a tonal range to pick up, and with no glass there is
	 * nothing picking anything up. What is left is one blend of the whole
	 * screen, every frame, for a corner darkening nobody is meant to notice —
	 * on the idle screen that is the entire per-frame cost of the chrome, and
	 * the idle screen is where a slow device is looked at longest. The
	 * gradient underneath stays: it is opaque, so it costs what the clear it
	 * replaced cost and the backdrop is not flat black without it. */
	if (m_frost())
		radial(ren, lw / 2, cy, 0.7071f * (float)lw, 0.7071f * (float)ih,
		      C_SHADOW, 0, 138);
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

/* The entrance, and the guarantee that it is undone. A modal fades up and
 * rises the last ten pixels into place; cur_dlg() carries the offset so the
 * hit test follows. 150ms — long enough to read as motion, short enough that
 * nobody waiting to click a Close button ever notices it. */
static void draw_dialog(SDL_Renderer *r, int lw, int lh)
{
	static int seen = M_NONE;

	anim_watch((int)g_modal, &seen, &g_modal_at);
	if (g_modal == M_NONE) return;

	g_alpha = (Uint8)(255.0f * anim_t(g_modal_at, 150));
	draw_dialog_body(r, lw, lh);
	g_alpha = 255;
}

void ui_draw(SDL_Renderer *r, int lw, int lh)
{
	/* The flick lives here because this is the only thing in the file that
	 * happens every frame. It has to run before the draw rather than after, or
	 * the list is painted one frame behind the position it has moved to. */
	list_fling(lw, lh);
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
	/* `apps` opens the launcher at the top; `apps<N>` selects the Nth entry,
	 * which is how a scrolled list gets captured — the view follows the
	 * selection, so `apps400` proves the thing scrolls without anyone
	 * touching a wheel. `appsend` goes to the last one. */
	/* micromods<ProductID>: the launcher's Micromods screen for one title.
	 * Bare `micromods` picks the first app, which is enough to see the
	 * empty-state wording. */
	else if (!strncmp(name, "micromods", 9)) {
		ap_reload();
		g_modal = M_MICROMODS;
		if (name[9]) {
			int i, best = -1;
			/* A PRODUCTID IS NOT A TITLE. Two different games can share one:
			 * 0x00180025 is SpongeBob's Clam Prix as LPAD/MULT and the French
			 * "M. Crayon sauve Bourgribouille" as LST3, and taking the first
			 * match alphabetically named this screen after the wrong game.
			 * The real screen has no such problem — it is handed the row the
			 * user selected — so this only has to pick sensibly: prefer the
			 * LeapPad2 build, which is the one Tadpole runs. */
			snprintf(g_mm_title, sizeof(g_mm_title), "%s", name + 9);
			for (i = 0; i < g_ap_n; i++) {
				if (!strstr(g_ap[i].pkg, name + 9)) continue;
				if (best < 0) best = i;
				if (!strncmp(g_ap[i].pkg, "MULT", 4)) { best = i; break; }
			}
			if (best >= 0) {
				char fam[8];
				const char *dash = strchr(g_ap[best].pkg, '-');
				snprintf(g_mm_title, sizeof(g_mm_title), "%s", g_ap[best].name);
				if (dash && (size_t)(dash - g_ap[best].pkg) < sizeof(fam)) {
					memcpy(fam, g_ap[best].pkg, (size_t)(dash - g_ap[best].pkg));
					fam[dash - g_ap[best].pkg] = 0;
					mm_reload(name + 9, fam);
				} else {
					mm_reload(name + 9, NULL);
				}
			} else {
				mm_reload(name + 9, NULL);
			}
		} else if (g_ap_n) {
			const char *pkg = g_ap[0].pkg;
			const char *a = strchr(pkg, '-'), *b = a ? strchr(a + 1, '-') : NULL;
			if (a && b) {
				char prod[16];
				size_t n2 = (size_t)(b - a - 1);
				if (n2 < sizeof(prod)) {
					memcpy(prod, a + 1, n2); prod[n2] = 0;
					char fam2[8];
					size_t fl = (size_t)(a - pkg);
					snprintf(g_mm_title, sizeof(g_mm_title), "%s", g_ap[0].name);
					if (fl < sizeof(fam2)) { memcpy(fam2, pkg, fl); fam2[fl] = 0; }
					else fam2[0] = 0;
					mm_reload(prod, fam2[0] ? fam2 : NULL);
				}
			}
		}
	}
	else if (!strncmp(name, "apps", 4)) {
		ap_reload();
		g_modal = M_APPS;
		if (!strcmp(name + 4, "end")) g_ap_sel = g_ap_n - 1;
		else if (name[4])             g_ap_sel = atoi(name + 4);
		if (g_ap_sel < 0) g_ap_sel = 0;
		if (g_ap_sel > g_ap_n - 1) g_ap_sel = g_ap_n - 1;
	}
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
		/* A REAL over-long line, so this capture also proves the wrap. This is
		 * the message a Windows user actually saw cut off at 55 characters,
		 * back when this panel truncated instead of wrapping. */
		ui_progress_line("C:/Users/tadpole/AppData/Local/Programs/Glasspole/"
		                 "build/deps/python/python.exe: can't open file "
		                 "'C:/Users/tadpole/AppData/Local/Programs/Glasspole/"
		                 "tools/install-didj.py': [Errno 2] No such file or "
		                 "directory");
		/* An em-dash and an ellipsis, exactly as the tools write them, so the
		 * capture also proves the ASCII fold. */
		ui_progress_line("install-didj: no LF/Base here \xE2\x80\x94 install "
		                 "the system firmware first\xE2\x80\xA6");
	}
	/* The dialog the frame pump raises when host-GPU replay dies. Worth a
	 * capture state of its own: it is the one modal no sequence of clicks can
	 * produce, so without this nobody ever looks at it. */
	else if (!strcmp(name, "alert")) {
		char body[160];
		snprintf(body, sizeof(body),
		         "GPU render engine CRASHED. Please restart %s.",
		         ui_brand_name());
		ui_alert("Graphics", body);
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
			if (inside(g_mx, g_my, m->x, menu_item_y(m, i), w,
			           menu_item_h(&m->items[i])))
				g_hot_item = i;
	}
}

/* WHICH LIST IS ON SCREEN, and where it is. -> 0 when the modal has none.
 *
 * The rectangles are the same ones the draw uses, worked out from the same
 * helpers, because a body that disagreed with the rows inside it would scroll
 * when pressed just off a row and select when pressed just on one.
 *
 * The visible counts come from the globals the draw writes each frame rather
 * than being recomputed here: for the library and the micromods list that is
 * the only place they exist, and recomputing them would be a second opinion
 * about a number that already has one.
 */
static int list_view(const struct dlg *d, struct list_view *v)
{
	switch (g_modal) {
	case M_APPS: {
		int vis = ap_rows_fit(d);
		if (!g_ap_n) return 0;
		v->body.x = d->x + 8;
		v->body.y = ap_list_y(d);
		v->body.w = ap_row_w(d, vis);
		v->body.h = vis * AP_ROW_H;
		v->row_h  = AP_ROW_H;
		v->top    = &g_ap_top;
		v->n      = g_ap_n;
		v->vis    = vis;
		return 1;
	}
	case M_GAMES:
		if (!g_gm_n) return 0;
		v->body.x = d->x + 6;
		v->body.y = d->y + 26;
		v->body.w = gm_list_w(d);
		v->body.h = gm_list_h(d);
		v->row_h  = GM_ROW_H;
		v->top    = &g_gm_top;
		v->n      = g_gm_n;
		v->vis    = g_gm_rows;
		return 1;
	case M_MICROMODS:
		if (!g_mm_n) return 0;
		v->body.x = d->x + 4;
		v->body.y = d->y + 25;
		v->body.w = d->w - 16;
		v->body.h = g_mm_rows * ui_list_row_h() + 6;
		v->row_h  = ui_list_row_h();
		v->top    = &g_mm_top;
		v->n      = g_mm_n;
		v->vis    = g_mm_rows;
		return 1;
	case M_FILES:
		if (!g_fb_n) return 0;
		v->body.x = d->x + 6;
		v->body.y = d->y + 27;
		v->body.w = d->w - 12;
		v->body.h = FB_ROWS * ui_list_row_h() + 2;
		v->row_h  = ui_list_row_h();
		v->top    = &g_fb_top;
		v->n      = g_fb_n;
		v->vis    = FB_ROWS;
		return 1;
	case M_UPDATE: {
		/* Release notes, which scroll by LINE rather than by row — the same
		 * gesture over a different unit. The visible count is whatever the
		 * body has room for; the draw clamps against its own idea of that
		 * every frame, so a slightly generous number here costs nothing. */
		int vis = (d->h - 60) / 9;
		if (vis < 1) vis = 1;
		if (g_up_nnote <= vis) return 0;
		v->body.x = d->x + 6;
		v->body.y = d->y + 40;
		v->body.w = d->w - 12;
		v->body.h = vis * 9;
		v->row_h  = 9;
		v->top    = &g_up_scroll;
		v->n      = g_up_nnote;
		v->vis    = vis;
		return 1;
	}
	default:
		return 0;
	}
}

/* ---- events -------------------------------------------------------------- */

static void cycle_rotate(void)
{
	g_cfg.rotate = (g_cfg.rotate + 90) % 360;
	g_action = UI_ACT_RELAYOUT;
	ui_cfg_save();
}

/* WHERE CLOSING A PANEL GOES BACK TO — decided in ONE place.
 *
 * A panel that was opened from somewhere records that in g_fb_return, and
 * honouring it is the whole rule: a Cancel or a finished install returns the
 * user to setup, or to the library, rather than to an empty screen. But the
 * two ways to close a panel each enumerated the destinations they would
 * honour, and the lists had already drifted apart — the Close button allowed
 * the wizard and the Game Library, the Escape key allowed only the wizard, so
 * closing the same panel two ways landed you in two different places.
 *
 * The Micromods screen is what made that expensive. Scan and Install open the
 * progress panel over it, and neither list mentioned Micromods, so finishing
 * a scan dropped the user onto the idle screen — precisely when they wanted
 * to look at the list the scan had just filled in. Adding it to two lists
 * would have made three copies of one decision. There is one now, and any
 * panel that records where it came from returns there.
 */
static void modal_dismiss(void)
{
	/* Only panels that can be opened FROM something consult g_fb_return; a
	 * value left over from an earlier open must not teleport anyone. */
	if ((g_modal == M_FILES || g_modal == M_PROGRESS || g_modal == M_MSG) &&
	    g_fb_return != M_NONE) {
		g_modal = g_fb_return;
		g_fb_return = M_NONE;
	} else if (g_modal == M_GAMES) {
		g_modal = g_gm_return;
		g_gm_return = M_NONE;
	} else {
		g_modal = M_NONE;
	}
}

static int dialog_click(int lw, int lh, int mx, int my)
{
	struct dlg d = cur_dlg(lw, lh);
	SDL_Rect cb = close_rect(&d);

	if (g_modal == M_MICROMODS) {
		SDL_Rect in = { cb.x - 62, cb.y, 58, ui_btn_h() };
		SDL_Rect sc = { cb.x - 62 - 62, cb.y, 58, ui_btn_h() };
		char picked[256];
		/* Ask LeapFrog what this title has. Read-only: it downloads into the
		 * cache and writes the index this screen lists from, and installs
		 * nothing until something is ticked. */
		if (inside(mx, my, sc.x, sc.y, sc.w, sc.h)) {
			snprintf(g_action_path, sizeof(g_action_path), "%s", g_mm_product);
			g_action = UI_ACT_MICROMODS_SCAN;
			/* The progress panel opens over this screen; closing it comes
			 * back here, to the list the scan has just filled in. Without
			 * this it returned to nothing, which reads as "the scan threw
			 * my results away". */
			g_fb_return = M_MICROMODS;
			return 1;
		}
		if (inside(mx, my, in.x, in.y, in.w, in.h) &&
		    ui_micromods_picked(picked, sizeof(picked))) {
			/* PRODUCTID AND SLOTS, because the tool takes the choice as
			 * --only and there is no sense installing what was not asked
			 * for. */
			snprintf(g_action_path, sizeof(g_action_path), "%s",
			         g_mm_product);
			snprintf(g_action_arg, sizeof(g_action_arg), "%s", picked);
			g_action = UI_ACT_MICROMODS_INSTALL;
			g_fb_return = M_MICROMODS;   /* and back to the list afterwards */
			return 1;
		}
		if (inside(mx, my, cb.x, cb.y, cb.w, cb.h)) {
			g_modal = M_GAMES;     /* back to the list you came from */
			return 1;
		}
		/* A row toggles its tick. Same geometry the draw uses. */
		{
			struct dlg dd = cur_dlg(lw, lh);
			int x = dd.x + 8, y = dd.y + 29, i;
			for (i = 0; i < g_mm_rows && g_mm_top + i < g_mm_n; i++) {
				struct mm_entry *e = &g_mm[g_mm_top + i];
				if (!e->avail || e->installed || e->dep) continue;
				if (inside(mx, my, x - 4, y + i * ui_list_row_h() - 1,
				           dd.w - 16, ui_list_row_h())) {
					e->pick = !e->pick;
					return 1;
				}
			}
		}
	}

	if (g_modal == M_APPS && g_ap_n) {
		/* The same helpers the draw uses, so the clickable row and the drawn
		 * row cannot drift apart — they did once already, when a scrollbar
		 * appeared beside rows whose hit test still ran under it. */
		int x = d.x + 8, y = ap_list_y(&d), i;
		int vis = ap_rows_fit(&d);
		int rw = ap_row_w(&d, vis);
		for (i = 0; i < vis && g_ap_top + i < g_ap_n; i++) {
			if (!inside(mx, my, x - 2, y + i * AP_ROW_H, rw, AP_ROW_H - 2))
				continue;
			g_ap_sel = g_ap_top + i;
			snprintf(g_action_path, sizeof(g_action_path), "%s",
			         g_ap[g_ap_sel].pkg);
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
		    inside(mx, my, d.x + 62 + 84, d.y + 92, 96, ui_btn_h())) {
			g_action = UI_ACT_BUILD_SYSROOT;
			return 1;
		}
		if (g_wiz_page == WIZ_SYSTEM &&
		    inside(mx, my, d.x + 62, d.y + 92, 76, ui_btn_h())) {
			path_join(start, sizeof(start), g_proj, "sources");
			if (access(start, R_OK) != 0)
				default_browse_dir(start, sizeof(start));
			if (access(start, R_OK) != 0)
				path_join(start, sizeof(start), g_proj, "");
			/* A directory OR a package: install-firmware.sh takes either, and a
			 * user with a loose .lfp should not have to know the difference. */
			fb_open("Firmware folder or package", start, "",
			        UI_ACT_SETUP_FIRMWARE);
			return 1;
		}
		/* Both Didj buttons open the file browser on the user's Downloads, which
		 * is where a freshly fetched DIDJ.zip or ControlOverlay.zip actually is.
		 * Gated on the system files for the reason the page states: they are
		 * installed INTO LF/Base and there has to be one. */
		if (g_wiz_page == WIZ_DIDJ && pq.rootfs && pq.sysroot) {
			const char *home = getenv("HOME");
			char dl[PATHMAX * 2];
			if (home && *home) snprintf(dl, sizeof(dl), "%s/Downloads", home);
			else               snprintf(dl, sizeof(dl), "%s", g_proj);
			if (access(dl, R_OK) != 0)
				default_browse_dir(dl, sizeof(dl));
			if (access(dl, R_OK) != 0)
				snprintf(dl, sizeof(dl), "%s", home && *home ? home : g_proj);
			/* by = d.y + 38, and the two buttons are at by+69 and by+113,
			 * indented 12px under their numbered headings. Kept in step with the
			 * draw code by hand, as every other page here does. */
			if (inside(mx, my, d.x + 74, d.y + 107, 76, ui_btn_h())) {
				fb_open("Didj compatibility files (DIDJ.zip)", dl, ".zip",
				        UI_ACT_SETUP_DIDJ);
				return 1;
			}
			if (inside(mx, my, d.x + 158, d.y + 107, 76, ui_btn_h())) {
				g_action = UI_ACT_FETCH_DIDJ;
				g_fb_return = M_WIZARD;    /* back to setup when it finishes */
				return 1;
			}
			if (inside(mx, my, d.x + 74, d.y + 151, 76, ui_btn_h())) {
				fb_open("Controller overlay (ControlOverlay.zip)", dl, ".zip",
				        UI_ACT_SETUP_DIDJ_OVERLAY);
				return 1;
			}
			if (inside(mx, my, d.x + 158, d.y + 151, 76, ui_btn_h())) {
				g_action = UI_ACT_FETCH_DIDJ_OVERLAY;
				g_fb_return = M_WIZARD;
				return 1;
			}
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
		    inside(mx, my, d.x + 62, d.y + 100, 96, ui_btn_h())) {
			games_open();          /* remembers that it must come back here */
			return 1;
		}
		return 1;
	}
	if (g_modal == M_MSG && g_confirm) {
		SDL_Rect y = { d.x + d.w - 96, btn_row_y(&d), 44, ui_btn_h() };
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
		modal_dismiss();
		ui_cfg_save();
		return 1;
	}
	if (g_modal == M_GAMES) {
		int pw = gm_panel(&d), lw2 = gm_list_w(&d), lh2 = gm_list_h(&d);
		int lx = d.x + 6, ly = d.y + 26, i;
		int by = btn_row_y(&d);
		(void)pw;

		if (inside(mx, my, d.x + 6, by, 52, ui_btn_h())) {      /* Folder... */
			games_choose_folder();
			return 1;
		}
		if (inside(mx, my, d.x + 62, by, 44, ui_btn_h())) {     /* Rescan */
			if (g_gm_dir[0]) {
				snprintf(g_action_path, sizeof(g_action_path), "%s", g_gm_dir);
				g_action = UI_ACT_SCAN_GAMES;
				g_fb_return = M_GAMES;   /* the progress panel comes back here */
			}
			return 1;
		}
		{ SDL_Rect mb;
		if (gm_micro_rect(&d, &mb) && inside(mx, my, mb.x, mb.y, mb.w, mb.h)) {
			/* The ProductID is the middle field of the selected row's
			 * PackageID — MULT-0x00180025-000000 — and it is the only part
			 * of the name this needs: a title filters the Downloads folder
			 * by exactly that when it looks for its bonus content. */
			if (g_gm_n && g_gm_sel >= 0 && g_gm_sel < g_gm_n) {
				const char *pid = g_gm[g_gm_sel].pid;
				const char *a = strchr(pid, '-');
				const char *b2 = a ? strchr(a + 1, '-') : NULL;
				g_mm_title[0] = 0;
				if (a && b2 && (size_t)(b2 - a - 1) < sizeof(g_mm_product)
				    && (size_t)(a - pid) < sizeof(g_mm_family)) {
					char prod[16], fam[8];
					memcpy(prod, a + 1, (size_t)(b2 - a - 1));
					prod[b2 - a - 1] = 0;
					memcpy(fam, pid, (size_t)(a - pid));
					fam[a - pid] = 0;
					snprintf(g_mm_title, sizeof(g_mm_title), "%.60s",
					         g_gm[g_gm_sel].name);
					mm_reload(prod, fam);
				}
				g_modal = M_MICROMODS;
			}
			return 1;
		}
		}                       /* scope of the Micromods rect */
		{                                               /* Install N */
			SDL_Rect b = { d.x + d.w - 8 - 48 - 62, by, 62, ui_btn_h() };
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
		SDL_Rect ok = { cb.x - 46, cb.y, 42, ui_btn_h() };
		int ly = d.y + 16, i;
		if (g_fb_action == UI_ACT_SETUP_FIRMWARE ||
		    g_fb_action == UI_ACT_SCAN_GAMES) {
			SDL_Rect uf = { ok.x - 74, ok.y, 70, ui_btn_h() };
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
			if (inside(mx, my, d.x + 8, ly + 12 + i * ui_list_row_h(),
			           d.w - 16, ui_list_row_h())) {
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
			/* Locked on — see the note where this row is drawn. Say why once
			 * rather than letting a click look like a dead spot in the UI. */
			ui_status("Host GPU replay is always on "
			          "(TADPOLE_GL_SOFTWARE=1 for the old path)");
		}
		else if (row_hit(&d, 2, mx, my)) {
			static const int s[] = { 0, 2, 4, 8 };
			int i, k = 0;
			for (i = 0; i < 4; i++)
				if (s[i] == g_cfg.msaa) k = (i + 1) % 4;
			g_cfg.msaa = s[k];
			/* The viewer notices the change and rebuilds the render target on
			 * the next frame, so this takes effect while you watch — which is
			 * the only way to judge whether it was worth having. */
		}
		else if (row_hit(&d, 3, mx, my)) {
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
		else if (row_hit(&d, 4, mx, my)) {
			static const int hz[] = { 60, 30, 0 };   /* 0 = uncapped */
			int i, k = 0;
			for (i = 0; i < 3; i++)
				if (hz[i] == g_cfg.frame_cap) k = (i + 1) % 3;
			g_cfg.frame_cap = hz[k];
		}
		else if (row_hit(&d, 5, mx, my)) cycle_rotate();
		else if (row_hit(&d, 6, mx, my)) g_cfg.auto_rotate = !g_cfg.auto_rotate;
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
	if (g_modal == M_PAD) {
		if (row_hit(&d, 0, mx, my)) g_cfg.pad_on = !g_cfg.pad_on;
		else if (row_hit(&d, 1, mx, my)) {
			static const int steps[] = { 75, 100, 125, 150 };
			int i, k = 0;
			for (i = 0; i < 4; i++)
				if (steps[i] == g_cfg.pad_size) k = (i + 1) % 4;
			g_cfg.pad_size = steps[k];
		}
		else if (row_hit(&d, 2, mx, my)) {
			static const int steps[] = { 40, 55, 70, 85, 100 };
			int i, k = 0;
			for (i = 0; i < 5; i++)
				if (steps[i] == g_cfg.pad_opacity) k = (i + 1) % 5;
			g_cfg.pad_opacity = steps[k];
		}
		else if (row_hit(&d, 3, mx, my)) g_cfg.pad_left = !g_cfg.pad_left;
		else return 1;          /* the key list is not clickable */
		/* The viewer re-reads these on the next frame; nothing to notify. */
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
		else if (row_hit(&d, 1, mx, my)) g_cfg.fast_boot = !g_cfg.fast_boot;
		else if (row_hit(&d, 2, mx, my)) g_cfg.touch_ui = !g_cfg.touch_ui;
		else if (row_hit(&d, 3, mx, my)) {
			g_cfg.frost = !g_cfg.frost;
			/* Takes effect on the very next frame — this dialog is one of the
			 * panels it governs, so the answer to "was that worth it" is on
			 * screen before the finger is off the glass. */
			ui_status("%s", g_cfg.frost
			          ? "Frosted glass on"
			          : "Frosted glass off - faster panels");
		}
		else if (inside(mx, my, d.x + 10, row_y(&d, 6) + 2, 76, ui_btn_h())) {
			ui_cfg_save();
			games_open();
			return 1;
		}
		else if (inside(mx, my, d.x + 94, row_y(&d, 6) + 2, 96, ui_btn_h())) {
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
		if (g_drag.active) {
			struct dlg d = cur_dlg(lw, lh);
			struct list_view v;
			int dy = g_my - g_drag.last, far = g_my - g_drag.y0;
			g_drag.last = g_my;
			if (far < 0) far = -far;
			if (far > g_drag.moved) g_drag.moved = far;
			if (list_view(&d, &v))
				list_scroll_px(&v, dy);
			/* A running average rather than the last delta alone: one frame
			 * can be a jitter, and a flick thrown by a jitter goes the wrong
			 * way at speed. */
			g_drag.vel = g_drag.vel * 0.6f + (float)dy * 0.4f;
			return 1;
		}
		if (g_open_menu >= 0) {
			const struct menu *m = &MENUS[g_open_menu];
			int w = menu_width(m), i;
			g_hot_item = -1;
			for (i = 0; i < m->n; i++)
				if (inside(g_mx, g_my, m->x, menu_item_y(m, i), w,
				           menu_item_h(&m->items[i])))
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

		if (g_modal != M_NONE) {
			/* A PRESS INSIDE A LIST IS NOT YET A CHOICE. It might be the start
			 * of a scroll, and there is no way to know until the finger either
			 * moves or lifts — so the decision waits for the release, and the
			 * press only arms the drag. Everything else in a dialog still acts
			 * on the press, because nothing else in one can be dragged.
			 *
			 * This matters most in the file browser, where activating a row
			 * ENTERS a directory: dispatching on the press would mean every
			 * attempt to scroll navigated somewhere first. */
			struct dlg d = cur_dlg(lw, lh);
			struct list_view v;
			if (list_view(&d, &v) &&
			    inside(mx, my, v.body.x, v.body.y, v.body.w, v.body.h)) {
				g_drag.active = 1;
				g_drag.x0 = mx; g_drag.y0 = my; g_drag.last = my;
				g_drag.moved = 0;
				g_drag.rem = 0;
				g_drag.vel = 0;
				return 1;
			}
			return dialog_click(lw, lh, mx, my);
		}

		if (g_open_menu >= 0) {
			const struct menu *m = &MENUS[g_open_menu];
			int w = menu_width(m);
			for (i = 0; i < m->n; i++) {
				if (inside(mx, my, m->x, menu_item_y(m, i), w,
				           menu_item_h(&m->items[i]))) {
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

	case SDL_MOUSEBUTTONUP: {
		/* SDL_TOUCH_MOUSEID marks the mouse event SDL synthesised from a
		 * finger. That finger has just left the screen, so the pointer is no
		 * longer anywhere — see pointer_left(). */
		int was_touch = (e->button.which == SDL_TOUCH_MOUSEID);
		int in_bar = e->button.y < UI_BAR_H;
		if (g_drag.active) {
			int tap = g_drag.moved <= DRAG_SLOP;
			g_drag.active = 0;
			if (tap) {
				/* It never moved, so it was a press after all — dispatched
				 * from where it STARTED, not from where the pointer happens
				 * to be now. */
				g_drag.vel = 0;
				dialog_click(lw, lh, g_drag.x0, g_drag.y0);
			}
			if (was_touch) { pointer_left(); g_hot_item = -1; }
			return 1;
		}
		if (was_touch) {
			pointer_left();
			g_hot_item = -1;
		}
		if (g_modal != M_NONE) return 1;
		if (g_open_menu >= 0) return 1;
		return in_bar;
	}

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
		if (g_modal == M_MICROMODS) {
			g_mm_top -= e->wheel.y * 2;
			if (g_mm_top > g_mm_n - g_mm_rows) g_mm_top = g_mm_n - g_mm_rows;
			if (g_mm_top < 0) g_mm_top = 0;
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
		/* THE LAUNCHER HAD NO KEYBOARD AT ALL. Every other list in this file
		 * has arrows, paging and Home/End; this one fell through to the
		 * generic "a modal is up" branch below, which swallows the key and
		 * does nothing — so the only way to move through four hundred apps
		 * was the wheel, and nothing on screen said even that would work. */
		if (g_modal == M_APPS && g_ap_n &&
		    e->key.keysym.sym != SDLK_ESCAPE) {   /* Escape closes, below */
			switch (e->key.keysym.sym) {
			case SDLK_RETURN:
				if (g_ap_sel >= 0 && g_ap_sel < g_ap_n) {
					snprintf(g_action_path, sizeof(g_action_path), "%s",
					         g_ap[g_ap_sel].pkg);
					g_action = UI_ACT_RUN_APP;
					g_modal = M_NONE;
				}
				return 1;
			case SDLK_UP:       g_ap_sel--; break;
			case SDLK_DOWN:     g_ap_sel++; break;
			case SDLK_PAGEUP:   g_ap_sel -= g_ap_rows; break;
			case SDLK_PAGEDOWN: g_ap_sel += g_ap_rows; break;
			case SDLK_HOME:     g_ap_sel = 0; break;
			case SDLK_END:      g_ap_sel = g_ap_n - 1; break;
			default:
				/* TYPE A LETTER TO JUMP TO IT. Four hundred and sixty-two
				 * titles is seventy-odd presses of PageDown end to end, and
				 * the list is already sorted by name — the letter you want is
				 * the one thing you reliably know about where you are going.
				 *
				 * The search starts one PAST the selection and wraps, so
				 * pressing the same letter again walks the matches instead of
				 * sticking on the first. That is what makes it usable for the
				 * dozen titles here beginning with "L". */
				{
					SDL_Keycode k = e->key.keysym.sym;
					int i, start;
					char want;
					if (k < SDLK_a || k > SDLK_z)
						return 1;    /* not a letter: the modal swallows it */
					want = (char)('a' + (k - SDLK_a));
					start = g_ap_sel + 1;
					for (i = 0; i < g_ap_n; i++) {
						int j = (start + i) % g_ap_n;
						char c = g_ap[j].name[0];
						if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
						if (c == want) { g_ap_sel = j; break; }
					}
				}
				break;
			}
			if (g_ap_sel < 0) g_ap_sel = 0;
			if (g_ap_sel > g_ap_n - 1) g_ap_sel = g_ap_n - 1;
			/* The view follows the selection; the draw clamps g_ap_top
			 * against the row count it actually has room for. */
			if (g_ap_sel < g_ap_top) g_ap_top = g_ap_sel;
			if (g_ap_sel >= g_ap_top + g_ap_rows)
				g_ap_top = g_ap_sel - g_ap_rows + 1;
			return 1;
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
				/* The same decision the Close button makes. It used to be a
				 * second, smaller copy of it, which is why Escape forgot the
				 * Game Library when the mouse did not. */
				modal_dismiss();
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
