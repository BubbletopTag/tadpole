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
 * Unimplemented entry points are no-op stubs in tadpole_gles_stubs.c so the
 * guest never hits an unresolved symbol.
 */

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

/* TADPOLE_GL_DEBUG=1 traces the call sequence to stderr. Essential here: the
 * only way to know what the UI actually asks for is to watch it ask. */
static int g_trace = -1;
static void tr(const char *msg)
{
	u32 n = 0;
	if (g_trace < 0)
		g_trace = getenv("TADPOLE_GL_DEBUG") ? 1 : 0;
	if (!g_trace)
		return;
	while (msg[n]) n++;
	write(2, "[gl] ", 5);
	write(2, msg, n);
	write(2, "\n", 1);
}
/* ALWAYS printed, unlike tr()/tr2(). Reserved for conditions that mean the
 * output is definitely wrong — a texture with no data, a format we cannot
 * decode, a name table that ran out. Each is one-shot or near enough, so this
 * costs nothing in a healthy run, and it means a user's ordinary log already
 * contains the evidence instead of needing TADPOLE_GL_DEBUG=1 and a second
 * attempt at reproducing. */
static void warn2(const char *msg, int a, int b)
{
	char buf[160];
	u32 n = 0;
	snprintf(buf, sizeof(buf), "[gl] WARN %s %d %d\n", msg, a, b);
	while (buf[n]) n++;
	write(2, buf, n);

	/* ALSO to a file, because stderr is easy to lose. A log captured with
	 * `> log` and no `2>&1` contains Brio's stdout traces and none of ours,
	 * which wasted a whole round trip diagnosing white textures. This file is
	 * always there to be read afterwards. */
	{
		static int fd = -2;
		if (fd == -2) {
			char path[512];
			const char *d = getenv("TADPOLE_DIR");
			if (!d) d = "/tmp/tadpole";
			snprintf(path, sizeof(path), "%s/gl-warnings.log", d);
			fd = open(path, 01 | 0100 | 02000, 0666);   /* WRONLY|CREAT|APPEND */
		}
		if (fd >= 0) write(fd, buf, n);
	}
}

static void tr2(const char *msg, int a, int b)
{
	char buf[128];
	if (g_trace < 0)
		g_trace = getenv("TADPOLE_GL_DEBUG") ? 1 : 0;
	if (!g_trace)
		return;
	snprintf(buf, sizeof(buf), "[gl] %s %d %d\n", msg, a, b);
	{ u32 n = 0; while (buf[n]) n++; write(2, buf, n); }
}

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
#define GL_MATRIX_INDEX_ARRAY_OES 0x8844
#define GL_WEIGHT_ARRAY_OES       0x86AD
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_TEXTURE_         0x1702
#define GL_VERTEX_ARRAY     0x8074
#define GL_COLOR_ARRAY      0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
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

	if (!g_trace) return;
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

/* TADPOLE_GL_HLE=1 selects host-GPU replay. The transport and encoder are not
 * built yet, so say so once and keep rasterising in software rather than
 * silently doing nothing — a toggle that appears to work but changes nothing is
 * worse than one that admits it. Feasibility is already proven: see
 * viewer/hle_probe.c and the HLE section of HANDOVER. */
static void hle_notice(void)
{
	static int said;
	if (said || !getenv("TADPOLE_GL_HLE")) return;
	said = 1;
	warn2("TADPOLE_GL_HLE requested but the encoder is NOT built yet;"
	      " still rasterising in software", 0, 0);
}

void tadpole_gl_present(void)
{
	hle_notice();
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

	if (g_trace) {
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
	if (g_trace && g_f_flat_area) {
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
	if (g_trace)
		tr2("FRAME draws/tris-in", g_f_draws, g_f_tris_in);
	if (g_trace)
		tr2("FRAME tris-on-screen/pixels", g_f_tris_out, g_f_pixels);
	if (g_trace && !g_f_pixels)
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

/* ---- matrix stack -------------------------------------------------------- */

typedef struct { float m[16]; } mat4;
#define STACK_DEPTH 16

static mat4 g_mv[STACK_DEPTH], g_proj[STACK_DEPTH];
/* A THIRD STACK, for GL_TEXTURE (0x1702). Without it that mode fell through to
 * the modelview stack — texture-matrix edits silently corrupted geometry
 * transforms, and the guest could never mirror what the host was doing. */
static mat4 g_texm[STACK_DEPTH];
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

void glMatrixMode(GLenum mode) { tr2("glMatrixMode", (int)mode, 0); g_matrix_mode = mode;
  if (hle_ready()) hle_matrixmode(mode); }
void glLoadIdentity(void)      { mat_identity(cur_top());
  if (hle_ready()) hle_loadidentity(); }

static int g_push_drop, g_pop_under, g_max_mv, g_max_pj;

void glPushMatrix(void)
{
	mat_init_once();
	if (g_matrix_mode == GL_PROJECTION) {
		if (g_proj_sp + 1 < STACK_DEPTH) { g_proj[g_proj_sp+1] = g_proj[g_proj_sp]; g_proj_sp++; }
		else g_push_drop++;
	} else if (g_matrix_mode == GL_TEXTURE_) {
		if (g_texm_sp + 1 < STACK_DEPTH) { g_texm[g_texm_sp+1] = g_texm[g_texm_sp]; g_texm_sp++; }
		else g_push_drop++;
	} else {
		if (g_mv_sp + 1 < STACK_DEPTH) { g_mv[g_mv_sp+1] = g_mv[g_mv_sp]; g_mv_sp++; }
		else g_push_drop++;
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
		if (g_proj_sp > 0) g_proj_sp--; else g_pop_under++;
	} else if (g_matrix_mode == GL_TEXTURE_) {
		if (g_texm_sp > 0) g_texm_sp--; else g_pop_under++;
	} else {
		if (g_mv_sp > 0) g_mv_sp--; else g_pop_under++;
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

void glTexEnvx(GLenum tgt, GLenum pname, GLint v)
{
	(void)tgt;
	/* Only GL_TEXTURE_ENV_MODE matters to us, and only when the value is a
	 * mode we recognise — the game also passes values that are not valid
	 * modes, which we must not latch onto. */
	if (pname == 0x2200 && (v == (GLint)GL_MODULATE || v == (GLint)GL_REPLACE))
		g_tex_env = (GLenum)v;
	if (hle_ready()) hle_texenv(tgt, pname, v);
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
struct gl_texture { GLuint name; u32 w, h; u32 *argb; };
static struct gl_texture g_texs[MAX_TEXS];
static GLuint g_bound_tex;
static int    g_blend_on;

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
	if (!t->name) { t->name = n; t->argb = NULL; t->w = t->h = 0; }
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
static struct array g_vtx, g_col, g_tex;
/* Declared here with the other client arrays rather than beside the skinning
 * code, because glEnableClientState switches them on well before that point. */
static struct array g_midx, g_wgt;      /* matrix indices, blend weights */
static u32 g_cur_color = 0xFFFFFFFFu;

void glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ tr2("glVertexPointer size/type", s, (int)t); tr2("glVertexPointer stride/off", (int)st, (int)(unsigned long)p); tr2("glVertexPointer boundbuf", (int)g_bound_array, 0); g_vtx.buf = g_bound_array; g_vtx.size=s; g_vtx.type=t; g_vtx.stride=st; g_vtx.ptr=p;
  if (hle_ready()) hle_arraypointer(0, g_bound_array, s, t, st, (u32)(unsigned long)p); }
void glColorPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ g_col.buf = g_bound_array; g_col.size=s; g_col.type=t; g_col.stride=st; g_col.ptr=p;
  if (hle_ready()) hle_arraypointer(1, g_bound_array, s, t, st, (u32)(unsigned long)p); }
void glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p)
{ tr2("glTexCoordPointer size/type", s, (int)t); tr2("glTexCoordPointer stride/off", (int)st, (int)(unsigned long)p); g_tex.buf = g_bound_array; g_tex.size=s; g_tex.type=t; g_tex.stride=st; g_tex.ptr=p;
  if (hle_ready()) hle_arraypointer(2, g_bound_array, s, t, st, (u32)(unsigned long)p); }

void glEnableClientState(GLenum a)
{ tr2("glEnableClientState", (int)a, 0); if (a==GL_VERTEX_ARRAY) g_vtx.on=1; else if (a==GL_COLOR_ARRAY) g_col.on=1;
  else if (a==GL_TEXTURE_COORD_ARRAY) g_tex.on=1;
  /* Consumed here, never forwarded: the host has no such client arrays. */
  else if (a==GL_MATRIX_INDEX_ARRAY_OES) { g_midx.on = 1; return; }
  else if (a==GL_WEIGHT_ARRAY_OES)       { g_wgt.on  = 1; return; }
  if (hle_ready()) hle_clientstate(a==GL_VERTEX_ARRAY ? 0u :
                                a==GL_COLOR_ARRAY  ? 1u :
                                a==GL_TEXTURE_COORD_ARRAY ? 2u : 3u, 1); }
void glDisableClientState(GLenum a)
{ if (a==GL_VERTEX_ARRAY) g_vtx.on=0; else if (a==GL_COLOR_ARRAY) g_col.on=0;
  else if (a==GL_TEXTURE_COORD_ARRAY) g_tex.on=0;
  if (hle_ready()) hle_clientstate(a==GL_VERTEX_ARRAY ? 0u :
                                a==GL_COLOR_ARRAY  ? 1u :
                                a==GL_TEXTURE_COORD_ARRAY ? 2u : 3u, 0); }

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
		if (g_trace && g_f_untex_tris < 4) {
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
	if (g_trace && g_tri_logged < 12) {
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
	if (g_trace && count >= 100) {
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
typedef char hle_client_names_above_maxbufs[
	(HLE_CLIENT_VTX > MAX_BUFS) ? 1 : -1];

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
static mat4  g_palette[MAX_PALETTE];
static u32   g_cur_palette;
static int   g_palette_on;

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
void glLoadPaletteFromModelViewMatrixOES(void)
{ tr2("glLoadPaletteFromModelViewMatrixOES", (int)g_cur_palette, 0);
  if (g_cur_palette < MAX_PALETTE) g_palette[g_cur_palette] = g_mv[g_mv_sp]; }

/* Read one component of a client array as float, whatever it is stored as. */
static float arr_get(const struct array *a, u32 vertex, int comp)
{
	u32 stride = a->stride ? (u32)a->stride : elem_size(a->type, a->size);
	const u8 *q = a->ptr + (unsigned long)vertex * stride;
	switch (a->type) {
	case GL_FLOAT:         return ((const float *)q)[comp];
	case GL_FIXED:         return fx2f(((const GLfixed *)q)[comp]);
	case GL_UNSIGNED_BYTE: return (float)q[comp];
	case GL_BYTE:          return (float)((const signed char *)q)[comp];
	case GL_SHORT:         return (float)((const short *)q)[comp];
	default:               return 0.0f;
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

	if (!g_palette_on || !g_vtx.on || !g_vtx.ptr) return 0;
	if (!g_midx.ptr || !g_wgt.ptr) {
		/* Palette on but no bone data: say so ONCE. Silence here would look
		 * identical to skinning working, and the character would still be
		 * missing with nothing to explain why. */
		static int said;
		if (g_palette_on && !said) { said = 1;
			warn2("palette ON but index/weight arrays missing (idx,wgt set?)",
			      g_midx.ptr ? 1 : 0, g_wgt.ptr ? 1 : 0); }
		return 0;
	}
	if (!nverts) return 0;
	{ static int said; if (!said) { said = 1;
		warn2("SKINNING a draw: bones, verts",
		      g_midx.size < g_wgt.size ? g_midx.size : g_wgt.size, (int)nverts); } }

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
		pos[0] = arr_get(&g_vtx, i, 0);
		pos[1] = arr_get(&g_vtx, i, 1);
		pos[2] = g_vtx.size > 2 ? arr_get(&g_vtx, i, 2) : 0.0f;
		pos[3] = 1.0f;
		for (k = 0; k < nb; k++) {
			float w = arr_get(&g_wgt, i, k), t[4];
			u32 mi;
			if (w == 0.0f) continue;      /* the common case: 1-2 live bones */
			mi = (u32)arr_get(&g_midx, i, k);
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
	/* The one place HLE REPLACES work rather than adding to it: the whole point
	 * is not to rasterise here. */
	if (hle_ready()) {
		u32 nv = (u32)first + (u32)count;
		int sk = skin_begin(nv);
		if (!sk) hle_send_array(&g_vtx, 0, nv, HLE_CLIENT_VTX);
		hle_send_array(&g_col, 1, nv, HLE_CLIENT_COL);
		hle_send_array(&g_tex, 2, nv, HLE_CLIENT_TEX);
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

void glEnable(GLenum cap)
{ tr2("glEnable cap", (int)cap, 0);
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
void glTexParameterx(GLenum t, GLenum p, GLint v) { (void)t; tr2("glTexParameterx pname/val", (int)p, (int)v);
  if (hle_ready()) hle_texparam(p, v); }
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

/* Integer form of glTexParameterx. Same state, different argument type — the
 * stub meant wrap and filter modes set through the int entry point were lost. */
void glTexParameteri(GLenum t, GLenum p, GLint v)
{
	(void)t; tr2("glTexParameteri pname/val", (int)p, (int)v);
	if (hle_ready()) hle_texparam(p, v);
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
		}
	}
}

GLenum glGetError(void) { return 0; }              /* GL_NO_ERROR */
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
#define GL_MAX_LIGHTS                    0x0D31
#define GL_NORMAL_ARRAY                  0x8075
#define GL_CULL_FACE                     0x0B44

void glGetIntegerv(GLenum p, GLint *v)
{
	if (!v) return;
	switch (p) {
	case GL_MAX_TEXTURE_SIZE:             *v = 1024; break;
	case GL_MAX_TEXTURE_UNITS:            *v = MAX_TEXUNITS; break;
	case GL_TEXTURE_BINDING_2D:           *v = (GLint)g_bound_tex; break;
	case GL_ARRAY_BUFFER_BINDING:         *v = (GLint)g_bound_array; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = (GLint)g_bound_elem; break;
	case GL_MAX_MODELVIEW_STACK_DEPTH:    *v = 16; break;
	case GL_MAX_PROJECTION_STACK_DEPTH:   *v = 16; break;
	case GL_MAX_LIGHTS:                   *v = 8; break;
	case GL_DEPTH_BITS:                   *v = 16; break;
	case GL_RED_BITS: case GL_GREEN_BITS:
	case GL_BLUE_BITS: case GL_ALPHA_BITS: *v = 8; break;
	case GL_VIEWPORT:
		view_init();
		v[0] = g_vx; v[1] = g_vy; v[2] = g_vw; v[3] = g_vh;
		break;
	default:
		tr2("glGetIntegerv UNHANDLED pname", (int)p, 0);
		*v = 0;
		break;
	}
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
	g_vtx.on  = g_col.on  = g_tex.on  = 0;
	g_vtx.ptr = g_col.ptr = g_tex.ptr = 0;
	g_vtx.buf = g_col.buf = g_tex.buf = 0;
	g_cur_color = 0xFFFFFFFFu;
	g_tex_gen_fail = 0;

	/* Drop the host's mirrors too, then force a fresh sync — otherwise the
	 * host keeps the old title's images under names the next one reuses. */
	if (hle_on()) hle_reset();
	g_hle_synced = 0;
}
