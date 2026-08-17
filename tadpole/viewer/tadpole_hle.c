/* Tadpole — host-side replay of the guest's GL command stream.
 *
 * Runs in the viewer, as native x86. Drains the ring the guest fills (see
 * shim/tadpole_glcmd.h) and reissues each call against the host's
 * compatibility-profile OpenGL, then reads the finished frame back into the fb1
 * arena — exactly where the software rasteriser writes today, so the existing
 * three-layer compositor needs no changes at all.
 *
 * WHY THIS IS TRACTABLE
 * ---------------------
 * GLES 1.x has no shaders, and desktop GL's compatibility profile still exposes
 * the whole fixed-function pipeline, so the mapping is nearly 1:1. Verified by
 * viewer/hle_probe.c on this machine: glVertexPointer, glMatrixMode, glTexEnvi
 * and glAlphaFunc all resolve, and a frame-shaped load runs at 1563 Mpx/s
 * against the software rasteriser's 1.07.
 *
 * THE TWO REAL DIFFERENCES FROM GLES1
 * -----------------------------------
 * 1. GL_FIXED does not exist on the desktop. These titles use
 *    `glVertexPointer(2, GL_FIXED, ...)` throughout, so arrays are converted to
 *    float HERE — the host is the only side that knows how many vertices a draw
 *    actually touches, which for glDrawElements means scanning the indices.
 * 2. Buffer objects are mirrored as plain host memory rather than host VBOs, and
 *    draws use client-side arrays. That sidesteps GL_FIXED entirely and keeps
 *    the conversion in one place; the cost is a per-draw copy of a few hundred
 *    vertices, which is nothing next to the 78 ms it replaces.
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/mman.h>   /* the shared GL ring; see hle_host_init */
#endif

#include "../shim/tadpole_glcmd.h"
#include "tadpole_hle.h"

/* ---- OpenGL past 1.1, resolved at runtime -------------------------------
 *
 * Mesa's libGL exports every entry point, which is why a GL_GLEXT_PROTOTYPES
 * build ever linked. Windows' opengl32.dll exports only OpenGL 1.1;
 * glActiveTexture and the whole framebuffer-object family live behind
 * wglGetProcAddress and do not link at all. So the fourteen post-1.1 entry
 * points this file uses are fetched through SDL_GL_GetProcAddress once the
 * context exists — on every platform, because a driver that cannot supply one
 * (Windows' GDI OpenGL-1.1 fallback) should mean "HLE unavailable; software
 * raster", not a link error on somebody else's machine. The PFN typedefs come
 * from SDL_opengl_glext.h.
 *
 * Everything else this file calls is core 1.1 and binds at link time. A NEW
 * call past 1.1 must be added to the table, the defines and gl_resolve()
 * together — and the compiler enforces the first: without GL_GLEXT_PROTOTYPES
 * there is no prototype for it to quietly fall back on. */
/* OUR OWN TYPEDEF, because this one cannot be relied on to exist. Mesa's
 * GL/gl.h defines GL_VERSION_1_3 before SDL_opengl_glext.h is reached, so that
 * header skips its whole 1.3 block — the block holding
 * PFNGLCLIENTACTIVETEXTUREPROC — while GL/gl.h itself declares a bare
 * glClientActiveTexture prototype and no function-pointer type to go with it.
 * Naming it ourselves sidesteps the question of which header won.
 *
 * It stays behind the resolver rather than being called directly for the reason
 * in the comment above: glClientActiveTexture is GL 1.3, Windows' opengl32.dll
 * exports 1.1, and a direct call would not link there. */
typedef void (APIENTRYP tad_clientactivetexture_fn)(GLenum texture);

static struct {
	PFNGLACTIVETEXTUREPROC                  ActiveTexture;
	/* The CLIENT-side twin of ActiveTexture: selects which unit
	 * glTexCoordPointer and glEnable/DisableClientState(GL_TEXTURE_COORD_ARRAY)
	 * address. Needed since the guest started sending a second coordinate set —
	 * see setup_arrays(). */
	tad_clientactivetexture_fn              ClientActiveTexture;
	PFNGLGENFRAMEBUFFERSPROC                GenFramebuffers;
	PFNGLBINDFRAMEBUFFERPROC                BindFramebuffer;
	PFNGLDELETEFRAMEBUFFERSPROC             DeleteFramebuffers;
	PFNGLGENRENDERBUFFERSPROC               GenRenderbuffers;
	PFNGLBINDRENDERBUFFERPROC               BindRenderbuffer;
	PFNGLDELETERENDERBUFFERSPROC            DeleteRenderbuffers;
	PFNGLRENDERBUFFERSTORAGEPROC            RenderbufferStorage;
	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample;
	PFNGLFRAMEBUFFERTEXTURE2DPROC           FramebufferTexture2D;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC        FramebufferRenderbuffer;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC         CheckFramebufferStatus;
	PFNGLBLITFRAMEBUFFERPROC                BlitFramebuffer;
} g_gl;

/* Call sites keep the real GL names; these defines are the loader's whole
 * footprint on the rest of the file. */
#define glActiveTexture                  g_gl.ActiveTexture
#define glGenFramebuffers                g_gl.GenFramebuffers
#define glBindFramebuffer                g_gl.BindFramebuffer
#define glDeleteFramebuffers             g_gl.DeleteFramebuffers
#define glGenRenderbuffers               g_gl.GenRenderbuffers
#define glBindRenderbuffer               g_gl.BindRenderbuffer
#define glDeleteRenderbuffers            g_gl.DeleteRenderbuffers
#define glRenderbufferStorage            g_gl.RenderbufferStorage
#define glRenderbufferStorageMultisample g_gl.RenderbufferStorageMultisample
#define glFramebufferTexture2D           g_gl.FramebufferTexture2D
#define glFramebufferRenderbuffer        g_gl.FramebufferRenderbuffer
#define glCheckFramebufferStatus         g_gl.CheckFramebufferStatus
#define glBlitFramebuffer                g_gl.BlitFramebuffer

/* Fill the table against the CURRENT context. Zero on any miss, and every
 * miss is named: one absent entry point usually means a whole family is
 * absent, and the full list says which at a glance. */
static int gl_resolve(void)
{
	const struct { const char *name; void **slot; } tab[] = {
		{ "glActiveTexture",         (void **)&g_gl.ActiveTexture },
		{ "glClientActiveTexture",   (void **)&g_gl.ClientActiveTexture },
		{ "glGenFramebuffers",       (void **)&g_gl.GenFramebuffers },
		{ "glBindFramebuffer",       (void **)&g_gl.BindFramebuffer },
		{ "glDeleteFramebuffers",    (void **)&g_gl.DeleteFramebuffers },
		{ "glGenRenderbuffers",      (void **)&g_gl.GenRenderbuffers },
		{ "glBindRenderbuffer",      (void **)&g_gl.BindRenderbuffer },
		{ "glDeleteRenderbuffers",   (void **)&g_gl.DeleteRenderbuffers },
		{ "glRenderbufferStorage",   (void **)&g_gl.RenderbufferStorage },
		{ "glRenderbufferStorageMultisample",
		                             (void **)&g_gl.RenderbufferStorageMultisample },
		{ "glFramebufferTexture2D",  (void **)&g_gl.FramebufferTexture2D },
		{ "glFramebufferRenderbuffer",
		                             (void **)&g_gl.FramebufferRenderbuffer },
		{ "glCheckFramebufferStatus",(void **)&g_gl.CheckFramebufferStatus },
		{ "glBlitFramebuffer",       (void **)&g_gl.BlitFramebuffer },
	};
	unsigned int i;
	int missing = 0;

	for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
		*tab[i].slot = SDL_GL_GetProcAddress(tab[i].name);
		if (!*tab[i].slot) {
			fprintf(stderr, "hle: %s did not resolve — driver stops at"
			        " OpenGL 1.1?\n", tab[i].name);
			missing++;
		}
	}
	return missing == 0;
}

/* GLES1 enums that differ from, or are absent on, the desktop. */
#define GLES_FIXED  0x140C

/* Indexed by GUEST texture name, so this MUST stay larger than the guest's
 * MAX_TEXS in tadpole_gles_core.c — names run 1..MAX_TEXS, and the guard here
 * is `name >= MAX_TEX`. A name at or above this is dropped, and a dropped
 * texture draws black while raising no GL error — silent, and
 * indistinguishable from a texture that simply never arrived.
 *
 * Raised with MAX_TEXS (32767) when Sonic ran out of texture names and drew
 * white tiles. Costs 5 bytes a name here — a host name and a have-flag — so
 * about 160 KB, and only the once-per-context-teardown reset walks all of it. */
#define MAX_TEX  32768
/* Indexed by GUEST buffer name, so it must exceed both the guest's MAX_BUFS
 * (2048) and the reserved client-array names HLE_CLIENT_* (4000..4003). A name
 * at or above this is dropped, and a dropped ELEMENT buffer makes the host skip
 * the draw entirely — silently, and the frame comes back black. */
#define MAX_BUF  4096

struct hbuf { unsigned char *data; unsigned int size; };
struct harr { unsigned int buf, type, on; int size, stride; unsigned int off; };

static struct tadgl_hdr *g_ring;
static unsigned char    *g_data;
static SDL_Window       *g_win;
static SDL_GLContext     g_ctx;
/* THE RENDER TARGET, AND ITS ANTI-ALIASED VARIANT.
 *
 * Without multisampling there is one framebuffer object: g_fbo, with the
 * texture g_colour attached, and the readback comes straight out of it. That
 * is the original arrangement and it is still exactly what happens when
 * anti-aliasing is off — g_resolve is then the same object as g_fbo, and the
 * blit below is skipped.
 *
 * With it, the replay draws into a MULTISAMPLED g_fbo (renderbuffers, because
 * a multisample texture would need a shader to resolve) and PRESENT blits that
 * down into the single-sample g_resolve before reading it back. The guest
 * cannot tell: the pixels it gets are the same 480x272, only with the edges
 * averaged instead of stepped.
 *
 * Why this is worth so little code: the title's own glViewport is discarded —
 * we set the viewport from the layer's size — so the sample count is entirely
 * ours to choose and nothing in the guest has an opinion about it.
 */
static GLuint            g_fbo, g_resolve, g_colour, g_depth;
static GLuint            g_ms_colour, g_ms_depth;
static GLuint            g_final, g_final_tex;
static int               g_msaa;      /* samples in use; 0 = off */
/* RENDER SCALE. The replay draws into a buffer g_ss times the panel in each
 * axis and the finished frame is filtered back down to 480x272 on its way to
 * the guest — supersampling, the oldest and least clever anti-aliasing there
 * is, and the one that improves everything at once: polygon edges, texture
 * minification, the lot.
 *
 * The guest cannot tell. Its glViewport is discarded (we set the viewport from
 * the layer's size) and its geometry is all in projection space, so the
 * only thing that has to be scaled by hand is anything expressed in PIXELS:
 * the viewport itself and the scissor box. Those are the two places below.
 *
 * It cannot make the picture sharper on screen — the guest still receives
 * 480x272 — but it decides how much detail survives being squeezed into it. */
static int               g_ss = 1;
static int               g_dw, g_dh;  /* draw-buffer size: panel * g_ss */
static GLuint            g_tex[MAX_TEX];       /* guest name -> host name */
/* host_tex() creates an empty GL texture object on demand, so "the object
 * exists" says nothing about whether we ever received its PIXELS. A draw with a
 * bound-but-empty texture is not an error and raises no GL error — it just comes
 * out black. That is why the Virtuos boot logo went black under HLE while the
 * steady-state menu was fine: the logo's upload happened before the encoder
 * attached, and only a missing BUFFER used to trigger a resync. */
static unsigned char     g_tex_have[MAX_TEX];
static unsigned int      g_bound_name;   /* guest texture name currently bound */
static int               g_tex_enabled;  /* GL_TEXTURE_2D on unit 0 */
static struct hbuf       g_buf[MAX_BUF];
static struct harr       g_arr[TADGL_ARR_COUNT];
static int               g_w, g_h;
/* The layer rectangle the guest told us about. Only g_vw x g_vh of the draw
 * buffer is rendered and read back: blanket-writing the whole panel filled the
 * REST of fb1 with the FBO's untouched black, and since the compositor draws
 * fb2 (video) beneath fb1, an opaque fb1 hides it.
 *
 * g_vx/g_vy are the layer's PLACE ON THE PANEL and are never a draw parameter —
 * see the note above apply_scissor(). Only hle_host_rect() reads them, so the
 * viewer can composite the finished picture where it belongs. */
static int               g_vx, g_vy, g_vw, g_vh;
static int               g_ready;
/* SDL_Renderer with SDL_RENDERER_ACCELERATED is an OpenGL renderer here, and it
 * has its own context. SDL_GL_CreateContext MAKES THE NEW CONTEXT CURRENT, so
 * after init every GL call the viewer made — SDL's included — landed in the
 * wrong context. The symptom was unmistakable once seen: the ENTIRE viewer
 * output, menu bar and all, squeezed into a 320x240 corner, because our
 * glViewport/glScissor had leaked into SDL's context. And our own replay then
 * ran against SDL's context, where our FBO does not exist.
 *
 * So remember whose context was current before we barged in, and hand it back
 * around every entry point. */
static SDL_Window   *g_prev_win;
static SDL_GLContext g_prev_ctx;

static void ctx_enter(void)
{
	g_prev_ctx = SDL_GL_GetCurrentContext();
	g_prev_win = SDL_GL_GetCurrentWindow();
	SDL_GL_MakeCurrent(g_win, g_ctx);
}

static void ctx_leave(void)
{
	if (g_prev_ctx) SDL_GL_MakeCurrent(g_prev_win, g_prev_ctx);
	else            SDL_GL_MakeCurrent(g_win, NULL);
}
static unsigned long     g_frames, g_packets;
static unsigned int      g_desync;
static unsigned long     g_draws;      /* draws replayed in the current frame */
static unsigned long     g_de_pkts, g_de_nobuf, g_de_noarr;
static unsigned int      g_filtered;
static unsigned int      g_nobuf_logged;
static unsigned int      g_bd_logged;
static unsigned int      g_notex_logged;
static unsigned int      g_ti_logged;
static unsigned int      g_texenv_logged, g_texparam_logged;
static int               g_fellback_seen;
/* Last few packets consumed, for post-mortem on a desync. The desync is
 * DETERMINISTIC (identical tail/head numbers across runs), so the packet that
 * over-advances tail is in here. */
#define HIST 10
static struct { unsigned int op, len, tail_before, tail_after; } g_hist[HIST];
static unsigned int g_hist_n;
/* TADPOLE_HLE_DEBUG=1 for the per-frame accounting. Unconditional stderr spam
 * from a working replayer is noise; these exist for when it is NOT working. */
static int               g_verbose;
/* TADPOLE_GL_DEBUG, the SAME variable and the same three levels the guest shim
 * uses (see shim/tadpole_gles_debug.c). A GL bug in this project has two
 * possible homes — the guest encoder and this replayer — and needing a
 * different switch for each meant every investigation started by picking the
 * wrong one. Level 2 aborts the viewer on the first host GL error, which is
 * what makes "which of the 900 commands in this frame was rejected" a question
 * with an answer instead of a bisection. */
static int               g_level;
static unsigned int      g_mat_mode = 0x1700;   /* GL_MODELVIEW */
static unsigned long     g_mat_tex_sets, g_mat_tex_ops;
static unsigned int      g_gl_err;     /* first GL error seen, 0 if none */
static unsigned long     g_err_count;

/* GL ERRORS ARE THE FIRST THING TO CHECK when a replay produces nothing. A
 * single rejected call (bad enum, unsupported combination) silently draws
 * nothing and every later call still "works", so the frame comes back empty with
 * no other symptom. Sample once per frame — glGetError is a pipeline flush point
 * and calling it per command would dominate the cost. */
/* Opcode names, in enum order, so a GL error can name the call that caused it
 * instead of just the frame. The compile-time check below is the point: add an
 * opcode to tadgl_op and forget this table, and every name after the insertion
 * point silently shifts by one — a diagnostic that lies is worse than none. */
static const char *const g_opnames[] = {
	"NOP", "PRESENT", "CLEAR", "VIEWPORT",
	"ENABLE", "DISABLE", "BLENDFUNC", "DEPTHFUNC", "DEPTHMASK", "ALPHAFUNC",
	"TEXENV", "COLOR", "CULLFACE", "FRONTFACE", "SHADEMODEL",
	"MATRIXMODE", "LOADIDENTITY", "LOADMATRIX", "MULTMATRIX",
	"PUSHMATRIX", "POPMATRIX", "ORTHO", "FRUSTUM",
	"TRANSLATE", "ROTATE", "SCALE",
	"BINDTEXTURE", "ACTIVETEXTURE", "TEXIMAGE2D", "TEXSUBIMAGE2D",
	"TEXPARAM", "DELETETEXTURE",
	"BUFFERDATA", "BUFFERSUBDATA", "DELETEBUFFER",
	"ARRAYPOINTER", "CLIENTSTATE",
	"DRAWARRAYS", "DRAWELEMENTS",
	"RESET", "TEXENVCOLOR",
	"SCISSOR", "COLORMASK", "LINEWIDTH", "POINTSIZE", "POLYGONOFFSET",
	"LIGHT", "MATERIAL", "LIGHTMODEL", "NORMAL",
};
typedef char tadgl_opnames_match[
	(sizeof(g_opnames) / sizeof(g_opnames[0]) == TADGL_OP_COUNT) ? 1 : -1];

static const char *tadgl_opname(unsigned int op)
{
	return op < TADGL_OP_COUNT ? g_opnames[op] : "?";
}

/* ---- WE DRAW AT THE LAYER'S OWN ORIGIN, NOT AT ITS PLACE ON THE PANEL -----
 *
 * GL renders into /dev/fb1, one plane of a three-layer compositor. The plane's
 * buffer holds a win_w x win_h image starting at its base address, and the MLC
 * composites it at (win_x, win_y) — Brio's own log for a 250x250 window at
 * (76,11) reads `CreateHandle: 250x250 (1920) @ <pan base>`, with no offset
 * anywhere in it.
 *
 * This file used to draw at (win_x, win_y) instead, which came to the same
 * picture only because the viewer then copied fb1 onto the panel one-for-one.
 * That was the wrong half of the pipeline to compensate in: every title whose
 * pixels do NOT come through here — the Flash-rendered Leapster titles, which
 * is most of them — got no compensation at all and drew in the panel's corner.
 * The viewer's compositor now places every layer at its window (see
 * layer_window() in tadpole_view.c), so this side must draw at the origin or
 * the offset would be applied twice.
 *
 * So the layer clip is (0,0,g_vw,g_vh) in the layer's own top-left space, and
 * everything below is expressed there.
 *
 * GL_SCISSOR_TEST IS ALWAYS ON HERE, and glEnable/glDisable of it are consumed
 * rather than forwarded. A title that disables scissoring is asking to stop
 * clipping to ITS box; it is not — and has no way of knowing it could be —
 * asking to escape the layer window. Forwarding the disable is what let a title
 * paint outside its plane.
 */
static int g_sc_on;            /* the TITLE's GL_SCISSOR_TEST */
static int g_sc[4];            /* the TITLE's box, as it sent it */
static int g_sc_honour = -1;   /* TADPOLE_GL_SCISSOR — off by default */
static unsigned int g_sc_logged;

/* THE TITLE'S BOX IS STILL NOT HONOURED BY DEFAULT, BUT THE REASON HAS CHANGED.
 *
 * It used to be that the two rectangles were in frames we could not relate: the
 * layer window arrived in PANEL coordinates with y from the top, while the
 * title's glScissor is in ITS surface's coordinates with y from the bottom, and
 * since its glViewport was discarded we did not know that surface's size.
 * Intersecting them directly moved Pet Pals 2's 3D plane; flipping one of them
 * moved the error from the top edge to the bottom.
 *
 * Both rectangles now live in the same space. The title's surface IS the layer,
 * g_vw x g_vh, so its box converts to the layer's top-left space by flipping y
 * about g_vh — which is what the intersection below does. That makes the
 * composition derivable rather than guessed.
 *
 * It stays off by default because turning it on changes what several titles
 * clip, and that is a separate change with its own before/after captures.
 * TADPOLE_GL_SCISSOR=1 enables it; TADPOLE_GL_DEBUG=1 prints both rectangles.
 */
static void apply_scissor(void)
{
	int x = 0, y = 0, w = g_vw, h = g_vh;

	if (g_sc_honour < 0)
		g_sc_honour = getenv("TADPOLE_GL_SCISSOR") != NULL;

	if (g_level >= 1 && g_sc_on && g_sc_logged < 12) {
		g_sc_logged++;
		fprintf(stderr, "hle: scissor — layer %dx%d (drawn at its own origin,"
		        " composited at %d,%d)   title(raw) %d,%d %dx%d   honoured=%d\n",
		        g_vw, g_vh, g_vx, g_vy,
		        g_sc[0], g_sc[1], g_sc[2], g_sc[3], g_sc_honour);
	}

	if (g_sc_honour && g_sc_on) {
		/* The title's box, flipped into the layer's top-left space. */
		int tx = g_sc[0], ty = g_vh - (g_sc[1] + g_sc[3]);
		int x0 = tx > x ? tx : x;
		int y0 = ty > y ? ty : y;
		int x1 = tx + g_sc[2];
		int y1 = ty + g_sc[3];
		if (x1 > x + w) x1 = x + w;
		if (y1 > y + h) y1 = y + h;
		x = x0; y = y0;
		w = x1 - x0; h = y1 - y0;
		if (w < 0) w = 0;
		if (h < 0) h = 0;   /* disjoint: draw nothing, which is correct */
	}
	/* Into GL's bottom-up coordinates, and SCALED INTO THE DRAW BUFFER. The box
	 * is in layer pixels; the draw buffer is g_ss times larger in each axis, and
	 * an unscaled scissor would clip everything outside the bottom-left 1/9th of
	 * a 3x frame. The layer occupies the TOP-left of a panel-sized FBO, so the
	 * flip is about the panel height. */
	glScissor(x * g_ss, (g_h - y - h) * g_ss, w * g_ss, h * g_ss);
	glEnable(GL_SCISSOR_TEST);
}

static void check_gl(const char *where)
{
	GLenum e = glGetError();
	if (e == GL_NO_ERROR) return;
	g_err_count++;
	if (!g_gl_err) {
		g_gl_err = e;
		fprintf(stderr, "hle: GL error 0x%04X at %s (first of possibly many)\n",
		        e, where);
	}
}
static float            *g_conv[TADGL_ARR_COUNT];
static unsigned int      g_convn[TADGL_ARR_COUNT];

/* ---- setup -------------------------------------------------------------- */

/* The plain single-sample target: a texture for colour, a renderbuffer for
 * depth. Also the resolve destination when multisampling is on, in which case
 * it needs no depth — only the colour is ever blitted down. */
static int make_plain(int w, int h, int want_depth)
{
	glGenFramebuffers(1, &g_resolve);
	glBindFramebuffer(GL_FRAMEBUFFER, g_resolve);
	glGenTextures(1, &g_colour);
	glBindTexture(GL_TEXTURE_2D, g_colour);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, g_colour, 0);
	if (want_depth) {
		glGenRenderbuffers(1, &g_depth);
		glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                          GL_RENDERBUFFER, g_depth);
	}
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int make_multisample(int w, int h, int samples)
{
	glGenFramebuffers(1, &g_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	glGenRenderbuffers(1, &g_ms_colour);
	glBindRenderbuffer(GL_RENDERBUFFER, g_ms_colour);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                          GL_RENDERBUFFER, g_ms_colour);
	glGenRenderbuffers(1, &g_ms_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, g_ms_depth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
	                                 GL_DEPTH_COMPONENT16, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                          GL_RENDERBUFFER, g_ms_depth);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void drop_multisample(void)
{
	if (g_ms_colour) glDeleteRenderbuffers(1, &g_ms_colour);
	if (g_ms_depth)  glDeleteRenderbuffers(1, &g_ms_depth);
	if (g_fbo && g_fbo != g_resolve) glDeleteFramebuffers(1, &g_fbo);
	g_ms_colour = g_ms_depth = 0;
	g_fbo = g_resolve;
	g_msaa = 0;
}

/* The panel-sized buffer the guest actually receives. Only needed when the
 * draw buffer is larger; otherwise the resolve target IS panel-sized and this
 * step does not exist. */
static int make_final(int w, int h)
{
	glGenFramebuffers(1, &g_final);
	glBindFramebuffer(GL_FRAMEBUFFER, g_final);
	glGenTextures(1, &g_final_tex);
	glBindTexture(GL_TEXTURE_2D, g_final_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, g_final_tex, 0);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int make_target(int w, int h, int samples, int ss)
{
	GLint maxs = 0, maxtex = 0;

	/* A draw buffer the driver cannot allocate is not a smaller picture, it
	 * is an incomplete framebuffer and a black screen. Ask first. */
	if (ss < 1) ss = 1;
	/* BOTH LIMITS, not just the texture one. The single-sample targets are
	 * textures, but the multisampled colour and depth are RENDERBUFFERS, and
	 * a driver is free to cap those lower. Exceeding either makes an
	 * incomplete framebuffer, which here means a black screen. */
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
	{
		GLint maxrb = 0;
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
		if (maxrb > 0 && (maxtex <= 0 || maxrb < maxtex))
			maxtex = maxrb;
	}
	while (ss > 1 && maxtex > 0 && (w * ss > maxtex || h * ss > maxtex)) {
		fprintf(stderr, "hle: %dx render scale needs %dx%d, driver allows "
		        "%d — using %dx\n", ss, w * ss, h * ss, (int)maxtex, ss - 1);
		ss--;
	}
	g_ss = ss;
	g_dw = w * ss;
	g_dh = h * ss;

	if (samples > 0) {
		/* ASK THE DRIVER, do not assume. A request for 8x on hardware that
		 * offers 4 makes an INCOMPLETE framebuffer, and an incomplete
		 * framebuffer here means every frame comes out black — which looks
		 * exactly like the emulator being broken rather than one setting
		 * being too ambitious. */
		glGetIntegerv(GL_MAX_SAMPLES, &maxs);
		if (maxs < 2) {
			fprintf(stderr, "hle: no multisampling available; AA off\n");
			samples = 0;
		} else if (samples > maxs) {
			fprintf(stderr, "hle: %dx AA requested, %dx is the maximum here\n",
			        samples, (int)maxs);
			samples = (int)maxs;
		}
	}

	/* The chain, longest first:
	 *
	 *   [draw, g_dw x g_dh, multisampled]   the replay renders here
	 *        -> resolve blit (NEAREST)
	 *   [resolve, g_dw x g_dh]              samples averaged
	 *        -> downscale blit (LINEAR)
	 *   [final, w x h]                      what the guest reads back
	 *
	 * Each step only exists if its setting is on, and with both off all three
	 * names refer to the same framebuffer object — which is exactly the
	 * original one-FBO arrangement, with no blits at all. */
	if (!make_plain(g_dw, g_dh, samples == 0))
		return 0;
	if (samples == 0) {
		g_fbo = g_resolve;
		g_msaa = 0;
	} else if (!make_multisample(g_dw, g_dh, samples)) {
		/* Never fail the whole replayer over a cosmetic setting: fall back to
		 * no anti-aliasing and say so. */
		fprintf(stderr, "hle: %dx AA target incomplete; falling back to none\n",
		        samples);
		drop_multisample();
		glBindFramebuffer(GL_FRAMEBUFFER, g_resolve);
		glGenRenderbuffers(1, &g_depth);
		glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, g_dw, g_dh);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                          GL_RENDERBUFFER, g_depth);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return 0;
	} else {
		g_msaa = samples;
	}

	if (g_ss > 1) {
		if (!make_final(w, h)) {
			fprintf(stderr, "hle: %dx render scale target incomplete;"
			        " falling back to 1x\n", g_ss);
			if (g_final) glDeleteFramebuffers(1, &g_final);
			if (g_final_tex) glDeleteTextures(1, &g_final_tex);
			g_final = 0; g_final_tex = 0;
			g_ss = 1;
			/* The draw buffer is already the wrong size for 1x, so rebuild
			 * the whole chain rather than leaving a mismatch behind. */
			return make_target(w, h, samples, 1);
		}
	} else {
		g_final = g_resolve;
	}

	if (g_msaa || g_ss > 1)
		fprintf(stderr, "hle: render %dx%d (%dx scale)%s\n", g_dw, g_dh, g_ss,
		        g_msaa ? ", multisampled" : "");
	return 1;
}

/* Change the sample count on a LIVE replayer.
 *
 * Anti-aliasing is a checkbox in a settings panel, and a checkbox that needs
 * the emulator restarted to take effect is not a checkbox — it is a note to
 * self. Only the framebuffer object and its attachments depend on the sample
 * count; textures, buffers and every scrap of guest GL state live elsewhere
 * and are untouched, so the target can simply be rebuilt underneath the
 * replay.
 *
 * Called from the viewer's loop, never from inside a pump: between frames the
 * FBO holds nothing anyone still wants. The viewport and scissor DO belong to
 * the framebuffer, so they are re-applied afterwards — without that, the frame
 * after a toggle renders into a default viewport, which looks like the picture
 * jumping to a corner.
 */
void hle_host_set_quality(int samples, int ss)
{
	if (!g_ctx) return;
	if (samples < 0) samples = 0;
	if (ss < 1) ss = 1;
	if (samples == g_msaa && ss == g_ss) return;

	ctx_enter();
	/* Tear down the old target completely. g_fbo and g_resolve are the SAME
	 * object when AA is off, so deleting both handles blindly would delete a
	 * name twice and leave the second delete pointing at whatever the driver
	 * recycled that name for. */
	if (g_fbo && g_fbo != g_resolve) glDeleteFramebuffers(1, &g_fbo);
	if (g_resolve) glDeleteFramebuffers(1, &g_resolve);
	if (g_colour) glDeleteTextures(1, &g_colour);
	if (g_depth) glDeleteRenderbuffers(1, &g_depth);
	if (g_ms_colour) glDeleteRenderbuffers(1, &g_ms_colour);
	if (g_ms_depth) glDeleteRenderbuffers(1, &g_ms_depth);
	if (g_final && g_final != g_resolve) glDeleteFramebuffers(1, &g_final);
	if (g_final_tex) glDeleteTextures(1, &g_final_tex);
	g_fbo = g_resolve = g_colour = g_depth = g_ms_colour = g_ms_depth = 0;
	g_final = g_final_tex = 0;
	g_msaa = 0; g_ss = 1;

	if (!make_target(g_w, g_h, samples, ss)) {
		fprintf(stderr, "hle: could not rebuild the target (%dx AA, %dx scale)\n",
		        samples, ss);
		make_target(g_w, g_h, 0, 1);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	if (g_vw && g_vh)
		glViewport(0, (g_h - g_vh) * g_ss, g_vw * g_ss, g_vh * g_ss);
	apply_scissor();
	ctx_leave();
}

int hle_host_msaa(void) { return g_msaa; }
int hle_host_scale(void) { return g_ss; }

/* ---- handing the frame over at draw resolution --------------------------- */

static int g_want_full;      /* the viewer will draw the game layer itself */
static int g_full_ready;     /* a frame is sitting in the resolve buffer */

void hle_host_want_full(int on) { g_want_full = on ? 1 : 0; }

/* The size of what hle_host_read_full produces: the LAYER RECTANGLE at draw
 * scale, not the whole draw buffer. Only that rectangle is ever rendered — the
 * multisample resolve covers only it, and the rest holds whatever was there
 * last time. Reporting the full buffer would have the viewer upload and draw
 * that leftover as if it were picture. */
void hle_host_full(int *w, int *h)
{
	int rw = g_vw ? g_vw : g_w, rh = g_vh ? g_vh : g_h;
	if (w) *w = rw * g_ss;
	if (h) *h = rh * g_ss;
}

/* WHERE THE PICTURE BELONGS ON THE PANEL — a destination, not a draw origin.
 *
 * The replay draws the layer at its own origin (see apply_scissor above), so
 * this is the one place the layer's PANEL position is still needed: the viewer
 * uses it to place the full-resolution game texture, exactly where the
 * compositor would have put the guest's own panel-sized copy. */
void hle_host_rect(int *x, int *y, int *w, int *h)
{
	if (x) *x = g_vw ? g_vx : 0;
	if (y) *y = g_vw ? g_vy : 0;
	if (w) *w = g_vw ? g_vw : g_w;
	if (h) *h = g_vh ? g_vh : g_h;
}

/* Read the finished frame at draw size. Costs a glReadPixels of g_dw x g_dh —
 * nine times the pixels of the old panel-sized one at 3x — which is the price
 * of not having a second GL context to share the texture with. Whether that
 * price is worth paying is a measurement, not an opinion: compare frame counts
 * over the same scripted route with it on and off. */
int hle_host_read_full(unsigned int *out)
{
	int rw = g_vw ? g_vw : g_w, rh = g_vh ? g_vh : g_h;
	int dw = rw * g_ss, dh = rh * g_ss;
	int y;

	if (!g_ready || !g_full_ready || !out) return 0;
	ctx_enter();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_resolve);
	/* Bottom-up to top-down, a row at a time, exactly as the panel-sized
	 * readback does. The layer sits at the TOP-LEFT of a panel-sized draw
	 * buffer, so its rows are the last rh of it in GL's order. */
	for (y = 0; y < dh; y++)
		glReadPixels(0, (g_h - rh) * g_ss + (dh - 1 - y),
		             dw, 1, GL_BGRA, GL_UNSIGNED_BYTE,
		             out + (size_t)y * (size_t)dw);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	ctx_leave();
	g_full_ready = 0;
	return 1;
}

int hle_host_init(const char *dir, int w, int h, int samples, int scale)
{
	char path[600];
	int fd;
	void *m;

	g_w = w; g_h = h;
#ifndef _WIN32
	snprintf(path, sizeof(path), "%s/glcmd.bin", dir);
	fd = open(path, O_RDWR | O_CREAT, 0666);
	if (fd < 0) { fprintf(stderr, "hle: cannot open %s\n", path); return 0; }
	if (ftruncate(fd, (off_t)TADGL_FILE_BYTES) != 0) { close(fd); return 0; }
	m = mmap(NULL, TADGL_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) { fprintf(stderr, "hle: cannot map glcmd.bin\n"); return 0; }
#else
	/* The ring is SHARED with the guest's GL shim, which maps glcmd.bin as a
	 * real view now — so the viewer maps the same file, and the two alias
	 * exactly as MAP_SHARED does on Linux. (A calloc'd private buffer here
	 * was the STALL: the guest wrote GL commands to the file while the
	 * replayer read an empty copy nobody filled.) Own address space, any
	 * address, plain MapViewOfFile — none of the placement gymnastics the
	 * emulator's in-reservation views needed. */
	{
		HANDLE fh, mh;
		(void)fd;
		snprintf(path, sizeof(path), "%s/glcmd.bin", dir);
		fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
		                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (fh == INVALID_HANDLE_VALUE) {
			fprintf(stderr, "hle: cannot open %s\n", path); return 0;
		}
		/* Size it so the mapping covers the whole ring even if we win the
		 * race to create the file before the guest does. */
		mh = CreateFileMappingA(fh, NULL, PAGE_READWRITE,
		                        (DWORD)((uint64_t)TADGL_FILE_BYTES >> 32),
		                        (DWORD)(TADGL_FILE_BYTES & 0xffffffffu), NULL);
		CloseHandle(fh);
		if (!mh) { fprintf(stderr, "hle: cannot map glcmd.bin\n"); return 0; }
		m = MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, TADGL_FILE_BYTES);
		CloseHandle(mh);
		if (!m) { fprintf(stderr, "hle: cannot view glcmd.bin\n"); return 0; }
	}
#endif

	g_ring = m;
	g_data = TADGL_DATA(g_ring);

	/* A SEPARATE hidden window. The visible one belongs to SDL_Renderer, and
	 * putting a GL context on it would fight with the 2D compositor. */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	                    SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	g_win = SDL_CreateWindow("tadpole hle", 0, 0, 64, 64,
	                         SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!g_win) { fprintf(stderr, "hle: no GL window: %s\n", SDL_GetError()); return 0; }
	/* Capture SDL's context BEFORE creating ours — CreateContext steals current. */
	g_prev_ctx = SDL_GL_GetCurrentContext();
	g_prev_win = SDL_GL_GetCurrentWindow();
	g_ctx = SDL_GL_CreateContext(g_win);
	if (!g_ctx) { fprintf(stderr, "hle: no GL context: %s\n", SDL_GetError()); return 0; }
	SDL_GL_SetSwapInterval(0);

	if (!gl_resolve()) {
		fprintf(stderr, "hle: OpenGL past 1.1 unavailable on this driver\n");
		return 0;
	}
	if (!make_target(w, h, samples, scale)) {
		fprintf(stderr, "hle: incomplete framebuffer object\n");
		return 0;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_DITHER);

	/* The guest checks these before it will encode anything — publish the
	 * header LAST so a half-initialised ring is never advertised. */
	g_ring->ring_bytes = TADGL_RING;
	g_ring->head = g_ring->tail = 0;
	g_ring->frames_sent = g_ring->frames_done = 0;
	g_ring->guest_fellback = 0;
	g_ring->host_alive = 1;
	g_ring->version = TADGL_VERSION;
	g_ring->magic = TADGL_MAGIC;

	printf("tadpole-view: HLE replay on %s (%s)\n",
	       (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION));
	fflush(stdout);
	{
		const char *d = getenv("TADPOLE_GL_DEBUG");
		g_level = (!d || !*d) ? 0 : (*d >= '0' && *d <= '9') ? *d - '0' : 1;
		if (g_level > 2) g_level = 2;
	}
	/* TADPOLE_HLE_DEBUG stays as its own switch for the per-frame accounting,
	 * which is replayer bookkeeping rather than GL correctness — but either
	 * variable turns on the per-opcode error check, because there is no reason
	 * to have asked for one kind of GL noise and not want that. */
	g_verbose = getenv("TADPOLE_HLE_DEBUG") != NULL;
	if (g_level >= 1)
		fprintf(stderr, "hle: TADPOLE_GL_DEBUG=%d — checking GL errors after"
		        " every opcode%s\n", g_level,
		        g_level >= 2 ? ", and ABORTING on the first" : "");
	g_ready = 1;
	ctx_leave();                 /* give SDL its context back */
	return 1;
}

void hle_host_shutdown(void)
{
	if (g_ring) g_ring->host_alive = 0;
	if (g_ctx) { ctx_leave(); SDL_GL_DeleteContext(g_ctx); }
	if (g_win) SDL_DestroyWindow(g_win);
	g_ctx = NULL; g_win = NULL; g_ready = 0;
}

int hle_host_ready(void) { return g_ready; }

/* ---- reading the ring --------------------------------------------------- */

static void ring_get(void *dst, unsigned int n)
{
	unsigned int off = g_ring->tail % TADGL_RING;
	unsigned int first = TADGL_RING - off;
	if (first > n) first = n;
	memcpy(dst, g_data + off, first);
	if (n > first)
		memcpy((unsigned char *)dst + first, g_data, n - first);
	g_ring->tail += n;
}

/* A contiguous view of the next n bytes. Returns a pointer into the ring when
 * the span does not wrap, else copies into `scratch`. Saves a copy for the
 * common case, which for a texture upload is 300 KB. */
static const unsigned char *ring_peek(unsigned int n, unsigned char **scratch,
                                      unsigned int *scap)
{
	unsigned int off = g_ring->tail % TADGL_RING;
	if (off + n <= TADGL_RING) {
		const unsigned char *p = g_data + off;
		g_ring->tail += n;
		return p;
	}
	if (*scap < n) {
		free(*scratch);
		*scratch = malloc(n);
		*scap = *scratch ? n : 0;
	}
	if (!*scratch) { g_ring->tail += n; return NULL; }
	ring_get(*scratch, n);
	return *scratch;
}

/* ---- helpers ------------------------------------------------------------ */

static GLuint host_tex(unsigned int name)
{
	if (name == 0 || name >= MAX_TEX) return 0;
	if (!g_tex[name]) glGenTextures(1, &g_tex[name]);
	return g_tex[name];
}

static unsigned int type_bytes(unsigned int t)
{
	switch (t) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
	default: return 4;                 /* GL_FLOAT, GLES_FIXED */
	}
}

/* Point host GL at one attribute array, converting GL_FIXED to float.
 *
 * `nverts` is how many vertices the draw will touch — the caller works it out,
 * because for glDrawElements that means scanning the index buffer. Without it we
 * would not know how much of a fixed-point array to convert.
 */
static const void *bind_array(int which, unsigned int nverts)
{
	struct harr *a = &g_arr[which];
	struct hbuf *b;
	unsigned int stride, i, c;
	const unsigned char *base;

	if (!a->on) return NULL;
	if (a->buf == 0 || a->buf >= MAX_BUF) return NULL;
	b = &g_buf[a->buf];
	if (!b->data) return NULL;

	stride = a->stride ? (unsigned int)a->stride
	                   : (unsigned int)a->size * type_bytes(a->type);
	if (a->off >= b->size) return NULL;
	base = b->data + a->off;

	if (a->type != GLES_FIXED)
		return base;                   /* host understands it as-is */

	/* 16.16 -> float. Desktop GL has no GL_FIXED, so this conversion is not
	 * optional; it is the single largest difference from GLES1. */
	c = (unsigned int)a->size;
	if (g_convn[which] < nverts * c) {
		free(g_conv[which]);
		g_conv[which] = malloc(nverts * c * sizeof(float));
		g_convn[which] = g_conv[which] ? nverts * c : 0;
	}
	if (!g_conv[which]) return NULL;
	for (i = 0; i < nverts; i++) {
		const int *src = (const int *)(base + (size_t)i * stride);
		unsigned int k;
		for (k = 0; k < c; k++)
			g_conv[which][i * c + k] = (float)src[k] / 65536.0f;
	}
	return g_conv[which];
}

/* Called before each draw. A draw that samples a texture we hold no pixels for
 * renders black and raises no GL error, so this is the only place the gap is
 * actually observable. */
static void want_tex_if_missing(void)
{
	if (!g_tex_enabled || !g_bound_name || g_bound_name >= MAX_TEX)
		return;
	if (g_tex_have[g_bound_name])
		return;
	g_ring->want_resync = 1;
	if (g_verbose && g_notex_logged < 12)
		fprintf(stderr, "hle: DRAW samples texture %u with no image at frame"
		        " %lu; asking for resync\n", g_bound_name, g_frames),
		g_notex_logged++;
}

/* WHICH TEXTURE UNIT A SLOT'S CLIENT STATE BELONGS TO, or -1 for the arrays
 * that are not per-unit. GL_TEXTURE_COORD_ARRAY's enable bit, and the pointer
 * behind it, live on the unit selected by glClientActiveTexture; vertex, colour
 * and normal have exactly one each and must not be touched by that selector. */
static int slot_texunit(int slot)
{
	if (slot == TADGL_ARR_TEXCOORD)  return 0;
	if (slot == TADGL_ARR_TEXCOORD1) return 1;
	return -1;
}

static void setup_arrays(unsigned int nverts)
{
	static const GLenum client[TADGL_ARR_COUNT] = {
		GL_VERTEX_ARRAY, GL_COLOR_ARRAY, GL_TEXTURE_COORD_ARRAY,
		GL_NORMAL_ARRAY, GL_TEXTURE_COORD_ARRAY };
	int i;

	for (i = 0; i < TADGL_ARR_COUNT; i++) {
		const void *p = bind_array(i, nverts);
		struct harr *a = &g_arr[i];
		unsigned int stride;
		int unit = slot_texunit(i);

		/* SELECT THE UNIT BEFORE TOUCHING ITS CLIENT STATE. Without this every
		 * glTexCoordPointer landed on whichever unit was selected last — in
		 * practice unit 0, forever — so a title with two coordinate sets had
		 * both writes hit one unit and the second silently won. That is what put
		 * a pink patch over Pet Pals 2's dogs: unit 0 held the 256x256 skin and
		 * was handed the 512x512 eye atlas's coordinates. */
		if (unit >= 0 && g_gl.ClientActiveTexture)
			g_gl.ClientActiveTexture(GL_TEXTURE0 + (GLenum)unit);

		if (!p) {
			if (i == TADGL_ARR_VERTEX) g_de_noarr++;   /* nothing to draw from */
			glDisableClientState(client[i]);
			continue;
		}
		/* After conversion the data is tightly packed floats, so the original
		 * stride no longer applies. */
		stride = (a->type == GLES_FIXED) ? 0 : (unsigned int)a->stride;
		glEnableClientState(client[i]);
		switch (i) {
		case TADGL_ARR_VERTEX:
			glVertexPointer(a->size, a->type == GLES_FIXED ? GL_FLOAT : a->type,
			                (GLsizei)stride, p);
			break;
		case TADGL_ARR_COLOR:
			glColorPointer(a->size, a->type == GLES_FIXED ? GL_FLOAT : a->type,
			               (GLsizei)stride, p);
			break;
		/* Both texcoord slots take the same call; they differ only in the unit
		 * selected above. */
		case TADGL_ARR_TEXCOORD:
		case TADGL_ARR_TEXCOORD1:
			glTexCoordPointer(a->size, a->type == GLES_FIXED ? GL_FLOAT : a->type,
			                  (GLsizei)stride, p);
			break;
		case TADGL_ARR_NORMAL:
			glNormalPointer(a->type == GLES_FIXED ? GL_FLOAT : a->type,
			                (GLsizei)stride, p);
			break;
		}
	}
	/* HAND THE CLIENT SELECTOR BACK TO UNIT 0. Nothing else in this file sets
	 * it, so leaving it on unit 1 would make the next draw's first
	 * glTexCoordPointer land on the wrong unit — the same class of leak as the
	 * matrix mode in skin_end(). */
	if (g_gl.ClientActiveTexture)
		g_gl.ClientActiveTexture(GL_TEXTURE0);
}

/* ---- the replay loop ---------------------------------------------------- */

/* Returns 1 when a PRESENT was replayed, i.e. `out` now holds a finished frame. */
int hle_host_pump(unsigned int *out, unsigned int pitch_px)
{
	static unsigned char *scratch;
	static unsigned int scap;
	int presented = 0;

	if (!g_ready) return 0;
	g_ring->host_alive++;              /* counter, not a flag — see the header */

	/* HOW LONG SINCE THE LAST PUMP? The guest waits on us, so a long gap here is
	 * what made it conclude the host was dead. Report the outliers so the cause
	 * is visible rather than inferred. */
	{
		static Uint32 last;
		Uint32 now = SDL_GetTicks();
		if (last && now - last > 250)
			fprintf(stderr, "hle: STALL — %u ms since the last pump"
			        " (guest is waiting on us)\n", now - last);
		last = now;
	}
	if (g_ring->guest_fellback && !g_fellback_seen) {
		g_fellback_seen = 1;
		fprintf(stderr, "hle: ***** THE GUEST FELL BACK TO SOFTWARE *****\n");
	}
	ctx_enter();
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);

	while (g_ring->tail != g_ring->head) {
		struct tadgl_pkt p;
		unsigned int padded, avail;

		/* NEVER LET tail PASS head. head - tail is unsigned, so one bad packet
		 * that over-advances tail makes `avail` underflow to ~4 billion, every
		 * subsequent read is garbage, and each garbage length advances tail
		 * further — a runaway that ends with tail at 2.6 billion against a head
		 * of 84. That is exactly what happened on the first live run, and the
		 * guest saw it as "host stopped draining" and fell back to software.
		 *
		 * Treat a desync as fatal to the FRAME, not to the session: resync to
		 * head, say so once, and carry on. */
		/* Read head once, with a barrier, so the payload stores that preceded
		 * it are visible before we treat those bytes as a packet. */
		__sync_synchronize();
		avail = g_ring->head - g_ring->tail;
		if (avail > TADGL_RING) {
			if (!g_desync++) {
				unsigned int k;
				fprintf(stderr, "hle: DESYNC (tail %u past head %u); resyncing\n",
				        g_ring->tail, g_ring->head);
				fprintf(stderr, "hle: last %d packets (op, len, tail before,"
				        " tail after):\n", HIST);
				for (k = (g_hist_n > HIST ? g_hist_n - HIST : 0);
				     k < g_hist_n; k++) {
					unsigned int i2 = k % HIST;
					fprintf(stderr, "hle:   op %2u len %7u  tail %8u -> %8u"
					        " (consumed %d)\n",
					        g_hist[i2].op, g_hist[i2].len,
					        g_hist[i2].tail_before, g_hist[i2].tail_after,
					        (int)(g_hist[i2].tail_after -
					              g_hist[i2].tail_before));
				}
			}
			g_ring->tail = g_ring->head;
			break;
		}
		if (avail < sizeof p) break;              /* header not fully written */
		{
			unsigned int save = g_ring->tail;
			ring_get(&p, sizeof p);
			padded = (p.len + 3u) & ~3u;
			/* Validate before trusting either field. A bad op or an absurd
			 * length means we are not looking at a packet boundary at all, and
			 * crawling forward would only corrupt more.
			 *
			 * "Absurd" IS THE WRITER'S LIMIT, not a rule of thumb. This read
			 * TADGL_RING / 2 and threw away a legitimate 1024x1024 texture that
			 * pkt_begin had been perfectly willing to write — see
			 * TADGL_MAX_PAYLOAD, which both ends now share so they cannot drift
			 * apart again. */
			if (p.op >= TADGL_OP_COUNT || padded > TADGL_MAX_PAYLOAD) {
				if (!g_desync++)
					fprintf(stderr, "hle: bad packet op=%u len=%u at %u;"
					        " resyncing\n", p.op, p.len, save);
				g_ring->tail = g_ring->head;
				break;
			}
			if (g_ring->head - g_ring->tail < padded) {
				g_ring->tail = save;              /* payload still in flight */
				break;
			}
		}
		g_packets++;
		{
			unsigned int k = g_hist_n % HIST;
			g_hist[k].op = p.op; g_hist[k].len = p.len;
			g_hist[k].tail_before = g_ring->tail;
			g_hist_n++;
		}

		switch (p.op) {
		case TADGL_PRESENT: {
			/* Read straight into the caller's framebuffer row order. GL's
			 * origin is bottom-left and the panel's is top-left, so flip.
			 *
			 * The layer is drawn at ITS OWN origin and read back to the same
			 * place in the guest's buffer, which is where the guest's driver
			 * puts it: win_w x win_h from the layer's base address. Placing it
			 * on the panel is the compositor's job. */
			int rw = g_vw ? g_vw : g_w, rh = g_vh ? g_vh : g_h;
			int y;
			check_gl("frame");
			/* Resolve the samples down before reading. Only the layer
			 * rectangle is blitted, for the same reason only it is read back:
			 * the rest of the panel belongs to other layers, and writing the
			 * FBO's untouched black over it would hide the video plane
			 * underneath. GL_NEAREST is required for a multisample source —
			 * the averaging is the resolve itself, not the filter. */
			{
				/* The layer rectangle in GL's bottom-up coordinates, at panel
				 * scale and at draw scale. It occupies the top-left of a
				 * panel-sized buffer, so in GL's order it is the last rh rows. */
				int py0 = g_h - rh, py1 = g_h;
				int dx0 = 0, dx1 = rw * g_ss;
				int dy0 = py0 * g_ss, dy1 = py1 * g_ss;

				if (g_msaa) {
					/* Same size, so GL_NEAREST — the averaging IS the resolve,
					 * not the filter. */
					glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve);
					glBlitFramebuffer(dx0, dy0, dx1, dy1,
					                  dx0, dy0, dx1, dy1,
					                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
				}
				/* THE DOWNSCALE IS SKIPPED when the viewer is taking the
				 * full-size frame: squeezing 1440x816 into 320x240 only to
				 * throw it away is the one step in this chain with no
				 * purpose. The guest's own layer then holds a stale picture,
				 * which is exactly right — nothing reads it, and the viewer
				 * draws over that rectangle with the big one. */
				if (g_ss > 1 && !g_want_full) {
					/* Down to panel size. GL_LINEAR here is the whole point:
					 * it is what turns g_ss*g_ss rendered samples into one
					 * averaged pixel. */
					glBindFramebuffer(GL_READ_FRAMEBUFFER, g_resolve);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_final);
					glBlitFramebuffer(dx0, dy0, dx1, dy1,
					                  0, py0, rw, py1,
					                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
				}
				g_full_ready = 1;
				if (g_msaa || g_ss > 1)
					glBindFramebuffer(GL_READ_FRAMEBUFFER, g_final);
			}
			glFinish();
			/* The panel-sized readback exists to fill the guest's layer. When
			 * the viewer is drawing the game itself it does not need filling,
			 * and this is the expensive line in the whole replay — a
			 * synchronous transfer of the finished frame, every frame. */
			if (!g_want_full)
				for (y = 0; y < rh; y++)
					glReadPixels(0, g_h - 1 - y, rw, 1, GL_BGRA,
					             GL_UNSIGNED_BYTE,
					             out + (size_t)y * pitch_px);
			/* Back to the draw target: more frames may follow in this same
			 * pump, and they must not land in a resolve buffer. */
			if (g_msaa || g_ss > 1)
				glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
			g_frames++;
			g_ring->frames_done++;
			presented = 1;
			/* Report what the frame actually contained, once a second. "60 fps"
			 * means nothing if every frame is empty. */
			if (g_verbose) {
				static unsigned long last_report;
				if (g_frames - last_report >= 60) {
					unsigned int i2, nz = 0;
					for (i2 = 0; i2 < (unsigned)(g_w * g_h); i2 += 37)
						if (out[i2] & 0x00FFFFFFu) nz++;
					/* THE MATRIX ITSELF, not a count of pokes at it. If the
					 * translation or scale terms grow every second, the texture
					 * matrix is accumulating and every textured surface slides —
					 * which is the "conveyor belt" symptom exactly. Identity
					 * reads 1,1 scale and 0,0 translation. */
					GLfloat tm[16];
					GLint pm = 0;
					glGetIntegerv(GL_MATRIX_MODE, &pm);
					glMatrixMode(GL_TEXTURE);
					glGetFloatv(GL_TEXTURE_MATRIX, tm);
					glMatrixMode((GLenum)pm);
					fprintf(stderr, "hle: texmatrix: %lu sets, %lu ops | "
					        "scale %.3f,%.3f  translate %.3f,%.3f\n",
					        g_mat_tex_sets, g_mat_tex_ops,
					        tm[0], tm[5], tm[12], tm[13]);
					fprintf(stderr, "hle: frame %lu: %lu draws, %u/%u px non-black,"
					        " glerr 0x%04X x%lu | drawelem pkts %lu skipped-nobuf"
					        " %lu no-array %lu\n",
					        g_frames, g_draws, nz,
					        (unsigned)(g_w * g_h) / 37, g_gl_err, g_err_count,
					        g_de_pkts, g_de_nobuf, g_de_noarr);
					g_de_pkts = g_de_nobuf = g_de_noarr = 0;
					last_report = g_frames;
				}
			}
			g_draws = 0;
			break;
		}
		case TADGL_CLEAR: {
			unsigned int v[3]; float d;
			ring_get(v, 12);
			memcpy(&d, &v[2], 4);
			glClearColor((float)((v[1] >> 16) & 0xFF) / 255.0f,
			             (float)((v[1] >> 8) & 0xFF) / 255.0f,
			             (float)(v[1] & 0xFF) / 255.0f,
			             (float)((v[1] >> 24) & 0xFF) / 255.0f);
			glClearDepth((double)d);
			glClear((v[0] & 0x4000 ? GL_COLOR_BUFFER_BIT : 0) |
			        (v[0] & 0x0100 ? GL_DEPTH_BUFFER_BIT : 0));
			break;
		}
		case TADGL_VIEWPORT: {
			int v[4]; ring_get(v, 16);
			/* The packet carries the whole layer rectangle, but only its SIZE
			 * is a draw parameter: the picture is rendered at the layer's own
			 * origin, and (v[0],v[1]) is where the compositor will later put
			 * it. Drawing at the panel position as well would offset it twice.
			 *
			 * The guest's y is measured from the top and GL's from the bottom,
			 * and the layer lives in the top-left of a panel-sized draw buffer,
			 * so the flip is about the panel height. Scaled into that buffer,
			 * which is g_ss times the panel. */
			glViewport(0, (g_h - v[3]) * g_ss, v[2] * g_ss, v[3] * g_ss);
			g_vx = v[0]; g_vy = v[1]; g_vw = v[2]; g_vh = v[3];
			apply_scissor();
			break;
		}
		/* MATCH THE REFERENCE RENDERER, do not out-implement it.
		 *
		 * The software rasteriser ignores culling and lighting entirely, and its
		 * output is known good — it is the thing we diff against. Honouring
		 * those caps on the host introduces failures the reference cannot have:
		 * with GL_CULL_FACE on, the host culls using its DEFAULT winding and
		 * face, because glCullFace/glFrontFace were never part of the stream, so
		 * a title whose triangles wind the other way loses every one of them.
		 * That is exactly the symptom this hit — 14 draws issued per frame, no
		 * GL error, and nothing but the clear colour on screen.
		 *
		 * Lighting is the same trade: enabling it without the light state the
		 * reference never consumed would shade everything black.
		 */
		case TADGL_ENABLE: {
			unsigned int c;
			ring_get(&c, 4);
			/* GL_CULL_FACE IS NOW HONOURED. The exemption above was
			 * conditional on glCullFace/glFrontFace not being in the stream —
			 * both are forwarded now, so the host culls with the title's own
			 * winding rather than its default, and the reason to skip it is
			 * gone. Leaving it off is not neutral: without culling the back
			 * faces of signs and buildings draw over their fronts, which is
			 * why lettering appeared mirrored.
			 *
			 * GL_LIGHTING IS NO LONGER FILTERED. The reason it was — "the
			 * light and material state is still missing, so enabling lighting
			 * shades everything black" — stopped being true when glLight*,
			 * glMaterial*, glLightModel* and glNormalPointer landed. The
			 * filter was always a symptom, and removing it is the point of
			 * having implemented them.
			 *
			 * GL_FOG stays filtered, and now for a measured reason rather than
			 * an aspirational one: tools/gl-demand.py reports that NO installed
			 * title imports any glFog* entry point, so fog state can only ever
			 * be the GL defaults here — white, exponential, density 1 — which
			 * is not what any title that enabled fog would be asking for. */
			if (c == 0x0B60 /* GL_FOG */) {
				/* SAY WHY, not just what. "ignoring glEnable(0x0B60)" reads
				 * as arbitrary policy; the log is where someone will go
				 * looking for why fog does nothing. */
				if (!g_filtered++)
					fprintf(stderr, "hle: ignoring glEnable(GL_FOG) — every"
					        " glFog* entry point is still a stub, and no"
					        " installed title imports one (tools/gl-demand.py),"
					        " so fog state here could only ever be the GL"
					        " defaults. Implement glFog*/glFogx* and delete"
					        " this filter if a title ever needs it.\n");
				break;
			}
			/* CONSUMED, NOT FORWARDED — see apply_scissor(). The host's
			 * scissor register belongs to the layer window; the title's box
			 * only narrows it. */
			if (c == 0x0C11 /* GL_SCISSOR_TEST */) {
				g_sc_on = 1; apply_scissor(); break; }
			if (c == 0x0DE1 /* GL_TEXTURE_2D */) g_tex_enabled = 1;
			glEnable(c);
			break;
		}
		case TADGL_DISABLE: {
			unsigned int c;
			ring_get(&c, 4);
			if (c == 0x0C11 /* GL_SCISSOR_TEST */) {
				g_sc_on = 0; apply_scissor(); break; }
			if (c == 0x0DE1 /* GL_TEXTURE_2D */) g_tex_enabled = 0;
			glDisable(c);
			break;
		}
		case TADGL_BLENDFUNC: { unsigned int v[2]; ring_get(v,8); glBlendFunc(v[0],v[1]); break; }
		case TADGL_DEPTHFUNC: { unsigned int f; ring_get(&f,4); glDepthFunc(f); break; }
		case TADGL_DEPTHMASK: { unsigned int o; ring_get(&o,4); glDepthMask(o?GL_TRUE:GL_FALSE); break; }
		case TADGL_CULLFACE:  { unsigned int m; ring_get(&m,4); glCullFace(m); break; }
		case TADGL_FRONTFACE: { unsigned int m; ring_get(&m,4); glFrontFace(m); break; }
		case TADGL_SHADEMODEL:{ unsigned int m; ring_get(&m,4); glShadeModel(m); break; }
		case TADGL_ALPHAFUNC: { unsigned int v[2]; float r; ring_get(v,8);
		                        memcpy(&r,&v[1],4); glAlphaFunc(v[0], r); break; }
		case TADGL_TEXENV: {
			unsigned int v[3];
			ring_get(v, 12);
			/* STILL A WHITELIST, because forwarding whatever arrives is a
			 * guaranteed GL_INVALID_ENUM the moment a title passes something
			 * that is not a mode, and one rejected call leaves the error flag
			 * set for everything after it. But it is now the whole of GLES 1.1
			 * TexEnv, GL_COMBINE included.
			 *
			 * COMBINE WAS DROPPED AND IT MATTERED. With the drops logged, one
			 * Clam Prix race showed exactly what it was asking for:
			 *
			 *   ENV_MODE=COMBINE  COMBINE_RGB=MODULATE  COMBINE_ALPHA=MODULATE
			 *   SRC0_RGB=TEXTURE  SRC1_RGB=CONSTANT
			 *   SRC0_ALPHA=TEXTURE SRC1_ALPHA=CONSTANT  OPERAND0_ALPHA=SRC_ALPHA
			 *
			 * — texture times the constant colour, in both colour and alpha.
			 * That is a per-object tint and fade. Dropping it left those
			 * surfaces modulating against the primary colour instead: wrong
			 * tint, and no fade at all.
			 *
			 * It was dropped originally because the combiner SOURCES were not
			 * forwarded, so honouring the mode would have used stale operands.
			 * They are all forwarded now, which is what makes this safe. Every
			 * enum here is core desktop GL 1.3 with the same value. */
			if (v[1] == 0x2200 /* GL_TEXTURE_ENV_MODE */) {
				switch (v[2]) {
				case 0x2100: case 0x1E01: case 0x2101:   /* MODULATE REPLACE DECAL */
				case 0x0BE2: case 0x0104: case 0x8570:   /* BLEND ADD COMBINE */
					glTexEnvi(0x2300, 0x2200, (GLint)v[2]);
					break;
				default: goto texenv_dropped;
				}
			} else if (v[1] == 0x8573 /* RGB_SCALE */ ||
			           v[1] == 0x0D1C /* ALPHA_SCALE */) {
				/* Sent as an integer because GLES allows only 1, 2 or 4 —
				 * see the guest side. Back to float here, where GL wants it. */
				glTexEnvf(0x2300, v[1], (GLfloat)(GLint)v[2]);
			} else if ((v[1] >= 0x8571 && v[1] <= 0x8572) ||  /* COMBINE_RGB/ALPHA */
			           (v[1] >= 0x8580 && v[1] <= 0x8582) ||  /* SRC0..2_RGB */
			           (v[1] >= 0x8588 && v[1] <= 0x858A) ||  /* SRC0..2_ALPHA */
			           (v[1] >= 0x8590 && v[1] <= 0x8592) ||  /* OPERAND0..2_RGB */
			           (v[1] >= 0x8598 && v[1] <= 0x859A)) {  /* OPERAND0..2_ALPHA */
				glTexEnvi(0x2300, v[1], (GLint)v[2]);
			} else {
texenv_dropped:
				if (g_level >= 1 && g_texenv_logged < 8) {
					g_texenv_logged++;
					fprintf(stderr, "hle: dropping glTexEnv(pname=0x%04X"
					        " value=0x%04X) — not forwarded\n", v[1], v[2]);
				}
			}
			break;
		}
		case TADGL_TEXENVCOLOR: {
			float c[4];
			ring_get(c, 16);
			glTexEnvfv(0x2300 /* GL_TEXTURE_ENV */, 0x2201 /* ENV_COLOR */, c);
			break;
		}

		/* SCISSOR COORDINATES NEED NO TRANSLATION. We render into an FBO whose
		 * pixels are the guest's framebuffer pixels one-for-one — that is the
		 * same reason TADGL_VIEWPORT is forwarded verbatim — so the guest's
		 * scissor box is already in this context's window space. */
		case TADGL_SCISSOR: {
			int v[4];
			ring_get(v, 16);
			g_sc[0] = v[0]; g_sc[1] = v[1]; g_sc[2] = v[2]; g_sc[3] = v[3];
			apply_scissor();
			break;
		}
		case TADGL_COLORMASK: {
			unsigned int v[4];
			ring_get(v, 16);
			glColorMask(v[0] ? GL_TRUE : GL_FALSE, v[1] ? GL_TRUE : GL_FALSE,
			            v[2] ? GL_TRUE : GL_FALSE, v[3] ? GL_TRUE : GL_FALSE);
			break;
		}
		case TADGL_LINEWIDTH: {
			float w; ring_get(&w, 4);
			/* Desktop GL rejects a width of 0 with GL_INVALID_VALUE, and
			 * anything above GL_ALIASED_LINE_WIDTH_RANGE's maximum is clamped
			 * rather than refused — so guard only the zero. */
			if (w > 0.0f) glLineWidth(w);
			break;
		}
		case TADGL_POINTSIZE: {
			float s; ring_get(&s, 4);
			if (s > 0.0f) glPointSize(s);
			break;
		}
		case TADGL_POLYGONOFFSET: {
			float v[2]; ring_get(v, 8);
			glPolygonOffset(v[0], v[1]);
			break;
		}

		/* ---- lighting ---------------------------------------------------
		 * Straight through. Desktop GL's compatibility profile has the same
		 * fixed-function pipeline and the same enum values, and the guest has
		 * already validated light index, face and pname — so anything arriving
		 * here is a combination GLES 1.1 defines, which desktop GL also
		 * defines. Nothing to filter, unlike TEXENV and TEXPARAM where GLES
		 * has parameters the desktop does not. */
		case TADGL_LIGHT: {
			unsigned int hd[3]; float v[4];
			ring_get(hd, 12);
			if (hd[2] > 4) hd[2] = 4;
			ring_get(v, hd[2] * 4);
			glLightfv(hd[0], hd[1], v);
			break;
		}
		case TADGL_MATERIAL: {
			unsigned int hd[3]; float v[4];
			ring_get(hd, 12);
			if (hd[2] > 4) hd[2] = 4;
			ring_get(v, hd[2] * 4);
			glMaterialfv(hd[0], hd[1], v);
			break;
		}
		case TADGL_LIGHTMODEL: {
			unsigned int hd[2]; float v[4];
			ring_get(hd, 8);
			if (hd[1] > 4) hd[1] = 4;
			ring_get(v, hd[1] * 4);
			glLightModelfv(hd[0], v);
			break;
		}
		case TADGL_NORMAL: {
			float v[3]; ring_get(v, 12);
			glNormal3f(v[0], v[1], v[2]);
			break;
		}
		case TADGL_COLOR:     { float v[4]; ring_get(v,16);
		                        glColor4f(v[0],v[1],v[2],v[3]); break; }

		case TADGL_MATRIXMODE:  {
			unsigned int m; ring_get(&m,4);
			/* IS THE TEXTURE MATRIX IN PLAY? The guest has only two matrix
			 * stacks — projection and modelview — so glMatrixMode(GL_TEXTURE)
			 * silently lands in its modelview one, while the raw enum is
			 * forwarded here where a real texture matrix exists. If a title
			 * animates that matrix (a standard trick for scrolling road and
			 * water) the two sides disagree about what is being transformed,
			 * and every textured surface slides. Counting the ops per mode is
			 * how we find out whether that is happening at all. */
			g_mat_mode = m;
			if (m == 0x1702) g_mat_tex_sets++;
			glMatrixMode(m); break; }
		case TADGL_LOADIDENTITY:
			if (g_mat_mode == 0x1702) g_mat_tex_ops++;
			glLoadIdentity(); break;
		case TADGL_PUSHMATRIX:
			if (g_mat_mode == 0x1702) g_mat_tex_ops++;
			glPushMatrix();   break;
		case TADGL_POPMATRIX:
			if (g_mat_mode == 0x1702) g_mat_tex_ops++;
			glPopMatrix();    break;
		case TADGL_LOADMATRIX:  { float m[16]; ring_get(m,64);
			if (g_mat_mode == 0x1702) g_mat_tex_ops++;
			glLoadMatrixf(m); break; }
		case TADGL_MULTMATRIX:  { float m[16]; ring_get(m,64);
			if (g_mat_mode == 0x1702) g_mat_tex_ops++;
			glMultMatrixf(m); break; }
		case TADGL_ORTHO:       { float v[6]; ring_get(v,24);
		                          glOrtho(v[0],v[1],v[2],v[3],v[4],v[5]); break; }
		case TADGL_FRUSTUM:     { float v[6]; ring_get(v,24);
		                          glFrustum(v[0],v[1],v[2],v[3],v[4],v[5]); break; }
		case TADGL_TRANSLATE:   { float v[3]; ring_get(v,12); (g_mat_mode == 0x1702 ? g_mat_tex_ops++ : 0), glTranslatef(v[0],v[1],v[2]); break; }
		case TADGL_SCALE:       { float v[3]; ring_get(v,12); (g_mat_mode == 0x1702 ? g_mat_tex_ops++ : 0), glScalef(v[0],v[1],v[2]); break; }
		case TADGL_ROTATE:      { float v[4]; ring_get(v,16); (g_mat_mode == 0x1702 ? g_mat_tex_ops++ : 0), glRotatef(v[0],v[1],v[2],v[3]); break; }

		case TADGL_BINDTEXTURE: {
			unsigned int n;
			ring_get(&n, 4);
			glBindTexture(GL_TEXTURE_2D, host_tex(n));
			/* DO NOT request a resync here. glGenTextures -> glBindTexture ->
			 * glTexImage2D is the normal GL idiom, so "bound with no image yet"
			 * is the expected state for every texture the title creates, not an
			 * error. Treating it as one asked for a full resync on every single
			 * creation — 11 of them, each re-sending EVERY texture — which
			 * flooded the ring with megabytes of redundant uploads. Frames drawn
			 * during that flood came out black, which is exactly why the first
			 * two boot logos failed and the third, after things settled, worked.
			 *
			 * The genuine failure is a DRAW that samples a texture we have no
			 * pixels for, so the check belongs at the draw, below. */
			g_bound_name = n;
			break;
		}
		case TADGL_ACTIVETEXTURE:{ unsigned int u; ring_get(&u,4);
		                          glActiveTexture(GL_TEXTURE0 + u); break; }
		/* WHITELISTED, and this is the standing "GL error 0x0500 from TEXPARAM"
		 * in every race. It used to forward whatever pname arrived, and GLES1
		 * has parameters desktop GL does not — GL_TEXTURE_CROP_RECT_OES above
		 * all — so one such call per frame poisoned every draw after it until
		 * something read glGetError. The guest filters these now too; this is
		 * the second line of defence, because the guest and this replayer are
		 * built separately and a stale pair must not resurrect the bug. */
		case TADGL_TEXPARAM: {
			unsigned int v[2];
			ring_get(v, 8);
			switch (v[0]) {
			case 0x2800: /* MAG_FILTER */ case 0x2801: /* MIN_FILTER */
			case 0x2802: /* WRAP_S */     case 0x2803: /* WRAP_T */
			case 0x8191: /* GENERATE_MIPMAP */
				glTexParameteri(GL_TEXTURE_2D, v[0], (GLint)v[1]);
				break;
			default:
				if (g_level >= 1 && g_texparam_logged < 8) {
					g_texparam_logged++;
					fprintf(stderr, "hle: dropping glTexParameteri(pname=0x%04X"
					        " value=0x%04X) — desktop GL has no such"
					        " parameter\n", v[0], v[1]);
				}
				break;
			}
			break;
		}
		case TADGL_DELETETEXTURE:{
			unsigned int n; ring_get(&n,4);
			if (n < MAX_TEX) {
				if (g_tex[n]) { glDeleteTextures(1, &g_tex[n]); g_tex[n] = 0; }
				/* CLEAR have[] TOO. Leaving it set tells
				 * want_tex_if_missing() we still hold an image for a texture
				 * that no longer exists, so the next draw on that name
				 * silently samples nothing instead of asking for the new
				 * upload — and never reports why. */
				g_tex_have[n] = 0;
			}
			break; }
		case TADGL_TEXIMAGE2D: {
			unsigned int hd[3];
			const unsigned char *px;
			ring_get(hd, 12);
			/* Derived from p.len, not w*h*4, for the same reason. */
			px = (p.len > 12) ? ring_peek(p.len - 12, &scratch, &scap) : NULL;
			glBindTexture(GL_TEXTURE_2D, host_tex(hd[0]));
			/* The guest already converted to ARGB8888, which on a
			 * little-endian host is BGRA byte order. */
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)hd[1],
			             (GLsizei)hd[2], 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
			if (hd[0] < MAX_TEX) g_tex_have[hd[0]] = 1;
			if (g_verbose && g_ti_logged < 40) {
				g_ti_logged++;
				fprintf(stderr, "hle: TEXIMAGE name=%u %ux%u frame=%lu\n",
				        hd[0], hd[1], hd[2], g_frames);
			}
			break;
		}
		case TADGL_TEXSUBIMAGE2D: {
			unsigned int hd[5];
			const unsigned char *px;
			ring_get(hd, 20);
			px = (p.len > 20) ? ring_peek(p.len - 20, &scratch, &scap) : NULL;
			glBindTexture(GL_TEXTURE_2D, host_tex(hd[0]));
			glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)hd[1], (GLint)hd[2],
			                (GLsizei)hd[3], (GLsizei)hd[4], GL_BGRA,
			                GL_UNSIGNED_BYTE, px);
			break;
		}

		case TADGL_BUFFERDATA: {
			unsigned int hd[2], blob;
			const unsigned char *src = NULL;
			ring_get(hd, 8);
			/* THE PACKET LENGTH IS AUTHORITATIVE, never a header field.
			 *
			 * glBufferData(target, size, NULL) allocates without initialising,
			 * so the guest sends name+size and NO payload — an 8-byte packet
			 * whose size field still says 38400. Trusting the size field made
			 * the host consume 38408 bytes for an 8-byte packet, which pushed
			 * tail past head and destroyed every upload that followed. That is
			 * what blacked out the boot logos, and it was deterministic:
			 * "tail 38468 past head 17636" reproduced exactly across runs. */
			blob = (p.len >= 8) ? p.len - 8 : 0;
			if (blob) src = ring_peek(blob, &scratch, &scap);
			if (g_verbose && g_bd_logged < 24) {
				g_bd_logged++;
				fprintf(stderr, "hle: BUFFERDATA name=%u size=%u src=%s\n",
				        hd[0], hd[1], src ? "yes" : "NULL");
			}
			if (hd[0] && hd[0] < MAX_BUF) {
				struct hbuf *b = &g_buf[hd[0]];
				free(b->data);
				b->data = malloc(hd[1] ? hd[1] : 1);
				b->size = b->data ? hd[1] : 0;
				if (b->data && src)
					memcpy(b->data, src, blob < hd[1] ? blob : hd[1]);
			}
			break;
		}
		case TADGL_BUFFERSUBDATA: {
			unsigned int hd[3], blob;
			const unsigned char *src = NULL;
			ring_get(hd, 12);
			blob = (p.len >= 12) ? p.len - 12 : 0;   /* not hd[2] — see above */
			if (blob) src = ring_peek(blob, &scratch, &scap);
			if (hd[0] && hd[0] < MAX_BUF && src) {
				struct hbuf *b = &g_buf[hd[0]];
				if (b->data && hd[1] + blob <= b->size)
					memcpy(b->data + hd[1], src, blob);
			}
			break;
		}
		case TADGL_DELETEBUFFER: {
			unsigned int n; ring_get(&n,4);
			if (n && n < MAX_BUF) { free(g_buf[n].data);
			                        g_buf[n].data = NULL; g_buf[n].size = 0; }
			break;
		}

		case TADGL_RESET: {
			/* The guest tore its context down. Drop EVERY mirror.
			 *
			 * Not doing so is worse than a leak: the next title's
			 * glGenTextures reuses low names, and a stale g_tex_have[] entry
			 * says we already hold an image for one — so the draw silently
			 * samples the PREVIOUS game's texture instead of asking for the
			 * new one. Clearing g_tex_have is therefore the important half;
			 * deleting the GL objects merely reclaims memory. */
			unsigned int k;
			for (k = 0; k < MAX_TEX; k++) {
				if (g_tex[k]) glDeleteTextures(1, &g_tex[k]);
				g_tex[k] = 0;
				g_tex_have[k] = 0;
			}
			for (k = 0; k < MAX_BUF; k++) {
				free(g_buf[k].data);
				g_buf[k].data = NULL;
				g_buf[k].size = 0;
			}
			for (k = 0; k < TADGL_ARR_COUNT; k++) {
				g_arr[k].on = 0;
				g_arr[k].buf = 0;
			}
			g_bound_name = 0;
			g_tex_enabled = 0;
			/* The next title starts with no scissor box of its own. Leaving
			 * the previous one set would clip the new title to a rectangle it
			 * never asked for — the same class of cross-title leak as the
			 * texture mirrors above, and just as hard to attribute. */
			g_sc_on = 0;
			apply_scissor();
			if (g_verbose)
				fprintf(stderr, "hle: context reset — mirrors dropped\n");
			break;
		}

		case TADGL_ARRAYPOINTER: {
			unsigned int v[6]; ring_get(v,24);
			if (v[0] < TADGL_ARR_COUNT) {
				struct harr *a = &g_arr[v[0]];
				a->buf = v[1]; a->size = (int)v[2]; a->type = v[3];
				a->stride = (int)v[4]; a->off = v[5];
			}
			break;
		}
		case TADGL_CLIENTSTATE: {
			unsigned int v[2]; ring_get(v,8);
			if (v[0] < TADGL_ARR_COUNT) g_arr[v[0]].on = v[1];
			break;
		}

		case TADGL_DRAWARRAYS: {
			unsigned int v[3]; ring_get(v,12);
			want_tex_if_missing();
			setup_arrays((unsigned)v[1] + (unsigned)v[2]);
			glDrawArrays(v[0], (GLint)v[1], (GLsizei)v[2]);
			g_draws++;
			break;
		}
		case TADGL_DRAWELEMENTS: {
			unsigned int v[5];
			ring_get(v, 20);
			g_de_pkts++;
			if (!(v[3] && v[3] < MAX_BUF && g_buf[v[3]].data)) {
				/* No mirrored element buffer. Say WHICH name and what we
				 * hold for it — "no buffer" could equally mean the guest sent
				 * name 0, a name past our table, or a name we never got data
				 * for, and those need different fixes. */
				g_de_nobuf++;
				g_ring->want_resync = 1;      /* ask the guest to resend */
				if (g_verbose && !g_nobuf_logged++)
					fprintf(stderr, "hle: drawelements elembuf=%u (MAX_BUF %d)"
					        " data=%p size=%u; buffers held:%s\n",
					        v[3], MAX_BUF,
					        (v[3] < MAX_BUF) ? (void *)g_buf[v[3]].data : NULL,
					        (v[3] < MAX_BUF) ? g_buf[v[3]].size : 0,
					        "");
				if (g_verbose && g_nobuf_logged == 1) {
					unsigned int k;
					fprintf(stderr, "hle:   mirrored buffers:");
					for (k = 1; k < MAX_BUF; k++)
						if (g_buf[k].data)
							fprintf(stderr, " %u(%u bytes)", k, g_buf[k].size);
					fprintf(stderr, "\n");
					g_nobuf_logged = 2;
				}
			}
			if (v[3] && v[3] < MAX_BUF && g_buf[v[3]].data) {
				struct hbuf *eb = &g_buf[v[3]];
				const unsigned char *idx = eb->data + v[4];
				unsigned int n = (unsigned int)v[1], i, maxi = 0;
				/* Scan for the highest index: that is how many vertices a
				 * fixed-point array has to convert. Nothing else knows. */
				if (v[2] == GL_UNSIGNED_SHORT) {
					const unsigned short *s = (const unsigned short *)idx;
					for (i = 0; i < n; i++) if (s[i] > maxi) maxi = s[i];
				} else {
					for (i = 0; i < n; i++) if (idx[i] > maxi) maxi = idx[i];
				}
				want_tex_if_missing();
				setup_arrays(maxi + 1);
				glDrawElements(v[0], (GLsizei)n, v[2], idx);
				g_draws++;
			}
			break;
		}
		default:
			/* Unknown opcode: the length field is what makes this survivable —
			 * skip the payload and carry on rather than desynchronising. */
			g_ring->tail += padded;
			padded = 0;
			break;
		}

		/* WHICH CALL WAS REJECTED. check_gl() samples once per frame because
		 * glGetError is a pipeline flush point, which is right for the steady
		 * state but useless for diagnosis: "GL error 0x0500 at frame" says an
		 * enum was rejected somewhere among thousands of commands. Under
		 * TADPOLE_GL_DEBUG (or TADPOLE_HLE_DEBUG), check after every opcode and
		 * name it — once per distinct (opcode, error) pair, so a call that fails
		 * on every draw reports once rather than flooding.
		 *
		 * At level 2 the FIRST one is fatal. That is the whole point of the
		 * level: a GL error silently disables the call that raised it and every
		 * later call still succeeds, so the frame comes back subtly wrong with
		 * no other symptom, and by the time anyone looks the evidence is 900
		 * commands downstream. Stopping here means the packet that caused it is
		 * still on the screen. */
		if (g_verbose || g_level >= 1) {
			GLenum e = glGetError();
			if (e != GL_NO_ERROR) {
				static unsigned char seen[TADGL_OP_COUNT];
				unsigned int op = p.op < TADGL_OP_COUNT ? p.op : 0;
				if (!seen[op] || g_level >= 2) {
					seen[op] = 1;
					fprintf(stderr, "hle: GL error 0x%04X from %s (op %u, len %u)"
					        " at frame %lu\n",
					        e, tadgl_opname(p.op), p.op, p.len, g_frames);
				}
				if (g_level >= 2) {
					fprintf(stderr,
					  "hle: ============================================\n"
					  "hle: FATAL (TADPOLE_GL_DEBUG=2): the host rejected a\n"
					  "hle: replayed command. The opcode named above is the\n"
					  "hle: one to fix — either the guest encoded a GLES enum\n"
					  "hle: desktop GL does not accept, or this replayer built\n"
					  "hle: a call desktop GL will not take. Rerun without\n"
					  "hle: TADPOLE_GL_DEBUG=2 to carry on regardless.\n"
					  "hle: ============================================\n");
					fflush(stderr);
					abort();
				}
			}
		}

		/* Consume any tail padding the encoder added for alignment. */
		if (padded > p.len) g_ring->tail += padded - p.len;
		if (g_hist_n) g_hist[(g_hist_n - 1) % HIST].tail_after = g_ring->tail;
		/* Release the space only after the data has actually been consumed,
		 * otherwise the guest may overwrite bytes we are still reading. */
		__sync_synchronize();
	}
	ctx_leave();
	return presented;
}

void hle_host_stats(unsigned long *frames, unsigned long *packets)
{
	if (frames) *frames = g_frames;
	if (packets) *packets = g_packets;
}

unsigned int hle_host_desyncs(void) { return g_desync; }

/* Has the guest given up on us? The front end shows this, because a silent
 * fallback is indistinguishable from "HLE never worked". */
int hle_guest_fell_back(void) { return g_ring ? (int)g_ring->guest_fellback : 0; }
