/* Tadpole for Android — the three GL entry points ES spells differently.
 *
 * Linked in beside the viewer. Not one line of tadpole/viewer/ changes for it.
 *
 * ### Why there are only three
 *
 * The expectation going in was that the viewer's OpenGL would be the hard part
 * of an Android port. It is very nearly a non-problem, for a reason that is
 * obvious in hindsight: the whole of tadpole_hle.c exists to replay a guest
 * that speaks OpenGL ES 1.1, so what it calls is fixed-function ES 1.1 in all
 * but name — glMatrixMode, glLoadMatrixf, glEnableClientState with client-side
 * arrays, glTexEnvi, glAlphaFunc, glFogx. Android ships exactly that library,
 * libGLESv1_CM.so, on every device there has ever been.
 *
 * The calls that genuinely are not in ES 1.1 — glGenFramebuffers,
 * glBlitFramebuffer, glRenderbufferStorageMultisample — were already resolved
 * through SDL_GL_GetProcAddress rather than bound at link time, because
 * Windows' opengl32.dll exports nothing past GL 1.1 and the Windows port had
 * to. That work paid for this port before anyone wrote it.
 *
 * So the viewer links against libGLESv1_CM with exactly three undefined
 * symbols, and all three are one difference: ES has no GLdouble, so what takes
 * doubles on the desktop takes floats here and carries an `f`.
 *
 * ### Functions and not macros, which was the first attempt
 *
 * `#define glOrtho(...) glOrthof(...)`, force-included, does not work: the
 * macro reaches SDL_opengl.h's own *declaration* of glOrtho and rewrites it
 * into nonsense, and clang reports it a hundred lines later as
 *
 *     SDL_opengl.h:893:23: error: type specifier missing, defaults to 'int'
 *
 * which names the header rather than the cause. Definitions have none of that
 * problem: the declarations stay exactly as SDL wrote them and the linker finds
 * a body.
 *
 * ### The conversion is not lossy — it removes a round trip
 *
 *     case TADGL_ORTHO: { float v[6]; ring_get(v,24);
 *                         glOrtho(v[0],v[1],v[2],v[3],v[4],v[5]); ... }
 *
 * The values in the ring are already floats, because the guest put GLfloats
 * there. The desktop build widens them to double at the call and the driver
 * narrows them again. This goes float to float. Likewise glClearDepth, whose
 * one call site is literally `glClearDepth((double)d)` on a float d.
 *
 * ### What this does NOT paper over
 *
 * Nothing here makes the FBO paths work. Those resolve at runtime through
 * SDL_GL_GetProcAddress, and under an ES 1.1 context the names present are the
 * OES-suffixed ones — glGenFramebuffersOES and friends — or nothing at all.
 * The lookup returns NULL and the viewer's own probe is what will say so. That
 * is real work and it is not this file's.
 */
/* GLES/gl.h ONLY, and not SDL_opengl.h beside it. The two cannot both be
 * included: SDL_opengl.h carries a full copy of the desktop GL 1.1 header and
 * defines the __gl_h_ guard, so whichever is second compiles to nothing, and
 * the failure is three "call to undeclared function 'glOrthof'" errors that
 * look like a missing NDK rather than a header collision.
 *
 * The desktop-shaped parameter types are spelled out below instead. C has no
 * name mangling, so all the linker needs is that the definition's ABI matches
 * the declaration the viewer compiled against — GLdouble and GLclampd are both
 * `double` in SDL_opengl.h, and both are `double` here. */
#include <GLES/gl.h>

typedef double GLdouble;
typedef double GLclampd;

void glClearDepth(GLclampd depth)
{
	glClearDepthf((GLclampf)depth);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
             GLdouble n, GLdouble f)
{
	glOrthof((GLfloat)l, (GLfloat)r, (GLfloat)b, (GLfloat)t,
	         (GLfloat)n, (GLfloat)f);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
               GLdouble n, GLdouble f)
{
	glFrustumf((GLfloat)l, (GLfloat)r, (GLfloat)b, (GLfloat)t,
	           (GLfloat)n, (GLfloat)f);
}
