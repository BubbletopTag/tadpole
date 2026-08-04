/* Tadpole — the GL shim's diagnosis and fail-fast policy.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * For most of this project's life the GL shim failed SILENTLY in three
 * different ways at once, and every one of them cost a debugging session that
 * started from a symptom instead of from a cause:
 *
 *   1. An unimplemented entry point was `void glFoo(void) { }`. It swallowed
 *      its arguments, returned, and said nothing. A title calling glLightfv
 *      1200 times a frame looked exactly like a title that never lit anything,
 *      so the visible bug was "the character is black" and the actual bug was
 *      "we never wrote glLightfv". There is no way to tell those apart short of
 *      guessing, and guessing is what happened.
 *
 *   2. glGetError() was `return 0;`. Not a stale value — a LIE, hardcoded.
 *      Every title that error-checks its own GL usage was told it was fine, and
 *      so was every test we might have written against it.
 *
 *   3. Even the diagnostics that did exist only fired under TADPOLE_GL_DEBUG=1,
 *      which nobody sets until they already suspect GL. The evidence was absent
 *      from exactly the logs that get filed as bug reports.
 *
 * So: the shim now says what it did not do, at three volumes.
 *
 *   TADPOLE_GL_DEBUG unset  the FIRST hit on each unimplemented entry point is
 *                           still reported once, to stderr and to
 *                           gl-warnings.log, and a summary table is printed at
 *                           exit. One line per missing function per run, and an
 *                           ordinary log now contains the answer.
 *   TADPOLE_GL_DEBUG=1      every stub hit, every GL error, the full call trace.
 *   TADPOLE_GL_DEBUG=2      the first stub hit or GL error is FATAL.
 *
 * Level 2 is the one that changes how this project gets debugged. Dying at the
 * call site puts tadpole_crash.c's stack scan on the screen, and that scan names
 * the guest library and offset that made the call — so "SpongeBob does not
 * render" becomes "App.so+0x2f1a4 called glLightfv, which we never wrote",
 * which is a fix rather than a hypothesis.
 *
 * NOTHING HERE IS NAMED gl* OR egl*. tools/gen-gl-stubs.py classifies an entry
 * point as implemented by scanning for definitions matching
 * /\b(gl|egl)[A-Za-z0-9_]*\s*\(...\)\s*\{/, so a helper called glTraceSomething
 * would be counted as a GL entry point that does not exist, and would then go
 * missing from the generated stub list — which is fatal at LOAD time. tad_gl_*
 * has no word boundary before its "gl" and is invisible to that scan, on
 * purpose.
 */
#include "tadpole_gles_debug.h"

typedef unsigned int u32;
/* The compiler's own size_t, as in tadpole_crash.c. Spelling it `unsigned int`
 * instead makes clang warn that every libc prototype here is an incompatible
 * redeclaration — the shim builds -nostdlib, so these are hand-written. */
typedef __SIZE_TYPE__ size_t;

#define NULL ((void *)0)

extern int   open(const char *path, int flags, ...);
extern long  write(int fd, const void *buf, size_t n);
extern char *getenv(const char *name);
extern int   snprintf(char *s, size_t n, const char *fmt, ...);
extern void  abort(void);

#define O_WRONLY 01
#define O_CREAT  0100
#define O_APPEND 02000

/* ---- level ------------------------------------------------------------- */

static int g_level = -1;

int tad_gl_level(void)
{
	if (g_level < 0) {
		const char *e = getenv("TADPOLE_GL_DEBUG");
		if (!e || !e[0])
			g_level = 0;
		else if (e[0] >= '0' && e[0] <= '9')
			g_level = e[0] - '0';
		else
			g_level = 1;      /* see the header: any value used to mean "on" */
		if (g_level > 2)
			g_level = 2;
	}
	return g_level;
}

/* ---- output ------------------------------------------------------------ */

/* stderr AND a file. A log captured with `> log` and no `2>&1` holds Brio's
 * stdout traces and none of ours, which once wasted a whole round trip
 * diagnosing white textures. gl-warnings.log is always there to be read
 * afterwards. */
static void say(const char *buf, u32 n)
{
	static int fd = -2;

	write(2, buf, n);

	if (fd == -2) {
		char path[512];
		const char *d = getenv("TADPOLE_DIR");
		if (!d) d = "/tmp/tadpole";
		snprintf(path, sizeof(path), "%s/gl-warnings.log", d);
		fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	}
	if (fd >= 0)
		write(fd, buf, n);
}

void tad_gl_line(const char *s)
{
	char b[288];
	int n = snprintf(b, sizeof(b), "%s\n", s);
	if (n > 0) say(b, (u32)(n < (int)sizeof(b) ? n : (int)sizeof(b) - 1));
}

void tad_gl_warn(const char *msg, int a, int b)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "[gl] WARN %s %d %d\n", msg, a, b);
	if (n > 0) say(buf, (u32)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

/* Trace is stderr only and deliberately NOT written to gl-warnings.log: at
 * level 1 it is thousands of lines a frame, and mixing that into the file whose
 * whole value is being short enough to read would destroy it. */
void tad_gl_trace(const char *msg, int a, int b)
{
	char buf[192];
	int n;
	if (tad_gl_level() < 1)
		return;
	n = snprintf(buf, sizeof(buf), "[gl] %s %d %d\n", msg, a, b);
	if (n > 0) write(2, buf, (u32)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

/* ---- fatal ------------------------------------------------------------- */

void tad_gl_fatal(const char *what, const char *detail)
{
	char b[320];
	int n;

	tad_gl_line("[gl] ============================================================");
	n = snprintf(b, sizeof(b), "[gl] FATAL (TADPOLE_GL_DEBUG=2): %s", what);
	if (n > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }
	if (detail && detail[0]) {
		n = snprintf(b, sizeof(b), "[gl]   %s", detail);
		if (n > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }
	}
	tad_gl_line("[gl]");
	tad_gl_line("[gl] Tadpole stopped deliberately — the title did not crash.");
	/* The stack scan only appears when tadpole_crash.c is in the process, which
	 * means a title running under the libdl/libz shim. A standalone guest binary
	 * like tools/glconform loads no shim and simply dies here, so promising a
	 * report unconditionally would have someone hunting for output that was
	 * never going to exist. */
	tad_gl_line("[gl] If the shim is loaded, tadpole_crash.c follows with a stack");
	tad_gl_line("[gl] scan naming the guest library and offset that made the call.");
	tad_gl_line("[gl] Rerun without TADPOLE_GL_DEBUG=2 to carry on regardless.");
	tad_gl_line("[gl] ============================================================");

	tad_gl_report("at the fatal stop");

	abort();
	/* abort() is uClibc's, and this process is full of libraries we impersonate
	 * — including one that replaces libc symbols. Fatal means fatal, so do not
	 * depend on someone else's function to end the process. */
	*(volatile int *)0 = 0;
}

/* ---- GL error state ---------------------------------------------------- */

/* GLES 1.1 §2.5: errors are STICKY. The first error raised since the last
 * glGetError is the one reported, and later errors are discarded until it is
 * read — not the other way round. Getting this backwards would report the most
 * recent error, which is the one furthest from the cause. */
static u32         g_err;              /* current sticky code, 0 if none */
static u32         g_err_total;        /* raised this run, for the summary */
static u32         g_err_first;        /* very first code raised */
static const char *g_err_first_where;

static const char *errname(u32 c)
{
	switch (c) {
	case TAD_GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
	case TAD_GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
	case TAD_GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
	case TAD_GL_STACK_OVERFLOW:    return "GL_STACK_OVERFLOW";
	case TAD_GL_STACK_UNDERFLOW:   return "GL_STACK_UNDERFLOW";
	case TAD_GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
	case TAD_GL_INVALID_FRAMEBUFFER_OPERATION:
	                               return "GL_INVALID_FRAMEBUFFER_OPERATION";
	default:                       return "GL_?";
	}
}

void tad_gl_error(u32 code, const char *where)
{
	char b[224];
	int n;

	if (!code)
		return;

	g_err_total++;
	if (!g_err)
		g_err = code;                       /* sticky: keep the FIRST */
	if (!g_err_first) {
		g_err_first = code;
		g_err_first_where = where;
	}

	/* Always the first one, at any level: an error the guest never reads is
	 * still the reason its next 400 draws did nothing. After that it is
	 * level-1 material, because a title that raises one error per frame would
	 * otherwise fill the log with the same line. */
	n = snprintf(b, sizeof(b), "[gl] %s raised by %s",
	             errname(code), where ? where : "?");
	if (n > 0) {
		b[sizeof(b) - 1] = 0;
		if (g_err_total == 1 || tad_gl_level() >= 1)
			tad_gl_line(b);
	}

	if (tad_gl_level() >= 2)
		tad_gl_fatal("a GL call raised an error", b + 5 /* skip "[gl] " */);
}

u32 tad_gl_error_take(void)
{
	u32 e = g_err;
	g_err = TAD_GL_NO_ERROR;
	return e;
}

/* ---- the stub hit table ------------------------------------------------ */

/* 180 gl* entry points exist on the device and the table only ever holds the
 * ones actually CALLED, so this is comfortably oversized. */
#define MAX_STUBS 224
static struct { const char *name; unsigned int *hits; } g_stub[MAX_STUBS];
static unsigned int g_stub_n;
static unsigned int g_stub_lost;       /* distinct names that did not fit */

/* A DSO DESTRUCTOR, NOT atexit(). AppManager dlopen()s a title's App.so and
 * dlclose()s it when the title exits, and this library can go with it — an
 * atexit handler would then be a pointer into unmapped memory, and the process
 * would die on the way out for a reason that has nothing to do with the bug
 * being chased. .fini_array is run by the dynamic loader as part of unloading
 * THIS object, whenever that happens, which is exactly the lifetime wanted.
 * It works under -nostdlib because the loader, not crt1.o, is what runs it. */
__attribute__((destructor))
static void report_on_unload(void)
{
	tad_gl_report("on GL library unload");
}

void tad_gl_stub_hit(const char *name, unsigned int *hits)
{
	unsigned int n = ++(*hits);

	if (n == 1) {
		if (g_stub_n < MAX_STUBS) {
			g_stub[g_stub_n].name = name;
			g_stub[g_stub_n].hits = hits;
			g_stub_n++;
		} else {
			g_stub_lost++;
		}

		/* Once, at ANY level. A missing entry point is the definition of "the
		 * output is definitely wrong", and one line per missing function per
		 * run is a price worth paying to have that evidence in logs nobody
		 * thought to instrument. */
		{
			char b[224];
			int k = snprintf(b, sizeof(b),
			                 "[gl] UNIMPLEMENTED %s — called, did nothing", name);
			if (k > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }
		}
	} else if (tad_gl_level() >= 1) {
		tad_gl_trace(name, (int)n, -1);
	}

	if (tad_gl_level() >= 2)
		tad_gl_fatal("the title called an entry point we never implemented",
		             name);
}

/* ---- the summary ------------------------------------------------------- */

void tad_gl_report(const char *when)
{
	char b[256];
	unsigned int i;
	int n;

	if (!g_stub_n && !g_err_total)
		return;

	n = snprintf(b, sizeof(b), "[gl] ==== GL gaps this run (%s) ====",
	             when ? when : "");
	if (n > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }

	for (i = 0; i < g_stub_n; i++) {
		n = snprintf(b, sizeof(b), "[gl]   unimplemented  %-34s %u call%s",
		             g_stub[i].name, *g_stub[i].hits,
		             *g_stub[i].hits == 1 ? "" : "s");
		if (n > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }
	}
	if (g_stub_lost) {
		n = snprintf(b, sizeof(b), "[gl]   (+%u more distinct names; table full)",
		             g_stub_lost);
		if (n > 0) tad_gl_line(b);
	}
	if (g_err_total) {
		n = snprintf(b, sizeof(b), "[gl]   GL errors raised: %u; first was %s from %s",
		             g_err_total, errname(g_err_first),
		             g_err_first_where ? g_err_first_where : "?");
		if (n > 0) { b[sizeof(b) - 1] = 0; tad_gl_line(b); }
	}
	tad_gl_line("[gl] ================================================");

	/* The table is per-TITLE, not per-process: AppManager dlopen()s App.so,
	 * runs it and unloads it without exiting, so a session-long table would
	 * merge every title into one row set and lose which one asked for what.
	 * tad_gl_context_reset() reports and then clears. */
}

/* BECAUSE ALMOST NOTHING HERE EXITS CLEANLY.
 *
 * The summary is printed when the GL library unloads and when a title tears its
 * context down, and a probe run reaches NEITHER: tools/probe-race.sh drives the
 * guest to a screen and then kills it, which is the right thing for a harness to
 * do and leaves no chance to run a destructor. So the table that says how many
 * times each missing entry point was called existed and was never once printed
 * by the tool that most needed it.
 *
 * Called every present. It emits only while the set of missing entry points is
 * still GROWING, and at most once per 300 frames, so it converges: a title that
 * discovers its last stub in the first second prints twice and then goes quiet
 * forever, and a `kill -9` at any later point still leaves a current table in
 * gl-warnings.log.
 */
void tad_gl_report_tick(void)
{
	static unsigned int frame, last_frame, last_n;

	frame++;
	if (g_stub_n == last_n)
		return;                       /* nothing new since the last report */
	if (frame - last_frame < 300)
		return;                       /* too soon; the count is still moving */
	last_n = g_stub_n;
	last_frame = frame;
	tad_gl_report("running total");
}

void tad_gl_report_reset(void)
{
	unsigned int i;
	for (i = 0; i < g_stub_n; i++)
		*g_stub[i].hits = 0;
	g_stub_n = 0;
	g_stub_lost = 0;
	g_err = 0;
	g_err_total = 0;
	g_err_first = 0;
	g_err_first_where = NULL;
}
