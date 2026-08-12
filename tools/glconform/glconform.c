/* Tadpole — GLES 1.x differential conformance probe.
 *
 * ONE ARM BINARY, TWO IMPLEMENTATIONS. It links the device's own library
 * FILENAMES (libopengles_lite.so, libEGL.so), which exist on both sides, so
 * which GL it actually gets is decided at load time by LD_LIBRARY_PATH:
 *
 *     tools/glconform/run-emu.sh          -> runtime/shimlibs-gl, our shim
 *     tools/glconform/run-hw.py 192.168.0.111  -> /usr/lib, the real VR5 driver
 *
 * A divergence between the two logs is a candidate bug rather than a guess.
 * That is the entire point: every fix in this project so far was found by
 * running something and diffing it against hardware, never by reading the GLES
 * spec, because the spec cannot tell you which of OUR 2800 lines is wrong.
 *
 * WHY IT IS FREESTANDING. There is no ARM crt1.o in the guest lib set and no
 * published SDK for building a signed Brio title, so this is the same shape as
 * shim/tone_test.c: its own _start, -nostdlib, linked against the real
 * libc.so.0 for printf/exit. No packaging, no AppManager, no signing.
 *
 * NO FLOATS EVER REACH printf. The guest ABI is softfp and a float passed
 * through varargs is promoted to double in core register pairs; getting that
 * subtly wrong would corrupt the log rather than fail loudly. Every measured
 * value is printed as an integer in thousandths.
 *
 * Log format, one line per event, stable and greppable:
 *
 *     META   key=value key=value ...
 *     EGLINIT OK|FAIL detail="..."
 *     RESULT <test-name> OK|FAIL|SKIP err=0x<hex> detail="..."
 *
 * A run that stops partway simply has no more RESULT lines after the last one
 * printed; diff-conform.py treats a missing test as its own category rather
 * than as a silent pass.
 *
 * DO NOT RUN THIS UNDER TADPOLE_GL_DEBUG=2. Several tests raise GL errors and
 * call unimplemented entry points ON PURPOSE, and level 2 turns the first of
 * those into a deliberate abort. Level 0 or 1 is what you want here — the
 * point of this binary is to RECORD those, not to stop at them.
 */

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed int     GLint;
typedef signed int     GLsizei;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef int            GLfixed;
typedef void           GLvoid;

#define NULL ((void *)0)

/* ---- libc (the real one, chained exactly like tone_test.c) -------------- */
extern int  printf(const char *fmt, ...);
extern int  snprintf(char *buf, unsigned long size, const char *fmt, ...);
extern void exit(int code);
extern void *memset(void *s, int c, unsigned long n);

/* ---- EGL --------------------------------------------------------------- */
extern void *eglGetDisplay(void *native_display);
extern GLuint eglInitialize(void *dpy, GLint *major, GLint *minor);
extern GLuint eglChooseConfig(void *dpy, const GLint *attrib, void **configs,
                              GLint config_size, GLint *num_config);
extern void *eglCreateWindowSurface(void *dpy, void *config, void *win,
                                    const GLint *attrs);
extern void *eglCreateContext(void *dpy, void *config, void *share,
                              const GLint *attrs);
extern GLuint eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
extern GLuint eglSwapBuffers(void *dpy, void *surf);
extern GLuint eglGetError(void);
extern const char *eglQueryString(void *dpy, GLint name);

/* ---- GLES 1.1 ---------------------------------------------------------- */
extern GLenum glGetError(void);
extern const unsigned char *glGetString(GLenum name);
extern void glEnable(GLenum cap);
extern void glDisable(GLenum cap);
extern void glClear(GLbitfield mask);
extern void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
extern void glMatrixMode(GLenum mode);
extern void glLoadIdentity(void);
extern void glPushMatrix(void);
extern void glPopMatrix(void);
extern void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
extern void glFlush(void);
extern void glFinish(void);
extern void glHint(GLenum target, GLenum mode);
extern void glPixelStorei(GLenum pname, GLint param);
extern void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
extern void glGetIntegerv(GLenum pname, GLint *params);
extern void glGetFloatv(GLenum pname, GLfloat *params);
extern void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
                         GLenum format, GLenum type, GLvoid *pixels);
extern void glBindTexture(GLenum target, GLuint name);
extern void glGenTextures(GLsizei n, GLuint *names);
extern void glTexParameteri(GLenum target, GLenum pname, GLint param);
extern void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);

/* lighting / material / fog — the biggest stub cluster (22 of 108) */
extern void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
extern void glGetLightfv(GLenum light, GLenum pname, GLfloat *params);
extern void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
extern void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params);
extern void glFogf(GLenum pname, GLfloat param);
extern void glFogfv(GLenum pname, const GLfloat *params);

/* texenv (15 of 108) */
extern void glTexEnvi(GLenum target, GLenum pname, GLint param);
extern void glGetTexEnviv(GLenum target, GLenum pname, GLint *params);

/* blending */
extern void glBlendFunc(GLenum sfactor, GLenum dfactor);

/* client arrays — per-unit texcoord state (see t_multitex_arrays) */
extern void glClientActiveTexture(GLenum texture);
extern void glEnableClientState(GLenum array);
extern void glDisableClientState(GLenum array);
extern GLboolean glIsEnabled(GLenum cap);

/* point/line/polygon/stencil/clipplane (23 of 108) */
extern void glPointSize(GLfloat size);
extern void glLineWidth(GLfloat width);
extern void glPolygonOffset(GLfloat factor, GLfloat units);
extern void glStencilFunc(GLenum func, GLint ref, GLuint mask);
extern void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
extern void glClipPlanef(GLenum plane, const GLfloat *eqn);
extern void glGetClipPlanef(GLenum plane, GLfloat *eqn);

/* ---- enums (public Khronos values) ------------------------------------- */
#define GL_NO_ERROR                     0x0000
#define GL_INVALID_ENUM                 0x0500
#define GL_INVALID_VALUE                0x0501
#define GL_STACK_OVERFLOW               0x0503
#define GL_DEPTH_BUFFER_BIT             0x0100
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_VENDOR                       0x1F00
#define GL_RENDERER                     0x1F01
#define GL_VERSION                      0x1F02
#define GL_MODELVIEW                    0x1700
#define GL_PROJECTION                   0x1701
#define GL_MODELVIEW_MATRIX             0x0BA6
#define GL_LIGHTING                     0x0B50
#define GL_LIGHT0                       0x4000
#define GL_AMBIENT                      0x1200
#define GL_DIFFUSE                      0x1201
#define GL_POSITION                     0x1203
#define GL_FRONT_AND_BACK               0x0408
#define GL_FOG                          0x0B60
#define GL_FOG_DENSITY                  0x0B62
#define GL_FOG_COLOR                    0x0B66
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE_ENV                  0x2300
#define GL_TEXTURE_ENV_MODE             0x2200
#define GL_DECAL                        0x2101
#define GL_BLEND_DST                    0x0BE0
#define GL_BLEND_SRC                    0x0BE1
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_NEAREST                      0x2600
#define GL_STENCIL_TEST                 0x0B90
#define GL_KEEP                         0x1E00
#define GL_ALWAYS                       0x0207
#define GL_CLIP_PLANE0                  0x3000
#define GL_TEXTURE0                     0x84C0
#define GL_TEXTURE1                     0x84C1
#define GL_TEXTURE_COORD_ARRAY          0x8078
#define GL_DONT_CARE                    0x1100
#define GL_PERSPECTIVE_CORRECTION_HINT  0x0C50
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_RGBA                         0x1908
#define GL_UNSIGNED_BYTE                0x1401
#define GL_MAX_LIGHTS                   0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH    0x0D36
#define GL_MAX_TEXTURE_SIZE             0x0D33
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS     0x86A3

#define EGL_NONE   0x3038
#define EGL_VENDOR 0x3053

/* ================================================================== */

static int g_ok, g_fail, g_skip;

static float f_abs(float v) { return v < 0.0f ? -v : v; }

/* Thousandths, as an int. See the header: no float reaches printf. */
static int milli(float v) { return (int)(v * 1000.0f + (v < 0 ? -0.5f : 0.5f)); }

/* GL errors are sticky and accumulate, so a well-formed check drains whatever
 * was already pending and reports the FIRST code — that is the one attributable
 * to the call just made. Bounded, because an implementation stuck returning
 * nonzero forever must not hang the harness. */
static GLenum drain(void)
{
	GLenum first = GL_NO_ERROR, e;
	int i;
	for (i = 0; i < 8; i++) {
		e = glGetError();
		if (e == GL_NO_ERROR) break;
		if (first == GL_NO_ERROR) first = e;
	}
	return first;
}

static void result(const char *name, int pass, const char *detail)
{
	printf("RESULT %s %s err=0x%04x detail=\"%s\"\n", name,
	       pass ? "OK" : "FAIL", (unsigned)drain(), detail);
	if (pass) g_ok++; else g_fail++;
}

static void skip(const char *name, const char *why)
{
	printf("RESULT %s SKIP err=0x0000 detail=\"%s\"\n", name, why);
	g_skip++;
}

/* A 4-float set/get round trip, the shape most of these tests share. */
static int vec4_close(const GLfloat *a, const GLfloat *b)
{
	int i;
	for (i = 0; i < 4; i++)
		if (f_abs(a[i] - b[i]) > 0.01f) return 0;
	return 1;
}

static void report_vec4(const char *name, const GLfloat *want, const GLfloat *got)
{
	int ok = vec4_close(want, got);
	printf("RESULT %s %s err=0x%04x detail=\"want %d,%d,%d,%d got %d,%d,%d,%d"
	       " (thousandths)\"\n",
	       name, ok ? "OK" : "FAIL", (unsigned)drain(),
	       milli(want[0]), milli(want[1]), milli(want[2]), milli(want[3]),
	       milli(got[0]), milli(got[1]), milli(got[2]), milli(got[3]));
	if (ok) g_ok++; else g_fail++;
}

/* ---- tests ------------------------------------------------------------- */

/* READ THIS ONE FIRST IN ANY LOG. Nothing else in this file does anything
 * invalid on purpose, so a side that does not raise GL_INVALID_ENUM here cannot
 * be trusted about any other err= code in its log. This is exactly the test
 * that used to fail on our side, back when glGetError was `return 0;`. */
static void t_error_tracking(void)
{
	GLenum e;
	drain();
	glEnable((GLenum)0xBEEF);
	e = glGetError();
	if (e == GL_INVALID_ENUM) {
		result("selfcheck.error_tracking", 1, "raised GL_INVALID_ENUM as expected");
	} else {
		printf("RESULT selfcheck.error_tracking FAIL err=0x%04x detail=\"expected"
		       " 0x0500; error state is not trustworthy on this side for the rest"
		       " of this log\"\n", (unsigned)e);
		g_fail++;
	}
}

/* Errors are sticky: the FIRST since the last read is what glGetError returns,
 * and reading clears it. An implementation that returns the LATEST error is
 * reporting the one furthest from the cause. */
static void t_error_sticky(void)
{
	GLenum first, second;
	drain();
	glEnable((GLenum)0xBEE1);
	glEnable((GLenum)0xBEE2);
	first = glGetError();
	second = glGetError();
	result("selfcheck.error_sticky",
	       first == GL_INVALID_ENUM && second == GL_NO_ERROR,
	       first != GL_INVALID_ENUM ? "first read was not GL_INVALID_ENUM"
	       : second != GL_NO_ERROR ? "second read did not clear the error"
	       : "first read reports, second reads clean");
}

static void t_light_ambient(void)
{
	GLfloat want[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	GLfloat got[4]  = { -1, -1, -1, -1 };
	drain();
	glLightfv(GL_LIGHT0, GL_AMBIENT, want);
	glGetLightfv(GL_LIGHT0, GL_AMBIENT, got);
	report_vec4("light.ambient_roundtrip", want, got);
}

static void t_light_position(void)
{
	GLfloat want[4] = { 1.0f, 2.0f, 3.0f, 0.0f };   /* directional */
	GLfloat got[4]  = { -9, -9, -9, -9 };
	drain();
	glLightfv(GL_LIGHT0, GL_POSITION, want);
	glGetLightfv(GL_LIGHT0, GL_POSITION, got);
	report_vec4("light.position_roundtrip", want, got);
}

static void t_material_diffuse(void)
{
	GLfloat want[4] = { 0.9f, 0.1f, 0.4f, 1.0f };
	GLfloat got[4]  = { -1, -1, -1, -1 };
	drain();
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, want);
	glGetMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, got);
	report_vec4("material.diffuse_roundtrip", want, got);
}

static void t_fog(void)
{
	GLfloat col[4] = { 0.2f, 0.2f, 0.3f, 1.0f };
	drain();
	glFogf(GL_FOG_DENSITY, 0.35f);
	glFogfv(GL_FOG_COLOR, col);
	result("fog.set_no_error", 1,
	       "set only — GLES1 has no getter for fog state; the err code is the test");
}

static void t_texenv(void)
{
	GLint got = -1;
	drain();
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
	glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &got);
	printf("RESULT texenv.decal_roundtrip %s err=0x%04x detail=\"want 0x2101"
	       " got 0x%04x\"\n", got == GL_DECAL ? "OK" : "FAIL",
	       (unsigned)drain(), (unsigned)got);
	if (got == GL_DECAL) g_ok++; else g_fail++;
}

/* THE PAIR THAT DREW BEN 10'S MENU TEXT IN BLACK.
 *
 * Same save/restore shape as t_texenv above, and the same cost when the getter
 * does not answer — except that this one is invisible until a host GPU is
 * doing the blending. Traced out of Ben 10: Ultimate Alien, one frame of its
 * menu:
 *
 *     glGetTexEnvxv(GL_TEXTURE_ENV_MODE)   // saved, and we DO answer this
 *     glGetIntegerv(GL_BLEND_DST, &dst)    // saved
 *     glGetIntegerv(GL_BLEND_SRC, &src)    // saved
 *     glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
 *     ... draw the text ...
 *     glTexEnvx(GL_TEXTURE_ENV_MODE, GL_MODULATE)
 *     glEnable(GL_BLEND)
 *     glBlendFunc(src, dst)                // restore what it read back
 *
 * Unhandled, both queries wrote 0, so the restore was glBlendFunc(GL_ZERO,
 * GL_ZERO) — 1227 of them against 444 correct ones in a single run — and every
 * blended draw afterwards multiplied to black. The software rasteriser ignores
 * the factors and hardcodes src-alpha-over, so it showed nothing; the HLE path
 * forwards them to the host glBlendFunc, so it showed everything. A rendering
 * bug that only exists on the fast path is exactly the kind this binary is for.
 *
 * The initial values are worth a second line on hardware: GLES 1.1 §4.1.7 says
 * GL_ONE / GL_ZERO, and that is what our shim now starts from, but only the
 * device can confirm the VR5 agrees. */
static void t_blendfunc(void)
{
	GLint src = -1, dst = -1;
	drain();
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glGetIntegerv(GL_BLEND_SRC, &src);
	glGetIntegerv(GL_BLEND_DST, &dst);
	printf("RESULT blend.func_roundtrip %s err=0x%04x detail=\"want 0x0302,0x0303"
	       " got 0x%04x,0x%04x\"\n",
	       (src == (GLint)GL_SRC_ALPHA && dst == (GLint)GL_ONE_MINUS_SRC_ALPHA)
	           ? "OK" : "FAIL",
	       (unsigned)drain(), (unsigned)src, (unsigned)dst);
	if (src == (GLint)GL_SRC_ALPHA && dst == (GLint)GL_ONE_MINUS_SRC_ALPHA)
		g_ok++;
	else
		g_fail++;
}

static void t_texparam(void)
{
	GLuint name = 0;
	GLint got = -1;
	drain();
	glGenTextures(1, &name);
	if (!name) { skip("texparam.wrap_roundtrip", "glGenTextures returned 0"); return; }
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &got);
	printf("RESULT texparam.wrap_roundtrip %s err=0x%04x detail=\"want 0x812F"
	       " got 0x%04x\"\n", got == GL_CLAMP_TO_EDGE ? "OK" : "FAIL",
	       (unsigned)drain(), (unsigned)got);
	if (got == GL_CLAMP_TO_EDGE) g_ok++; else g_fail++;
}

/* glGetFloatv(GL_MODELVIEW_MATRIX) after glLoadIdentity. A cheap, total check
 * of the matrix stack AND of the float getter — and glGetFloatv is one of the
 * entry points gen-gl-stubs.py already flags as needed-but-stubbed. */
static void t_matrix_readback(void)
{
	GLfloat m[16];
	int i, ok = 1;
	for (i = 0; i < 16; i++) m[i] = -99.0f;      /* poison */
	drain();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	for (i = 0; i < 16; i++) {
		float want = (i % 5 == 0) ? 1.0f : 0.0f;  /* identity, column major */
		if (f_abs(m[i] - want) > 0.01f) ok = 0;
	}
	printf("RESULT matrix.identity_readback %s err=0x%04x detail=\"diag"
	       " %d,%d,%d,%d off[1] %d (thousandths; want 1000,1000,1000,1000 and"
	       " 0)\"\n", ok ? "OK" : "FAIL", (unsigned)drain(),
	       milli(m[0]), milli(m[5]), milli(m[10]), milli(m[15]), milli(m[1]));
	if (ok) g_ok++; else g_fail++;
}

/* Push past the advertised stack depth. GLES 1.1 says that is GL_STACK_OVERFLOW
 * and that the stack is left alone; silently dropping the push instead is how a
 * title's matrices quietly desynchronise from ours, which is a live suspect in
 * the skinning bug (HANDOVER: "confirm the app really is at depth 7"). */
static void t_matrix_overflow(void)
{
	GLint depth = 0;
	GLenum e;
	int i, n;
	drain();
	glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &depth);
	if (depth < 2 || depth > 64) {
		printf("RESULT matrix.stack_overflow SKIP err=0x0000 detail=\"implausible"
		       " GL_MAX_MODELVIEW_STACK_DEPTH %d\"\n", (int)depth);
		g_skip++;
		return;
	}
	glMatrixMode(GL_MODELVIEW);
	n = (int)depth + 2;
	for (i = 0; i < n; i++) glPushMatrix();
	e = glGetError();
	for (i = 0; i < n; i++) glPopMatrix();
	drain();
	printf("RESULT matrix.stack_overflow %s err=0x%04x detail=\"depth %d,"
	       " pushed %d, got 0x%04x, want 0x0503\"\n",
	       e == GL_STACK_OVERFLOW ? "OK" : "FAIL", (unsigned)e,
	       (int)depth, n, (unsigned)e);
	if (e == GL_STACK_OVERFLOW) g_ok++; else g_fail++;
}

/* CAPABILITY ANSWERS, ONE RESULT LINE EACH.
 *
 * A getter that answers 0 to everything is worse than one that is missing: a
 * title reads "zero palette matrices" and quietly disables its own skinning.
 * But the sharper failure is the one that PASSES on both sides with different
 * numbers — we answered MAX_MODELVIEW_STACK_DEPTH=16 where the device says 32,
 * which is not a wrong answer to a query so much as a smaller stack, and a
 * title that nests deeper than ours silently loses pushes from that point on.
 * One line per limit so diff-conform.py can flag each one on its own; a single
 * line holding all of them would collapse into one row and hide which moved. */
static const struct { GLenum e; const char *name; } g_limits[] = {
	{ 0x0D31, "MAX_LIGHTS" },
	{ 0x0D32, "MAX_CLIP_PLANES" },
	{ 0x0D33, "MAX_TEXTURE_SIZE" },
	{ 0x0D36, "MAX_MODELVIEW_STACK_DEPTH" },
	{ 0x0D38, "MAX_PROJECTION_STACK_DEPTH" },
	{ 0x0D39, "MAX_TEXTURE_STACK_DEPTH" },
	{ 0x0D50, "SUBPIXEL_BITS" },
	{ 0x0D52, "RED_BITS" },
	{ 0x0D53, "GREEN_BITS" },
	{ 0x0D54, "BLUE_BITS" },
	{ 0x0D55, "ALPHA_BITS" },
	{ 0x0D56, "DEPTH_BITS" },
	{ 0x0D57, "STENCIL_BITS" },
	{ 0x84E2, "MAX_TEXTURE_UNITS" },
	{ 0x80E8, "MAX_ELEMENTS_VERTICES" },
	{ 0x80E9, "MAX_ELEMENTS_INDICES" },
	{ 0x8842, "MAX_PALETTE_MATRICES_OES" },
	{ 0x86A4, "MAX_VERTEX_UNITS_OES" },
};

static void t_limits(void)
{
	unsigned i;
	for (i = 0; i < sizeof g_limits / sizeof g_limits[0]; i++) {
		GLint v = -1;
		GLenum e;
		drain();
		glGetIntegerv(g_limits[i].e, &v);
		e = drain();
		if (e == GL_INVALID_ENUM) {
			/* CLEANLY REJECTED IS A RESULT, NOT A FAILURE, and the value must
			 * not be reported: a rejected glGet leaves its destination
			 * UNTOUCHED, so whatever is in `v` is this harness's own poison on
			 * one side and could be anything on the other. Printing it would
			 * manufacture a divergence out of two implementations that agree
			 * exactly — both refuse the enum, which is the whole answer.
			 * MAX_ELEMENTS_VERTICES is the live example: a GLES2 enum, refused
			 * by the VR5 and refused by us. */
			printf("RESULT limit.%s OK err=0x%04x detail=\"%s unsupported,"
			       " rejected with GL_INVALID_ENUM\"\n",
			       g_limits[i].name, (unsigned)e, g_limits[i].name);
			g_ok++;
			continue;
		}
		/* A limit is allowed to be legitimately zero — STENCIL_BITS on a config
		 * with no stencil buffer — so "did it answer at all" is the pass
		 * condition and the VALUE is what gets compared across sides. */
		printf("RESULT limit.%s %s err=0x%04x detail=\"%s=%d\"\n",
		       g_limits[i].name, v >= 0 ? "OK" : "FAIL", (unsigned)e,
		       g_limits[i].name, (int)v);
		if (v >= 0) g_ok++; else g_fail++;
	}
}

/* THE QUERY PAIR A REAL TITLE MAKES, IN THE ORDER IT MAKES IT.
 *
 * This is not a generic limit probe — it is a transcription of
 * CGraphics2D::init in LF/Base/lib/libLightning2D.so, the 2D engine every
 * Lightning title links, disassembled:
 *
 *     glGetIntegerv(0x86A2, &this->count);      NUM_COMPRESSED_TEXTURE_FORMATS
 *     this->list = new int[this->count];
 *     glGetIntegerv(0x86A3, this->list);        COMPRESSED_TEXTURE_FORMATS
 *     BaseUtils::Assert(gl_checkError() == 0, "CGraphics2D.cpp", 162,
 *                       "CGraphics2D::init Initialization Error");
 *
 * Three separate things are therefore being measured here, and the assert is
 * the reason the first one is the pass condition rather than a detail:
 *
 *   1. THE ERROR. An implementation that rejects these two pnames trips that
 *      assert during engine startup. Answering "0 formats" quietly is a
 *      different bug from raising GL_INVALID_ENUM, and only the second one
 *      reaches a title's own error handling.
 *   2. THE COUNT, which decides how big that `new int[]` is.
 *   3. HOW MANY THE SECOND QUERY ACTUALLY WROTE, counted by poisoning the
 *      buffer first. This is the one a pass/fail check cannot see: writing more
 *      entries than the count promised is a heap overflow in the caller, and
 *      writing fewer leaves it reading uninitialised memory as texture formats.
 *
 * The list is sorted before printing. The two sides are free to enumerate in
 * different orders — the SET is the answer — and an unsorted diff would report
 * that permutation as a divergence.
 */
static void t_compressed_formats(void)
{
	enum { CAP = 64 };
	const GLint poison = 0x7F7F7F7F;
	GLint n = -1, got[CAP];
	GLenum e_count, e_list;
	char detail[512];
	int wrote = 0, i, j, len;

	drain();
	glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
	e_count = drain();
	if (e_count != GL_NO_ERROR || n < 0) {
		/* REJECTED, or no answer at all. Do not print `n` — a rejected glGet
		 * leaves the destination untouched, so it holds this harness's poison
		 * on one side and anything at all on the other (README: never report
		 * the output of a rejected query). The error code IS the result. */
		printf("RESULT compressed.formats FAIL err=0x%04x detail=\"NUM_COMPRESSED"
		       "_TEXTURE_FORMATS not answered; Lightning2D asserts on this\"\n",
		       (unsigned)e_count);
		g_fail++;
		return;
	}

	for (i = 0; i < CAP; i++) got[i] = poison;
	glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, got);
	e_list = drain();
	for (i = 0; i < CAP; i++)
		if (got[i] != poison) wrote++;

	if (e_list != GL_NO_ERROR) {
		printf("RESULT compressed.formats FAIL err=0x%04x detail=\"count=%d but"
		       " COMPRESSED_TEXTURE_FORMATS was rejected\"\n",
		       (unsigned)e_list, (int)n);
		g_fail++;
		return;
	}

	/* Insertion sort over what was actually written, not over `n`: if the two
	 * disagree that is the finding, and sorting past the written entries would
	 * drag poison into the middle of the list. */
	for (i = 1; i < wrote && i < CAP; i++) {
		GLint v = got[i];
		for (j = i - 1; j >= 0 && got[j] > v; j--) got[j + 1] = got[j];
		got[j + 1] = v;
	}

	len = snprintf(detail, sizeof detail, "count=%d wrote=%d formats=", (int)n, wrote);
	for (i = 0; i < wrote && i < CAP && len > 0 && len < (int)sizeof detail; i++)
		len += snprintf(detail + len, sizeof detail - (unsigned)len,
		                "%s0x%x", i ? "," : "", (unsigned)got[i]);
	if (wrote == 0 && len > 0 && len < (int)sizeof detail)
		snprintf(detail + len, sizeof detail - (unsigned)len, "none");

	/* PASS = the pair answered cleanly and agreed with itself. The contents are
	 * the device's business, not ours to assert — they get compared across the
	 * two logs, which is what this harness is for. */
	printf("RESULT compressed.formats %s err=0x0000 detail=\"%s\"\n",
	       wrote == n ? "OK" : "FAIL", detail);
	if (wrote == n) g_ok++; else g_fail++;
}

static void t_state_setters(void)
{
	drain();
	glPointSize(4.0f);
	glLineWidth(2.0f);
	glPolygonOffset(1.0f, 1.0f);
	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glScissor(0, 0, 64, 64);
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_DONT_CARE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glFlush();
	glFinish();
	result("state.setters_no_error", 1,
	       "GLES1 has no getters for most of these; the err code is the test");
}

/* PER-UNIT TEXCOORD ARRAY STATE — the Pet Pals 2 "pink eye" bug, reduced.
 *
 * GLES 1.1 §6.1.2: GL_TEXTURE_COORD_ARRAY, and the pointer/size/type/stride
 * that go with it, are PER TEXTURE UNIT, and glClientActiveTexture selects
 * which unit the client-array calls address. GL_VERTEX_ARRAY and
 * GL_COLOR_ARRAY are not — there is exactly one of each — which is precisely
 * the asymmetry an implementation is likely to miss.
 *
 * We missed it. The shim kept ONE texcoord array, so a title doing
 *
 *     glClientActiveTexture(GL_TEXTURE0); glTexCoordPointer(uv_skin);
 *     glClientActiveTexture(GL_TEXTURE1); glTexCoordPointer(uv_eyes);
 *
 * had its second call overwrite its first. Pet Pals 2 does exactly that, 3468
 * times in a 40-second capture: unit 0 carries a 256x256 skin at VBO offset
 * 8088, unit 1 a 512x512 eye atlas at offset 13480. The host then sampled the
 * SKIN with the EYE ATLAS's coordinates, which landed on the pink strip in the
 * skin's top-left corner — a magenta patch over the dog's eye, whose pixels
 * (200,120,144) and (192,120,136) matched that corner byte for byte.
 *
 * Enable state is the cheapest expression of it that needs no draw and no
 * readback: set unit 0 on and unit 1 off, then ask each. One array cannot
 * remember two answers.
 */
static void t_multitex_arrays(void)
{
	GLboolean on0, on1;
	char detail[160];
	drain();

	glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glClientActiveTexture(GL_TEXTURE0);
	on0 = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE1);
	on1 = glIsEnabled(GL_TEXTURE_COORD_ARRAY);

	/* Leave the client unit where every other test expects it. */
	glClientActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	snprintf(detail, sizeof detail,
	         "want unit0=1 unit1=0 got unit0=%d unit1=%d", (int)on0, (int)on1);
	result("multitex.texcoord_array_per_unit", on0 == 1 && on1 == 0, detail);
}

static void t_clipplane(void)
{
	GLfloat want[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	GLfloat got[4]  = { -9, -9, -9, -9 };
	drain();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClipPlanef(GL_CLIP_PLANE0, want);
	glGetClipPlanef(GL_CLIP_PLANE0, got);
	report_vec4("clipplane.roundtrip", want, got);
}

/* REPORTS, DOES NOT JUDGE. The spec says glScissor with a negative size is
 * GL_INVALID_VALUE; the VR5 driver raises nothing. Asserting the spec here made
 * this test fail forever on hardware, which trains people to ignore it, and it
 * pushed our side into raising an error the device does not — a title that
 * makes this call and checks glGetError would then behave differently under
 * Tadpole than on the device it was written for.
 *
 * So: record the code and let diff-conform.py decide. Agreement is the pass
 * condition, and a difference surfaces as VALUE_DIFF. */
static void t_negative_count(void)
{
	GLenum e;
	drain();
	glScissor(0, 0, -1, -1);
	e = glGetError();
	printf("RESULT errors.negative_size OK err=0x%04x detail=\"glScissor(0,0,-1,-1)"
	       " raised 0x%04x (spec says 0x0501; the device raises nothing —"
	       " agreement between the two sides is what matters)\"\n",
	       (unsigned)e, (unsigned)e);
	g_ok++;
}

static unsigned char g_pix[4 * 4 * 4];       /* 4x4 RGBA8 */

/* THE LOAD-BEARING TEST: clear to a known colour and ask for it back. Every
 * other test here proves state was remembered; only this one proves anything
 * was RENDERED. It is also the one that cannot work on our side yet —
 * glReadPixels is still a stub — which is why the buffer is poisoned first: a
 * stub that never touches it produces 0xAA and an obvious FAIL, rather than
 * whatever happened to be in .bss and a coincidental pass. */
static void t_readback(void)
{
	unsigned r, g, b, a;
	int want_r = 51, want_g = 102, want_b = 153;   /* 0.2/0.4/0.6 * 255 */
	int ok;
	memset(g_pix, 0xAA, sizeof g_pix);
	drain();
	glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glFinish();
	glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, g_pix);
	r = g_pix[0]; g = g_pix[1]; b = g_pix[2]; a = g_pix[3];
	ok = (r + 8 > (unsigned)want_r && r < (unsigned)want_r + 8) &&
	     (g + 8 > (unsigned)want_g && g < (unsigned)want_g + 8) &&
	     (b + 8 > (unsigned)want_b && b < (unsigned)want_b + 8);
	printf("RESULT readback.clear_color %s err=0x%04x detail=\"got %u,%u,%u,%u"
	       " want ~%d,%d,%d,255%s\"\n", ok ? "OK" : "FAIL", (unsigned)drain(),
	       r, g, b, a, want_r, want_g, want_b,
	       (r == 0xAA && g == 0xAA && b == 0xAA)
	           ? " — buffer still holds its poison, glReadPixels wrote nothing"
	           : "");
	if (ok) g_ok++; else g_fail++;
}

/* ================================================================== */

/* The native window. tadpole_egl.c's own comment says the real driver expects
 * "a Brio display handle of undocumented layout" whose FIRST TWO WORDS are the
 * panel size — so hand it exactly that, zero-filled and comfortably oversized,
 * rather than the NULL a fbdev platform might or might not accept. Our shim
 * ignores the argument entirely, so both sides take it and the two logs stay
 * comparable. Which value was used is logged, because if hardware rejects it
 * that is the first thing the next person needs to know. */
static unsigned int g_native_win[16] = { 480, 272, 0, 0 };

static int egl_bringup(void)
{
	static const GLint cfg_attrs[] = { EGL_NONE };
	void *dpy, *cfg = NULL, *surf, *ctx;
	GLint maj = 0, min = 0, ncfg = 0;

	dpy = eglGetDisplay(NULL);
	if (!eglInitialize(dpy, &maj, &min)) {
		printf("EGLINIT FAIL detail=\"eglInitialize failed, eglGetError=0x%04x\"\n",
		       (unsigned)eglGetError());
		return 0;
	}
	if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
		printf("EGLINIT FAIL detail=\"eglChooseConfig returned %d configs,"
		       " eglGetError=0x%04x\"\n", (int)ncfg, (unsigned)eglGetError());
		return 0;
	}
	surf = eglCreateWindowSurface(dpy, cfg, g_native_win, NULL);
	if (!surf) {
		printf("EGLINIT FAIL detail=\"eglCreateWindowSurface(win={480,272,...})"
		       " returned NULL, eglGetError=0x%04x — the native-window layout"
		       " guess is wrong on this side\"\n", (unsigned)eglGetError());
		return 0;
	}
	ctx = eglCreateContext(dpy, cfg, NULL, NULL);
	if (!ctx) {
		printf("EGLINIT FAIL detail=\"eglCreateContext returned NULL,"
		       " eglGetError=0x%04x\"\n", (unsigned)eglGetError());
		return 0;
	}
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		printf("EGLINIT FAIL detail=\"eglMakeCurrent failed,"
		       " eglGetError=0x%04x\"\n", (unsigned)eglGetError());
		return 0;
	}
	printf("EGLINIT OK detail=\"egl %d.%d vendor=%s\"\n", (int)maj, (int)min,
	       eglQueryString(dpy, EGL_VENDOR));
	printf("META gl_vendor=\"%s\" gl_renderer=\"%s\" gl_version=\"%s\"\n",
	       (const char *)glGetString(GL_VENDOR),
	       (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION));

	glViewport(0, 0, 480, 272);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
	return 1;
}

static int glconform_main(void)
{
	printf("META tool=glconform version=2\n");

	if (!egl_bringup()) {
		printf("META aborted=1 reason=egl_init_failed\n");
		return 1;
	}

	/* The self-checks come first and in this order deliberately: everything
	 * below reports an err= code, and none of those codes mean anything on a
	 * side that fails these two. */
	t_error_tracking();
	t_error_sticky();

	t_limits();
	t_compressed_formats();
	t_matrix_readback();
	t_matrix_overflow();

	t_light_ambient();
	t_light_position();
	t_material_diffuse();
	t_fog();

	t_texenv();
	t_texparam();
	t_blendfunc();

	t_state_setters();
	t_multitex_arrays();
	t_clipplane();
	t_negative_count();

	t_readback();

	printf("META done=1 ok=%d fail=%d skip=%d\n", g_ok, g_fail, g_skip);
	return g_fail ? 1 : 0;
}

/* Freestanding — no ARM crt1.o in the guest lib set, same as tone_test.c. */
void _start(void);
void _start(void) { exit(glconform_main()); }
