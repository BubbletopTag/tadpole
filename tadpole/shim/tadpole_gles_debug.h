/* Tadpole — the GL shim's diagnosis and fail-fast interface.
 *
 * Shared by tadpole_gles_core.c (the real implementations), by the GENERATED
 * tadpole_gles_stubs.c, and by tadpole_egl.c. Keep it free of libc and of any
 * GL header: the shim builds -nostdlib against the guest's own uClibc.
 *
 * Nothing here is named gl* or egl*. tools/gen-gl-stubs.py decides what is
 * "implemented" by scanning these sources for definitions matching
 * /\b(gl|egl)[A-Za-z0-9_]*\s*\(...\)\s*\{/, and a helper called glSomething
 * would be counted as a GL entry point that does not exist.
 */
#ifndef TADPOLE_GLES_DEBUG_H
#define TADPOLE_GLES_DEBUG_H

/* TADPOLE_GL_DEBUG, parsed once:
 *
 *   unset, "" or "0"   0  quiet. First hit on an unimplemented entry point is
 *                         still reported once — see tad_gl_stub_hit().
 *   "1"                1  LOUD. Trace the call sequence, report every stub hit
 *                         and every GL error as it happens.
 *   "2" or more        2  FATAL. The first unimplemented entry point or GL
 *                         error aborts the guest on the spot, so the crash
 *                         handler's stack scan names the CALLER.
 *
 * Any non-numeric value means 1, because that is what every value used to mean
 * back when this was a plain getenv() != NULL test, and tools/ and the docs are
 * full of TADPOLE_GL_DEBUG=1.
 */
int tad_gl_level(void);

/* Level >= 1. "[gl] msg a b" to stderr. */
void tad_gl_trace(const char *msg, int a, int b);

/* ALWAYS, whatever the level, and also appended to $TADPOLE_DIR/gl-warnings.log
 * because stderr is easy to lose. Reserved for conditions that mean the output
 * is definitely wrong. */
void tad_gl_warn(const char *msg, int a, int b);

/* One raw line, always, same two destinations. No formatting. */
void tad_gl_line(const char *s);

/* Print a banner explaining what went wrong and why it is fatal, then die in a
 * way tadpole_crash.c can report — its stack scan is the whole point, because
 * it names the guest library and offset that made the call. Never returns. */
void tad_gl_fatal(const char *what, const char *detail);

/* Called by every generated stub. `hits` is a per-stub counter owned by the
 * stub itself, so the common path is one increment and no search. */
void tad_gl_stub_hit(const char *name, unsigned int *hits);

/* Raise a GL error. `where` is the entry point that raised it, so a level-2
 * abort can name the call rather than the frame. */
void tad_gl_error(unsigned int code, const char *where);

/* Consume the sticky error, GL_NO_ERROR if none. This is what glGetError()
 * returns. */
unsigned int tad_gl_error_take(void);

/* Everything the guest asked for and did not get: the stub hit table with
 * counts, and the GL error tally. Printed at exit, and again whenever a title
 * tears its context down — AppManager does not exit between games, so waiting
 * for process exit would merge every title in the session into one table.
 * `when` labels the report. */
void tad_gl_report(const char *when);

/* Call once per presented frame. Emits tad_gl_report() only while the set of
 * missing entry points is still growing, rate-limited — so a run that is KILLED
 * rather than exited (every probe harness in tools/) still leaves a current
 * table behind. */
void tad_gl_report_tick(void);

/* Clear the table so the next title starts from nothing. Call after
 * tad_gl_report(), not instead of it. */
void tad_gl_report_reset(void);

/* GL error codes. Spelled here so the stubs file and the core agree without
 * either one pulling in a GL header. */
#define TAD_GL_NO_ERROR                       0x0000
#define TAD_GL_INVALID_ENUM                   0x0500
#define TAD_GL_INVALID_VALUE                  0x0501
#define TAD_GL_INVALID_OPERATION              0x0502
#define TAD_GL_STACK_OVERFLOW                 0x0503
#define TAD_GL_STACK_UNDERFLOW                0x0504
#define TAD_GL_OUT_OF_MEMORY                  0x0505
#define TAD_GL_INVALID_FRAMEBUFFER_OPERATION  0x0506

#endif /* TADPOLE_GLES_DEBUG_H */
