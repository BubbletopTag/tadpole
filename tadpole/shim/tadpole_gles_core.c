/* Tadpole — GLES 1.x software rasteriser for the LeapPad2 guest.
 *
 * WHY A RASTERISER AND NOT A COMMAND STREAM
 * -----------------------------------------
 * The obvious design is to encode each GL call, ship it to the native viewer,
 * and replay it against host GL. That needs a wire protocol, a host GL context,
 * texture upload, readback, and a blit of the result back into the shared
 * framebuffer — a second copy of a path that already works.
 *
 * This runs INSIDE the guest and draws STRAIGHT into the framebuffer the shim
 * already owns: the same memory tadpole-view already maps and displays. No
 * protocol, no readback. The panel is 480x272, small enough that a scanline
 * rasteriser under qemu is entirely reasonable.
 *
 * WHAT HAS TO WORK FIRST: THE UI, NOT THE GAMES
 * ---------------------------------------------
 * libLightning2D/libLightningUI/libDisplayMPI composite the home screen through
 * GL, so a GL stack that draws nothing blanks the parts of Tadpole that already
 * work. That rules out a stub-now-fill-in-later shim. The UI needs 40 gl*
 * calls; the games need 89, and every one of the UI's 40 is in that set — one
 * implementation serves both.
 *
 * Two things make this tractable:
 *   - The `x`-suffixed entry points take GLfixed (16.16), NOT float, so the
 *     guest's soft-float ABI is irrelevant for most of the API.
 *   - GLES 1.x is fixed-function: matrix stack, vertex arrays, one texture
 *     unit, alpha blend. No shader compiler.
 *
 * STATE: steps 1-2 of the plan in HANDOVER.
 *   [x] glClear / glClearColorx
 *   [x] matrix stack: MatrixMode, LoadIdentity, Push/Pop, Ortho/Translate/
 *       Rotate/Scale (fixed AND float variants)
 *   [x] vertex + colour arrays, glDrawArrays / glDrawElements, triangles
 *   [ ] textures (glTexImage2D, glTexCoordPointer)   <- step 3
 *   [ ] alpha blending                               <- step 4
 * Unimplemented entry points are stubs in tadpole_gles_stubs.c so the guest
 * never hits an unresolved symbol. They are no longer SILENT: every one reports
 * itself once, and TADPOLE_GL_DEBUG=2 makes the first one fatal. See
 * tadpole_gles_debug.c.
 */
#include "tadpole_gles_debug.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            GLfixed;
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef unsigned int   GLbitfield;

#define NULL ((void *)0)

extern int   open(const char *path, int flags, ...);
extern void *mmap(void *addr, u32 len, int prot, int flags, int fd, long off);
extern int   ioctl(int fd, u32 req, ...);
extern void *malloc(u32 n);
extern void  free(void *p);
extern int   close(int fd);
extern void *memcpy(void *d, const void *s, u32 n);
extern long  write(int fd, const void *buf, u32 n);
extern char *getenv(const char *name);
extern int   snprintf(char *s, u32 n, const char *fmt, ...);

/* Timebase for frame pacing. struct timespec is two longs on this target. */
struct tad_ts { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_ts *tp);
extern int nanosleep(const struct tad_ts *req, struct tad_ts *rem);
#define CLOCK_MONOTONIC_ 1

/* Tracing, warning, error state and the fail-fast policy all live in
 * tadpole_gles_debug.c now, so the stubs file and the core cannot drift into
 * two different ideas of what TADPOLE_GL_DEBUG means. These three keep their
 * old names and signatures because ~200 call sites use them.
 *
 *   tr2      level >= 1. Trace the call sequence. The only way to know what the
 *            UI actually asks for is to watch it ask.
 *   warn2    ALWAYS, and to gl-warnings.log as well. Reserved for conditions
 *            that mean the output is definitely wrong — a texture with no data,
 *            a format we cannot decode, a name table that ran out. Each is
 *            one-shot or near enough, so a healthy run pays nothing and a
 *            user's ordinary log already contains the evidence instead of
 *            needing TADPOLE_GL_DEBUG=1 and a second attempt at reproducing.
 */
static void tr2(const char *msg, int a, int b) { tad_gl_trace(msg, a, b); }
static void warn2(const char *msg, int a, int b) { tad_gl_warn(msg, a, b); }

#define O_RDWR      02
#define PROT_RW     3          /* PROT_READ|PROT_WRITE */
#define MAP_SHARED  1

#define GL_TRIANGLES        0x0004
#define GL_TRIANGLE_STRIP   0x0005
#define GL_TRIANGLE_FAN     0x0006
#define GL_BYTE             0x1400
#define GL_UNSIGNED_BYTE    0x1401
#define GL_SHORT            0x1402
#define GL_UNSIGNED_SHORT   0x1403
#define GL_FLOAT            0x1406
#define GL_FIXED            0x140C
#define GL_MATRIX_PALETTE_OES     0x8840
#define GL_MAX_PALETTE_MATRICES_OES 0x8842
#define GL_MAX_VERTEX_UNITS_OES     0x86A4
#define GL_MATRIX_INDEX_ARRAY_OES 0x8844
#define GL_WEIGHT_ARRAY_OES       0x86AD
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_TEXTURE_         0x1702
#define GL_VERTEX_ARRAY     0x8074
#define GL_COLOR_ARRAY      0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_NORMAL_ARRAY_       0x8075
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_TEXTURE_2D           0x0DE1
#define GL_BLEND                0x0BE2
#define GL_ALPHA                0x1906
#define GL_RGB                  0x1907
#define GL_RGBA                 0x1908
#define GL_LUMINANCE            0x1909
#define GL_LUMINANCE_ALPHA      0x190A
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5   0x8363

/* Alpha test. The game enables GL_ALPHA_TEST and sets GL_GREATER, so texels
 * below the reference must be DISCARDED, not blended. Ignoring it draws pixels
 * the game intends to be invisible. */
#define GL_ALPHA_TEST 0x0BC0
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
static int g_alpha_test_on;
static GLenum g_alpha_func = GL_ALWAYS;
static u32 g_alpha_ref;              /* 0..255 */

/* GL_TEXTURE_ENV_MODE. Default is MODULATE; REPLACE ignores the vertex colour.
 * Anything we do not recognise is treated as MODULATE, which is the default
 * and the common case. */
#define GL_MODULATE 0x2100
#define GL_REPLACE  0x1E01
static GLenum g_tex_env = GL_MODULATE;

/* x*y/255 for 8-bit operands, exactly, without a divide. Integer division is
 * slow under qemu and this ran up to eight times per pixel. */
#define MUL255(x, y) ({ u32 t_ = (u32)(x) * (u32)(y) + 128u; \
                        (t_ + (t_ >> 8)) >> 8; })

static int alpha_passes(u32 a)
{
	if (!g_alpha_test_on) return 1;
	switch (g_alpha_func) {
	case GL_NEVER:    return 0;
	case GL_LESS:     return a <  g_alpha_ref;
	case GL_EQUAL:    return a == g_alpha_ref;
	case GL_LEQUAL:   return a <= g_alpha_ref;
	case GL_GREATER:  return a >  g_alpha_ref;
	case GL_NOTEQUAL: return a != g_alpha_ref;
	case GL_GEQUAL:   return a >= g_alpha_ref;
	default:          return 1;
	}
}

/* TADPOLE_GL_MAXDRAW=N ignores every draw after the Nth in a frame, so a frame
 * can be bisected to find which single draw introduces an artefact. Counter
 * resets on glClear, i.e. once per frame. */
static int g_draw_no, g_max_draw = -1;
/* Culling diagnostic: if roughly half of all submitted triangles paint nothing,
 * one triangle of every quad is being rejected — which is exactly what "2D
 * textures cut off diagonally" and circular buttons rendering as crescents
 * would look like. */
static int g_tri_submitted, g_tri_painted, g_tri_zero_area;
static int g_tri_nearclip, g_nearclip = -1;
static int g_tri_offscreen, g_tri_sliver;
static int g_clear_done, g_clear_nofb, g_clear_nomask;
static int g_uv_out, g_uv_wild;
/* TADPOLE_GL_DRAWLOG=1 emits one line per DRAW: frame, draw index, primitive
 * mode, vertex count, the bound texture and its size, the fragment state, and
 * the first triangle's three screen positions and three UVs. Enough to
 * reconstruct a frame by hand and spot the draw that does not belong. */
static int g_drawlog = -1, g_frame_no, g_draw_in_frame, g_log_this_draw;
static int g_tint = -1;

/* THE GAME'S SURFACE IS NOT THE WHOLE PANEL.
 *
 * A Leapster title renders into a window that the ViewFrame art surrounds. For
 * Clam Prix, EmeraldTitles/<pkg>/ViewFrame.json says:
 *     "png": "SB_racing.png", "x": 15, "y": 17, "w": 320, "h": 240
 * and Brio duly logs `CreateHandle: 320x240 (1920)` — a 320x240 image inside a
 * 480-pixel-wide framebuffer, drawn at (15,17).
 *
 * Rendering at the full 480x272 makes everything oversized, and the right-hand
 * part of the scene disappears under the ViewFrame's button panel.
 *
 * TADPOLE_GL_VIEW="x,y,w,h" overrides the rectangle; default is the full panel
 * so nothing changes for content that really is full-screen.
 */
#define FB_W 480
#define FB_H 272

/* The layer rectangle, defined next to the viewport logic further down but
 * declared here because the presenter and the HLE forwarding need it. */
static int g_vx, g_vy, g_vw, g_vh, g_view_init;
static void view_update(void);

/* ---- framebuffer ---------------------------------------------------------
 * We draw into the SAME /dev/fbN the display stack uses, opened the ordinary
 * way so tadpole_shim's open/ioctl/mmap emulation handles it. Real EGL opens
 * /dev/fb1 (visible in the shim log just before the VR5 assert), so we do too
 * and land on the layer the compositor expects.
 */
static u32 *g_fb;
static int  g_fbfd = -1;

static void fb_init(void)
{
	int fd;
	if (g_fb)
		return;
	fd = open("/dev/fb1", O_RDWR);
	tr2("fb_init open /dev/fb1 ->", fd, 0);
	if (fd < 0)
		return;
	g_fbfd = fd;
	/* Three screens: Brio double/triple-buffers inside one fb and flips with
	 * FBIOPAN_DISPLAY. We only touch page 0; presentation stays the guest's. */
	g_fb = mmap(NULL, FB_W * FB_H * 4 * 3, PROT_RW, MAP_SHARED, fd, 0);
	if (g_fb == (u32 *)-1)
		g_fb = NULL;
	tr2("fb_init mmap ok?", g_fb ? 1 : 0, 0);
}

/* WHERE TO DRAW: follow the pan, do not assume page 0.
 *
 * Brio double/triple-buffers inside ONE framebuffer and flips with
 * FBIOPAN_DISPLAY, so the visible page starts yoffset lines in — the same trap
 * that made the display look stale in HANDOVER 4.5. Writing to offset 0
 * unconditionally puts every triangle on a page nobody is scanning out, which
 * looks exactly like "the rasteriser does not work".
 *
 * fb_var_screeninfo begins xres, yres, xres_virtual, yres_virtual, xoffset,
 * yoffset — all u32 — so yoffset is word 5. FBIOGET_VSCREENINFO is 0x4600 and
 * the shim already emulates it.
 */
#define FBIOGET_VSCREENINFO 0x4600

/* PROPER DOUBLE BUFFERING.
 *
 * Symptom this fixes: animated menu elements smear across the screen, "spinning
 * and flying up leaving lines behind". That is a TRAIL, and a trail means the
 * surface being drawn is not the surface being cleared.
 *
 * Brio double/triple-buffers inside one framebuffer and flips with
 * FBIOPAN_DISPLAY. Drawing at whatever the CURRENT pan offset happens to be
 * means glClear and the draws that follow it can straddle a flip and land on
 * different pages — so each page keeps content from two frames ago and the
 * animation accumulates instead of replacing.
 *
 * Render into our own back buffer instead, and blit it to the visible page when
 * the app says the frame is done (eglSwapBuffers). One surface, cleared and
 * drawn consistently, presented atomically.
 */
static u32 g_back[FB_W * FB_H];

static u32 *fb_visible(void)
{
	u32 var[40];
	if (!g_fb)
		return NULL;
	if (g_fbfd >= 0 && ioctl(g_fbfd, FBIOGET_VSCREENINFO, var) == 0) {
		u32 yoff = var[5];
		if (yoff && yoff <= FB_H * 2)
			return g_fb + (u32)yoff * FB_W;
	}
	return g_fb;
}

static u32 *fb_target(void) { return g_back; }

/* TADPOLE_GL_DUMPTEX=1 writes every texture, as raw ARGB8888 with a tiny
 * header, to $TADPOLE_DIR/tex_<id>_<w>x<h>.raw at upload time. Lets us see what
 * the game actually put in a texture instead of inferring it from the frame. */
static void dump_tex(GLuint id, u32 w, u32 h, const u32 *argb)
{
	static int on = -1;
	char path[512];
	const char *d;
	int fd;
	if (on < 0) on = getenv("TADPOLE_GL_DUMPTEX") ? 1 : 0;
	if (!on || !argb) return;
	d = getenv("TADPOLE_DIR"); if (!d) d = "/tmp/tadpole";
	snprintf(path, sizeof(path), "%s/tex_%u_%ux%u.raw", d, id, w, h);
	fd = open(path, 01 | 0100 | 01000, 0666);       /* WRONLY|CREAT|TRUNC */
	if (fd < 0) return;
	write(fd, argb, w * h * 4);
	close(fd);
}

/* Called from eglSwapBuffers in the EGL shim. */
int g_swaps, g_clears;
/* Per-frame accounting. "Nothing renders" has three completely different
 * causes — no draws arrive, draws arrive but every triangle is rejected, or
 * triangles are painted in the wrong colour — and they need different fixes.
 * Counting all three per frame tells them apart in one run. */
/* GL_TEXTURE_2D enable is PER TEXTURE UNIT in GLES 1.x, and glActiveTexture
 * selects which one glEnable/glDisable applies to. Tracking a single global flag
 * means an enable aimed at unit 1 turns texturing on for unit 0 (and vice
 * versa), so a draw can come out flat-shaded — opaque white — when it should be
 * textured. Track the unit; the sampler still reads unit 0, which is the only
 * one a single-textured draw may use. */
#define MAX_TEXUNITS 4
static GLenum g_active_tex;              /* 0-based unit index */
static int    g_tex2d_unit[MAX_TEXUNITS];
static int    g_blend_on;

/* ---- DEPTH BUFFER --------------------------------------------------------
 *
 * There was none at all: glDepthFunc only traced, glDepthMask/glDepthRangef/
 * glClearDepth* were no-op stubs, and GL_DEPTH_TEST was not even tracked. Every
 * fragment was written unconditionally, so the visible result was purely
 * submission order.
 *
 * That is not only a 3D problem. A 2D screen can rely on the depth test to
 * reject geometry it submits LAST — the Clam Prix credits screen draws its text
 * first and then flat quads that should fail the test, and without a z-buffer
 * those quads simply painted over the finished screen. Same defect, two very
 * different-looking symptoms.
 *
 * Depth is stored as the post-divide NDC z mapped to 0..1, which interpolates
 * linearly in screen space, so plain barycentric interpolation is correct here.
 */
#define GL_DEPTH_TEST        0x0B71
#define GL_DEPTH_BUFFER_BIT  0x00000100
static float g_zbuf[FB_W * FB_H];
static int   g_depth_test_on;
static int   g_depth_mask = 1;          /* GL default: writes enabled */
static GLenum g_depth_func = GL_LESS;   /* GL default */
static float g_depth_clear = 1.0f;

static int depth_passes(float z, float ref)
{
	switch (g_depth_func) {
	case GL_NEVER:    return 0;
	case GL_LESS:     return z <  ref;
	case GL_EQUAL:    return z == ref;
	case GL_LEQUAL:   return z <= ref;
	case GL_GREATER:  return z >  ref;
	case GL_NOTEQUAL: return z != ref;
	case GL_GEQUAL:   return z >= ref;
	default:          return 1;         /* GL_ALWAYS */
	}
}
#define g_tex2d_on (g_tex2d_unit[0])

/* Texture-name capacity. glGenTextures names a texture after its slot index and
 * fails once every slot is taken, returning name 0 — and a title that draws
 * with name 0 renders untextured. Clam Prix alone loads ~11.8 MB of textures,
 * so 192 was not obviously enough for a single title, never mind that nothing
 * used to free them between titles (see tad_gl_context_reset).
 *
 * The table costs 16 bytes a slot; the pixel copies are allocated per texture
 * either way, so raising this buys headroom for almost nothing. tadpole_hle.c's
 * MAX_TEX indexes by guest name and MUST stay larger than this. If the
 * "glGenTextures EXHAUSTED" warning still appears, raise it again. */
#define MAX_TEXS 512          /* hoisted: the frame counters below index by it */
static int g_f_draws, g_f_tris_in, g_f_tris_out, g_f_pixels;
static u8  g_f_texused[MAX_TEXS + 1];
static int g_f_untex_tris;      /* triangles drawn with no texture at all */
static int g_f_flat_area, g_f_flat_w, g_f_flat_h, g_f_flat_x, g_f_flat_y;
static u32 g_f_flat_argb, g_f_flat_cur;
static int g_f_flat_state, g_f_flat_draw;
static u32 g_clear_argb = 0xFF000000u;   /* set by glClearColor */
static int g_clear_on_swap = -1;

/* ---- HOST-GPU REPLAY (HLE) ----------------------------------------------
 *
 * With TADPOLE_GL_HLE=1 the entry points below ALSO forward to the encoder in
 * tadpole_gles_hle.c, and the DRAW calls skip rasterising entirely. State,
 * matrix, texture and buffer calls still do their normal work: they are cheap,
 * the texture path is where pixel formats get converted to the one canonical
 * layout the wire carries, and keeping the software state live means the
 * software rasteriser remains a working reference to diff against.
 *
 * hle_on() is false unless a host replayer has stamped the ring AND is
 * heartbeating, so headless runs and older viewers keep the software path.
 */
extern int  hle_on(void);
extern void hle_present(void);
extern void hle_clear(u32 mask, u32 argb, float depth);
extern void hle_viewport(int x, int y, int w, int h);
extern void hle_enable(u32 cap);
extern void hle_disable(u32 cap);
extern void hle_blendfunc(u32 s, u32 d);
extern void hle_depthfunc(u32 f);
extern void hle_depthmask(u32 on);
extern void hle_cullface(u32 m);
extern void hle_frontface(u32 m);
extern void hle_shademodel(u32 m);
extern void hle_alphafunc(u32 f, float ref);
extern void hle_texenv(u32 target, u32 pname, int value);
extern void hle_texenvcolor(const float *rgba);
extern void hle_scissor(int x, int y, int w, int h);
extern void hle_colormask(u32 r, u32 g, u32 b, u32 a);
extern void hle_linewidth(float w);
extern void hle_pointsize(float s);
extern void hle_polygonoffset(float factor, float units);
extern void hle_light(u32 light, u32 pname, const float *v, u32 count);
extern void hle_material(u32 face, u32 pname, const float *v, u32 count);
extern void hle_lightmodel(u32 pname, const float *v, u32 count);
extern void hle_normal(float x, float y, float z);
extern void hle_color(float r, float g, float b, float a);
extern void hle_matrixmode(u32 m);
extern void hle_loadidentity(void);
extern void hle_pushmatrix(void);
extern void hle_popmatrix(void);
extern void hle_loadmatrix(const float *m);
extern void hle_multmatrix(const float *m);
extern void hle_ortho(float l, float r, float b, float t, float n, float f);
extern void hle_frustum(float l, float r, float b, float t, float n, float f);
extern void hle_translate(float x, float y, float z);
extern void hle_rotate(float a, float x, float y, float z);
extern void hle_scale(float x, float y, float z);
extern void hle_bindtexture(u32 name);
extern void hle_activetexture(u32 unit);
extern void hle_teximage2d(u32 name, u32 w, u32 h, const u32 *argb);
extern void hle_texsubimage2d(u32 name, u32 x, u32 y, u32 w, u32 h, const u32 *argb);
extern void hle_texparam(u32 pname, int v);
extern void hle_deletetexture(u32 name);
extern void hle_bufferdata(u32 name, u32 size, const void *data);
extern void hle_buffersubdata(u32 name, u32 off, u32 size, const void *data);
extern void hle_deletebuffer(u32 name);
extern void hle_arraypointer(u32 which, u32 buf, int size, u32 type, int stride, u32 off);
extern void hle_clientstate(u32 which, u32 on);
extern void hle_drawarrays(u32 mode, int first, int count);
extern void hle_drawelements(u32 mode, int count, u32 type, u32 elembuf, u32 off);

/* Defined after the buffer and texture tables it walks; see the resync note
 * there for why the encoder cannot rely on lazy attach alone. */
static int hle_ready(void);


/* ---- FRAME PACING -------------------------------------------------------
 *
 * On hardware, eglSwapBuffers blocks until the panel flips. Ours returned
 * immediately, so a title's render loop ran as fast as qemu could go. That is
 * not cosmetic: the game also GENERATES AUDIO per frame, so running at several
 * hundred Hz produces sound far faster than it can be played. The viewer trims
 * the backlog to keep latency down, and the result is audio that skips forward
 * — fast, garbled, phrases cut short. One missing wait, two symptoms.
 *
 * FBIO_WAITFORVSYNC is paced in tadpole_shim.c for the same reason, but a GL
 * title never gets there: with our EGL the frame loop is bounded HERE.
 *
 * A guest already slower than the period never sleeps, so this cannot make
 * heavy 3D slower. TADPOLE_HZ=0 disables it.
 */
static long g_pace_ns = -1;
static long g_pace_sec, g_pace_nsec;

static void pace_frame(void)
{
	struct tad_ts now, req;
	long dsec, dnsec;

	if (g_pace_ns < 0) {
		const char *e = getenv("TADPOLE_HZ");
		int v = 60;
		if (e) { v = 0; while (*e >= '0' && *e <= '9') v = v*10 + (*e++ - '0'); }
		g_pace_ns = (v > 0 && v <= 1000) ? (1000000000L / v) : 0;
		tr2("frame pacing Hz", v, 0);
	}
	if (!g_pace_ns) return;
	if (clock_gettime(CLOCK_MONOTONIC_, &now) != 0) return;

	if (!g_pace_sec) { g_pace_sec = now.tv_sec; g_pace_nsec = now.tv_nsec; }
	g_pace_nsec += g_pace_ns;
	while (g_pace_nsec >= 1000000000L) { g_pace_nsec -= 1000000000L; g_pace_sec++; }

	dsec  = g_pace_sec  - now.tv_sec;
	dnsec = g_pace_nsec - now.tv_nsec;
	if (dnsec < 0) { dnsec += 1000000000L; dsec--; }

	if (dsec < 0) {                 /* behind: resync rather than build debt */
		g_pace_sec = now.tv_sec; g_pace_nsec = now.tv_nsec;
		return;
	}
	if (dsec > 1) return;           /* clock jumped */
	req.tv_sec = dsec; req.tv_nsec = dnsec;
	nanosleep(&req, 0);
}

/* Measured, not assumed: report the rate the guest actually achieves. */
static void fps_report(void)
{
	static struct tad_ts t0;
	static int frames;
	struct tad_ts now;

	if (!tad_gl_level()) return;
	if (clock_gettime(CLOCK_MONOTONIC_, &now) != 0) return;
	if (!t0.tv_sec) { t0 = now; frames = 0; return; }
	if (++frames < 60) return;
	{
		long ms = (now.tv_sec - t0.tv_sec) * 1000
		        + (now.tv_nsec - t0.tv_nsec) / 1000000L;
		if (ms > 0) tr2("fps x100 over 60 frames", (int)(frames * 100000L / ms), 0);
	}
	t0 = now; frames = 0;
}

/* TADPOLE_GL_HLE=1 selects host-GPU replay. If it is asked for and does not
 * happen, say so once and keep rasterising rather than silently doing nothing —
 * a toggle that appears to work but changes nothing is worse than one that
 * admits it.
 *
 * THE ENCODER IS BUILT. This message used to say it was not, which was true
 * when it was written and has been wrong ever since tadpole_gles_hle.c landed —
 * and it is printed on EVERY headless run, so the log of every probe capture
 * asserted that a working feature did not exist. Diagnostics that lie cost more
 * than diagnostics that are missing.
 *
 * The real reason is always one of three, and hle_on() has already decided
 * which: no glcmd.bin, no host stamp on it, or no host heartbeat. It logs its
 * own reason via hle_log(), so say only what THIS side knows. */
static void hle_notice(void)
{
	static int said;
	if (said || !getenv("TADPOLE_GL_HLE") || hle_on()) return;
	said = 1;
	warn2("TADPOLE_GL_HLE was requested but no host replayer is attached"
	      " (is the viewer running?); rasterising in software", 0, 0);
}

void tadpole_gl_present(void)
{
	hle_notice();
	/* Keep the "entry points this title asked for and did not get" table
	 * current in gl-warnings.log. Every probe harness in tools/ kills the guest
	 * rather than letting it exit, so waiting for teardown means never printing
	 * it at all. Self-limiting — see tad_gl_report_tick(). */
	tad_gl_report_tick();
	if (hle_ready()) {
		/* The host owns the viewport, and it can only learn it from us. Sending
		 * it every frame is one packet and removes any chance of the two sides
		 * disagreeing about the layer rectangle. */
		view_update();
		hle_viewport(g_vx, g_vy, g_vw, g_vh);
		hle_present();
		/* STILL PACE THE GUEST. hle_present() waits for the host, so the guest
		 * inherits the VIEWER's loop rate — which on a 120 Hz display is 120 Hz,
		 * and a Brio title ties its logic to the frame it just drew, so the game
		 * runs at double speed. Observed directly in Fists of Fury. The panel is
		 * 60 Hz, so cap here regardless of what the host monitor does. */
		pace_frame();
		fps_report();
		g_swaps++;
		g_frame_no++;
		g_draw_in_frame = 0;
		return;                  /* the host blits; nothing to do locally */
	}
	u32 *vis;
	int i;
	fb_init();
	view_update();
	g_swaps++;
	g_frame_no++;
	g_draw_in_frame = 0;
	/* TADPOLE_GL_DUMPFRAME=1 writes OUR back buffer at every present. This is
	 * exactly what the rasteriser produced, with no compositing, no other
	 * layers and no pan-offset ambiguity — the only way to tell our output
	 * apart from what the viewer assembles from three framebuffers. */
	{
		static int on = -1, n = 0;
		if (on < 0) on = getenv("TADPOLE_GL_DUMPFRAME") ? 1 : 0;
		if (on) {
			char path[512];
			const char *d = getenv("TADPOLE_DIR");
			int fd;
			if (!d) d = "/tmp/tadpole";
			snprintf(path, sizeof(path), "%s/frame_%03d.raw", d, n++ % 8);
			fd = open(path, 01 | 0100 | 01000, 0666);
			if (fd >= 0) { write(fd, g_back, FB_W * FB_H * 4); close(fd); }
		}
	}
	vis = fb_visible();
	if (!vis)
		return;
	/* memcpy, not a scalar loop: this runs every frame and 130560 iterations
	 * of ARM code under qemu is not free. uClibc's memcpy is word-at-a-time. */
	memcpy(vis, g_back, FB_W * FB_H * 4);

	if (tad_gl_level()) {
		/* Which textures the frame actually SAMPLED. "A texture is bound and
		 * has data" says nothing if the draw never reads it. */
		char b[160];
		int i2, n = 0, listed = 0;
		n += snprintf(b + n, sizeof(b) - n, "[gl] FRAME textures used:");
		for (i2 = 1; i2 <= MAX_TEXS && listed < 12; i2++)
			if (g_f_texused[i2]) {
				n += snprintf(b + n, sizeof(b) - n, " %d", i2);
				listed++;
			}
		if (!listed) n += snprintf(b + n, sizeof(b) - n, " none");
		n += snprintf(b + n, sizeof(b) - n, "  untextured-tris %d\n",
		              g_f_untex_tris);
		{ u32 k = 0; while (b[k]) k++; write(2, b, k); }
	}
	if (tad_gl_level() && g_f_flat_area) {
		char b2[160];
		int n2 = snprintf(b2, sizeof(b2),
		    "[gl] FRAME biggest flat tri %dx%d at (%d,%d) argb %08x"
		    " curcolor %08x draw#%d tex2d=%d blend=%d depth=%d alpha=%d\n",
		    g_f_flat_w, g_f_flat_h, g_f_flat_x, g_f_flat_y, g_f_flat_argb,
		    g_f_flat_cur, g_f_flat_draw,
		    g_f_flat_state & 1, (g_f_flat_state >> 1) & 1,
		    (g_f_flat_state >> 2) & 1, (g_f_flat_state >> 3) & 1);
		if (n2 > 0) { u32 k = 0; while (b2[k]) k++; write(2, b2, k); }
	}
	g_f_flat_area = 0;
	for (i = 0; i <= MAX_TEXS; i++) g_f_texused[i] = 0;
	g_f_untex_tris = 0;
	if (tad_gl_level())
		tr2("FRAME draws/tris-in", g_f_draws, g_f_tris_in);
	if (tad_gl_level())
		tr2("FRAME tris-on-screen/pixels", g_f_tris_out, g_f_pixels);
	if (tad_gl_level() && !g_f_pixels)
		tr2("FRAME PAINTED NOTHING; clear argb", (int)g_clear_argb, g_f_tris_in);
	g_f_draws = g_f_tris_in = g_f_tris_out = g_f_pixels = 0;

	/* Hold the guest to the panel's cadence — see pace_frame(). */
	pace_frame();
	fps_report();

	/* TADPOLE_GL_CLEARSWAP=1: after a swap, GL says the back buffer contents
	 * are UNDEFINED — an app is expected to clear or fully redraw. If the
	 * streaks are accumulated fragments, discarding here removes them (and the
	 * frame will look sparse, telling us we are MISSING draws rather than
	 * drawing rubbish). If the game relies on the surface persisting, this
	 * will instead blank content that was previously correct. */
	if (g_clear_on_swap < 0)
		g_clear_on_swap = getenv("TADPOLE_GL_CLEARSWAP") ? 1 : 0;
	if (g_clear_on_swap)
		for (i = 0; i < FB_W * FB_H; i++)
			g_back[i] = 0xFF000000u;
}

static int parse_int(const char **p)
{
	int v = 0;
	while (**p == ' ') (*p)++;
	while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
	if (**p == ',') (*p)++;
	return v;
}

/* THE LAYER RECTANGLE, READ FROM THE SHIM'S SHARED STATE.
 *
 * Rendering at the full 480x272 makes a Leapster title oversized: its menu runs
 * off the right-hand side, under the ViewFrame's button panel. The game is
 * meant to occupy a window — 320x240 at (15,17) for Clam Prix — and the guest
 * announces that window through the fb driver. tadpole_shim.c intercepts those
 * two ioctls and publishes the result in state.bin; we read it back here.
 *
 * Deliberately NOT hardcoded and NOT parsed from ViewFrame.json: every title
 * has its own rect (the reading games use 250x250 at x=76), and the values
 * arriving at the driver are the ones the hardware would actually composite.
 *
 * Re-read once per frame, from tadpole_gl_present(). The rect is set while the
 * title loads, which can race the first draw call, and it changes again when
 * the app exits and the Flash UI takes fb1 back at full size.
 *
 * TADPOLE_GL_VIEW="x,y,w,h" overrides everything, for bisecting.
 */
struct tad_layer_state {
	u32 enabled, xres, yres, bpp, xoffset, yoffset;
	u32 nonstd, alpha, blank;
	u32 win_x, win_y, win_w, win_h;
};
struct tad_state {
	u32 magic, version, width, height, vsync_count;
	struct tad_layer_state layer[3];
};

static const struct tad_state *g_tstate;
static int g_view_forced = -1;      /* from TADPOLE_GL_VIEW */
static int g_fx, g_fy, g_fw, g_fh;

static void view_update(void)
{
	int x, y, w, h;

	if (!g_view_init) {
		const char *d, *e;
		char path[512];
		int fd;

		g_view_init = 1;
		g_vx = 0; g_vy = 0; g_vw = FB_W; g_vh = FB_H;

		e = getenv("TADPOLE_GL_VIEW");
		if (e) {
			g_fx = parse_int(&e); g_fy = parse_int(&e);
			g_fw = parse_int(&e); g_fh = parse_int(&e);
			g_view_forced = 1;
		} else {
			g_view_forced = 0;
			d = getenv("TADPOLE_DIR"); if (!d) d = "/tmp/tadpole";
			snprintf(path, sizeof(path), "%s/state.bin", d);
			fd = open(path, O_RDWR);
			if (fd >= 0) {
				void *m = mmap(NULL, sizeof(struct tad_state),
				               PROT_RW, MAP_SHARED, fd, 0);
				close(fd);
				if (m != (void *)-1)
					g_tstate = m;
			}
			tr2("view state mapped?", g_tstate ? 1 : 0, 0);
		}
	}

	if (g_view_forced) {
		x = g_fx; y = g_fy; w = g_fw; h = g_fh;
	} else if (g_tstate) {
		/* Layer 1 — GL renders into /dev/fb1, the 3D plane. */
		x = (int)g_tstate->layer[1].win_x;
		y = (int)g_tstate->layer[1].win_y;
		w = (int)g_tstate->layer[1].win_w;
		h = (int)g_tstate->layer[1].win_h;
	} else {
		return;
	}

	if (w > 0 && h > 0 && x >= 0 && y >= 0 &&
	    x + w <= FB_W && y + h <= FB_H &&
	    (x != g_vx || y != g_vy || w != g_vw || h != g_vh)) {
		g_vx = x; g_vy = y; g_vw = w; g_vh = h;
		tr2("viewport now", w, h);
		tr2("viewport at", x, y);
	}
}

static void view_init(void) { if (!g_view_init) view_update(); }

static float fx2f(GLfixed v) { return (float)v / 65536.0f; }
/* 16.16, the format every x-suffixed GLES 1.x entry point speaks. */
static GLfixed f2fx(float v) { return (GLfixed)(v * 65536.0f); }

/* ---- matrix stack -------------------------------------------------------- */

typedef struct { float m[16]; } mat4;

/* THESE DEPTHS ARE MEASURED FROM THE DEVICE, not chosen.
 *
 * All three used to be 16, and the modelview one was WRONG: real hardware
 * reports 32 (tools/glconform, limit.MAX_MODELVIEW_STACK_DEPTH, run against the
 * LeapPad2 itself). That is not a cosmetic mismatch in a query answer — it is a
 * smaller stack. A title that nests deeper than 16 had its 17th glPushMatrix
 * silently DROPPED here while succeeding on the device, and from that point on
 * our modelview no longer matched the one the title believed it had. Every
 * matrix downstream is then wrong, including the ones
 * glLoadPaletteFromModelViewMatrixOES snapshots for skinning.
 *
 * The texture stack really is 16 on the device, so it stays 16: being MORE
 * permissive than the hardware is its own bug, because a title that overflows
 * on the device and copes gets different behaviour here.
 *
 *   modelview   32      projection  32      texture  16
 */
#define MV_STACK_DEPTH   32
#define PROJ_STACK_DEPTH 32
#define TEXM_STACK_DEPTH 16
#define STACK_DEPTH MV_STACK_DEPTH      /* the largest, for anything shared */

static mat4 g_mv[MV_STACK_DEPTH], g_proj[PROJ_STACK_DEPTH];
/* A THIRD STACK, for GL_TEXTURE (0x1702). Without it that mode fell through to
 * the modelview stack — texture-matrix edits silently corrupted geometry
 * transforms, and the guest could never mirror what the host was doing. */
static mat4 g_texm[TEXM_STACK_DEPTH];
static int  g_texm_sp;
static int  g_mv_sp, g_proj_sp, g_mat_inited;
static GLenum g_matrix_mode = GL_MODELVIEW;

static void mat_identity(mat4 *r)
{
	int i;
	for (i = 0; i < 16; i++) r->m[i] = 0.0f;
	r->m[0] = r->m[5] = r->m[10] = r->m[15] = 1.0f;
}

static void mat_init_once(void)
{
	if (g_mat_inited) return;
	mat_identity(&g_mv[0]); mat_identity(&g_proj[0]);
	g_mat_inited = 1;
}

static mat4 *cur_top(void)
{
	mat_init_once();
	if (g_matrix_mode == GL_PROJECTION) return &g_proj[g_proj_sp];
	if (g_matrix_mode == GL_TEXTURE_)   return &g_texm[g_texm_sp];
	return &g_mv[g_mv_sp];
}

/* Column-major, like GL. r = a * b */
static void mat_mul(mat4 *r, const mat4 *a, const mat4 *b)
{
	mat4 t; int c, i;
	for (c = 0; c < 4; c++)
		for (i = 0; i < 4; i++)
			t.m[c*4+i] = a->m[0*4+i]*b->m[c*4+0] + a->m[1*4+i]*b->m[c*4+1]
			           + a->m[2*4+i]*b->m[c*4+2] + a->m[3*4+i]*b->m[c*4+3];
	*r = t;
}

static void mat_apply(const mat4 *m2) { mat4 *t = cur_top(); mat_mul(t, t, m2); }

static void vec_xform(const mat4 *m, const float *in, float *out)
{
	int i;
	for (i = 0; i < 4; i++)
		out[i] = m->m[0*4+i]*in[0] + m->m[1*4+i]*in[1]
		       + m->m[2*4+i]*in[2] + m->m[3*4+i]*in[3];
}

void glMatrixMode(GLenum mode) { tr2("glMatrixMode", (int)mode, 0);
  if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE_ &&
      mode != GL_MATRIX_PALETTE_OES) {
    /* Silently accepting an unknown mode meant every later matrix call landed
     * on the MODELVIEW stack — the fallback in cur_top() — so the projection a
     * title thought it was building quietly modified the view instead. */
    tad_gl_error(TAD_GL_INVALID_ENUM, "glMatrixMode"); return; }
  g_matrix_mode = mode;
  if (hle_ready()) hle_matrixmode(mode); }
void glLoadIdentity(void)      { mat_identity(cur_top());
  if (hle_ready()) hle_loadidentity(); }

static int g_push_drop, g_pop_under, g_max_mv, g_max_pj;

void glPushMatrix(void)
{
	mat_init_once();
	if (g_matrix_mode == GL_PROJECTION) {
		if (g_proj_sp + 1 < PROJ_STACK_DEPTH) { g_proj[g_proj_sp+1] = g_proj[g_proj_sp]; g_proj_sp++; }
		else { g_push_drop++; tad_gl_error(TAD_GL_STACK_OVERFLOW, "glPushMatrix"); }
	} else if (g_matrix_mode == GL_TEXTURE_) {
		if (g_texm_sp + 1 < TEXM_STACK_DEPTH) { g_texm[g_texm_sp+1] = g_texm[g_texm_sp]; g_texm_sp++; }
		else { g_push_drop++; tad_gl_error(TAD_GL_STACK_OVERFLOW, "glPushMatrix"); }
	} else {
		if (g_mv_sp + 1 < MV_STACK_DEPTH) { g_mv[g_mv_sp+1] = g_mv[g_mv_sp]; g_mv_sp++; }
		else { g_push_drop++; tad_gl_error(TAD_GL_STACK_OVERFLOW, "glPushMatrix"); }
	}
	/* FORWARD IT. This was missing, and it was the "conveyor belt": Clam Prix
	 * wraps each textured draw in glMatrixMode(GL_TEXTURE); glPushMatrix();
	 * glTranslatef(u,0,0); ... glPopMatrix(). The translate was forwarded and
	 * the push and pop were not, so on the host nothing ever restored the
	 * matrix and the U translation grew without bound — measured running from
	 * -377 to -579 texture-widths in eight reports, accelerating. Every
	 * textured surface slid, fast, forever. */
	if (hle_ready()) hle_pushmatrix();
}
void glPopMatrix(void)
{
	if (g_matrix_mode == GL_PROJECTION) {
		if (g_proj_sp > 0) g_proj_sp--; else { g_pop_under++; tad_gl_error(TAD_GL_STACK_UNDERFLOW, "glPopMatrix"); }
	} else if (g_matrix_mode == GL_TEXTURE_) {
		if (g_texm_sp > 0) g_texm_sp--; else { g_pop_under++; tad_gl_error(TAD_GL_STACK_UNDERFLOW, "glPopMatrix"); }
	} else {
		if (g_mv_sp > 0) g_mv_sp--; else { g_pop_under++; tad_gl_error(TAD_GL_STACK_UNDERFLOW, "glPopMatrix"); }
	}
	if (g_mv_sp > g_max_mv) g_max_mv = g_mv_sp;
	if (g_proj_sp > g_max_pj) g_max_pj = g_proj_sp;
	if (hle_ready()) hle_popmatrix();
}

static void ortho_f(float l, float r, float b, float t, float n, float f)
{
	mat4 o;
	if (r == l || t == b || f == n) return;
	mat_identity(&o);
	o.m[0]  =  2.0f/(r-l);  o.m[5]  =  2.0f/(t-b);  o.m[10] = -2.0f/(f-n);
	o.m[12] = -(r+l)/(r-l); o.m[13] = -(t+b)/(t-b); o.m[14] = -(f+n)/(f-n);
	mat_apply(&o);
}
void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
{ ortho_f(fx2f(l), fx2f(r), fx2f(b), fx2f(t), fx2f(n), fx2f(f));
  if (hle_ready()) hle_ortho(fx2f(l), fx2f(r), fx2f(b), fx2f(t), fx2f(n), fx2f(f)); }
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{ ortho_f(l, r, b, t, n, f);
  if (hle_ready()) hle_ortho(l, r, b, t, n, f); }

/* PERSPECTIVE. Both entry points were no-op stubs, which is why a race rendered
 * as a thin horizontal band: with no projection the transform stays orthographic
 * and every depth collapses onto one plane, so the track and the scenery flatten
 * into a strip and objects string out along a line.
 *
 * The standard GL frustum matrix, column-major to match ortho_f above. The near
 * plane must be positive and the three ranges non-empty; a zero divisor here
 * would poison the whole matrix stack rather than fail visibly. */
static void frustum_f(float l, float r, float b, float t, float n, float f)
{
	mat4 p;
	if (r == l || t == b || f == n || n <= 0.0f || f <= 0.0f)
		return;
	mat_identity(&p);
	p.m[0]  =  (2.0f*n)/(r-l);
	p.m[5]  =  (2.0f*n)/(t-b);
	p.m[8]  =  (r+l)/(r-l);
	p.m[9]  =  (t+b)/(t-b);
	p.m[10] = -(f+n)/(f-n);
	p.m[11] = -1.0f;
	p.m[14] = -(2.0f*f*n)/(f-n);
	p.m[15] =  0.0f;
	mat_apply(&p);
}
void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
{ frustum_f(fx2f(l), fx2f(r), fx2f(b), fx2f(t), fx2f(n), fx2f(f));
  if (hle_ready()) hle_frustum(fx2f(l), fx2f(r), fx2f(b), fx2f(t), fx2f(n), fx2f(f)); }
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{ frustum_f(l, r, b, t, n, f);
  if (hle_ready()) hle_frustum(l, r, b, t, n, f); }
/* The OES spelling is the same function; titles link whichever their SDK
 * emitted, and a stub here would silently flatten the scene exactly as the
 * missing glFrustumx did. */
void glFrustumfOES(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{ frustum_f(l, r, b, t, n, f);
  if (hle_ready()) hle_frustum(l, r, b, t, n, f); }

static void translate_f(float x, float y, float z)
{ mat4 m; mat_identity(&m); m.m[12]=x; m.m[13]=y; m.m[14]=z; mat_apply(&m); }
void glTranslatex(GLfixed x, GLfixed y, GLfixed z) { translate_f(fx2f(x), fx2f(y), fx2f(z));
  if (hle_ready()) hle_translate(fx2f(x), fx2f(y), fx2f(z)); }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { translate_f(x, y, z);
  if (hle_ready()) hle_translate(x, y, z); }

static void scale_f(float x, float y, float z)
{ mat4 m; mat_identity(&m); m.m[0]=x; m.m[5]=y; m.m[10]=z; mat_apply(&m); }
void glScalex(GLfixed x, GLfixed y, GLfixed z) { scale_f(fx2f(x), fx2f(y), fx2f(z));
  if (hle_ready()) hle_scale(fx2f(x), fx2f(y), fx2f(z)); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { scale_f(x, y, z);
  if (hle_ready()) hle_scale(x, y, z); }

/* sin/cos without libm — this is a -nostdlib shared object. Bhaskara-style
 * approximation, |err| < 2e-3 over a full turn, which is far below one pixel
 * of error on a 480x272 panel. */
static float ts_abs(float v) { return v < 0.0f ? -v : v; }
static float ts_sin(float a)
{
	const float PI = 3.14159265f;
	float b, c, y;
	while (a >  PI) a -= 2.0f*PI;
	while (a < -PI) a += 2.0f*PI;
	b = 4.0f/PI; c = -4.0f/(PI*PI);
	y = b*a + c*a*ts_abs(a);
	return 0.775f*y + 0.225f*(y*ts_abs(y));
}
static float ts_cos(float a) { return ts_sin(a + 1.57079633f); }

static void rotate_f(float deg, float x, float y, float z)
{
	float a = deg * 0.0174532925f;
	float s = ts_sin(a), c = ts_cos(a), one = 1.0f - c;
	float len2 = x*x + y*y + z*z;
	mat4 m;
	if (len2 <= 0.0f) return;
	if (len2 != 1.0f) {                     /* normalise via Newton sqrt */
		float r = len2 > 1.0f ? len2 : 1.0f, i;
		r = 0.5f*(r + len2/r); r = 0.5f*(r + len2/r); r = 0.5f*(r + len2/r);
		i = 1.0f/r; x *= i; y *= i; z *= i;
	}
	mat_identity(&m);
	m.m[0]=x*x*one+c;   m.m[4]=x*y*one-z*s; m.m[8] =x*z*one+y*s;
	m.m[1]=y*x*one+z*s; m.m[5]=y*y*one+c;   m.m[9] =y*z*one-x*s;
	m.m[2]=x*z*one-y*s; m.m[6]=y*z*one+x*s; m.m[10]=z*z*one+c;
	mat_apply(&m);
}
void glRotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z)
{ rotate_f(fx2f(a), fx2f(x), fx2f(y), fx2f(z));
  if (hle_ready()) hle_rotate(fx2f(a), fx2f(x), fx2f(y), fx2f(z)); }
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { rotate_f(a, x, y, z);
  if (hle_ready()) hle_rotate(a, x, y, z); }

/* Direct matrix load/multiply. THESE ARE NOT OPTIONAL.
 *
 * Clam Prix sets its projection and modelview with glLoadMatrixx/glMultMatrixx
 * rather than the Ortho/Translate/Rotate/Scale helpers. Left as no-op stubs,
 * those draws inherit whatever transform the stack happened to hold, and the
 * geometry projects to nonsense — long thin triangles radiating across the
 * screen, mixed in with the draws that DID use glOrthox and looked correct.
 *
 * GL matrices are 16 values, column-major, same order we store internally.
 */
static void load_matrix_f(const float *m)
{
	mat4 *t = cur_top();
	int i;
	if (!m) return;
	for (i = 0; i < 16; i++) t->m[i] = m[i];
}
static void mult_matrix_f(const float *m)
{
	mat4 n;
	int i;
	if (!m) return;
	for (i = 0; i < 16; i++) n.m[i] = m[i];
	mat_apply(&n);
}

void glLoadMatrixf(const GLfloat *m) { load_matrix_f(m);
  if (hle_ready()) hle_loadmatrix(m); }
void glMultMatrixf(const GLfloat *m) { mult_matrix_f(m);
  if (hle_ready()) hle_multmatrix(m); }

/* ---- glTexEnv* / glGetTexEnv* -------------------------------------------
 *
 * MEASURED DEMAND, not guessed: one Clam Prix race calls glTexEnvfv 524 times
 * and glGetTexEnvxv 413 times, both of which were no-op stubs. The getter is the
 * worse half — a stub getter does not return a wrong answer, it never writes the
 * caller's buffer at all, so the title read 413 uninitialised values off its own
 * stack and carried on.
 *
 * ALL NINE ENTRY POINTS SHARE ONE STATE BLOCK. GLES 1.1 §3.7.12 defines the f /
 * i / x / v spellings as the same state reached through different argument
 * types, so they are converted at the edge and the state is stored once. Writing
 * nine switches is how the fixed-point and float paths drift apart, which this
 * project has already paid for once.
 *
 * The COMBINE parameters are STORED BUT NOT HONOURED. Our rasteriser and the
 * host replay both implement MODULATE/REPLACE/DECAL/BLEND/ADD only. Storing the
 * rest is not pretending — a title that sets a combiner and reads it back gets
 * its own value, which is what the spec promises, and it renders wrong for a
 * reason the log now names instead of silently.
 */
#define GL_TEXTURE_ENV_MODE_   0x2200
#define GL_TEXTURE_ENV_COLOR_  0x2201
#define GL_COMBINE_RGB_        0x8571
#define GL_COMBINE_ALPHA_      0x8572
#define GL_RGB_SCALE_          0x8573
#define GL_ALPHA_SCALE_        0x0D1C
#define GL_SRC0_RGB_           0x8580     /* SRC0..2_RGB are contiguous */
#define GL_SRC0_ALPHA_         0x8588
#define GL_OPERAND0_RGB_       0x8590
#define GL_OPERAND0_ALPHA_     0x8598
#define GL_COORD_REPLACE_OES_  0x8862

struct gl_texenv {
	GLenum mode;
	float  color[4];
	GLenum combine_rgb, combine_alpha;
	GLenum src_rgb[3], src_alpha[3];
	GLenum op_rgb[3], op_alpha[3];
	float  rgb_scale, alpha_scale;
	GLenum coord_replace;
};
static struct gl_texenv g_texenv[MAX_TEXUNITS];
static int g_texenv_init;

static void texenv_init_once(void)
{
	int u, i;
	if (g_texenv_init) return;
	g_texenv_init = 1;
	for (u = 0; u < MAX_TEXUNITS; u++) {
		struct gl_texenv *e = &g_texenv[u];
		e->mode = GL_MODULATE;
		e->color[0] = e->color[1] = e->color[2] = e->color[3] = 0.0f;
		e->combine_rgb = e->combine_alpha = GL_MODULATE;
		for (i = 0; i < 3; i++) {
			/* GLES1 defaults: SRC0=TEXTURE, SRC1=PREVIOUS, SRC2=CONSTANT. */
			e->src_rgb[i] = e->src_alpha[i] =
				(i == 0) ? 0x1702u : (i == 1) ? 0x8578u : 0x8576u;
			e->op_rgb[i]   = (i == 2) ? 0x0302u : 0x0300u;  /* SRC_ALPHA : SRC_COLOR */
			e->op_alpha[i] = 0x0302u;                        /* SRC_ALPHA */
		}
		e->rgb_scale = e->alpha_scale = 1.0f;
		e->coord_replace = 0;                                /* GL_FALSE */
	}
}

static struct gl_texenv *texenv_cur(void)
{
	texenv_init_once();
	return &g_texenv[g_active_tex < MAX_TEXUNITS ? g_active_tex : 0];
}

/* How many components this pname carries. 0 means "not a TexEnv parameter",
 * which is GL_INVALID_ENUM rather than something to store. */
static int texenv_count(GLenum pname)
{
	switch (pname) {
	case GL_TEXTURE_ENV_COLOR_: return 4;
	case GL_TEXTURE_ENV_MODE_:
	case GL_COMBINE_RGB_: case GL_COMBINE_ALPHA_:
	case GL_RGB_SCALE_:   case GL_ALPHA_SCALE_:
	case GL_COORD_REPLACE_OES_:
		return 1;
	default:
		if (pname >= GL_SRC0_RGB_     && pname <= GL_SRC0_RGB_ + 2)     return 1;
		if (pname >= GL_SRC0_ALPHA_   && pname <= GL_SRC0_ALPHA_ + 2)   return 1;
		if (pname >= GL_OPERAND0_RGB_ && pname <= GL_OPERAND0_RGB_ + 2) return 1;
		if (pname >= GL_OPERAND0_ALPHA_ && pname <= GL_OPERAND0_ALPHA_ + 2) return 1;
		return 0;
	}
}

/* The one place TexEnv state is written. `v` is always float; the integer and
 * fixed-point entry points convert before calling. */
static void texenv_set(GLenum tgt, GLenum pname, const float *v)
{
	struct gl_texenv *e = texenv_cur();
	int n = texenv_count(pname);

	/* GL_POINT_SPRITE_OES is the only other legal target, and it carries
	 * exactly one parameter. */
	if (tgt != 0x2300u /* GL_TEXTURE_ENV */ && tgt != 0x8861u) {
		tad_gl_error(TAD_GL_INVALID_ENUM, "glTexEnv"); return; }
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glTexEnv"); return; }

	if (pname == GL_TEXTURE_ENV_COLOR_) {
		int i; for (i = 0; i < 4; i++) e->color[i] = v[i];
		if (hle_ready()) hle_texenvcolor(e->color);
		return;
	}
	/* FORWARDED AS AN INTEGER, AND THAT IS LOSSLESS HERE. GLES 1.1 §3.7.12
	 * allows GL_RGB_SCALE and GL_ALPHA_SCALE to be 1.0, 2.0 or 4.0 and nothing
	 * else, so the cast cannot lose anything a legal value carries — which is
	 * what lets these ride the existing integer TEXENV opcode instead of
	 * needing a float one. The host converts back. */
	if (pname == GL_RGB_SCALE_ || pname == GL_ALPHA_SCALE_) {
		if (pname == GL_RGB_SCALE_) e->rgb_scale = v[0];
		else                        e->alpha_scale = v[0];
		if (hle_ready()) hle_texenv(tgt, pname, (int)v[0]);
		return;
	}

	{
		GLenum val = (GLenum)v[0];
		switch (pname) {
		case GL_TEXTURE_ENV_MODE_:
			e->mode = val;
			/* g_tex_env drives the software rasteriser's texel path, which
			 * only distinguishes REPLACE from everything else. Keep it in
			 * step, but only for modes it can act on — latching a mode it
			 * cannot honour would make it render as if it could. */
			if (val == GL_MODULATE || val == GL_REPLACE)
				g_tex_env = val;
			break;
		case GL_COMBINE_RGB_:       e->combine_rgb = val; break;
		case GL_COMBINE_ALPHA_:     e->combine_alpha = val; break;
		case GL_COORD_REPLACE_OES_: e->coord_replace = val; break;
		default:
			if (pname >= GL_SRC0_RGB_ && pname <= GL_SRC0_RGB_ + 2)
				e->src_rgb[pname - GL_SRC0_RGB_] = val;
			else if (pname >= GL_SRC0_ALPHA_ && pname <= GL_SRC0_ALPHA_ + 2)
				e->src_alpha[pname - GL_SRC0_ALPHA_] = val;
			else if (pname >= GL_OPERAND0_RGB_ && pname <= GL_OPERAND0_RGB_ + 2)
				e->op_rgb[pname - GL_OPERAND0_RGB_] = val;
			else
				e->op_alpha[pname - GL_OPERAND0_ALPHA_] = val;
			break;
		}
		if (hle_ready()) hle_texenv(tgt, pname, (int)val);
	}
}

/* Reads TexEnv state as float. Returns the component count, 0 if `pname` is not
 * TexEnv state — the caller turns that into GL_INVALID_ENUM. */
static int texenv_get(GLenum tgt, GLenum pname, float *out)
{
	struct gl_texenv *e = texenv_cur();
	int n = texenv_count(pname);

	if (tgt != 0x2300u && tgt != 0x8861u) return 0;
	if (!n) return 0;

	switch (pname) {
	case GL_TEXTURE_ENV_COLOR_:
		{ int i; for (i = 0; i < 4; i++) out[i] = e->color[i]; }
		return 4;
	case GL_TEXTURE_ENV_MODE_:   out[0] = (float)e->mode; return 1;
	case GL_COMBINE_RGB_:        out[0] = (float)e->combine_rgb; return 1;
	case GL_COMBINE_ALPHA_:      out[0] = (float)e->combine_alpha; return 1;
	case GL_RGB_SCALE_:          out[0] = e->rgb_scale; return 1;
	case GL_ALPHA_SCALE_:        out[0] = e->alpha_scale; return 1;
	case GL_COORD_REPLACE_OES_:  out[0] = (float)e->coord_replace; return 1;
	default:
		if (pname >= GL_SRC0_RGB_ && pname <= GL_SRC0_RGB_ + 2)
			out[0] = (float)e->src_rgb[pname - GL_SRC0_RGB_];
		else if (pname >= GL_SRC0_ALPHA_ && pname <= GL_SRC0_ALPHA_ + 2)
			out[0] = (float)e->src_alpha[pname - GL_SRC0_ALPHA_];
		else if (pname >= GL_OPERAND0_RGB_ && pname <= GL_OPERAND0_RGB_ + 2)
			out[0] = (float)e->op_rgb[pname - GL_OPERAND0_RGB_];
		else
			out[0] = (float)e->op_alpha[pname - GL_OPERAND0_ALPHA_];
		return 1;
	}
}

void glTexEnvf(GLenum tgt, GLenum pname, GLfloat v)
{ tr2("glTexEnvf pname", (int)pname, (int)v); texenv_set(tgt, pname, &v); }

void glTexEnvfv(GLenum tgt, GLenum pname, const GLfloat *v)
{ tr2("glTexEnvfv pname", (int)pname, 0);
  if (v) texenv_set(tgt, pname, v); }

void glTexEnvi(GLenum tgt, GLenum pname, GLint v)
{ float f = (float)v; tr2("glTexEnvi pname/val", (int)pname, (int)v);
  texenv_set(tgt, pname, &f); }

void glTexEnviv(GLenum tgt, GLenum pname, const GLint *v)
{
	float f[4]; int i, n;
	if (!v) return;
	tr2("glTexEnviv pname", (int)pname, 0);
	n = texenv_count(pname); if (!n) n = 1;
	for (i = 0; i < n; i++) f[i] = (float)v[i];
	texenv_set(tgt, pname, f);
}

/* THE ONLY SCALED ONE. GL_TEXTURE_ENV_COLOR is a colour, so its fixed-point
 * form really is 16.16; every other TexEnv parameter is an ENUM, and 16.16 of
 * an enum is meaningless — GLES 1.1 §3.7.12 passes those through unscaled.
 * Dividing GL_MODULATE by 65536 would set the mode to 0. */
void glTexEnvx(GLenum tgt, GLenum pname, GLfixed v)
{
	float f;
	tr2("glTexEnvx pname/val", (int)pname, (int)v);
	f = (pname == GL_TEXTURE_ENV_COLOR_ || pname == GL_RGB_SCALE_ ||
	     pname == GL_ALPHA_SCALE_) ? fx2f(v) : (float)v;
	texenv_set(tgt, pname, &f);
}

void glTexEnvxv(GLenum tgt, GLenum pname, const GLfixed *v)
{
	float f[4]; int i, n;
	int scaled;
	if (!v) return;
	tr2("glTexEnvxv pname", (int)pname, 0);
	scaled = (pname == GL_TEXTURE_ENV_COLOR_ || pname == GL_RGB_SCALE_ ||
	          pname == GL_ALPHA_SCALE_);
	n = texenv_count(pname); if (!n) n = 1;
	for (i = 0; i < n; i++) f[i] = scaled ? fx2f(v[i]) : (float)v[i];
	texenv_set(tgt, pname, f);
}

void glGetTexEnvfv(GLenum tgt, GLenum pname, GLfloat *v)
{
	float f[4]; int n, i;
	if (!v) return;
	tr2("glGetTexEnvfv pname", (int)pname, 0);
	n = texenv_get(tgt, pname, f);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetTexEnvfv"); return; }
	for (i = 0; i < n; i++) v[i] = f[i];
}

void glGetTexEnviv(GLenum tgt, GLenum pname, GLint *v)
{
	float f[4]; int n, i;
	if (!v) return;
	tr2("glGetTexEnviv pname", (int)pname, 0);
	n = texenv_get(tgt, pname, f);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetTexEnviv"); return; }
	/* A COLOUR ROUNDS, AN ENUM CASTS. glGetTexEnviv on GL_TEXTURE_ENV_COLOR
	 * returns the colour scaled to the full int range per GLES 1.1 §6.1.2;
	 * everything else is already an integer. */
	for (i = 0; i < n; i++)
		v[i] = (pname == GL_TEXTURE_ENV_COLOR_)
		           ? (GLint)(f[i] * 2147483647.0f) : (GLint)f[i];
}

void glGetTexEnvxv(GLenum tgt, GLenum pname, GLfixed *v)
{
	float f[4]; int n, i;
	if (!v) return;
	tr2("glGetTexEnvxv pname", (int)pname, 0);
	n = texenv_get(tgt, pname, f);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetTexEnvxv"); return; }
	for (i = 0; i < n; i++)
		v[i] = (pname == GL_TEXTURE_ENV_COLOR_ || pname == GL_RGB_SCALE_ ||
		        pname == GL_ALPHA_SCALE_) ? f2fx(f[i]) : (GLfixed)f[i];
}
void glAlphaFuncx(GLenum func, GLfixed ref)
{ g_alpha_func = func; g_alpha_ref = (u32)(fx2f(ref) * 255.0f);
  if (hle_ready()) hle_alphafunc(func, fx2f(ref)); }
void glAlphaFunc(GLenum func, GLfloat ref)
{ g_alpha_func = func; g_alpha_ref = (u32)(ref * 255.0f);
  if (hle_ready()) hle_alphafunc(func, ref); }

void glActiveTexture(GLenum t)
{
	GLenum u = t - 0x84C0;                    /* GL_TEXTURE0 */
	tr2("glActiveTexture unit", (int)u, 0);
	if (u < MAX_TEXUNITS) g_active_tex = u;
	if (hle_ready()) hle_activetexture(u);
}
void glClientActiveTexture(GLenum t) { tr2("glClientActiveTexture unit", (int)t - 0x84C0, 0); }

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{ tr2("glViewport xy", x, y); tr2("glViewport wh", (int)w, (int)h); }



void glLoadMatrixx(const GLfixed *m)
{
	tr2("glLoadMatrixx", 0, 0);
	float f[16]; int i;
	if (!m) return;
	for (i = 0; i < 16; i++) f[i] = fx2f(m[i]);
	load_matrix_f(f);
	if (hle_ready()) hle_loadmatrix(f);
}
void glMultMatrixx(const GLfixed *m)
{
	float f[16]; int i;
	tr2("glMultMatrixx", 0, 0);
	if (!m) return;
	for (i = 0; i < 16; i++) f[i] = fx2f(m[i]);
	mult_matrix_f(f);
	if (hle_ready()) hle_multmatrix(f);
}

/* ---- clear --------------------------------------------------------------- */

/* declared with the frame counters above */


static u32 pack(float r, float g, float b, float a)
{
	u32 R = (u32)(r*255.0f), G = (u32)(g*255.0f);
	u32 B = (u32)(b*255.0f), A = (u32)(a*255.0f);
	return (A<<24)|(R<<16)|(G<<8)|B;
}
void glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{ g_clear_argb = pack(fx2f(r), fx2f(g), fx2f(b), fx2f(a)); }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{ g_clear_argb = pack(r, g, b, a); }

void glClear(GLbitfield mask)
{
	int i;
	static int n;
	if ((++n % 100) == 1) {
		tr2("stack: push-dropped / pop-underflow", g_push_drop, g_pop_under);
		tr2("stack: max mv / max proj depth", g_max_mv, g_max_pj);
	}
	tr2("glClear mask", (int)mask, 0);
	g_draw_no = 0;                       /* new frame: restart draw numbering */
	g_clears++;
	tr2("cadence: swaps / clears", g_swaps, g_clears);
	tr2("tris submitted / painted", g_tri_submitted, g_tri_painted);
	tr2("tris zero-area", g_tri_zero_area, 0);
	tr2("tris near-clipped", g_tri_nearclip, 0);
	tr2("tris off-panel / slivers", g_tri_offscreen, g_tri_sliver);
	tr2("clears done / skipped-nofb", g_clear_done, g_clear_nofb);
	tr2("clears skipped-nomask", g_clear_nomask, 0);
	tr2("uv outside 0..1 / wild", g_uv_out, g_uv_wild);
	g_tri_offscreen = g_tri_sliver = 0;
	g_tri_nearclip = 0;
	g_tri_submitted = g_tri_painted = g_tri_zero_area = 0;
	if (hle_ready())
		hle_clear(mask, g_clear_argb, g_depth_clear);
	if (mask & GL_DEPTH_BUFFER_BIT) {
		int yy, xx;
		view_init();
		/* Only the window, not the whole panel — the colour clear is already
		 * scoped this way and 130560 float stores per frame is not free. */
		for (yy = g_vy; yy < g_vy + g_vh; yy++)
			for (xx = g_vx; xx < g_vx + g_vw; xx++)
				g_zbuf[yy*FB_W + xx] = g_depth_clear;
	}
	fb_init();
	/* Distinguish "glClear was called" from "the colour buffer was actually
	 * wiped". If g_fb is NULL the early return skips the wipe entirely, and
	 * content then accumulates across frames — which looks exactly like a
	 * rendering bug. */
	if (!g_fb) { g_clear_nofb++; return; }
	if (!(mask & GL_COLOR_BUFFER_BIT)) { g_clear_nomask++; return; }
	g_clear_done++;
	{
		u32 *dst = fb_target();
		int yy;
		if (!dst) return;
		view_init();
		for (yy = g_vy; yy < g_vy + g_vh; yy++)
			for (i = g_vx; i < g_vx + g_vw; i++)
				dst[yy*FB_W + i] = g_clear_argb;
	}
}

/* ---- vertex arrays ------------------------------------------------------- */

/* ---- buffer objects ------------------------------------------------------
 *
 * REQUIRED, not optional. The games do not pass client-side arrays at all:
 * they upload to a VBO and then call glVertexPointer with a byte OFFSET, which
 * is nearly always 0. Treating that offset as a pointer makes every draw look
 * like it has a NULL array and get skipped — which presents as "the rasteriser
 * draws nothing" while 31,768 glDrawElements calls sail past.
 *
 *     glBindBuffer GL_ARRAY_BUFFER 2 ; glBufferData ... 48000 bytes
 *     glVertexPointer(2, GL_FIXED, stride, (void*)0)
 *
 * So an array reference is (buffer name, offset) when a buffer is bound, and a
 * plain pointer when it is not. Both forms have to work: the UI may well use
 * client arrays even though the games do not.
 */
/* ---- texture objects -----------------------------------------------------
 *
 * Clam Prix uploads only uncompressed RGBA, in three pixel types (measured,
 * not assumed):
 *
 *     GL_UNSIGNED_BYTE          19 uploads   RGBA8888
 *     GL_UNSIGNED_SHORT_4_4_4_4 27 uploads   RGBA4444
 *     GL_UNSIGNED_SHORT_5_6_5    2 uploads   RGB565
 *
 * Everything is converted to ARGB8888 once at upload time so the inner sampling
 * loop has a single format to deal with. Sizes seen: 256x256 down to 38x28.
 */
/* Sampler state lives WITH THE TEXTURE OBJECT, not with the unit — that is what
 * makes glGetTexParameteriv answerable at all. Defaults are the spec's
 * (§3.7.4), and they are not arbitrary: a title that only ever sets WRAP_S must
 * still read GL_LINEAR back for MAG_FILTER, and a title that sets nothing at all
 * must read GL_REPEAT. Answering 0 to any of these tells the caller its texture
 * has no filter, which is not a value GL can produce. */
struct gl_texture {
	GLuint name; u32 w, h; u32 *argb;
	GLenum min_filter, mag_filter, wrap_s, wrap_t;
	GLenum gen_mipmap;
};
static struct gl_texture g_texs[MAX_TEXS];
static GLuint g_bound_tex;
static int    g_blend_on;

#define GL_NEAREST_               0x2600
#define GL_LINEAR_                0x2601
#define GL_NEAREST_MIPMAP_LINEAR_ 0x2702
#define GL_REPEAT_                0x2901
#define GL_TEXTURE_MAG_FILTER_    0x2800
#define GL_TEXTURE_MIN_FILTER_    0x2801
#define GL_TEXTURE_WRAP_S_        0x2802
#define GL_TEXTURE_WRAP_T_        0x2803
#define GL_GENERATE_MIPMAP_       0x8191

static void tex_defaults(struct gl_texture *t)
{
	t->min_filter = GL_NEAREST_MIPMAP_LINEAR_;
	t->mag_filter = GL_LINEAR_;
	t->wrap_s = t->wrap_t = GL_REPEAT_;
	t->gen_mipmap = 0;                       /* GL_FALSE */
}

/* THE DEFAULT TEXTURE, name 0. Binding 0 is legal and every glTexParameter
 * after it applies to this object, so without it those calls would either be
 * dropped or write through a null pointer. GL has always had it; we simply
 * never modelled it. */
static struct gl_texture g_tex_default;
static int g_tex_default_init;

static struct gl_texture *tex_find(GLuint n)
{
	if (!n || n > MAX_TEXS) return NULL;
	return g_texs[n - 1].name ? &g_texs[n - 1] : NULL;
}
/* NAME == SLOT INDEX + 1, always.
 *
 * glGenTextures allocates by scanning for a free slot and naming it after its
 * index. If tex_slot() were allowed to drop an arbitrary name into the first
 * free slot instead, the invariant breaks and glGenTextures can later hand out
 * a name that is already in use elsewhere in the table — two live textures with
 * the same name, and lookups resolving to whichever comes first. Keep the
 * mapping fixed so implicit creation (binding an unused name) and explicit
 * creation cannot collide. */
static struct gl_texture *tex_slot(GLuint n)
{
	struct gl_texture *t;
	if (!n || n > MAX_TEXS) return NULL;
	t = &g_texs[n - 1];
	if (!t->name) {
		t->name = n; t->argb = NULL; t->w = t->h = 0;
		tex_defaults(t);
	}
	return t;
}

/* Buffer-object capacity. Exhausting this is a SILENT catastrophe and was the
 * cause of the black 3D in Clam Prix races:
 *
 *   glGenBuffers returns name 0 -> glBindBuffer(ELEMENT_ARRAY, 0) clears
 *   g_bound_elem -> glBufferData finds no slot and stores nothing ->
 *   glDrawElements(..., NULL) sends elembuf=0 -> the host has no indices and
 *   skips the draw. 2439 of 2566 draws in one frame, and a black screen.
 *
 * 64 was the original; 256 was still far too small — Driving School's track
 * reported 549 failed allocations against it. The measured need is what set
 * this, not a guess. The old value was 64, which the race scene blew through:
 * host's mirrored-buffer list stopped dead at name 64 while the track was still
 * loading. Raising it also required moving HLE_CLIENT_* below, which were 100
 * ONLY because 64 could never reach them. */
#define MAX_BUFS 2048
struct gl_buffer { GLuint name; u8 *data; u32 size; };
static struct gl_buffer g_bufs[MAX_BUFS];
static GLuint g_bound_array, g_bound_elem;

static struct gl_buffer *buf_find(GLuint name)
{
	if (!name || name > MAX_BUFS) return NULL;
	return g_bufs[name - 1].name ? &g_bufs[name - 1] : NULL;
}

/* Same invariant as textures: name == slot index + 1. */
static struct gl_buffer *buf_slot(GLuint name)
{
	struct gl_buffer *b;
	if (!name || name > MAX_BUFS) return NULL;
	b = &g_bufs[name - 1];
	if (!b->name) { b->name = name; b->data = NULL; b->size = 0; }
	return b;
}

/* RESYNC ON ATTACH.
 *
 * The encoder attaches LAZILY, on the first GL call that asks. A title uploads
 * its textures and vertex buffers once while loading, so if attach happens after
 * that — which depends on timing and therefore varies run to run — the host has
 * no buffers, every draw is skipped for want of an element buffer, and the frame
 * shows the clear colour only. Measured directly: one run replayed 14 draws a
 * frame, the next skipped 863 of 863.
 *
 * We already hold everything the host needs, in the same tables the software
 * rasteriser uses, so replay it once at attach. This also covers a host that
 * restarts mid-session.
 */
static int g_hle_synced;

static void hle_sync_state(void)
{
	int i;
	g_hle_synced = 1;                 /* set FIRST: the sends below re-enter */
	for (i = 0; i < MAX_BUFS; i++)
		if (g_bufs[i].name && g_bufs[i].data)
			hle_bufferdata(g_bufs[i].name, g_bufs[i].size, g_bufs[i].data);
	for (i = 0; i < MAX_TEXS; i++)
		if (g_texs[i].name && g_texs[i].argb)
			hle_teximage2d(g_texs[i].name, g_texs[i].w, g_texs[i].h,
			               g_texs[i].argb);
	tr2("HLE resynced buffers/textures", MAX_BUFS, MAX_TEXS);
}

extern int hle_want_resync(void);
extern void hle_reset(void);



static int hle_ready(void)
{
	if (!hle_on()) return 0;
	if (!g_hle_synced || hle_want_resync())
		hle_sync_state();
	return 1;
}

struct array { const u8 *ptr; GLuint buf; GLint size; GLenum type; GLsizei stride; int on; };
/* g_nrm IS NEW, AND IT WAS THE MOST-NEEDED MISSING ENTRY POINT IN THE LIBRARY.
 * tools/gl-demand.py: 49 of the installed titles import glNormalPointer, more
 * than any other stub. The host replayer has handled TADGL_ARR_NORMAL and called
 * glNormalPointer since the array plumbing was written — the guest simply never
 * sent one, because its entry point was a no-op. Same shape as
 * libopengles_lite.so and hle_deletetexture: the mechanism was built and never
 * wired up. */
static struct array g_vtx, g_col, g_tex, g_nrm;
/* Declared here with the other client arrays rather than beside the skinning
 * code, because glEnableClientState switches them on well before that point. */
static struct array g_midx, g_wgt;      /* matrix indices, blend weights */
static u32 g_cur_color = 0xFFFFFFFFu;

void glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ tr2("glVertexPointer size/type", s, (int)t); tr2("glVertexPointer stride/off", (int)st, (int)(unsigned long)p); tr2("glVertexPointer boundbuf", (int)g_bound_array, 0); g_vtx.buf = g_bound_array; g_vtx.size=s; g_vtx.type=t; g_vtx.stride=st; g_vtx.ptr=p;
  if (hle_ready()) hle_arraypointer(0, g_bound_array, s, t, st, (u32)(unsigned long)p); }
/* NO size ARGUMENT — a normal is always three components, which is why this
 * cannot share glVertexPointer's shape and needs its own entry point. Recorded
 * as size 3 so the host's bind_array, which is written against `size`, converts
 * the right number of elements. */
void glNormalPointer(GLenum t, GLsizei st, const void *p)
{
	tr2("glNormalPointer type/stride", (int)t, (int)st);
	g_nrm.buf = g_bound_array; g_nrm.size = 3; g_nrm.type = t;
	g_nrm.stride = st; g_nrm.ptr = p;
	if (hle_ready())
		hle_arraypointer(3, g_bound_array, 3, t, st, (u32)(unsigned long)p);
}
void glColorPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ g_col.buf = g_bound_array; g_col.size=s; g_col.type=t; g_col.stride=st; g_col.ptr=p;
  if (hle_ready()) hle_arraypointer(1, g_bound_array, s, t, st, (u32)(unsigned long)p); }
void glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ tr2("glTexCoordPointer size/type", s, (int)t); tr2("glTexCoordPointer stride/off", (int)st, (int)(unsigned long)p); g_tex.buf = g_bound_array; g_tex.size=s; g_tex.type=t; g_tex.stride=st; g_tex.ptr=p;
  if (hle_ready()) hle_arraypointer(2, g_bound_array, s, t, st, (u32)(unsigned long)p); }

/* WHICH WIRE SLOT AN ARRAY ENUM MEANS, or -1 for one the host has no notion of.
 *
 * This used to be a ternary chain ending in `: 3u`, so EVERY unrecognised enum
 * — including GL_POINT_SIZE_ARRAY_OES — was forwarded as the NORMAL array and
 * silently rebound it. A default that names a real slot is worse than no
 * default at all. */
static int array_slot(GLenum a)
{
	switch (a) {
	case GL_VERTEX_ARRAY:        return 0;
	case GL_COLOR_ARRAY:         return 1;
	case GL_TEXTURE_COORD_ARRAY: return 2;
	case GL_NORMAL_ARRAY_:       return 3;
	default:                     return -1;
	}
}

static void client_state(GLenum a, int on)
{
	int slot;
	if (a == GL_VERTEX_ARRAY)             g_vtx.on = on;
	else if (a == GL_COLOR_ARRAY)         g_col.on = on;
	else if (a == GL_TEXTURE_COORD_ARRAY) g_tex.on = on;
	else if (a == GL_NORMAL_ARRAY_)       g_nrm.on = on;
	/* Consumed here, never forwarded: the host has no such client arrays. */
	else if (a == GL_MATRIX_INDEX_ARRAY_OES) { g_midx.on = on; return; }
	else if (a == GL_WEIGHT_ARRAY_OES)       { g_wgt.on  = on; return; }
	slot = array_slot(a);
	if (slot < 0) { tad_gl_error(TAD_GL_INVALID_ENUM, on ?
	                             "glEnableClientState" : "glDisableClientState");
	                return; }
	if (hle_ready()) hle_clientstate((u32)slot, (u32)on);
}

void glEnableClientState(GLenum a)
{ tr2("glEnableClientState", (int)a, 0); client_state(a, 1); }
void glDisableClientState(GLenum a)
{ tr2("glDisableClientState", (int)a, 0); client_state(a, 0); }

void glColor4ub(u8 r, u8 g, u8 b, u8 a)
{ g_cur_color = ((u32)a<<24)|((u32)r<<16)|((u32)g<<8)|b;
  tr2("glColor4ub argb", (int)g_cur_color, 0);
  if (hle_ready()) hle_color(r/255.0f, g/255.0f, b/255.0f, a/255.0f); }

/* Fixed-point colour, the counterpart of glColor4ub. */
void glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{ g_cur_color = pack(fx2f(r), fx2f(g), fx2f(b), fx2f(a));
  tr2("glColor4x argb", (int)g_cur_color, 0);
  if (hle_ready()) hle_color(fx2f(r), fx2f(g), fx2f(b), fx2f(a)); }

/* THE FLOAT VARIANTS AND THE FACE/SHADE STATE WERE ALL NO-OP STUBS, while their
 * encoder senders sat declared and never called — the same "built, never wired"
 * shape as libopengles_lite.so and hle_deletetexture.
 *
 * glColor4f is the one that shows: a title that sets its vertex colour in
 * floats had it silently dropped, so the host kept whatever colour was last
 * set and filled surfaces rendered black while line and point geometry, which
 * the engine colours differently, still appeared. Black road, visible markings.
 *
 * glCullFace/glFrontFace matter for the opposite reason: with neither
 * forwarded, the host culls by its own default and whole surfaces can vanish or
 * show their interior. */
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ g_cur_color = pack(r, g, b, a);
  tr2("glColor4f argb", (int)g_cur_color, 0);
  if (hle_ready()) hle_color(r, g, b, a); }

void glCullFace(GLenum mode)
{ tr2("glCullFace", (int)mode, 0);
  if (hle_ready()) hle_cullface((u32)mode); }

void glFrontFace(GLenum mode)
{ tr2("glFrontFace", (int)mode, 0);
  if (hle_ready()) hle_frontface((u32)mode); }

void glShadeModel(GLenum mode)
{ tr2("glShadeModel", (int)mode, 0);
  if (hle_ready()) hle_shademodel((u32)mode); }

static u32 elem_size(GLenum t, GLint n)
{
	switch (t) {
	case GL_BYTE: case GL_UNSIGNED_BYTE:   return (u32)n;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return (u32)n*2;
	default:                               return (u32)n*4;
	}
}

/* An array is either a client pointer or (buffer, offset). Resolve to a real
 * address, or NULL if the buffer has no data yet. */
static const u8 *array_base(const struct array *a)
{
	if (a->buf) {
		struct gl_buffer *b = buf_find(a->buf);
		if (!b || !b->data) return NULL;
		return b->data + (u32)(unsigned long)a->ptr;
	}
	return a->ptr;
}

static float fetch(const struct array *a, u32 i, GLint c)
{
	u32 stride = a->stride ? (u32)a->stride : elem_size(a->type, a->size);
	const u8 *ab = array_base(a);
	const u8 *base;
	if (!ab) return 0.0f;
	base = ab + i*stride;
	if (c >= a->size) return (c == 3) ? 1.0f : 0.0f;
	switch (a->type) {
	case GL_FLOAT:         return ((const float *)base)[c];
	case GL_FIXED:         return fx2f(((const GLfixed *)base)[c]);
	case GL_SHORT:         return (float)((const short *)base)[c];
	case GL_UNSIGNED_BYTE: return (float)base[c] / 255.0f;
	case GL_BYTE:          return (float)((const signed char *)base)[c];
	default:               return 0.0f;
	}
}

/* ---- rasteriser ----------------------------------------------------------
 * Barycentric edge-function fill, chosen over scanline because it handles any
 * winding without sorting and makes per-pixel colour interpolation three
 * multiply-adds.
 */
struct vert { float x, y, z, w, wclip; float u, v; u32 argb; };

static void to_screen(struct vert *v)
{
	v->wclip = v->w;                 /* keep pre-divide w for the near test */
	if (v->w != 0.0f) { v->x /= v->w; v->y /= v->w; v->z /= v->w; }
	/* NDC -> pixels. GL's Y is up; the framebuffer's is down. */
	view_init();
	v->x = (float)g_vx + (v->x*0.5f + 0.5f) * (float)g_vw;
	v->y = (float)g_vy + (1.0f - (v->y*0.5f + 0.5f)) * (float)g_vh;
}

static float edge(const struct vert *a, const struct vert *b, float px, float py)
{ return (px - a->x)*(b->y - a->y) - (py - a->y)*(b->x - a->x); }

static int g_tri_logged;
static int g_bail_logged;

static void raster_tri(struct vert a, struct vert b, struct vert c)
{
	float area, mnx, mxx, mny, mxy;
	int x0, x1, y0, y1, x, y;
	u32 *dst;
	struct gl_texture *tex = g_tex2d_on ? tex_find(g_bound_tex) : NULL;
	if (g_tri_logged < 3)
		tr2("tex: id / has-data", (int)g_bound_tex, tex ? (tex->argb ? (int)tex->w : -1) : -2);

	if (tex && tex->argb) {
		if (g_bound_tex <= MAX_TEXS) g_f_texused[g_bound_tex] = 1;
	} else {
		/* Flat-shaded triangle: the colour is whatever glColor last set. Log
		 * a few per frame with their screen extent, because "70 untextured
		 * triangles" could equally be small UI panels or one screen-filling
		 * quad, and only the geometry distinguishes them. */
		/* TADPOLE_GL_NOFLAT=1 drops every flat-shaded triangle. If the
		 * credits text appears once they are gone, the white is those quads
		 * painting over it and nothing is wrong with the text path. */
		{
			static int noflat = -1;
			if (noflat < 0) noflat = getenv("TADPOLE_GL_NOFLAT") ? 1 : 0;
			if (noflat) { g_f_untex_tris++; return; }
		}
		if (tad_gl_level() && g_f_untex_tris < 4) {
			char msg[192];
			int n = snprintf(msg, sizeof(msg),
			    "[gl] FLAT tri (%d,%d)(%d,%d)(%d,%d) argb %08x"
			    " tex2d=%d boundtex=%d colarr=%d unit=%d\n",
			    (int)a.x, (int)a.y, (int)b.x, (int)b.y, (int)c.x, (int)c.y,
			    a.argb, g_tex2d_on, (int)g_bound_tex, g_col.on,
			    (int)g_active_tex);
			if (n > 0) { u32 k = 0; while (msg[k]) k++; write(2, msg, k); }
		}
		g_f_untex_tris++;
	}

	/* WHITE SQUARES COME FROM HERE. Texturing is on but there is nothing to
	 * sample, so the fragment keeps the vertex colour — usually white. Report
	 * each offending name once; the id then says which upload went missing. */
	if (g_tex2d_on && (!tex || !tex->argb)) {
		static unsigned char seen[MAX_TEXS + 1];
		unsigned id = g_bound_tex;
		if (id <= MAX_TEXS && !seen[id]) {
			seen[id] = 1;
			warn2("UNTEXTURED DRAW: bound name / has-slot",
			      (int)id, tex ? 1 : 0);
		}
	}

	g_f_tris_in++;
	/* TADPOLE_GL_NORASTER=1 submits everything and paints nothing. The point is
	 * to separate the two costs that get conflated when people say "software
	 * rendering under qemu is slow":
	 *
	 *   - qemu translating the guest's ARM code (game logic, Brio, our own
	 *     vertex transform), and
	 *   - our per-pixel rasteriser inner loop.
	 *
	 * The frame rate with this set is the ceiling the rest of the guest imposes.
	 * If it is far above the normal rate, the inner loop is the bottleneck and
	 * moving rasterisation to the host is worth it; if it is barely higher, the
	 * guest itself is the wall and no rasteriser work will help. */
	{
		static int noraster = -1;
		if (noraster < 0) noraster = getenv("TADPOLE_GL_NORASTER") ? 1 : 0;
		if (noraster) return;
	}
	if (!g_fb) return;
	dst = fb_target();
	if (!dst) return;
	g_tri_logged++;
	if (tad_gl_level() && g_tri_logged < 12) {
		tr2("tri A", (int)a.x, (int)a.y);
		tr2("tri B", (int)b.x, (int)b.y);
		tr2("tri C", (int)c.x, (int)c.y);
		/* texcoords x1000, and whether a texture is bound */
		tr2("uvA x1000", (int)(a.u*1000.0f), (int)(a.v*1000.0f));
		tr2("uvB x1000", (int)(b.u*1000.0f), (int)(b.v*1000.0f));
		tr2("uvC x1000", (int)(c.u*1000.0f), (int)(c.v*1000.0f));
		tr2("tex bound / on", (int)g_bound_tex, g_tex2d_on);
	}
	/* Lazy init HERE, not in draw_indexed: an earlier attempt put it there and
	 * the patch silently failed to apply, leaving g_tint at its -1 sentinel.
	 * -1 is TRUTHY in C, so the debug tint was permanently on and every dumped
	 * frame came out uniformly magenta — an instrument reporting on itself. */
	if (g_tint < 0)
		g_tint = getenv("TADPOLE_GL_TINT") ? 1 : 0;

	if (g_log_this_draw) {
		g_log_this_draw--;            /* log the first TWO triangles of a draw */
		tr2("  vA", (int)a.x, (int)a.y);
		tr2("  vB", (int)b.x, (int)b.y);
		tr2("  vC", (int)c.x, (int)c.y);
		tr2("  uvA x1000", (int)(a.u*1000.0f), (int)(a.v*1000.0f));
		tr2("  uvC x1000", (int)(c.u*1000.0f), (int)(c.v*1000.0f));
	}

	g_tri_submitted++;

	/* NEAR-PLANE REJECTION (crude stand-in for real clipping).
	 *
	 * There is no frustum clipping anywhere in this rasteriser. A vertex with w
	 * at or near zero divides to an enormous screen coordinate, and the
	 * triangle becomes a long thin sliver stretching across the frame. Several
	 * such triangles sharing one good vertex produce streaks RADIATING FROM A
	 * POINT — exactly the artefact seen in Clam Prix.
	 *
	 * Properly this wants near-plane clipping that SPLITS the triangle and
	 * keeps the visible part. Rejecting outright loses geometry that should be
	 * partly visible, so treat this as a diagnostic/mitigation, not a fix.
	 * TADPOLE_GL_NOCLIP=1 disables it for comparison. */
	if (g_nearclip < 0)
		g_nearclip = getenv("TADPOLE_GL_NOCLIP") ? 0 : 1;
	if (g_nearclip) {
		const float EPS = 1.0f / 1024.0f;
		if (a.wclip < EPS || b.wclip < EPS || c.wclip < EPS) {
			g_tri_nearclip++;
			return;
		}
	}

	/* Classify every triangle by how far outside the panel it reaches, and how
	 * elongated it is. Streaks are long thin slivers; a histogram tells us
	 * whether they are a few pathological triangles or the normal geometry
	 * being rasterised wrongly. */
	{
		float mnx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
		float mxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
		float mny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
		float mxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
		float ww = mxx - mnx, hh = mxy - mny;
		if (mnx < -200.0f || mxx > 700.0f || mny < -200.0f || mxy > 500.0f)
			g_tri_offscreen++;
		if ((ww > 200.0f && hh < 12.0f) || (hh > 200.0f && ww < 12.0f))
			g_tri_sliver++;
	}

	/* UV range histogram over EVERY triangle, not a sample. Textures are known
	 * good and geometry is known sane, so wrong texcoords are the remaining way
	 * to smear a correct texture across the screen. */
	{
		float us[3], vs[3]; int k;
		us[0]=a.u; us[1]=b.u; us[2]=c.u; vs[0]=a.v; vs[1]=b.v; vs[2]=c.v;
		for (k = 0; k < 3; k++) {
			if (us[k] < -0.01f || us[k] > 1.01f || vs[k] < -0.01f || vs[k] > 1.01f) {
				g_uv_out++;
				if (us[k] < -8.0f || us[k] > 8.0f || vs[k] < -8.0f || vs[k] > 8.0f)
					g_uv_wild++;
				break;
			}
		}
	}

	area = edge(&a, &b, c.x, c.y);
	if (area == 0.0f) { g_tri_zero_area++; return; }
	if (area < 0.0f) { struct vert t = b; b = c; c = t; area = -area; }

	mnx = a.x<b.x ? (a.x<c.x?a.x:c.x) : (b.x<c.x?b.x:c.x);
	mxx = a.x>b.x ? (a.x>c.x?a.x:c.x) : (b.x>c.x?b.x:c.x);
	mny = a.y<b.y ? (a.y<c.y?a.y:c.y) : (b.y<c.y?b.y:c.y);
	mxy = a.y>b.y ? (a.y>c.y?a.y:c.y) : (b.y>c.y?b.y:c.y);

	x0 = (int)mnx; x1 = (int)mxx + 1; y0 = (int)mny; y1 = (int)mxy + 1;
	view_init();
	if (x0 < g_vx) x0 = g_vx;  if (x1 > g_vx + g_vw) x1 = g_vx + g_vw;
	if (y0 < g_vy) y0 = g_vy;  if (y1 > g_vy + g_vh) y1 = g_vy + g_vh;
	if (x1 > x0 && y1 > y0)
		g_f_tris_out++;          /* survived clipping: has on-screen area */

	/* Biggest FLAT triangle in the frame. Has to be here, AFTER the box is
	 * clamped — an earlier attempt read mnx/mxx before they were computed and
	 * silently logged nothing at all. The screen-coverers also come last in a
	 * frame, so a head-of-frame log limit never catches them. */
	if ((!tex || !tex->argb) && x1 > x0 && y1 > y0) {
		int aw = x1 - x0, ah = y1 - y0;
		if (aw * ah > g_f_flat_area) {
			g_f_flat_area = aw * ah;
			g_f_flat_argb = a.argb;
			g_f_flat_w = aw; g_f_flat_h = ah;
			g_f_flat_x = x0; g_f_flat_y = y0;
			/* Record the state AT THIS TRIANGLE. Logging it at draw entry
			 * instead reported a different draw's state: the covering quad
			 * turned out to be only two triangles, so a count-based filter
			 * missed it entirely and the two observations contradicted. */
			g_f_flat_state = (g_tex2d_on ? 1 : 0) | (g_blend_on ? 2 : 0)
			               | (g_depth_test_on ? 4 : 0)
			               | (g_alpha_test_on ? 8 : 0);
			g_f_flat_cur = g_cur_color;
			g_f_flat_draw = g_draw_in_frame;
		}
	}

	/* ---- INNER LOOP ------------------------------------------------------
	 *
	 * Measured 8.24 fps on the Clam Prix main menu before this was touched, so
	 * three things that cost float work on every single pixel are hoisted:
	 *
	 *  1. INCREMENTAL EDGE FUNCTIONS. The edge function is linear in the pixel
	 *     centre, so evaluating it three times per pixel (~12 float ops) can be
	 *     three ADDS instead: dE/dx and dE/dy are constant over the triangle.
	 *
	 *  2. RECIPROCAL AREA. `w/area` three times per pixel was three float
	 *     DIVIDES, the most expensive operation available under soft-float
	 *     qemu. One reciprocal per triangle turns them into multiplies.
	 *
	 *  3. UNIFORM VERTEX COLOUR. These titles never enable GL_COLOR_ARRAY
	 *     (measured: zero glEnableClientState(GL_COLOR_ARRAY) in a whole run),
	 *     so all three vertices carry the same glColor and interpolating it is
	 *     ~20 float ops per pixel spent computing a constant. Detect the case
	 *     and resolve the colour once per triangle.
	 *
	 * The barycentrics are still needed for texcoords and depth, so they are
	 * computed only when something actually consumes them.
	 */
	{
		int painted = 0;
		const float inv = (area != 0.0f) ? 1.0f / area : 0.0f;
		/* d/dx and d/dy of each edge function — see edge() above. */
		const float e0dx =  (c.y - b.y), e0dy = -(c.x - b.x);
		const float e1dx =  (a.y - c.y), e1dy = -(a.x - c.x);
		const float e2dx =  (b.y - a.y), e2dy = -(b.x - a.x);
		const float px0 = (float)x0 + 0.5f, py0 = (float)y0 + 0.5f;
		float row0 = edge(&b, &c, px0, py0);
		float row1 = edge(&c, &a, px0, py0);
		float row2 = edge(&a, &b, px0, py0);

		const int uniform = (a.argb == b.argb && b.argb == c.argb);
		const u32 uA = (a.argb >> 24) & 0xFF, uR = (a.argb >> 16) & 0xFF;
		const u32 uG = (a.argb >> 8) & 0xFF,  uB = a.argb & 0xFF;

		/* Hoisted out of the loop: chasing tex-> on every pixel is a
		 * dependent load each time. */
		const u32 *tpx = (tex && tex->argb) ? tex->argb : NULL;
		const int  tw = tpx ? (int)tex->w : 0;
		const int  th = tpx ? (int)tex->h : 0;
		const int  need_bary = (tpx != NULL) || !uniform
		                     || g_depth_test_on || g_depth_mask;
		/* By far the commonest fragment in these titles: a texture modulated
		 * by opaque white, which is just the texel. Taking that shortcut skips
		 * four multiplies and four divides per pixel. */
		const int  texel_direct = tpx && uniform && g_tex_env != GL_REPLACE
		                        && uR == 255 && uG == 255
		                        && uB == 255 && uA == 255;
		const int  atest = g_alpha_test_on;   /* hoisted out of the loop */

		for (y = y0; y < y1; y++) {
			float w0 = row0, w1 = row1, w2 = row2;
			u32 *drow = dst + (u32)y * FB_W;
			float *zrow = g_zbuf + (u32)y * FB_W;

			for (x = x0; x < x1; x++,
			     w0 += e0dx, w1 += e1dx, w2 += e2dx) {
				float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f;
				u32 R, G, B, A;

				if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
					continue;
				if (need_bary) {
					l0 = w0 * inv; l1 = w1 * inv; l2 = w2 * inv;
				}

				if (uniform) {
					R = uR; G = uG; B = uB; A = uA;
				} else {
					R = (u32)(l0*(float)((a.argb>>16)&0xFF)
					        + l1*(float)((b.argb>>16)&0xFF)
					        + l2*(float)((c.argb>>16)&0xFF));
					G = (u32)(l0*(float)((a.argb>>8)&0xFF)
					        + l1*(float)((b.argb>>8)&0xFF)
					        + l2*(float)((c.argb>>8)&0xFF));
					B = (u32)(l0*(float)(a.argb&0xFF)
					        + l1*(float)(b.argb&0xFF)
					        + l2*(float)(c.argb&0xFF));
					/* Alpha is interpolated like the rest. It used to be
					 * hardcoded to 255, which threw the vertex alpha away.
					 * A textured draw hid the mistake because the modulate
					 * below overwrote it with the texel's alpha; an
					 * untextured one did not, so blending never ran and the
					 * alpha test never discarded — every flat triangle came
					 * out fully opaque. */
					A = (u32)(l0*(float)((a.argb>>24)&0xFF)
					        + l1*(float)((b.argb>>24)&0xFF)
					        + l2*(float)((c.argb>>24)&0xFF));
					if (A > 255) A = 255;
				}

				/* MODULATE: texel * vertex colour, the GLES1 default. */
				if (tpx) {
					float uu = l0*a.u + l1*b.u + l2*c.u;
					float vv = l0*a.v + l1*b.v + l2*c.v;
					int tx = (int)(uu * (float)tw);
					int ty = (int)(vv * (float)th);
					u32 texel;
					tx %= tw; if (tx < 0) tx += tw;
					ty %= th; if (ty < 0) ty += th;
					texel = tpx[(u32)ty * (u32)tw + (u32)tx];
					if (texel_direct || g_tex_env == GL_REPLACE) {
						A = (texel>>24)&0xFF; R = (texel>>16)&0xFF;
						G = (texel>>8)&0xFF;  B = texel&0xFF;
					} else {
						A = MUL255((texel>>24)&0xFF, A);
						R = MUL255((texel>>16)&0xFF, R);
						G = MUL255((texel>>8)&0xFF,  G);
						B = MUL255(texel&0xFF,       B);
					}
				}

				if (A == 0 || (atest && !alpha_passes(A)))
					continue;                /* discarded */
				if (g_depth_test_on || g_depth_mask) {
					float z = l0*a.z + l1*b.z + l2*c.z;
					float zn = z * 0.5f + 0.5f;      /* NDC -> 0..1 */
					if (zn < 0.0f) zn = 0.0f;
					if (zn > 1.0f) zn = 1.0f;
					if (g_depth_test_on && !depth_passes(zn, zrow[x]))
						continue;
					if (g_depth_mask)
						zrow[x] = zn;
				}
				if (g_blend_on && A < 255) {
					u32 d = drow[x];
					u32 ia = 255 - A;
					R = MUL255(R, A) + MUL255((d>>16)&0xFF, ia);
					G = MUL255(G, A) + MUL255((d>>8)&0xFF,  ia);
					B = MUL255(B, A) + MUL255(d&0xFF,       ia);
				}
				/* TADPOLE_GL_TINT=1: paint a fixed unmistakable colour
				 * instead of the shaded texel. Every pixel that changes on
				 * screen is OURS; everything else belongs to another
				 * framebuffer layer and no rasteriser work will fix it. */
				g_f_pixels++;
				drow[x] = g_tint ? 0xFFFF00FFu
				                 : (0xFF000000u | (R<<16) | (G<<8) | B);
				painted++;
			}
			row0 += e0dy; row1 += e1dy; row2 += e2dy;
		}
		if (painted) g_tri_painted++;
	}
}

static void build_vert(struct vert *v, u32 idx)
{
	float in[4], out[4];
	mat4 mvp;

	mat_init_once();
	in[0] = fetch(&g_vtx, idx, 0);
	in[1] = fetch(&g_vtx, idx, 1);
	in[2] = (g_vtx.size >= 3) ? fetch(&g_vtx, idx, 2) : 0.0f;
	in[3] = 1.0f;

	mat_mul(&mvp, &g_proj[g_proj_sp], &g_mv[g_mv_sp]);
	vec_xform(&mvp, in, out);
	v->x = out[0]; v->y = out[1]; v->z = out[2]; v->w = out[3];

	if (g_tex.on && array_base(&g_tex)) {
		v->u = fetch(&g_tex, idx, 0);
		v->v = fetch(&g_tex, idx, 1);
	} else { v->u = 0.0f; v->v = 0.0f; }

	if (g_col.on && g_col.ptr) {
		/* ALPHA FROM THE COLOUR ARRAY, not forced opaque.
		 *
		 * Previously this packed alpha as 1.0, discarding per-vertex alpha
		 * entirely. Games fade elements in and out by modulating vertex alpha,
		 * so ignoring it draws every element at full opacity — everything
		 * that should be mid-fade lands on screen at once, piled on top of
		 * whatever is behind it.
		 *
		 * fetch() already returns 1.0 for a missing component, so a 3-component
		 * colour array still comes out opaque. */
		v->argb = pack(fetch(&g_col, idx, 0), fetch(&g_col, idx, 1),
		               fetch(&g_col, idx, 2), fetch(&g_col, idx, 3));
	} else {
		v->argb = g_cur_color;
	}
	to_screen(v);
}

static void draw_indexed(GLenum mode, GLsizei count, const void *indices, GLenum itype)
{
	struct vert v[3];
	GLsizei i;
	u32 a, b, c;

	fb_init();

	/* TADPOLE_GL_MAXDRAW=N: ignore every draw after the Nth since the last
	 * glClear, so a frame can be bisected down to the draw that introduces an
	 * artefact. */
	if (g_max_draw < 0) {
		const char *e = getenv("TADPOLE_GL_MAXDRAW");
		if (e) {
			int v = 0;
			while (*e >= '0' && *e <= '9') v = v * 10 + (*e++ - '0');
			g_max_draw = v;
		} else {
			g_max_draw = 1 << 30;
		}
	}
	if (++g_draw_no > g_max_draw)
		return;

	if (g_drawlog < 0)
		g_drawlog = getenv("TADPOLE_GL_DRAWLOG") ? 1 : 0;
	g_draw_in_frame++;
	g_f_draws++;
	/* Full pipeline state for BIG draws. The Clam Prix credits screen is
	 * covered by one 64-triangle untextured mesh submitted last; knowing
	 * exactly which tests were enabled for it is the only way to tell which
	 * of them should have rejected it. */
	if (tad_gl_level() && count >= 100) {
		char b3[224];
		int n3 = snprintf(b3, sizeof(b3),
		    "[gl] BIGDRAW count=%d tex2d=%d boundtex=%d blend=%d depth=%d"
		    " depthfunc=%d depthmask=%d alphatest=%d alphafunc=%d ref=%d"
		    " color=%08x\n",
		    (int)count, g_tex2d_on, (int)g_bound_tex, g_blend_on,
		    g_depth_test_on, (int)g_depth_func, g_depth_mask,
		    g_alpha_test_on, (int)g_alpha_func, (int)g_alpha_ref,
		    g_cur_color);
		if (n3 > 0) { u32 k = 0; while (b3[k]) k++; write(2, b3, k); }
	}
	g_log_this_draw = g_drawlog ? 2 : 0;
	if (g_drawlog) {
		struct gl_texture *bt = tex_find(g_bound_tex);
		/* Dump the actual index values — a degenerate second triangle points
		 * at indices being read wrongly, not at the geometry being wrong. */
		if (indices && count >= 6) {
			int k;
			for (k = 0; k < 6; k++) {
				u32 iv = (itype == GL_UNSIGNED_BYTE)
				         ? (u32)((const u8 *)indices)[k]
				         : (u32)((const u16 *)indices)[k];
				tr2("  IDX", k, (int)iv);
			}
			tr2("  itype / elembuf", (int)itype, (int)g_bound_elem);
		}
		tr2("DRAW frame/idx", g_frame_no, g_draw_in_frame);
		tr2("DRAW mode/count", (int)mode, (int)count);
		tr2("DRAW tex/size", (int)g_bound_tex,
		    bt && bt->argb ? (int)(bt->w * 1000 + bt->h) : -1);
		tr2("DRAW tex2d/blend", g_tex2d_on, g_blend_on);
	}

	if (g_bail_logged < 6) {
		g_bail_logged++;
		tr2("draw guard: vtx.on / ptr", g_vtx.on, (int)(unsigned long)g_vtx.ptr);
	}
	if (g_bail_logged < 8) {
		g_bail_logged++;
		tr2("draw bufs: vtx.buf / tex.buf", (int)g_vtx.buf, (int)g_tex.buf);
		tr2("draw modes: mode / tex.on", (int)mode, g_tex.on);
	}
	if (!g_fb || !g_vtx.on || !array_base(&g_vtx) || count < 3) return;

	/* With an element buffer bound, `indices` is a BYTE OFFSET into it — and
	 * that offset is almost always ZERO, which arrives as a NULL pointer.
	 *
	 * Testing `indices &&` here therefore skipped the resolution for every
	 * normal draw, leaving indices NULL so IDX(n) fell back to sequential
	 * 0,1,2,3... For a quad (4 vertices, 6 indices) triangle 1 = 0,1,2 is
	 * accidentally correct while triangle 2 reads vertices 3,4,5 — two of them
	 * past the end of the buffer. Result: exactly one triangle of every quad
	 * degenerates, so backgrounds render as a diagonal half and circular
	 * buttons render as crescents.
	 *
	 * This is the SAME offset-0-looks-like-NULL trap already fixed for
	 * glVertexPointer. Gate on the BINDING, never on the pointer value. */
	if (g_bound_elem) {
		struct gl_buffer *eb = buf_find(g_bound_elem);
		if (!eb || !eb->data) return;
		indices = eb->data + (u32)(unsigned long)indices;
	}

#define IDX(n) (indices \
	? ((itype == GL_UNSIGNED_BYTE) ? (u32)((const u8 *)indices)[n] \
	                               : (u32)((const u16 *)indices)[n]) \
	: (u32)(n))

	if (mode == GL_TRIANGLES) {
		for (i = 0; i + 2 < count; i += 3) {
			a = IDX(i); b = IDX(i+1); c = IDX(i+2);
			build_vert(&v[0],a); build_vert(&v[1],b); build_vert(&v[2],c);
			raster_tri(v[0], v[1], v[2]);
		}
	} else if (mode == GL_TRIANGLE_STRIP) {
		for (i = 0; i + 2 < count; i++) {
			/* Alternate winding so the strip stays consistently oriented —
			 * otherwise every other triangle is back-facing and would be
			 * dropped once face culling exists. */
			if (i & 1) { a = IDX(i+1); b = IDX(i); c = IDX(i+2); }
			else       { a = IDX(i); b = IDX(i+1); c = IDX(i+2); }
			build_vert(&v[0],a); build_vert(&v[1],b); build_vert(&v[2],c);
			raster_tri(v[0], v[1], v[2]);
		}
	} else if (mode == GL_TRIANGLE_FAN) {
		a = IDX(0);
		for (i = 1; i + 1 < count; i++) {
			b = IDX(i); c = IDX(i+1);
			build_vert(&v[0],a); build_vert(&v[1],b); build_vert(&v[2],c);
			raster_tri(v[0], v[1], v[2]);
		}
	}
#undef IDX
}

/* ---- CLIENT-SIDE ARRAYS -------------------------------------------------
 *
 * The wire carries array references as (buffer name, byte offset), because a
 * guest POINTER means nothing on the host. That covers everything the menus do,
 * which is why they render — they upload to VBOs and pass an offset. But a title
 * may also pass vertices directly with no buffer bound, and the boot logos do
 * exactly that: measured, 59 of 60 draw-elements packets during the logo had no
 * usable vertex array, so the host skipped them and the screen stayed black.
 *
 * Rather than add an opcode, upload the client data into RESERVED buffer names
 * and reference those. Every existing mechanism — mirroring, GL_FIXED
 * conversion on the host, the resync — then applies unchanged.
 *
 * THESE MUST STAY ABOVE MAX_BUFS. They were 100..103 when the guest's own names
 * could only reach 64. Raising MAX_BUFS to 256 would have made a real buffer
 * collide with a reserved one — the game's geometry and a client-array upload
 * writing over each other, which is far worse than the exhaustion it was meant
 * to fix. The compile-time check below is what makes that impossible to get
 * wrong again; tadpole_hle.c's MAX_BUF must exceed HLE_CLIENT_IDX in turn.
 */
#define HLE_CLIENT_VTX 4000
#define HLE_CLIENT_COL 4001
#define HLE_CLIENT_TEX 4002
#define HLE_CLIENT_IDX 4003
#define HLE_CLIENT_NRM 4004
/* Must stay below tadpole_hle.c's MAX_BUF (4096) as well as above MAX_BUFS. */
typedef char hle_client_names_above_maxbufs[
	(HLE_CLIENT_VTX > MAX_BUFS && HLE_CLIENT_NRM < 4096) ? 1 : -1];

static void hle_send_array(struct array *a, u32 which, u32 nverts, u32 name)
{
	u32 stride;

	if (!a->on || !a->ptr) return;
	if (a->buf) {                       /* already in a buffer: send the ref */
		hle_arraypointer(which, a->buf, a->size, a->type, a->stride,
		                 (u32)(unsigned long)a->ptr);
		return;
	}
	stride = a->stride ? (u32)a->stride : elem_size(a->type, a->size);
	hle_bufferdata(name, nverts * stride, a->ptr);
	/* Tightly packed after the copy, so the original stride still applies. */
	hle_arraypointer(which, name, a->size, a->type, (int)stride, 0);
}

/* Highest index a draw will touch, so we know how much of a client array to
 * copy. The host cannot work this out for a client array — it never sees one. */
static u32 hle_max_index(const void *indices, GLsizei count, GLenum type)
{
	const u8 *base;
	u32 i, mx = 0;

	if (g_bound_elem) {
		struct gl_buffer *eb = buf_find(g_bound_elem);
		if (!eb || !eb->data) return 0;
		base = eb->data + (u32)(unsigned long)indices;
	} else {
		if (!indices) return 0;
		base = indices;
	}
	if (type == GL_UNSIGNED_SHORT) {
		const u16 *s = (const u16 *)base;
		for (i = 0; i < (u32)count; i++) if (s[i] > mx) mx = s[i];
	} else {
		for (i = 0; i < (u32)count; i++) if (base[i] > mx) mx = base[i];
	}
	return mx + 1;
}

/* ---- matrix palette skinning ---------------------------------------------
 *
 * These four were tracing no-ops that threw every argument away, so animated
 * characters were never deformed by their skeleton at all: Clam Prix's drivers
 * (Models\\DriverAnimations\\...) simply did not appear, while their rigid
 * karts, which ride on the ordinary modelview, rendered correctly.
 *
 * IT HAS TO BE DONE ON THE CPU. GL_OES_matrix_palette has no desktop
 * equivalent — GL_ARB_matrix_palette was barely ever implemented and this
 * machine's Mesa does not advertise it — so there is nothing to forward the
 * palette TO. Instead the vertices are transformed here and plain positions are
 * sent, which needs no host capability and works identically for the software
 * rasteriser.
 */
#define MAX_PALETTE 32
/* THE PALETTE STARTS AS IDENTITY, NOT AS ZEROS.
 *
 * This array is static, so without help every entry begins as sixteen zero
 * floats — and a zero matrix does not leave a vertex alone, it maps EVERY
 * vertex to the origin. Any vertex weighted to a bone the title has not loaded
 * yet is therefore dragged toward a single point in proportion to its weight,
 * which is why Pet Pals 2's dogs grew long spikes stretched between where the
 * geometry belongs and (0,0,0), and why it got dramatically worse the moment
 * they animated: a stationary model sits near its bind pose and the error is
 * small, while a moving one pulls the loaded bones away and leaves the unloaded
 * ones anchored at the origin.
 *
 * GL_OES_matrix_palette specifies identity as the initial value, which is the
 * benign version of the same situation: an unloaded bone leaves its vertices at
 * their bind-pose position, visibly wrong but attached to the model rather than
 * flung across the scene.
 *
 * mat_init_once() does exactly this for g_mv, g_proj and g_texm. The palette
 * was simply never added to it. */
static mat4  g_palette[MAX_PALETTE];
static int   g_palette_inited;

static void palette_init_once(void)
{
	int i;
	if (g_palette_inited) return;
	g_palette_inited = 1;
	for (i = 0; i < MAX_PALETTE; i++)
		mat_identity(&g_palette[i]);
}
static u32   g_cur_palette;
static int   g_palette_on;
static u32   g_palette_loads;   /* did the app ever fill the palette? */

void glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride, const void *p)
{ tr2("glMatrixIndexPointerOES size/type", size, (int)type);
  g_midx.size = size; g_midx.type = type; g_midx.stride = stride;
  g_midx.ptr = p; g_midx.buf = g_bound_array; }

void glWeightPointerOES(GLint size, GLenum type, GLsizei stride, const void *p)
{ tr2("glWeightPointerOES size/type", size, (int)type);
  g_wgt.size = size; g_wgt.type = type; g_wgt.stride = stride;
  g_wgt.ptr = p; g_wgt.buf = g_bound_array; }

void glCurrentPaletteMatrixOES(GLuint idx)
{ tr2("glCurrentPaletteMatrixOES", (int)idx, 0);
  if (idx < MAX_PALETTE) g_cur_palette = idx; }

/* The palette entry becomes a COPY of the current modelview. That is the whole
 * mechanism: the app poses a bone into the modelview, snapshots it here, and
 * repeats for each bone. */
/* WHICH SLOTS HAVE EVER BEEN WRITTEN. A palette entry that no
 * glLoadPaletteFromModelViewMatrixOES ever reached is still the identity, and a
 * vertex weighted to it lands at its raw bind-pose position instead of its posed
 * one — which looks like a body part detaching and hanging in the air rather
 * than like a matrix being subtly wrong. Nothing distinguishes that from a
 * correctly-posed bone without recording it. */
static u8 g_palette_set[MAX_PALETTE];

void glLoadPaletteFromModelViewMatrixOES(void)
{ tr2("glLoadPaletteFromModelViewMatrixOES", (int)g_cur_palette, 0);
  palette_init_once();
  if (g_cur_palette < MAX_PALETTE) { g_palette[g_cur_palette] = g_mv[g_mv_sp];
      g_palette_set[g_cur_palette] = 1;
      g_palette_loads++; } }

/* Bone INDICES must not be normalised. fetch() divides GL_UNSIGNED_BYTE by 255
 * because that is right for colours; an index of 7 would arrive as 0.027 and
 * every vertex would collapse onto palette entry 0. */
static u32 fetch_index(const struct array *a, u32 i, GLint c)
{
	u32 stride = a->stride ? (u32)a->stride : elem_size(a->type, a->size);
	const u8 *ab = array_base(a);
	const u8 *base;
	if (!ab || c >= a->size) return 0;
	base = ab + i * stride;
	switch (a->type) {
	case GL_UNSIGNED_BYTE: return (u32)base[c];
	case GL_BYTE:          return (u32)(unsigned char)((const signed char *)base)[c];
	case GL_SHORT:         return (u32)((const short *)base)[c];
	case GL_FLOAT:         return (u32)((const float *)base)[c];
	case GL_FIXED:         return (u32)fx2f(((const GLfixed *)base)[c]);
	default:               return 0;
	}
}

static float *g_skin;
static u32    g_skin_cap;

/* Blend each vertex by its bones. Returns tightly packed xyz floats, or NULL
 * when skinning is not active — in which case the caller sends the vertex array
 * unchanged. */
static const float *skin_vertices(u32 nverts)
{
	u32 i;
	int k, nb;

	/* RESOLVE THROUGH array_base(). A vertex array living in a buffer object
	 * carries an OFFSET in ->ptr, and that offset is normally 0 — so testing
	 * ->ptr rejected exactly the case this needs to handle, and skinning was
	 * never once reached even though the title plainly enables it. */
	if (!g_palette_on || !g_vtx.on || !array_base(&g_vtx)) return 0;
	if (!array_base(&g_midx) || !array_base(&g_wgt)) {
		/* Palette on but no bone data: say so ONCE. Silence here would look
		 * identical to skinning working, and the character would still be
		 * missing with nothing to explain why. */
		static int said;
		if (g_palette_on && !said) { said = 1;
			warn2("palette ON but index/weight arrays missing (idx,wgt set?)",
			      array_base(&g_midx) ? 1 : 0, array_base(&g_wgt) ? 1 : 0); }
		return 0;
	}
	if (!nverts) return 0;
	palette_init_once();
	{ static int said; if (!said) { said = 1;
		warn2("SKINNING a draw: bones, verts",
		      g_midx.size < g_wgt.size ? g_midx.size : g_wgt.size, (int)nverts); } }

	/* ---- ONE-SHOT SKINNING AUDIT ----------------------------------------
	 *
	 * Pet Pals 2 poses its dogs' bodies correctly and leaves their ears and
	 * muzzles hanging in the air, stationary while the rest animates. Three
	 * different causes produce that same picture and nothing in the output
	 * tells them apart:
	 *
	 *   1. the vertex references a palette slot nothing ever loaded, so its
	 *      bone is still the identity;
	 *   2. the index array is being read wrong, so it references the wrong
	 *      slot — a real slot, holding a real matrix, for a different bone;
	 *   3. the weights do not sum to 1, so the vertex is scaled toward or
	 *      away from the origin.
	 *
	 * So measure all three, once, on the first skinned draw of a title. Cheap
	 * because it happens exactly once, and it names the cause instead of
	 * narrowing it.
	 */
	/* NOT GATED ON TADPOLE_GL_DEBUG. It was, and it therefore did not run in
	 * the one capture that mattered — the whole point of warn2 is that the
	 * evidence is in an ordinary log without anyone having had to predict they
	 * would need it. This is a single pass over a single draw, once per title. */
	{
		static int audited;
		if (!audited) {
			u32 seen = 0, unset = 0, oor = 0, j;
			int lo_w = 1000000, hi_w = -1000000, maxidx = -1;
			int nbb = g_midx.size < g_wgt.size ? g_midx.size : g_wgt.size;
			if (nbb > 4) nbb = 4;
			audited = 1;
			for (j = 0; j < nverts; j++) {
				float sum = 0.0f;
				int c;
				for (c = 0; c < nbb; c++) {
					u32 mi = fetch_index(&g_midx, j, c);
					float w = fetch(&g_wgt, j, c);
					sum += w;
					if (mi >= MAX_PALETTE) { oor++; continue; }
					if (mi < 32 && !(seen & (1u << mi))) {
						seen |= 1u << mi;
						if (!g_palette_set[mi]) unset++;
					}
					if ((int)mi > maxidx) maxidx = (int)mi;
				}
				{ int s = (int)(sum * 1000.0f);
				  if (s < lo_w) lo_w = s;
				  if (s > hi_w) hi_w = s; }
			}
			warn2("SKIN AUDIT distinct-slots-used / of-those-NEVER-loaded",
			      (int)__builtin_popcount(seen), (int)unset);
			warn2("SKIN AUDIT highest index used / palette loads so far",
			      maxidx, (int)g_palette_loads);
			warn2("SKIN AUDIT weight sum x1000: min / max (want 1000/1000)",
			      lo_w, hi_w);
			warn2("SKIN AUDIT indices out of range / bones per vertex",
			      (int)oor, nbb);
			/* WHAT THE TITLE ACTUALLY DECLARED.
			 *
			 * nb is min(index.size, weight.size), so a weight sum that lands
			 * short has two very different explanations and these lines are
			 * what separate them:
			 *
			 *   - the two sizes DIFFER, and the min is throwing away
			 *     influences the model really has. Then the missing weight is
			 *     sitting in components we never read, and the fix is ours.
			 *   - the sizes AGREE, and the title's own weights genuinely do
			 *     not sum to 1. Then real hardware renders the same shrunken
			 *     vertex and the fix is not to "correct" it.
			 *
			 * The types matter for the same reason: GL_UNSIGNED_BYTE weights
			 * are normalised by 255 and GL_FIXED are 16.16, and reading one as
			 * the other yields a plausible-looking number rather than an
			 * obviously broken one. */
			warn2("SKIN AUDIT weight array: size / type",
			      g_wgt.size, (int)g_wgt.type);
			warn2("SKIN AUDIT index array: size / type",
			      g_midx.size, (int)g_midx.type);
			warn2("SKIN AUDIT strides: weight / index",
			      (int)g_wgt.stride, (int)g_midx.stride);
			/* Vertex 0's raw components, past the declared size, in
			 * thousandths. If components 2 and 3 hold the missing weight, the
			 * min above is the bug and this is the proof. */
			warn2("SKIN AUDIT v0 weights x1000: [0] / [1]",
			      (int)(fetch(&g_wgt, 0, 0) * 1000.0f),
			      (int)(fetch(&g_wgt, 0, 1) * 1000.0f));
			warn2("SKIN AUDIT v0 weights x1000: [2] / [3]",
			      (int)(fetch(&g_wgt, 0, 2) * 1000.0f),
			      (int)(fetch(&g_wgt, 0, 3) * 1000.0f));
		}
	}


	if (g_skin_cap < nverts * 3) {
		if (g_skin) free(g_skin);
		g_skin = malloc(nverts * 3 * (u32)sizeof(float));
		g_skin_cap = g_skin ? nverts * 3 : 0;
	}
	if (!g_skin) return 0;

	nb = g_midx.size < g_wgt.size ? g_midx.size : g_wgt.size;
	if (nb > 4) nb = 4;

	for (i = 0; i < nverts; i++) {
		float pos[4], acc[3];
		acc[0] = acc[1] = acc[2] = 0.0f;
		pos[0] = fetch(&g_vtx, i, 0);
		pos[1] = fetch(&g_vtx, i, 1);
		pos[2] = g_vtx.size > 2 ? fetch(&g_vtx, i, 2) : 0.0f;
		pos[3] = 1.0f;
		for (k = 0; k < nb; k++) {
			float w = fetch(&g_wgt, i, k), t[4];
			u32 mi;
			if (w == 0.0f) continue;      /* the common case: 1-2 live bones */
			mi = fetch_index(&g_midx, i, k);
			if (mi >= MAX_PALETTE) continue;
			vec_xform(&g_palette[mi], pos, t);
			acc[0] += w * t[0]; acc[1] += w * t[1]; acc[2] += w * t[2];
		}
		g_skin[i*3+0] = acc[0];
		g_skin[i*3+1] = acc[1];
		g_skin[i*3+2] = acc[2];
	}
	return g_skin;
}

/* ---- normals have to be skinned TOO, and by the same matrices ------------
 *
 * skin_begin() loads IDENTITY on the host, because the vertices it sends are
 * already transformed into eye space here. So anything else the draw supplies
 * has to be in eye space as well — and a normal array sent straight from the
 * title is in BIND-POSE MODEL space.
 *
 * Sending them unskinned is what turned Pet Pals 2's dogs almost black the
 * moment lighting started working: the geometry was posed and lit against
 * normals pointing wherever the model happened to face before it was animated,
 * so most of the surface faced away from every light. The background, which is
 * not skinned, lit correctly the whole time — which is the tell.
 *
 * TRANSLATION IS EXCLUDED, deliberately. A normal is a direction: it must be
 * rotated by the bone but never moved by it, so only the upper 3x3 applies.
 * Running a normal through vec_xform() with w=1 would add the bone's world
 * position to it and produce a "normal" of length several hundred.
 *
 * Not renormalised here. The blend of two unit normals is slightly short, and
 * correcting it costs a square root per vertex inside qemu; GL_NORMALIZE exists
 * for exactly this and the title can ask for it. If a title turns out to need
 * it and not ask, this is where to add it.
 */
static float *g_skin_n;
static u32    g_skin_n_cap;

static const float *skin_normals(u32 nverts)
{
	u32 i;
	int k, nb;

	if (!g_nrm.on || !array_base(&g_nrm) || !nverts) return 0;

	if (g_skin_n_cap < nverts * 3) {
		if (g_skin_n) free(g_skin_n);
		g_skin_n = malloc(nverts * 3 * (u32)sizeof(float));
		g_skin_n_cap = g_skin_n ? nverts * 3 : 0;
	}
	if (!g_skin_n) return 0;

	nb = g_midx.size < g_wgt.size ? g_midx.size : g_wgt.size;
	if (nb > 4) nb = 4;

	for (i = 0; i < nverts; i++) {
		float n[3], acc[3];
		acc[0] = acc[1] = acc[2] = 0.0f;
		n[0] = fetch(&g_nrm, i, 0);
		n[1] = fetch(&g_nrm, i, 1);
		n[2] = fetch(&g_nrm, i, 2);
		for (k = 0; k < nb; k++) {
			float w = fetch(&g_wgt, i, k);
			const mat4 *m;
			u32 mi;
			if (w == 0.0f) continue;
			mi = fetch_index(&g_midx, i, k);
			if (mi >= MAX_PALETTE) continue;
			m = &g_palette[mi];
			/* Upper 3x3 only — column-major, same layout as vec_xform. */
			acc[0] += w * (m->m[0]*n[0] + m->m[4]*n[1] + m->m[8]*n[2]);
			acc[1] += w * (m->m[1]*n[0] + m->m[5]*n[1] + m->m[9]*n[2]);
			acc[2] += w * (m->m[2]*n[0] + m->m[6]*n[1] + m->m[10]*n[2]);
		}
		g_skin_n[i*3+0] = acc[0];
		g_skin_n[i*3+1] = acc[1];
		g_skin_n[i*3+2] = acc[2];
	}
	return g_skin_n;
}

/* Send skinned positions and neutralise the modelview for the draw.
 * The palette matrices ALREADY contain the modelview — that is what
 * glLoadPaletteFromModelViewMatrixOES copied — so leaving it applied would
 * transform the character twice. Push/pop keeps the surrounding state intact,
 * which only works now that both are actually forwarded to the host. */
static int skin_begin(u32 nverts)
{
	const float *sk = skin_vertices(nverts);
	if (!sk) return 0;
	hle_matrixmode(GL_MODELVIEW);
	hle_pushmatrix();
	hle_loadidentity();
	hle_bufferdata(HLE_CLIENT_VTX, nverts * 3 * (u32)sizeof(float), sk);
	hle_arraypointer(0, HLE_CLIENT_VTX, 3, GL_FLOAT, 0, 0);
	{
		const float *sn = skin_normals(nverts);
		if (sn) {
			hle_bufferdata(HLE_CLIENT_NRM, nverts * 3 * (u32)sizeof(float), sn);
			hle_arraypointer(3, HLE_CLIENT_NRM, 3, GL_FLOAT, 0, 0);
		}
	}
	return 1;
}

static void skin_end(int active)
{
	if (!active) return;
	hle_popmatrix();
	hle_matrixmode(g_matrix_mode);   /* hand the mode back to the app's choice */
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	tr2("glDrawArrays mode/count", (int)mode, (int)count);
	tr2("glDrawArrays first", first, 0);
	/* A negative count is GL_INVALID_VALUE, and it is not hypothetical here: it
	 * is cast to u32 three lines down and becomes a request for four billion
	 * vertices, which either exhausts the ring or reads far past the array. */
	if (count < 0) { tad_gl_error(TAD_GL_INVALID_VALUE, "glDrawArrays"); return; }
	/* The one place HLE REPLACES work rather than adding to it: the whole point
	 * is not to rasterise here. */
	if (hle_ready()) {
		u32 nv = (u32)first + (u32)count;
		int sk = skin_begin(nv);
		if (!sk) hle_send_array(&g_vtx, 0, nv, HLE_CLIENT_VTX);
		hle_send_array(&g_col, 1, nv, HLE_CLIENT_COL);
		hle_send_array(&g_tex, 2, nv, HLE_CLIENT_TEX);
		/* ONLY WHEN THE DRAW IS NOT SKINNED. A skinned draw's normals are
		 * sent by skin_begin(), already transformed by the same bone matrices
		 * as its vertices; sending the raw bind-pose array as well would
		 * overwrite them with normals that no longer match the geometry. */
		if (!sk) hle_send_array(&g_nrm, 3, nv, HLE_CLIENT_NRM);
		hle_drawarrays(mode, first, count);
		skin_end(sk);
		return;
	}
	const u8 *vs = g_vtx.ptr, *cs = g_col.ptr, *ts = g_tex.ptr;
	if (first > 0) {
		if (g_vtx.ptr)
			g_vtx.ptr += (u32)first * (g_vtx.stride ? (u32)g_vtx.stride
			                                        : elem_size(g_vtx.type, g_vtx.size));
		if (g_col.ptr)
			g_col.ptr += (u32)first * (g_col.stride ? (u32)g_col.stride
			                                        : elem_size(g_col.type, g_col.size));
		/* The TEXCOORD array must be offset too. Latent today because every
		 * observed call passes first=0, but wrong the moment one does not. */
		if (g_tex.ptr)
			g_tex.ptr += (u32)first * (g_tex.stride ? (u32)g_tex.stride
			                                        : elem_size(g_tex.type, g_tex.size));
	}
	draw_indexed(mode, count, NULL, 0);
	g_vtx.ptr = vs; g_col.ptr = cs; g_tex.ptr = ts;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	tr2("glDrawElements mode/count/type", (int)mode, (int)count);
	tr2("glDrawElements itype", (int)type, 0);
	if (count < 0) { tad_gl_error(TAD_GL_INVALID_VALUE, "glDrawElements"); return; }
	/* GLES 1.1 allows only these two index types — GL_UNSIGNED_INT is an
	 * extension the device does not export. An unrecognised type used to fall
	 * through to the byte reader and index the array at a third of the intended
	 * stride, which draws a real but wrong mesh: exactly the kind of failure
	 * that looks like a modelling bug rather than ours. */
	if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT) {
		tad_gl_error(TAD_GL_INVALID_ENUM, "glDrawElements"); return; }
	if (hle_ready()) {
		u32 nv = hle_max_index(indices, count, type);
		u32 ebuf = g_bound_elem;
		u32 eoff = (u32)(unsigned long)indices;

		/* Any array without a buffer has to travel by value — see the note on
		 * client-side arrays above. */
		int sk = skin_begin(nv);
		if (!sk) hle_send_array(&g_vtx, 0, nv, HLE_CLIENT_VTX);
		hle_send_array(&g_col, 1, nv, HLE_CLIENT_COL);
		hle_send_array(&g_tex, 2, nv, HLE_CLIENT_TEX);
		/* ONLY WHEN THE DRAW IS NOT SKINNED. A skinned draw's normals are
		 * sent by skin_begin(), already transformed by the same bone matrices
		 * as its vertices; sending the raw bind-pose array as well would
		 * overwrite them with normals that no longer match the geometry. */
		if (!sk) hle_send_array(&g_nrm, 3, nv, HLE_CLIENT_NRM);
		if (!ebuf && indices) {         /* client-side indices too */
			u32 isz = (type == GL_UNSIGNED_SHORT) ? 2u : 1u;
			hle_bufferdata(HLE_CLIENT_IDX, (u32)count * isz, indices);
			ebuf = HLE_CLIENT_IDX;
			eoff = 0;
		}
		/* With an element buffer bound, `indices` is a BYTE OFFSET into it, and
		 * that offset is usually zero — which arrives as a NULL pointer. Gate
		 * on the BINDING, never on the pointer value; the same trap cost hours
		 * in the software path. */
		hle_drawelements(mode, count, type, ebuf, eoff);
		skin_end(sk);
		return;
	}
	draw_indexed(mode, count, indices, type);
}

void glBindBuffer(GLenum target, GLuint buf)
{
	tr2("glBindBuffer target/name", (int)target, (int)buf);
	if (target == GL_ARRAY_BUFFER)              g_bound_array = buf;
	else if (target == GL_ELEMENT_ARRAY_BUFFER) g_bound_elem  = buf;
}

void glBufferData(GLenum target, GLint size, const void *data, GLenum usage)
{
	GLuint name = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_bound_elem : g_bound_array;
	struct gl_buffer *b = buf_slot(name);
	(void)usage;
	if (!b || size <= 0) return;
	if (b->data) { free(b->data); b->data = NULL; b->size = 0; }
	b->data = malloc((u32)size);
	if (!b->data) return;
	b->size = (u32)size;
	if (data) memcpy(b->data, data, (u32)size);
	/* Mirrored on the host as plain bytes, keyed by the same name. Sending the
	 * data rather than a pointer is what makes the boundary crossable. */
	if (hle_ready()) hle_bufferdata(name, (u32)size, data);
}

void glBufferSubData(GLenum target, GLint offset, GLint size, const void *data)
{
	GLuint name = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_bound_elem : g_bound_array;
	struct gl_buffer *b = buf_find(name);
	if (!b || !b->data || !data || size <= 0) return;
	if ((u32)offset + (u32)size > b->size) return;
	memcpy(b->data + offset, data, (u32)size);
	if (hle_ready()) hle_buffersubdata(name, (u32)offset, (u32)size, data);
}

void glDeleteBuffers(GLsizei n, const GLuint *names)
{
	GLsizei i;
	if (!names) return;
	for (i = 0; i < n; i++) {
		struct gl_buffer *b = buf_find(names[i]);
		if (b) { if (b->data) free(b->data); b->data = NULL; b->size = 0; b->name = 0; }
	}
}

void glBindTexture(GLenum target, GLuint n)
{
	(void)target;
	g_bound_tex = n;
	/* In GL, binding an unused name CREATES the object. Without this a game
	 * that binds a name it never generated gets no storage at all. */
	if (n) tex_slot(n);
	if (hle_ready()) hle_bindtexture(n);
}

/* THE CAPABILITIES glEnable/glDisable ACCEPT, per GLES 1.1 §6.3 plus the two
 * OES extensions we answer for. Anything else is GL_INVALID_ENUM.
 *
 * This is not pedantry. Forwarding an unrecognised cap to the host is how
 * GL_MATRIX_PALETTE_OES used to raise GL_INVALID_ENUM on the host every single
 * frame, and a host GL error poisons every draw after it until something calls
 * glGetError. Rejecting it here, where we can name it, is the difference
 * between "a frame came back empty" and "the title enabled 0x8840 and we do not
 * take that". It is also the one check tools/glconform can use to prove error
 * tracking works at all: it enables a deliberately bogus cap and looks for
 * GL_INVALID_ENUM. */
static int cap_valid(GLenum c)
{
	switch (c) {
	case 0x0B44: case 0x0B50: case 0x0B60: case 0x0B90: case 0x0BC0:
	/*   CULL_FACE     LIGHTING     FOG        STENCIL_TEST ALPHA_TEST */
	case 0x0BD0: case 0x0BE2: case 0x0B71: case 0x0C11: case 0x0DE1:
	/*   DITHER        BLEND        DEPTH_TEST SCISSOR_TEST TEXTURE_2D */
	case 0x0B57: case 0x0BA1: case 0x0B20: case 0x0B10: case 0x0C4D:
	/*   COLOR_MATERIAL NORMALIZE  LINE_SMOOTH POINT_SMOOTH RESCALE_NORMAL */
	case 0x0BF2: case 0x8037: case 0x809D: case 0x809E: case 0x809F:
	/*   COLOR_LOGIC_OP POLY_OFF_FILL MULTISAMPLE  S_A_TO_COVERAGE S_A_TO_ONE */
	case 0x80A0: case 0x8861: case 0x8840:
	/*   SAMPLE_COVERAGE POINT_SPRITE_OES MATRIX_PALETTE_OES */
		return 1;
	default:
		/* GL_LIGHT0..7 and GL_CLIP_PLANE0..5 are contiguous ranges. */
		return (c >= 0x4000 && c <= 0x4007) || (c >= 0x3000 && c <= 0x3005);
	}
}

void glEnable(GLenum cap)
{ tr2("glEnable cap", (int)cap, 0);
  if (!cap_valid(cap)) { tad_gl_error(TAD_GL_INVALID_ENUM, "glEnable"); return; }
  if (cap == GL_TEXTURE_2D) g_tex2d_unit[g_active_tex] = 1;
  else if (cap == GL_BLEND) g_blend_on = 1;
  else if (cap == GL_DEPTH_TEST) g_depth_test_on = 1;
  /* GL_MATRIX_PALETTE_OES is handled ENTIRELY on this side — see
   * skin_vertices(). Forwarding it raised GL_INVALID_ENUM on the host every
   * frame, because no desktop driver accepts it. */
  else if (cap == GL_MATRIX_PALETTE_OES) {
      static int said; if (!said) { said = 1;
          warn2("MATRIX_PALETTE enabled by the title (skinning is in use)", 0, 0); }
      g_palette_on = 1; return; }
  if (hle_ready()) hle_enable(cap); }
void glDisable(GLenum cap)
{ tr2("glDisable cap", (int)cap, 0);
  if (!cap_valid(cap)) { tad_gl_error(TAD_GL_INVALID_ENUM, "glDisable"); return; }
  if (cap == GL_TEXTURE_2D) g_tex2d_unit[g_active_tex] = 0;
  else if (cap == GL_BLEND) g_blend_on = 0;
  else if (cap == GL_DEPTH_TEST) g_depth_test_on = 0;
  else if (cap == GL_MATRIX_PALETTE_OES) { g_palette_on = 0; return; }
  if (hle_ready()) hle_disable(cap); }
void glDepthFunc(GLenum f) { tr2("glDepthFunc", (int)f, 0); g_depth_func = f;
  if (hle_ready()) hle_depthfunc(f); }
void glDepthMask(GLboolean on)   { g_depth_mask = on ? 1 : 0;
  if (hle_ready()) hle_depthmask(on ? 1 : 0); }
void glClearDepthf(GLfloat d)    { g_depth_clear = d; }
void glClearDepthx(GLfixed d)    { g_depth_clear = fx2f(d); }
void glDepthRangef(GLfloat n, GLfloat f) { (void)n; (void)f; }
/* Defined with the rest of the family further down; declared here because this
 * is where the other texture state setters live. */
void glTexParameterx(GLenum t, GLenum p, GLfixed v);
void glBlendFunc(GLenum sf, GLenum df) { tr2("glBlendFunc src/dst", (int)sf, (int)df);
  if (hle_ready()) hle_blendfunc(sf, df); }

/* TEXEL CONVERSION — one path for glTexImage2D and glTexSubImage2D.
 *
 * `fmt` was previously ignored and every GL_UNSIGNED_BYTE upload was read as
 * 4 bytes per texel. A GL_RGB texture is 3, and GL_ALPHA / GL_LUMINANCE are 1,
 * so those were read with the wrong stride: the image walked diagonally and the
 * tail ran off the end of the caller's buffer.
 *
 * GLES 1.x fixed-function expansion rules:
 *   GL_ALPHA            RGB = 0,        A = texel
 *   GL_LUMINANCE        RGB = texel,    A = 255
 *   GL_LUMINANCE_ALPHA  RGB = texel[0], A = texel[1]
 *   GL_RGB              RGB = texel,    A = 255
 */
static u32 texel_bytes(GLenum fmt, GLenum type)
{
	if (type != GL_UNSIGNED_BYTE)
		return 2;                       /* every 16-bit packed type */
	switch (fmt) {
	case GL_ALPHA:
	case GL_LUMINANCE:       return 1;
	case GL_LUMINANCE_ALPHA: return 2;
	case GL_RGB:             return 3;
	default:                 return 4;  /* GL_RGBA */
	}
}

static void convert_row(u32 *dst, const void *src, u32 n, GLenum fmt, GLenum type)
{
	u32 i;

	if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
		const u16 *s16 = src;
		for (i = 0; i < n; i++) {
			u32 v = s16[i];
			u32 R = ((v>>12)&0xF)*17, G = ((v>>8)&0xF)*17;
			u32 B = ((v>>4)&0xF)*17,  A = (v&0xF)*17;
			dst[i] = (A<<24)|(R<<16)|(G<<8)|B;
		}
	} else if (type == GL_UNSIGNED_SHORT_5_6_5) {
		const u16 *s16 = src;
		for (i = 0; i < n; i++) {
			u32 v = s16[i];
			u32 R = ((v>>11)&0x1F)<<3, G = ((v>>5)&0x3F)<<2, B = (v&0x1F)<<3;
			dst[i] = 0xFF000000u|(R<<16)|(G<<8)|B;
		}
	} else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
		const u16 *s16 = src;
		for (i = 0; i < n; i++) {
			u32 v = s16[i];
			u32 R = ((v>>11)&0x1F)<<3, G = ((v>>6)&0x1F)<<3;
			u32 B = ((v>>1)&0x1F)<<3,  A = (v&1) ? 255 : 0;
			dst[i] = (A<<24)|(R<<16)|(G<<8)|B;
		}
	} else {
		const u8 *s8 = src;
		switch (fmt) {
		case GL_ALPHA:
			for (i = 0; i < n; i++) dst[i] = (u32)s8[i] << 24;
			break;
		case GL_LUMINANCE:
			for (i = 0; i < n; i++) {
				u32 L = s8[i];
				dst[i] = 0xFF000000u|(L<<16)|(L<<8)|L;
			}
			break;
		case GL_LUMINANCE_ALPHA:
			for (i = 0; i < n; i++) {
				u32 L = s8[i*2], A = s8[i*2+1];
				dst[i] = (A<<24)|(L<<16)|(L<<8)|L;
			}
			break;
		case GL_RGB:
			for (i = 0; i < n; i++) {
				u32 R = s8[i*3], G = s8[i*3+1], B = s8[i*3+2];
				dst[i] = 0xFF000000u|(R<<16)|(G<<8)|B;
			}
			break;
		default:
			for (i = 0; i < n; i++) {
				u32 R = s8[i*4], G = s8[i*4+1], B = s8[i*4+2], A = s8[i*4+3];
				dst[i] = (A<<24)|(R<<16)|(G<<8)|B;
			}
			break;
		}
	}
}

void glTexImage2D(GLenum tgt, GLint lvl, GLint ifmt, GLsizei w, GLsizei h,
                  GLint brd, GLenum fmt, GLenum type, const void *px)
{
	struct gl_texture *t;
	u32 i, n;

	(void)tgt; (void)ifmt; (void)brd;
	if (lvl != 0 || w <= 0 || h <= 0)      /* mip levels ignored for now */
		return;
	t = tex_slot(g_bound_tex);
	tr2("upload to tex id / w", (int)g_bound_tex, (int)w);
	tr2("  fmt / type", (int)fmt, (int)type);
	if (!t) return;
	if (t->argb) { free(t->argb); t->argb = NULL; }
	n = (u32)w * (u32)h;
	t->w = (u32)w; t->h = (u32)h;
	t->argb = malloc(n * 4);
	if (!t->argb) { t->w = t->h = 0; return; }

	/* A NULL upload ALLOCATES storage; the pixels arrive later through
	 * glTexSubImage2D. Filling with opaque white is why unfilled textures
	 * showed up as white squares — keep it, but now the sub-upload lands. */
	if (!px) {
		for (i = 0; i < n; i++) t->argb[i] = 0xFFFFFFFFu;
		tr2("  NULL upload — awaiting glTexSubImage2D", (int)w, (int)h);
		return;
	}
	convert_row(t->argb, px, n, fmt, type);
	dump_tex(g_bound_tex, t->w, t->h, t->argb);
	/* One canonical layout on the wire. Every pixel-format decision stays on
	 * this side, where it is already tested, so the host only ever sees
	 * ARGB8888 — which on a little-endian host is GL_BGRA. */
	if (hle_ready()) hle_teximage2d(g_bound_tex, t->w, t->h, t->argb);
}

/* Fill part of an existing texture. Previously a no-op stub, which left every
 * texture built as "allocate then fill" stuck on the white fill above — the
 * "textures that flat out just don't render... they render as white squares"
 * the owner reported. */
void glTexSubImage2D(GLenum tgt, GLint lvl, GLint xoff, GLint yoff,
                     GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                     const void *px)
{
	struct gl_texture *t;
	const u8 *src = px;
	u32 bpt, row;
	GLint y;

	(void)tgt;
	if (lvl != 0 || w <= 0 || h <= 0 || !px)
		return;
	t = tex_find(g_bound_tex);
	if (!t || !t->argb) return;
	if (xoff < 0 || yoff < 0 ||
	    (u32)(xoff + w) > t->w || (u32)(yoff + h) > t->h) {
		tr2("glTexSubImage2D OUT OF RANGE w/h", (int)w, (int)h);
		return;
	}
	tr2("glTexSubImage2D tex/w", (int)g_bound_tex, (int)w);

	bpt = texel_bytes(fmt, type);
	row = (u32)w * bpt;
	for (y = 0; y < h; y++)
		convert_row(t->argb + (u32)(yoff + y) * t->w + (u32)xoff,
		            src + (u32)y * row, (u32)w, fmt, type);
	dump_tex(g_bound_tex, t->w, t->h, t->argb);
	/* Re-send the whole texture rather than the sub-rectangle: the converted
	 * rows are not contiguous, and a full upload of an already-converted image
	 * is cheaper than assembling a packed sub-image. */
	if (hle_ready()) hle_teximage2d(g_bound_tex, t->w, t->h, t->argb);
}

/* PALETTED TEXTURES — glCompressedTexImage2D.
 *
 * This entry point was defined NOWHERE in the shim. Because our libGLESv1_CM
 * keeps a DT_NEEDED on the renamed stock library, the call silently fell
 * through to Nexell's dead VR5 driver, which cannot do anything without
 * /dev/ogl_vr5. The texture therefore kept the opaque white that the NULL
 * glTexImage2D path fills in — "a lot of textures flat out just don't render,
 * they render as white squares".
 *
 * A missing symbol that RESOLVES to something useless is worse than one that
 * fails to link: nothing reports an error, the app runs, and the output is
 * merely wrong. App.so imports it, so it was always going to matter:
 *     glCompressedTexImage2D glTexImage2D glTexParameteri  (no glTexSubImage2D)
 *
 * OES_compressed_paletted_texture is core in GLES 1.x. Layout is a palette of
 * 2^n entries in the named base format, immediately followed by the index
 * stream for level 0 (then each smaller level, which we ignore). PALETTE4
 * packs two indices per byte, HIGH NIBBLE FIRST, as one continuous stream —
 * rows are not byte-aligned.
 */
#define GL_PALETTE4_RGB8_OES      0x8B90
#define GL_PALETTE4_RGBA8_OES     0x8B91
#define GL_PALETTE4_R5_G6_B5_OES  0x8B92
#define GL_PALETTE4_RGBA4_OES     0x8B93
#define GL_PALETTE4_RGB5_A1_OES   0x8B94
#define GL_PALETTE8_RGB8_OES      0x8B95
#define GL_PALETTE8_RGBA8_OES     0x8B96
#define GL_PALETTE8_R5_G6_B5_OES  0x8B97
#define GL_PALETTE8_RGBA4_OES     0x8B98
#define GL_PALETTE8_RGB5_A1_OES   0x8B99

/* -> bits per index (4 or 8), or 0 if this is not a paletted format. */
static u32 palette_bits(GLenum ifmt)
{
	if (ifmt >= GL_PALETTE4_RGB8_OES && ifmt <= GL_PALETTE4_RGB5_A1_OES) return 4;
	if (ifmt >= GL_PALETTE8_RGB8_OES && ifmt <= GL_PALETTE8_RGB5_A1_OES) return 8;
	return 0;
}

/* Base format of one palette entry, as an (fmt,type) pair convert_row understands. */
static void palette_entry_kind(GLenum ifmt, GLenum *fmt, GLenum *type, u32 *bytes)
{
	switch (ifmt) {
	case GL_PALETTE4_RGB8_OES:
	case GL_PALETTE8_RGB8_OES:
		*fmt = GL_RGB;  *type = GL_UNSIGNED_BYTE;          *bytes = 3; break;
	case GL_PALETTE4_RGBA8_OES:
	case GL_PALETTE8_RGBA8_OES:
		*fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE;          *bytes = 4; break;
	case GL_PALETTE4_R5_G6_B5_OES:
	case GL_PALETTE8_R5_G6_B5_OES:
		*fmt = GL_RGB;  *type = GL_UNSIGNED_SHORT_5_6_5;   *bytes = 2; break;
	case GL_PALETTE4_RGBA4_OES:
	case GL_PALETTE8_RGBA4_OES:
		*fmt = GL_RGBA; *type = GL_UNSIGNED_SHORT_4_4_4_4; *bytes = 2; break;
	default:   /* GL_PALETTE*_RGB5_A1_OES */
		*fmt = GL_RGBA; *type = GL_UNSIGNED_SHORT_5_5_5_1; *bytes = 2; break;
	}
}

void glCompressedTexImage2D(GLenum tgt, GLint lvl, GLenum ifmt,
                            GLsizei w, GLsizei h, GLint brd,
                            GLsizei imgsz, const void *data)
{
	struct gl_texture *t;
	u32 pal[256];
	GLenum pfmt, ptype;
	u32 bits, entries, ebytes, i, n;
	const u8 *idx;

	(void)tgt; (void)brd;
	tr2("glCompressedTexImage2D ifmt/size", (int)ifmt, (int)imgsz);
	tr2("  w / h", (int)w, (int)h);

	bits = palette_bits(ifmt);
	if (!bits) {
		/* Not paletted. Nothing else is core in GLES1, so this would be a
		 * vendor format we cannot decode — say so instead of drawing white. */
		warn2("UNSUPPORTED compressed format / size", (int)ifmt, (int)imgsz);
		tad_gl_error(TAD_GL_INVALID_ENUM, "glCompressedTexImage2D");
		return;
	}
	/* Level is non-positive in this extension (it encodes how many mip levels
	 * follow). Only the base level is used here. */
	if (lvl > 0 || w <= 0 || h <= 0 || !data)
		return;

	palette_entry_kind(ifmt, &pfmt, &ptype, &ebytes);
	entries = (bits == 4) ? 16u : 256u;
	convert_row(pal, data, entries, pfmt, ptype);
	idx = (const u8 *)data + entries * ebytes;

	t = tex_slot(g_bound_tex);
	if (!t) return;
	if (t->argb) { free(t->argb); t->argb = NULL; }
	n = (u32)w * (u32)h;
	t->w = (u32)w; t->h = (u32)h;
	t->argb = malloc(n * 4);
	if (!t->argb) { t->w = t->h = 0; return; }

	if (bits == 8) {
		for (i = 0; i < n; i++)
			t->argb[i] = pal[idx[i]];
	} else {
		for (i = 0; i < n; i++) {
			u32 b = idx[i >> 1];
			t->argb[i] = pal[(i & 1) ? (b & 0xF) : (b >> 4)];
		}
	}
	tr2("  paletted OK, bits/entries", (int)bits, (int)entries);
	dump_tex(g_bound_tex, t->w, t->h, t->argb);
	if (hle_ready()) hle_teximage2d(g_bound_tex, t->w, t->h, t->argb);
}

/* ---- glTexParameter* / glGetTexParameter* -------------------------------
 *
 * Six of these nine were stubs, and glGetTexParameteriv is the single
 * most-called missing entry point measured so far: 532 times in one Clam Prix
 * race, each one reading uninitialised memory because a stub getter never
 * touches the caller's buffer.
 *
 * All nine share tex_param_set/get for the same reason the TexEnv family does.
 * The state lives on the texture OBJECT — see struct gl_texture — because that
 * is where GL keeps it and because answering per-unit would give the wrong
 * answer the moment a title binds a second texture.
 */
static struct gl_texture *tex_param_target(GLenum target)
{
	struct gl_texture *t;
	if (target != GL_TEXTURE_2D) {
		tad_gl_error(TAD_GL_INVALID_ENUM, "glTexParameter");
		return NULL;
	}
	if (!g_bound_tex) {
		if (!g_tex_default_init) { g_tex_default_init = 1;
		                           tex_defaults(&g_tex_default); }
		return &g_tex_default;
	}
	t = tex_slot(g_bound_tex);
	return t;
}

/* `v` is the raw enum value in every case: GLES 1.1 has no float-valued texture
 * parameter, so nothing here is ever scaled and the f/x spellings are pure
 * casts. Getting that wrong is how GL_LINEAR becomes 0. */
static void tex_param_set(GLenum target, GLenum pname, GLint v)
{
	struct gl_texture *t = tex_param_target(target);
	if (!t) return;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER_: t->min_filter = (GLenum)v; break;
	case GL_TEXTURE_MAG_FILTER_: t->mag_filter = (GLenum)v; break;
	case GL_TEXTURE_WRAP_S_:     t->wrap_s = (GLenum)v; break;
	case GL_TEXTURE_WRAP_T_:     t->wrap_t = (GLenum)v; break;
	case GL_GENERATE_MIPMAP_:    t->gen_mipmap = (GLenum)v; break;
	default:
		/* GL_TEXTURE_CROP_RECT_OES and friends land here. Raise the error the
		 * device would raise rather than storing state we cannot answer for,
		 * and — critically — do NOT forward it. Forwarding whatever arrived was
		 * the cause of the standing "GL error 0x0500 from TEXPARAM" in every
		 * race: one rejected call on the host leaves the error flag set for
		 * every call after it. */
		tad_gl_error(TAD_GL_INVALID_ENUM, "glTexParameter");
		return;
	}
	if (hle_ready()) hle_texparam(pname, v);
}

static int tex_param_get(GLenum target, GLenum pname, GLint *out)
{
	struct gl_texture *t = tex_param_target(target);
	if (!t) return 0;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER_: *out = (GLint)t->min_filter; return 1;
	case GL_TEXTURE_MAG_FILTER_: *out = (GLint)t->mag_filter; return 1;
	case GL_TEXTURE_WRAP_S_:     *out = (GLint)t->wrap_s; return 1;
	case GL_TEXTURE_WRAP_T_:     *out = (GLint)t->wrap_t; return 1;
	case GL_GENERATE_MIPMAP_:    *out = (GLint)t->gen_mipmap; return 1;
	default: return 0;
	}
}

void glTexParameteri(GLenum t, GLenum p, GLint v)
{ tr2("glTexParameteri pname/val", (int)p, (int)v); tex_param_set(t, p, v); }

void glTexParameterx(GLenum t, GLenum p, GLfixed v)
{ tr2("glTexParameterx pname/val", (int)p, (int)v); tex_param_set(t, p, (GLint)v); }

/* ---- per-fragment and per-primitive state -------------------------------
 *
 * Fifteen entry points that were no-op stubs, ranked by how many of the 87
 * installed titles link them (tools/gl-demand.py): glLineWidthx 32,
 * glScissor 23, glPolygonOffsetx 20, glPointSizex 19, glStencilMask 19,
 * glColorMask 12. None of them is hard; they were skipped because "state
 * setters" sounded less important than geometry, and the cost was that a title
 * asking for a 2-pixel line or a clipped viewport got neither and was told
 * nothing.
 *
 * The x-suffixed forms take GLfixed and the plain forms take float. GLES 1.1
 * §2.3: the conversion is the value divided by 65536, never a cast — this is
 * exactly the trap glTexEnvx documents on the other side, where the value is an
 * enum and must NOT be scaled. Here every value really is a number.
 */
static float g_line_width = 1.0f, g_point_size = 1.0f;
static float g_poly_factor, g_poly_units;
static int   g_scissor_on;
static GLint g_scissor[4];
static GLboolean g_color_mask[4] = { 1, 1, 1, 1 };

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	tr2("glScissor xy", x, y);
	tr2("glScissor wh", (int)w, (int)h);
	/* NO ERROR, DELIBERATELY, THOUGH THE SPEC SAYS GL_INVALID_VALUE.
	 *
	 * Measured on the device: glconform's errors.negative_size calls
	 * glScissor(0,0,-1,-1) and the VR5 driver raises nothing at all. Raising it
	 * here would mean a title that makes this call — and ignores it on real
	 * hardware, because nothing happens — starts seeing a GL error only under
	 * Tadpole. Titles were written against this driver, not against the spec,
	 * so the driver is what to match.
	 *
	 * Still not FORWARDED: desktop GL does enforce it, and one rejected call
	 * leaves the host's error flag set for everything after it. And still said
	 * out loud, once, because a negative scissor box is a bug in the title
	 * whether or not anyone raises an error for it. */
	if (w < 0 || h < 0) {
		static int said;
		if (!said) { said = 1;
			warn2("glScissor with a negative size — ignored, as the device"
			      " ignores it (w, h)", (int)w, (int)h); }
		return;
	}
	g_scissor[0] = x; g_scissor[1] = y; g_scissor[2] = w; g_scissor[3] = h;
	g_scissor_on = 1;
	if (hle_ready()) hle_scissor(x, y, (int)w, (int)h);
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	tr2("glColorMask rg", r, g);
	g_color_mask[0] = r; g_color_mask[1] = g;
	g_color_mask[2] = b; g_color_mask[3] = a;
	if (hle_ready()) hle_colormask(r ? 1u : 0u, g ? 1u : 0u,
	                               b ? 1u : 0u, a ? 1u : 0u);
}

void glLineWidth(GLfloat w)
{
	tr2("glLineWidth x1000", (int)(w * 1000.0f), 0);
	if (w <= 0.0f) { tad_gl_error(TAD_GL_INVALID_VALUE, "glLineWidth"); return; }
	g_line_width = w;
	if (hle_ready()) hle_linewidth(w);
}
void glLineWidthx(GLfixed w) { glLineWidth(fx2f(w)); }

void glPointSize(GLfloat s)
{
	tr2("glPointSize x1000", (int)(s * 1000.0f), 0);
	if (s <= 0.0f) { tad_gl_error(TAD_GL_INVALID_VALUE, "glPointSize"); return; }
	g_point_size = s;
	if (hle_ready()) hle_pointsize(s);
}
void glPointSizex(GLfixed s) { glPointSize(fx2f(s)); }

void glPolygonOffset(GLfloat factor, GLfloat units)
{
	tr2("glPolygonOffset f/u x1000", (int)(factor * 1000.0f),
	    (int)(units * 1000.0f));
	g_poly_factor = factor; g_poly_units = units;
	if (hle_ready()) hle_polygonoffset(factor, units);
}
void glPolygonOffsetx(GLfixed factor, GLfixed units)
{ glPolygonOffset(fx2f(factor), fx2f(units)); }

/* ---- lighting, materials and the light model ----------------------------
 *
 * THE BIGGEST REMAINING CLUSTER, AND THE MEASUREMENT SAYS SO TWICE. 31 of the
 * 87 installed titles link glLightxv and 30 link glMaterialxv
 * (tools/gl-demand.py); one Pet Pals 2 session called glLightx and glLightxv
 * 6490 times EACH (gl-warnings.log). It is also why the viewer filtered
 * GL_LIGHTING out of glEnable entirely — turning lighting on with no light
 * state shades everything black, which is worse than flat — so implementing
 * this is what removes that hack rather than working around it.
 *
 * THE HOST DOES THE LIGHTING, NOT US. Desktop GL's compatibility profile has
 * the whole fixed-function lighting pipeline, so the state is forwarded and
 * evaluated there. The software rasteriser stays unlit: writing a second
 * lighting implementation to run inside qemu, in soft-float, per vertex, would
 * cost more than it could ever return. Titles run on the HLE path.
 *
 * EVERY x-SUFFIXED VALUE HERE IS SCALED, which is the opposite of the TexEnv
 * family a few hundred lines up. There, most parameters are enums and 16.16 of
 * an enum is meaningless. Here every parameter is a real number — a colour, a
 * position, an exponent, an attenuation coefficient — so glLightx and friends
 * divide by 65536. The one exception is GL_LIGHT_MODEL_TWO_SIDE, which is a
 * boolean; see glLightModelx.
 */
#define MAX_LIGHTS 8
#define GL_AMBIENT_               0x1200
#define GL_DIFFUSE_               0x1201
#define GL_SPECULAR_              0x1202
#define GL_POSITION_              0x1203
#define GL_SPOT_DIRECTION_        0x1204
#define GL_SPOT_EXPONENT_         0x1205
#define GL_SPOT_CUTOFF_           0x1206
#define GL_CONSTANT_ATTENUATION_  0x1207
#define GL_LINEAR_ATTENUATION_    0x1208
#define GL_QUADRATIC_ATTENUATION_ 0x1209
#define GL_EMISSION_              0x1600
#define GL_SHININESS_             0x1601
#define GL_AMBIENT_AND_DIFFUSE_   0x1602
#define GL_LIGHT_MODEL_TWO_SIDE_  0x0B52
#define GL_LIGHT_MODEL_AMBIENT_   0x0B53
#define GL_FRONT_                 0x0404
#define GL_BACK_                  0x0405
#define GL_FRONT_AND_BACK_        0x0408

struct gl_light {
	float ambient[4], diffuse[4], specular[4], position[4];
	float spot_dir[3], spot_exp, spot_cutoff;
	float att[3];                  /* constant, linear, quadratic */
};
struct gl_material {
	float ambient[4], diffuse[4], specular[4], emission[4], shininess;
};

static struct gl_light    g_lights[MAX_LIGHTS];
static struct gl_material g_material;
static float g_light_model_ambient[4];
static int   g_light_model_two_side;
static int   g_light_init;

static void set4(float *d, float a, float b, float c, float e)
{ d[0]=a; d[1]=b; d[2]=c; d[3]=e; }

/* GLES 1.1 §2.12.1 initial state. These are not filler: a title that enables
 * GL_LIGHT0 and sets only its position expects the SPEC's white diffuse and
 * specular for light 0 specifically, and black for lights 1-7. Getting that
 * wrong lights the scene at the wrong brightness with no call to blame. */
static void light_init_once(void)
{
	int i;
	if (g_light_init) return;
	g_light_init = 1;
	for (i = 0; i < MAX_LIGHTS; i++) {
		struct gl_light *l = &g_lights[i];
		set4(l->ambient, 0.0f, 0.0f, 0.0f, 1.0f);
		set4(l->diffuse,  i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f,
		                  i == 0 ? 1.0f : 0.0f, 1.0f);
		set4(l->specular, i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f,
		                  i == 0 ? 1.0f : 0.0f, 1.0f);
		set4(l->position, 0.0f, 0.0f, 1.0f, 0.0f);   /* directional, +z */
		l->spot_dir[0] = 0.0f; l->spot_dir[1] = 0.0f; l->spot_dir[2] = -1.0f;
		l->spot_exp = 0.0f; l->spot_cutoff = 180.0f;
		l->att[0] = 1.0f; l->att[1] = 0.0f; l->att[2] = 0.0f;
	}
	set4(g_material.ambient,  0.2f, 0.2f, 0.2f, 1.0f);
	set4(g_material.diffuse,  0.8f, 0.8f, 0.8f, 1.0f);
	set4(g_material.specular, 0.0f, 0.0f, 0.0f, 1.0f);
	set4(g_material.emission, 0.0f, 0.0f, 0.0f, 1.0f);
	g_material.shininess = 0.0f;
	set4(g_light_model_ambient, 0.2f, 0.2f, 0.2f, 1.0f);
	g_light_model_two_side = 0;
}

/* How many components a light parameter carries, 0 if it is not one. */
static int light_count(GLenum p)
{
	switch (p) {
	case GL_AMBIENT_: case GL_DIFFUSE_: case GL_SPECULAR_: case GL_POSITION_:
		return 4;
	case GL_SPOT_DIRECTION_: return 3;
	case GL_SPOT_EXPONENT_: case GL_SPOT_CUTOFF_:
	case GL_CONSTANT_ATTENUATION_: case GL_LINEAR_ATTENUATION_:
	case GL_QUADRATIC_ATTENUATION_:
		return 1;
	default: return 0;
	}
}

static int material_count(GLenum p)
{
	switch (p) {
	case GL_AMBIENT_: case GL_DIFFUSE_: case GL_SPECULAR_: case GL_EMISSION_:
	case GL_AMBIENT_AND_DIFFUSE_:
		return 4;
	case GL_SHININESS_: return 1;
	default: return 0;
	}
}

static void light_set(GLenum light, GLenum p, const float *v)
{
	int idx = (int)light - 0x4000;      /* GL_LIGHT0 */
	struct gl_light *l;
	int n = light_count(p), i;

	light_init_once();
	if (idx < 0 || idx >= MAX_LIGHTS) { tad_gl_error(TAD_GL_INVALID_ENUM, "glLight"); return; }
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glLight"); return; }
	l = &g_lights[idx];

	switch (p) {
	case GL_AMBIENT_:  for (i=0;i<4;i++) l->ambient[i]  = v[i]; break;
	case GL_DIFFUSE_:  for (i=0;i<4;i++) l->diffuse[i]  = v[i]; break;
	case GL_SPECULAR_: for (i=0;i<4;i++) l->specular[i] = v[i]; break;
	case GL_POSITION_: for (i=0;i<4;i++) l->position[i] = v[i]; break;
	case GL_SPOT_DIRECTION_: for (i=0;i<3;i++) l->spot_dir[i] = v[i]; break;
	case GL_SPOT_EXPONENT_: l->spot_exp = v[0]; break;
	case GL_SPOT_CUTOFF_:   l->spot_cutoff = v[0]; break;
	case GL_CONSTANT_ATTENUATION_:  l->att[0] = v[0]; break;
	case GL_LINEAR_ATTENUATION_:    l->att[1] = v[0]; break;
	case GL_QUADRATIC_ATTENUATION_: l->att[2] = v[0]; break;
	}
	/* FORWARDED IMMEDIATELY, AND THE ORDER IS THE POINT. GL_POSITION and
	 * GL_SPOT_DIRECTION are transformed by the modelview matrix in force AT THE
	 * MOMENT OF THE CALL, not at draw time. Matrix commands travel the same
	 * ring in the same order, so the host's modelview is whatever the guest's
	 * was when this call was made — provided we send it now rather than
	 * batching it. Deferring light state to the next draw would silently move
	 * every positional light. */
	if (hle_ready()) hle_light(light, p, v, n);
}

static void material_set(GLenum face, GLenum p, const float *v)
{
	int n = material_count(p), i;
	light_init_once();
	/* GLES 1.1 has no per-face materials: GL_FRONT_AND_BACK is the only legal
	 * face, and GL_FRONT/GL_BACK are an error rather than a subset. */
	if (face != GL_FRONT_AND_BACK_) { tad_gl_error(TAD_GL_INVALID_ENUM, "glMaterial"); return; }
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glMaterial"); return; }
	switch (p) {
	case GL_AMBIENT_:  for (i=0;i<4;i++) g_material.ambient[i]  = v[i]; break;
	case GL_DIFFUSE_:  for (i=0;i<4;i++) g_material.diffuse[i]  = v[i]; break;
	case GL_SPECULAR_: for (i=0;i<4;i++) g_material.specular[i] = v[i]; break;
	case GL_EMISSION_: for (i=0;i<4;i++) g_material.emission[i] = v[i]; break;
	case GL_AMBIENT_AND_DIFFUSE_:
		for (i=0;i<4;i++) { g_material.ambient[i] = v[i];
		                    g_material.diffuse[i] = v[i]; } break;
	case GL_SHININESS_: g_material.shininess = v[0]; break;
	}
	if (hle_ready()) hle_material(face, p, v, n);
}

static void lightmodel_set(GLenum p, const float *v)
{
	int i;
	light_init_once();
	if (p == GL_LIGHT_MODEL_AMBIENT_) {
		for (i = 0; i < 4; i++) g_light_model_ambient[i] = v[i];
		if (hle_ready()) hle_lightmodel(p, v, 4);
	} else if (p == GL_LIGHT_MODEL_TWO_SIDE_) {
		g_light_model_two_side = v[0] != 0.0f;
		if (hle_ready()) hle_lightmodel(p, v, 1);
	} else {
		tad_gl_error(TAD_GL_INVALID_ENUM, "glLightModel");
	}
}

/* ---- the twelve setters ------------------------------------------------- */

void glLightf(GLenum l, GLenum p, GLfloat v)  { light_set(l, p, &v); }
void glLightfv(GLenum l, GLenum p, const GLfloat *v) { if (v) light_set(l, p, v); }
void glLightx(GLenum l, GLenum p, GLfixed v)  { float f = fx2f(v); light_set(l, p, &f); }
void glLightxv(GLenum l, GLenum p, const GLfixed *v)
{
	float f[4]; int i, n = light_count(p);
	if (!v) return;
	if (!n) n = 1;
	for (i = 0; i < n; i++) f[i] = fx2f(v[i]);
	light_set(l, p, f);
}

void glMaterialf(GLenum f, GLenum p, GLfloat v)  { material_set(f, p, &v); }
void glMaterialfv(GLenum f, GLenum p, const GLfloat *v) { if (v) material_set(f, p, v); }
void glMaterialx(GLenum f, GLenum p, GLfixed v)  { float x = fx2f(v); material_set(f, p, &x); }
void glMaterialxv(GLenum f, GLenum p, const GLfixed *v)
{
	float x[4]; int i, n = material_count(p);
	if (!v) return;
	if (!n) n = 1;
	for (i = 0; i < n; i++) x[i] = fx2f(v[i]);
	material_set(f, p, x);
}

void glLightModelf(GLenum p, GLfloat v)  { lightmodel_set(p, &v); }
void glLightModelfv(GLenum p, const GLfloat *v) { if (v) lightmodel_set(p, v); }

/* NOT SCALED FOR TWO_SIDE. It is a boolean, and a title enabling two-sided
 * lighting passes GL_TRUE — the integer 1, not 1<<16. Dividing that by 65536
 * gives 0.0000152, which is non-zero and happens to work here, but would be a
 * live bug the moment anything compared it against 1.0. Treat it as the
 * boolean it is. */
void glLightModelx(GLenum p, GLfixed v)
{ float f = (p == GL_LIGHT_MODEL_TWO_SIDE_) ? (float)(v != 0) : fx2f(v);
  lightmodel_set(p, &f); }

void glLightModelxv(GLenum p, const GLfixed *v)
{
	float f[4]; int i, n;
	if (!v) return;
	if (p == GL_LIGHT_MODEL_TWO_SIDE_) { f[0] = (float)(v[0] != 0);
	                                     lightmodel_set(p, f); return; }
	n = (p == GL_LIGHT_MODEL_AMBIENT_) ? 4 : 1;
	for (i = 0; i < n; i++) f[i] = fx2f(v[i]);
	lightmodel_set(p, f);
}

/* ---- the four getters --------------------------------------------------- */

static int light_get(GLenum light, GLenum p, float *out)
{
	int idx = (int)light - 0x4000;
	struct gl_light *l;
	int n = light_count(p), i;
	light_init_once();
	if (idx < 0 || idx >= MAX_LIGHTS || !n) return 0;
	l = &g_lights[idx];
	switch (p) {
	case GL_AMBIENT_:  for (i=0;i<4;i++) out[i] = l->ambient[i];  break;
	case GL_DIFFUSE_:  for (i=0;i<4;i++) out[i] = l->diffuse[i];  break;
	case GL_SPECULAR_: for (i=0;i<4;i++) out[i] = l->specular[i]; break;
	/* A KNOWN, BOUNDED DIVERGENCE. Real GL transforms GL_POSITION and
	 * GL_SPOT_DIRECTION by the modelview matrix at the moment they are SET, and
	 * glGetLight returns the transformed value in eye coordinates. We return
	 * what the title passed in. The two agree whenever the modelview was
	 * identity at the time of the call — which is what glconform tests, and
	 * hardware agrees there — and diverge otherwise.
	 *
	 * Left as-is deliberately: no installed title imports glGetLightfv or
	 * glGetLightxv at all (tools/gl-demand.py), so this affects the conformance
	 * harness and nothing else. Doing it properly means snapshotting the
	 * modelview per light per set, which is real state for no measured
	 * benefit. If a title ever shows up in the demand table, do it then. */
	case GL_POSITION_: for (i=0;i<4;i++) out[i] = l->position[i]; break;
	case GL_SPOT_DIRECTION_: for (i=0;i<3;i++) out[i] = l->spot_dir[i]; break;
	case GL_SPOT_EXPONENT_: out[0] = l->spot_exp; break;
	case GL_SPOT_CUTOFF_:   out[0] = l->spot_cutoff; break;
	case GL_CONSTANT_ATTENUATION_:  out[0] = l->att[0]; break;
	case GL_LINEAR_ATTENUATION_:    out[0] = l->att[1]; break;
	case GL_QUADRATIC_ATTENUATION_: out[0] = l->att[2]; break;
	}
	return n;
}

static int material_get(GLenum face, GLenum p, float *out)
{
	int n = material_count(p), i;
	light_init_once();
	/* glGetMaterial DOES accept GL_FRONT and GL_BACK even though glMaterial
	 * only accepts GL_FRONT_AND_BACK — GLES 1.1 §6.1.3. Both return the same
	 * values here, because there is only one material. */
	if (face != GL_FRONT_ && face != GL_BACK_ && face != GL_FRONT_AND_BACK_) return 0;
	if (!n || p == GL_AMBIENT_AND_DIFFUSE_) return 0;   /* not a queryable pname */
	switch (p) {
	case GL_AMBIENT_:  for (i=0;i<4;i++) out[i] = g_material.ambient[i];  break;
	case GL_DIFFUSE_:  for (i=0;i<4;i++) out[i] = g_material.diffuse[i];  break;
	case GL_SPECULAR_: for (i=0;i<4;i++) out[i] = g_material.specular[i]; break;
	case GL_EMISSION_: for (i=0;i<4;i++) out[i] = g_material.emission[i]; break;
	case GL_SHININESS_: out[0] = g_material.shininess; break;
	}
	return n;
}

void glGetLightfv(GLenum l, GLenum p, GLfloat *v)
{
	float f[4]; int n, i;
	if (!v) return;
	n = light_get(l, p, f);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetLightfv"); return; }
	for (i = 0; i < n; i++) v[i] = f[i];
}
void glGetLightxv(GLenum l, GLenum p, GLfixed *v)
{
	float f[4]; int n, i;
	if (!v) return;
	n = light_get(l, p, f);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetLightxv"); return; }
	for (i = 0; i < n; i++) v[i] = f2fx(f[i]);
}
void glGetMaterialfv(GLenum f, GLenum p, GLfloat *v)
{
	float x[4]; int n, i;
	if (!v) return;
	n = material_get(f, p, x);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetMaterialfv"); return; }
	for (i = 0; i < n; i++) v[i] = x[i];
}
void glGetMaterialxv(GLenum f, GLenum p, GLfixed *v)
{
	float x[4]; int n, i;
	if (!v) return;
	n = material_get(f, p, x);
	if (!n) { tad_gl_error(TAD_GL_INVALID_ENUM, "glGetMaterialxv"); return; }
	for (i = 0; i < n; i++) v[i] = f2fx(x[i]);
}

/* ---- the current normal -------------------------------------------------
 *
 * Used when GL_NORMAL_ARRAY is off — one normal for every vertex in the draw.
 * No installed title imports these (tools/gl-demand.py), which is expected:
 * they all use glNormalPointer instead. Implemented anyway because they are two
 * lines each and because "the title lit nothing" and "we dropped its normal"
 * must not be able to look the same. */
static float g_normal[3] = { 0.0f, 0.0f, 1.0f };

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
	g_normal[0]=x; g_normal[1]=y; g_normal[2]=z;
	if (hle_ready()) hle_normal(x, y, z);
}
void glNormal3x(GLfixed x, GLfixed y, GLfixed z)
{ glNormal3f(fx2f(x), fx2f(y), fx2f(z)); }

/* ---- stencil: TRACKED, DELIBERATELY NOT FORWARDED -----------------------
 *
 * 19 titles link glStencilMask and 8 each link glStencilFunc/Op/ClearStencil,
 * which makes this look like a significant gap. It is not, and the device says
 * so: glconform reports GL_STENCIL_BITS = 0 on real hardware. The VR5's EGL
 * config has no stencil attachment, so on a real LeapPad2 every one of these
 * calls is accepted and changes nothing observable — a title that draws a
 * stencilled shadow gets no shadow on the device either.
 *
 * Forwarding them to the host WOULD change the picture, because the host's FBO
 * could be given a stencil buffer, and then we would render something the
 * hardware never renders. Matching the device is the goal, so the state is
 * stored — glGet* can answer honestly — and nothing crosses the wire.
 *
 * If a title ever turns out to depend on real stencilling, the fix is to add a
 * stencil attachment in make_target() and start forwarding; this comment is the
 * record of why that was not done speculatively.
 */
static GLenum g_stencil_func = GL_ALWAYS;
static GLint  g_stencil_ref;
static GLuint g_stencil_valuemask = 0xFFFFFFFFu, g_stencil_writemask = 0xFFFFFFFFu;
static GLenum g_stencil_fail = 0x1E00, g_stencil_zfail = 0x1E00, g_stencil_zpass = 0x1E00;
static GLint  g_stencil_clear;

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{ tr2("glStencilFunc func/ref", (int)func, ref);
  g_stencil_func = func; g_stencil_ref = ref; g_stencil_valuemask = mask; }

void glStencilOp(GLenum sfail, GLenum zfail, GLenum zpass)
{ tr2("glStencilOp sfail/zfail", (int)sfail, (int)zfail);
  g_stencil_fail = sfail; g_stencil_zfail = zfail; g_stencil_zpass = zpass; }

void glStencilMask(GLuint mask)
{ g_stencil_writemask = mask; }

void glClearStencil(GLint s)
{ g_stencil_clear = s; }

/* ---- ordering: honest no-ops -------------------------------------------
 *
 * There is nothing to flush. In software mode the rasteriser has already
 * written the pixels by the time either of these returns; in HLE mode the
 * command ring is drained by the host and the guest is held at PRESENT until
 * the frame is replayed, which is a stronger guarantee than glFinish asks for.
 *
 * Issuing a real host glFinish here would be actively harmful: it is a full
 * pipeline stall, and titles call it per frame. */
void glFlush(void)  { tr2("glFlush", 0, 0); }
void glFinish(void) { tr2("glFinish", 0, 0); }

/* Advisory by definition — GLES 1.1 §5.2 lets an implementation ignore any
 * hint. Validated rather than ignored silently, so a bad enum is still an
 * error the way it is on the device. */
void glHint(GLenum target, GLenum mode)
{
	tr2("glHint target/mode", (int)target, (int)mode);
	switch (target) {
	case 0x0C50: /* PERSPECTIVE_CORRECTION */ case 0x0C51: /* POINT_SMOOTH */
	case 0x0C52: /* LINE_SMOOTH */            case 0x0C53: /* POLYGON_SMOOTH */
	case 0x0C54: /* FOG */                    case 0x8192: /* GENERATE_MIPMAP */
		break;
	default: tad_gl_error(TAD_GL_INVALID_ENUM, "glHint"); return;
	}
	if (mode != 0x1100 /* DONT_CARE */ && mode != 0x1101 /* FASTEST */ &&
	    mode != 0x1102 /* NICEST */)
		tad_gl_error(TAD_GL_INVALID_ENUM, "glHint");
}

void glTexParameterf(GLenum t, GLenum p, GLfloat v)
{ tr2("glTexParameterf pname/val", (int)p, (int)v); tex_param_set(t, p, (GLint)v); }

void glTexParameteriv(GLenum t, GLenum p, const GLint *v)
{ if (v) { tr2("glTexParameteriv pname", (int)p, 0); tex_param_set(t, p, v[0]); } }

void glTexParameterfv(GLenum t, GLenum p, const GLfloat *v)
{ if (v) { tr2("glTexParameterfv pname", (int)p, 0);
           tex_param_set(t, p, (GLint)v[0]); } }

void glTexParameterxv(GLenum t, GLenum p, const GLfixed *v)
{ if (v) { tr2("glTexParameterxv pname", (int)p, 0); tex_param_set(t, p, v[0]); } }

void glGetTexParameteriv(GLenum t, GLenum p, GLint *v)
{
	tr2("glGetTexParameteriv pname", (int)p, 0);
	if (!v) return;
	if (!tex_param_get(t, p, v)) tad_gl_error(TAD_GL_INVALID_ENUM,
	                                          "glGetTexParameteriv");
}

void glGetTexParameterfv(GLenum t, GLenum p, GLfloat *v)
{
	GLint i = 0;
	tr2("glGetTexParameterfv pname", (int)p, 0);
	if (!v) return;
	if (tex_param_get(t, p, &i)) *v = (GLfloat)i;
	else tad_gl_error(TAD_GL_INVALID_ENUM, "glGetTexParameterfv");
}

void glGetTexParameterxv(GLenum t, GLenum p, GLfixed *v)
{
	GLint i = 0;
	tr2("glGetTexParameterxv pname", (int)p, 0);
	if (!v) return;
	/* Not scaled: every GLES1 texture parameter is an enum or a boolean, and
	 * 16.16 of GL_LINEAR is not GL_LINEAR. */
	if (tex_param_get(t, p, &i)) *v = (GLfixed)i;
	else tad_gl_error(TAD_GL_INVALID_ENUM, "glGetTexParameterxv");
}

void glDeleteTextures(GLsizei n, const GLuint *names)
{
	GLsizei i;
	if (!names) return;
	for (i = 0; i < n; i++) tr2("glDeleteTextures name", (int)names[i], 0);
	for (i = 0; i < n; i++) {
		struct gl_texture *t = tex_find(names[i]);
		if (t) { if (t->argb) free(t->argb); t->argb = NULL; t->w = t->h = 0; t->name = 0; }
		/* TELL THE HOST. Without this its mirror outlives the texture, and
		 * since names are handed out by slot index the game reuses them almost
		 * immediately — so a draw between the delete and the next upload
		 * samples the PREVIOUS texture's pixels. Clam Prix cycles textures
		 * continuously during a race ("Release Texture" / "Load Texture" in its
		 * own log), which is why the symptom was constant flicker rather than
		 * an occasional wrong image. */
		if (hle_ready()) hle_deletetexture(names[i]);
	}
}

/* ---- object names --------------------------------------------------------
 *
 * GL semantics, and both details matter:
 *
 *  1. TEXTURES AND BUFFERS HAVE SEPARATE NAMESPACES. Texture 1 and buffer 1
 *     are different objects. A single shared counter appears to work because
 *     names stay unique, but it makes our numbering diverge from what the
 *     application's own bookkeeping expects.
 *  2. DELETED NAMES ARE REUSED. glGenTextures returns the lowest unused name.
 *
 * Getting this wrong is why the background rendered solid white: the game's
 * texture manager continually loads and releases (its own log shows
 * "GLTextureManager Release Texture ... Load Texture ..."), our name space
 * drifted from the game's, and it ended up binding texture 37 while every
 * upload had gone to ids we had allocated differently. tex_find returned NULL,
 * the sampler fell through to the default vertex colour, and the default
 * vertex colour is opaque white.
 *
 * Allocating from the slot table itself makes name == slot and keeps the two
 * in step by construction.
 */
/* Returning 0 here is a SILENT catastrophe: the app binds name 0, every upload
 * goes nowhere, and the sampler falls through to the vertex colour — a white
 * square. Any "textures render as white" report should check this first. */
int g_tex_gen_fail;

void glGenTextures(GLsizei n, GLuint *o)
{
	GLsizei k; int i;
	if (!o) return;
	for (k = 0; k < n; k++) {
		o[k] = 0;
		for (i = 0; i < MAX_TEXS; i++)
			if (!g_texs[i].name) {
				g_texs[i].name = (GLuint)(i + 1);   /* names are 1-based */
				g_texs[i].argb = NULL; g_texs[i].w = g_texs[i].h = 0;
				o[k] = g_texs[i].name;
				break;
			}
		if (!o[k]) {
			g_tex_gen_fail++;
			warn2("glGenTextures EXHAUSTED (MAX_TEXS), fails so far",
			      MAX_TEXS, g_tex_gen_fail);
			tad_gl_error(TAD_GL_OUT_OF_MEMORY, "glGenTextures");
		}
	}
}

/* Returning 0 here was a SILENT catastrophe, and it is what made Clam Prix
 * races render black: the app binds element-array name 0, glBufferData finds no
 * slot and stores nothing, and every glDrawElements then arrives at the host
 * with elembuf=0 and is skipped. Unlike glGenTextures this said nothing at all,
 * so the only visible symptom was an empty frame. */
int g_buf_gen_fail;

void glGenBuffers(GLsizei n, GLuint *o)
{
	GLsizei k; int i;
	if (!o) return;
	for (k = 0; k < n; k++) {
		o[k] = 0;
		for (i = 0; i < MAX_BUFS; i++)
			if (!g_bufs[i].name) {
				g_bufs[i].name = (GLuint)(i + 1);
				g_bufs[i].data = NULL; g_bufs[i].size = 0;
				o[k] = g_bufs[i].name;
				break;
			}
		if (!o[k]) {
			g_buf_gen_fail++;
			warn2("glGenBuffers EXHAUSTED (MAX_BUFS), fails so far",
			      MAX_BUFS, g_buf_gen_fail);
			tad_gl_error(TAD_GL_OUT_OF_MEMORY, "glGenBuffers");
		}
	}
}

/* THIS USED TO BE `return 0;` — a hardcoded "everything is fine".
 *
 * Not a stale value, a lie, and the most expensive line in the file: a title
 * that checks its own GL usage was told it had none to fix, and so was every
 * test we might have written against this shim. The differential harness in
 * tools/glconform exists to compare us against real hardware, and until this
 * tracked real state every error assertion it made passed on our side by
 * construction.
 *
 * The state itself lives in tadpole_gles_debug.c so the stubs and the core
 * share one copy. Sticky-first-error semantics are GLES 1.1 §2.5: the FIRST
 * error since the last read is the one reported, because that is the one
 * nearest the cause. */
GLenum glGetError(void) { return tad_gl_error_take(); }
const u8 *glGetString(GLenum n) { (void)n; return (const u8 *)"Tadpole GLES 1.1"; }
/* ---- STATE QUERIES -------------------------------------------------------
 *
 * These used to answer "0" and "not enabled" to everything, which is far worse
 * than not implementing them. A renderer that saves and restores state does
 *
 *     GLboolean was = glIsEnabled(GL_TEXTURE_2D);
 *     ... draw ...
 *     if (was) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
 *
 * and a glIsEnabled that always says 0 turns every restore into a DISABLE. The
 * app then fights to re-enable texturing — 1156 glEnable(GL_TEXTURE_2D) against
 * 365 glDisable in one Clam Prix run — and any draw that lands on the wrong side
 * comes out flat white with its texture still bound. That is what covered the
 * credits screen with an opaque white quad while texture 44 sat bound and
 * unused, and it is not title-specific: any GL code that preserves state hits it.
 *
 * Answer honestly from the state we already track, and report plausible limits
 * for the rest.
 */
#define GL_MAX_TEXTURE_SIZE              0x0D33
#define GL_MAX_TEXTURE_UNITS             0x84E2
#define GL_TEXTURE_BINDING_2D            0x8069
#define GL_ARRAY_BUFFER_BINDING          0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING  0x8895
#define GL_VIEWPORT                      0x0BA2
#define GL_DEPTH_BITS                    0x0D56
#define GL_RED_BITS                      0x0D52
#define GL_GREEN_BITS                    0x0D53
#define GL_BLUE_BITS                     0x0D54
#define GL_ALPHA_BITS                    0x0D55
#define GL_MAX_MODELVIEW_STACK_DEPTH     0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH    0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH       0x0D39
#define GL_MAX_LIGHTS                    0x0D31
#define GL_MAX_CLIP_PLANES               0x0D32
#define GL_SUBPIXEL_BITS                 0x0D50
#define GL_STENCIL_BITS                  0x0D57
#define GL_NORMAL_ARRAY                  0x8075
#define GL_CULL_FACE                     0x0B44

/* EVERY CONSTANT BELOW WAS READ OFF THE DEVICE, not chosen to look plausible.
 * tools/glconform's limit.* tests query all of them through the same binary on
 * both sides, so this table can be re-derived rather than trusted:
 *
 *     ./tools/glconform/run-hw.py 192.168.0.111
 *     grep '^RESULT limit\.' tools/glconform/hw.log
 *
 * Three were wrong before that run and none of them looked wrong: 1024 for
 * MAX_TEXTURE_SIZE against the device's 4096, 16 for both stack depths against
 * 32, and MAX_TEXTURE_UNITS answered from our own array size (4) rather than
 * the two units the hardware actually has. A limit reported too SMALL is the
 * dangerous direction — a title that respects it produces different geometry
 * here than on the device, and nothing raises an error to say so. */
void glGetIntegerv(GLenum p, GLint *v)
{
	if (!v) return;
	switch (p) {
	case GL_MAX_TEXTURE_SIZE:             *v = 4096; break;
	/* Our arrays hold MAX_TEXUNITS (4); the DEVICE has 2, and that is the
	 * number a title is entitled to act on. Spare capacity on our side is
	 * harmless, advertising it is not. */
	case GL_MAX_TEXTURE_UNITS:            *v = 2; break;
	case GL_TEXTURE_BINDING_2D:           *v = (GLint)g_bound_tex; break;
	case GL_ARRAY_BUFFER_BINDING:         *v = (GLint)g_bound_array; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = (GLint)g_bound_elem; break;
	case GL_MAX_MODELVIEW_STACK_DEPTH:    *v = MV_STACK_DEPTH; break;
	/* SKINNING CAPABILITIES. Unanswered, these fell to the default below and
	 * reported ZERO — telling a title that no palette matrices and no bones per
	 * vertex exist. A model that asks before setting up skinning is then
	 * entitled to skip it. Clam Prix turns out not to ask, but answering 0 to a
	 * capability query is wrong regardless: the caller reads it as "this
	 * feature has zero capacity" and silently disables what depended on it. */
	case GL_MAX_PALETTE_MATRICES_OES:     *v = MAX_PALETTE; break;
	case GL_MAX_VERTEX_UNITS_OES:         *v = 4; break;
	case GL_MAX_PROJECTION_STACK_DEPTH:   *v = PROJ_STACK_DEPTH; break;
	case GL_MAX_TEXTURE_STACK_DEPTH:      *v = TEXM_STACK_DEPTH; break;
	case GL_MAX_LIGHTS:                   *v = 8; break;
	/* ONE. Not six, which is the GLES 1.1 minimum and the number every
	 * reference implementation offers — the VR5 gives a single user clip
	 * plane, so a title using two would silently lose one on hardware. */
	case GL_MAX_CLIP_PLANES:              *v = 1; break;
	case GL_SUBPIXEL_BITS:                *v = 4; break;
	case GL_DEPTH_BITS:                   *v = 16; break;
	/* Zero on the device: its EGL config has no stencil attachment at all,
	 * which is why the stencil entry points can stay unimplemented far longer
	 * than their count in the stub list suggests. */
	case GL_STENCIL_BITS:                 *v = 0; break;
	case GL_RED_BITS: case GL_GREEN_BITS:
	case GL_BLUE_BITS: case GL_ALPHA_BITS: *v = 8; break;
	case GL_VIEWPORT:
		view_init();
		v[0] = g_vx; v[1] = g_vy; v[2] = g_vw; v[3] = g_vh;
		break;
	default:
		/* SAY SO, AND RAISE THE ERROR THE DEVICE RAISES.
		 *
		 * A silent 0 is indistinguishable from a real answer and disables
		 * features in the caller without explanation. Hardware rejects a pname
		 * it does not know with GL_INVALID_ENUM — measured, via glconform's
		 * limit.MAX_ELEMENTS_VERTICES, which is a GLES2 enum the VR5 correctly
		 * refuses — so match that rather than inventing a value.
		 *
		 * The 0 is still written, deliberately. This branch cannot tell an enum
		 * that is invalid everywhere from a perfectly valid GLES1 query we have
		 * not implemented yet, and for the second kind a caller that ignores the
		 * error should read a defined value rather than its own stack. The
		 * warning is what distinguishes them: anything showing up here that a
		 * real title asks for belongs in the switch above.
		 *
		 * Reported per distinct pname, not once ever. A single "said" flag meant
		 * the first unhandled query hid every one after it, so the log named one
		 * pname and implied it was the only one. */
		tr2("glGetIntegerv UNHANDLED pname", (int)p, 0);
		{
			static GLenum said[16];
			static unsigned n_said;
			unsigned i;
			for (i = 0; i < n_said; i++)
				if (said[i] == p) break;
			if (i == n_said) {
				if (n_said < 16) said[n_said++] = p;
				warn2("glGetIntegerv UNHANDLED pname (answering 0 and raising"
				      " GL_INVALID_ENUM — implement it if a title needs it)",
				      (int)p, 0);
			}
		}
		tad_gl_error(TAD_GL_INVALID_ENUM, "glGetIntegerv");
		*v = 0;
		break;
	}
}

/* ---- glGetFloatv / glGetFixedv / glGetBooleanv ---------------------------
 *
 * ALL THREE WERE STUBS, AND CLAM PRIX CALLS glGetFixedv. That is not a guess:
 * a full headless run of the title under the new stub instrumentation reported
 * exactly two unimplemented entry points, and this was one of them. A stub
 * getter does not return a wrong answer — it never touches the caller's buffer
 * at all, so the title reads whatever was already on its own stack and carries
 * on. Every one of those reads was silent before this.
 *
 * ONE SOURCE OF TRUTH, THREE TYPES. GLES 1.1 §6.1.2 defines the glGet variants
 * as the same state converted on the way out, so state_floats() answers once and
 * the three entry points differ only in how they write it down. Writing three
 * separate switches is how the fixed-point and float paths drift apart, which
 * is a bug this project has already paid for once — see the note on
 * glLoadMatrixx/glLoadMatrixf being genuinely different functions on the device.
 *
 * Anything state_floats() does not know falls through to glGetIntegerv, which
 * owns the integer-typed state and raises GL_INVALID_ENUM for a pname nobody
 * knows. That keeps the "which queries do titles actually make" warning in one
 * place instead of three.
 */
#define GL_CURRENT_COLOR         0x0B00
#define GL_POINT_SIZE            0x0B11
#define GL_LINE_WIDTH            0x0B21
#define GL_ALPHA_TEST_REF        0x0BC2
#define GL_DEPTH_RANGE           0x0B70
#define GL_DEPTH_CLEAR_VALUE     0x0B73
#define GL_COLOR_CLEAR_VALUE     0x0C22
#define GL_MODELVIEW_MATRIX      0x0BA6
#define GL_PROJECTION_MATRIX     0x0BA7
#define GL_TEXTURE_MATRIX        0x0BA8
#define GL_POLYGON_OFFSET_UNITS  0x2A00
#define GL_POLYGON_OFFSET_FACTOR 0x8038

GLboolean glIsEnabled(GLenum c);      /* defined just below; used by glGetBooleanv */

/* Returns how many components were written, 0 if this pname is not float-typed
 * state we track. `out` must have room for 16. */
static int state_floats(GLenum p, float *out)
{
	int i;
	mat_init_once();
	switch (p) {
	case GL_MODELVIEW_MATRIX:
		for (i = 0; i < 16; i++) out[i] = g_mv[g_mv_sp].m[i];
		return 16;
	case GL_PROJECTION_MATRIX:
		for (i = 0; i < 16; i++) out[i] = g_proj[g_proj_sp].m[i];
		return 16;
	case GL_TEXTURE_MATRIX:
		for (i = 0; i < 16; i++) out[i] = g_texm[g_texm_sp].m[i];
		return 16;
	case GL_CURRENT_COLOR:
		out[0] = (float)((g_cur_color >> 16) & 0xFF) / 255.0f;
		out[1] = (float)((g_cur_color >>  8) & 0xFF) / 255.0f;
		out[2] = (float)( g_cur_color        & 0xFF) / 255.0f;
		out[3] = (float)((g_cur_color >> 24) & 0xFF) / 255.0f;
		return 4;
	case GL_COLOR_CLEAR_VALUE:
		out[0] = (float)((g_clear_argb >> 16) & 0xFF) / 255.0f;
		out[1] = (float)((g_clear_argb >>  8) & 0xFF) / 255.0f;
		out[2] = (float)( g_clear_argb        & 0xFF) / 255.0f;
		out[3] = (float)((g_clear_argb >> 24) & 0xFF) / 255.0f;
		return 4;
	case GL_DEPTH_CLEAR_VALUE: out[0] = g_depth_clear; return 1;
	case GL_ALPHA_TEST_REF:    out[0] = (float)g_alpha_ref / 255.0f; return 1;
	/* Fixed at the GL defaults: the entry points that would change them
	 * (glDepthRangef, glPointSize, glLineWidth, glPolygonOffset) are still
	 * stubs, so answering the default is the honest answer — and when one of
	 * them lands, this is where its state gets read back from. */
	case GL_DEPTH_RANGE:       out[0] = 0.0f; out[1] = 1.0f; return 2;
	case GL_POINT_SIZE:        out[0] = 1.0f; return 1;
	case GL_LINE_WIDTH:        out[0] = 1.0f; return 1;
	case GL_POLYGON_OFFSET_UNITS:
	case GL_POLYGON_OFFSET_FACTOR: out[0] = 0.0f; return 1;
	default: return 0;
	}
}

void glGetFloatv(GLenum p, GLfloat *v)
{
	float f[16];
	int n, i;
	if (!v) return;
	tr2("glGetFloatv pname", (int)p, 0);
	n = state_floats(p, f);
	if (n) { for (i = 0; i < n; i++) v[i] = f[i]; return; }
	{ GLint iv[4] = { 0, 0, 0, 0 };
	  glGetIntegerv(p, iv);
	  for (i = 0; i < 4; i++) v[i] = (GLfloat)iv[i]; }
}

void glGetFixedv(GLenum p, GLfixed *v)
{
	float f[16];
	int n, i;
	if (!v) return;
	/* THIS ENTRY POINT WAS THE PLAYER-CHARACTER BUG. Clam Prix reads the
	 * modelview matrix back 7210 times per race, and while this was a stub it
	 * never touched the caller's buffer — so the title built its bone
	 * transforms out of whatever was on its own stack, and the skinned
	 * character drew at nonsense coordinates while the unskinned kart beside it
	 * was fine. Proven by re-stubbing it for one run: SpongeBob vanishes and
	 * comes back. See HANDOVER. */
	tr2("glGetFixedv pname", (int)p, 0);
	n = state_floats(p, f);
	if (n) { for (i = 0; i < n; i++) v[i] = f2fx(f[i]); return; }
	/* INTEGER-TYPED STATE IS NOT SCALED. GLES 1.1 §6.1.2: a value that is
	 * already an integer — a limit, a binding, a bit count — is returned as-is
	 * by glGetFixedv, NOT converted to 16.16. Multiplying MAX_TEXTURE_SIZE by
	 * 65536 would hand the caller 268435456. */
	{ GLint iv[4] = { 0, 0, 0, 0 };
	  glGetIntegerv(p, iv);
	  for (i = 0; i < 4; i++) v[i] = (GLfixed)iv[i]; }
}

void glGetBooleanv(GLenum p, GLboolean *v)
{
	float f[16];
	int n, i;
	if (!v) return;
	tr2("glGetBooleanv pname", (int)p, 0);
	n = state_floats(p, f);
	if (n) { for (i = 0; i < n; i++) v[i] = f[i] != 0.0f; return; }
	/* A CAPABILITY reads through glIsEnabled, which already answers honestly
	 * from the state we track — and answering "not enabled" to everything is
	 * the specific bug that covered the credits screen with a white quad.
	 * cap_valid() is what keeps this from swallowing every other pname: without
	 * it, glGetBooleanv(GL_MAX_LIGHTS) would take the capability path and log a
	 * spurious "UNHANDLED cap" for something that was never a cap. */
	if (cap_valid(p)) { v[0] = glIsEnabled(p) ? 1 : 0; return; }
	{ GLint iv[4] = { 0, 0, 0, 0 };
	  glGetIntegerv(p, iv);
	  for (i = 0; i < 4; i++) v[i] = iv[i] != 0; }
}

GLboolean glIsEnabled(GLenum c)
{
	switch (c) {
	case GL_TEXTURE_2D:          return g_tex2d_unit[g_active_tex] ? 1 : 0;
	case GL_BLEND:               return g_blend_on ? 1 : 0;
	case GL_DEPTH_TEST:          return g_depth_test_on ? 1 : 0;
	case GL_ALPHA_TEST:          return g_alpha_test_on ? 1 : 0;
	case GL_VERTEX_ARRAY:        return g_vtx.on ? 1 : 0;
	case GL_COLOR_ARRAY:         return g_col.on ? 1 : 0;
	case GL_TEXTURE_COORD_ARRAY: return g_tex.on ? 1 : 0;
	case GL_NORMAL_ARRAY:        return 0;
	case GL_CULL_FACE:           return 0;   /* we never cull */
	default:
		tr2("glIsEnabled UNHANDLED cap", (int)c, 0);
		return 0;
	}
}
GLboolean glIsTexture(GLuint t) { (void)t; return 0; }
GLboolean glIsBuffer (GLuint b) { (void)b; return 0; }

/* ---- symbols the rest of the stock GPU stack links against -----------------
 *
 * The stock libGLESv1_CM.so.1.1 is a 1.9 MB blob exporting 697 symbols, not
 * just the 180 GL entry points. Replacing it wholesale removed four that other
 * pieces of the VR5 driver import, and the dynamic loader then refused to start
 * AppManager at all:
 *
 *     symbol 'DataConvert_to_enumv': can't resolve symbol
 *
 * Importers, checked across every binary in the guest:
 *     DataConvert_to_enumv, DataConvert_to_intv  <- libvr5.so ONLY
 *     g_shader_es1_fs, g_shader_es1_vs           <- libGLES.so ONLY
 *
 * Both of those are parts of the GPU stack this shim supersedes: libvr5 exports
 * no GL or EGL entry points of its own, so with our definitions winning it is
 * unreachable dead code. The symbols therefore only need to RESOLVE, not work.
 *
 * Chaining to the real library instead was tried and abandoned: it drags in 101
 * more unmet symbols (EGL::FrameBuffer, EGL::TextureLevel, EGL::ProgramContainer
 * ...) because libGLESv1_CM, libEGL and libvr5 share C++ internals. Satisfying
 * those would mean shipping the whole GPU driver, which is the thing we cannot
 * run.
 *
 * Sizes match the originals so anything reading them sees a sane extent.
 */
void DataConvert_to_enumv(void) { }
void DataConvert_to_intv(void)  { }
char g_shader_es1_fs[512];
char g_shader_es1_vs[4096];

/* ---- context teardown --------------------------------------------------- */
/* Placed at the end of the file deliberately: it touches nearly every static in
 * it, and C wants them all declared first.
 *
 * AppManager does NOT exit between games. It dlopen()s the title's App.so, runs
 * it, and UnloadModule()s it, so everything static here is process-lifetime and
 * used to survive into the next title. The consequences compounded rather than
 * merely lingering:
 *
 *   * glGenTextures names a texture after its slot index and scans for a free
 *     one. With all 192 slots still held by the previous game it found none,
 *     returned name 0, and the new title drew untextured — the "melting", and
 *     worse with every title launched.
 *   * A recycled name that DID get a slot still had the previous title's pixels
 *     mirrored on the host, so it drew the wrong image.
 *   * Every draw against a texture the host held no image for set want_resync,
 *     and hle_sync_state() can only resend textures the guest still has a
 *     decoded copy of — so the request could never be satisfied and the host
 *     asked again on the next draw, forever. That is the "host asked for a
 *     state resync" flood.
 *
 * Real hardware tears the context down inside its driver, which is why the
 * device never logs Brio's complaint and Tadpole always did:
 *     ExitPopUnloadApp: OGL context still active after unloading
 */
void tad_gl_context_reset(void)
{
	int i;

	for (i = 0; i < MAX_TEXS; i++) {
		if (g_texs[i].argb) free(g_texs[i].argb);
		g_texs[i].argb = NULL;
		g_texs[i].name = 0;
		g_texs[i].w = g_texs[i].h = 0;
	}
	for (i = 0; i < MAX_BUFS; i++) {
		if (g_bufs[i].data) free(g_bufs[i].data);
		g_bufs[i].data = NULL;
		g_bufs[i].name = 0;
		g_bufs[i].size = 0;
	}

	g_bound_tex   = 0;
	g_bound_array = 0;
	g_bound_elem  = 0;
	g_vtx.on  = g_col.on  = g_tex.on  = g_nrm.on  = 0;
	g_vtx.ptr = g_col.ptr = g_tex.ptr = g_nrm.ptr = 0;
	g_vtx.buf = g_col.buf = g_tex.buf = g_nrm.buf = 0;
	g_cur_color = 0xFFFFFFFFu;
	g_tex_gen_fail = 0;
	/* TexEnv is per-unit, not per-object, so clearing the texture table does
	 * not touch it — the next title would inherit the previous one's combiner
	 * and env colour. Texture sampler state needs nothing here: tex_slot()
	 * re-applies the defaults when it reuses a freed slot. */
	g_texenv_init = 0;
	g_tex_default_init = 0;
	g_tex_env = GL_MODULATE;
	/* The bone palette is per-title too. Carrying the previous game's poses
	 * into the next one is the same cross-title leak as the texture mirrors,
	 * and on a title that loads only some slots it would skin with another
	 * game's skeleton. */
	g_palette_inited = 0;
	{ int k; for (k = 0; k < MAX_PALETTE; k++) g_palette_set[k] = 0; }
	g_palette_loads = 0;
	g_cur_palette = 0;

	/* Drop the host's mirrors too, then force a fresh sync — otherwise the
	 * host keeps the old title's images under names the next one reuses. */
	if (hle_on()) hle_reset();
	g_hle_synced = 0;

	/* SAY WHAT THIS TITLE ASKED FOR AND DID NOT GET, then start a fresh tally.
	 * This is the natural boundary: AppManager dlopen()s a title, runs it and
	 * unloads it without the process ever exiting, so a session-long table would
	 * merge a dozen titles into one row set and lose the only thing that makes
	 * it actionable — WHICH title needs which entry point. */
	tad_gl_report("title unloaded");
	tad_gl_report_reset();
}
