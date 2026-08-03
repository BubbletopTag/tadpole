/* Tadpole — fake libEGL.so.1 for the LeapPad2 guest.
 *
 * WHY THIS EXISTS
 * ---------------
 * Native Brio apps (Camera, and every Leapster game) render through OpenGL ES
 * 1.x on Nexell's VR5 GPU. Without a GPU they all die identically:
 *
 *     [0x5] eglInitialize()
 *     ERROR src/egl/vr5_platform_fbdev.cpp line 391
 *     The memory device has not been opened.  <ASSERT>: eglInitialize() failed
 *     PowerDown (Assert) exit !!
 *
 * `libvr5.so` mmaps /dev/vmem for its memory heap and submits command buffers
 * through /dev/ogl_vr5. We have neither, and faking the command format would
 * mean reverse-engineering a proprietary GPU ABI.
 *
 * Instead we replace the stack at the STANDARD API boundary. The stock
 * front-ends are ordinary versioned ELFs exporting the documented APIs:
 *
 *     libEGL.so       -> libEGL.so.1.4          34 egl* entry points
 *     libGLESv1_CM.so -> libGLESv1_CM.so.1.1   180 gl*  entry points
 *
 * Brio and the games between them import only 16 egl* and 89 gl*.
 *
 * WHY NOT "FORWARD TO HOST MESA" — this is the important bit.
 * ----------------------------------------------------------
 * The ALSA shim (tadpole_asound.c) works by writing bytes to a FIFO, which any
 * guest process can do. GL cannot work that way: this is ARM code running
 * inside qemu-user, and it CANNOT call host libraries. There is no host
 * boundary to cross from in here.
 *
 * So the eventual design is to SERIALISE: encode each GL call into a command
 * stream in shared memory and let the native viewer replay it against host GL,
 * blitting the result into the framebuffer it already maps. That is what virgl
 * does, one layer up — at the GLES1 API instead of virtio.
 *
 * GLES 1.x is fixed-function, so there is no shader compiler to reimplement.
 *
 * MILESTONE 1 (this file): make EGL SUCCEED.
 * Every call returns a plausible value and eglSwapBuffers is wired to the
 * framebuffer. GL entry points are stubs (tadpole_gles.c). The game therefore
 * runs instead of asserting — nothing is drawn yet, but the app lives, its
 * ViewFrame loads, and input reaches it. That is the difference between "dies
 * at startup" and "a surface we can start filling in".
 */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  ulong;

#define NULL ((void *)0)
#define EGL_FALSE 0
#define EGL_TRUE  1

/* EGL error codes */
#define EGL_SUCCESS           0x3000
#define EGL_BAD_DISPLAY       0x3008

/* eglQueryString names */
#define EGL_VENDOR        0x3053
#define EGL_VERSION       0x3054
#define EGL_EXTENSIONS    0x3055
#define EGL_CLIENT_APIS   0x308D

/* Handles. Non-NULL sentinels — callers only ever check against NULL/0 and
 * pass them back to us, so distinct constants are enough to catch mix-ups. */
#define TAD_DISPLAY ((void *)0x7EDD1500)
#define TAD_CONFIG  ((void *)0x7EDD1C06)
#define TAD_SURFACE ((void *)0x7EDD5F00)
#define TAD_CONTEXT ((void *)0x7EDDC017)

extern int   snprintf(char *s, unsigned int n, const char *fmt, ...);
extern char *getenv(const char *name);

static u32 g_egl_error = EGL_SUCCESS;

/* --- display / lifecycle ------------------------------------------------- */

void *eglGetDisplay(void *native_display)
{
	(void)native_display;
	return TAD_DISPLAY;
}

u32 eglInitialize(void *dpy, i32 *major, i32 *minor)
{
	if (dpy != TAD_DISPLAY) {
		g_egl_error = EGL_BAD_DISPLAY;
		return EGL_FALSE;
	}
	/* Claim EGL 1.4 — that is what libEGL.so.1.4 on the device reports, and
	 * Brio's DisplayMPI checks the version before proceeding. */
	if (major) *major = 1;
	if (minor) *minor = 4;
	g_egl_error = EGL_SUCCESS;
	return EGL_TRUE;
}

/* CONTEXT TEARDOWN IS DEFERRED, NOT IMMEDIATE.
 *
 * AppManager stays resident across game launches — it dlopen()s App.so and
 * later UnloadModule()s it — so the GL shim's tables are process-lifetime.
 * Without a teardown the previous title's textures were still held when the
 * next one started, its glGenTextures found no free slot and handed back name
 * 0, and it rendered untextured; the damage accumulated across launches.
 *
 * But freeing on eglTerminate/eglDestroyContext CRASHES. Brio goes on using GL
 * after destroying a context — the guest segfaulted immediately after the
 * host logged "context reset — mirrors dropped", on sign-in and the home
 * screen, repeatedly. It was reading textures we had just freed. Real hardware
 * survives this because its driver keeps the objects alive until the memory is
 * genuinely reclaimed.
 *
 * So destruction only RAISES A FLAG, and the tables are cleared at the start of
 * the next context instead. The new title still gets a clean table — which is
 * the whole point — and nothing is ever freed while something may still be
 * holding it.
 */
extern void tad_gl_context_reset(void);
static int g_reset_pending;

static void reset_if_pending(void)
{
	if (!g_reset_pending)
		return;
	g_reset_pending = 0;
	tad_gl_context_reset();
}

u32 eglTerminate(void *dpy)
{
	(void)dpy;
	g_reset_pending = 1;
	return EGL_TRUE;
}

const char *eglQueryString(void *dpy, i32 name)
{
	(void)dpy;
	switch (name) {
	case EGL_VENDOR:      return "Tadpole";
	case EGL_VERSION:     return "1.4 Tadpole";
	case EGL_CLIENT_APIS: return "OpenGL_ES";
	case EGL_EXTENSIONS:  return "";        /* advertise nothing */
	default:              return "";
	}
}

u32 eglGetError(void) { u32 e = g_egl_error; g_egl_error = EGL_SUCCESS; return e; }

/* --- config / surface / context ------------------------------------------ */

/* One config, always. Returning a single config for any attribute list is
 * safe here: there is no hardware to mismatch, and the caller picks the
 * framebuffer geometry separately through the fb ioctls we already emulate. */
u32 eglChooseConfig(void *dpy, const i32 *attrib, void **configs,
                    i32 config_size, i32 *num_config)
{
	(void)dpy; (void)attrib;
	if (configs && config_size > 0)
		configs[0] = TAD_CONFIG;
	if (num_config)
		*num_config = (config_size > 0) ? 1 : 0;
	return EGL_TRUE;
}

/* The native window is a Brio display handle of undocumented layout. We do not
 * need to decode it: its first two words are the PANEL size (480x272), not the
 * window the title actually gets. The real per-title rectangle arrives at the
 * fb driver instead — see the layer-window note in tadpole_shim.c. */
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const i32 *a)
{
	(void)dpy; (void)config; (void)win; (void)a;
	return TAD_SURFACE;
}

u32 eglDestroySurface(void *dpy, void *surf) { (void)dpy; (void)surf; return EGL_TRUE; }

void *eglCreateContext(void *dpy, void *config, void *share, const i32 *a)
{
	(void)dpy; (void)config; (void)share; (void)a;
	/* The safe moment to drop the previous title's tables: the old context is
	 * gone and the new one has not been handed out yet, so nothing can be
	 * mid-draw against what we are about to free. */
	reset_if_pending();
	return TAD_CONTEXT;
}

u32 eglDestroyContext(void *dpy, void *ctx)
{
	(void)dpy; (void)ctx;
	g_reset_pending = 1;      /* cleared at the next eglCreateContext */
	return EGL_TRUE;
}

u32 eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx)
{ (void)dpy; (void)draw; (void)read; (void)ctx; return EGL_TRUE; }

void *eglGetCurrentContext(void) { return TAD_CONTEXT; }
void *eglGetCurrentDisplay(void) { return TAD_DISPLAY; }
void *eglGetCurrentSurface(i32 readdraw) { (void)readdraw; return TAD_SURFACE; }

/* --- presentation --------------------------------------------------------
 *
 * eglSwapBuffers is where a real driver flips the rendered frame onto the
 * panel. Nothing is rendered yet, so this only counts frames — but it is the
 * hook the command-stream replay will attach to, and the count proves the app
 * reached a render loop rather than stalling at init.
 */
static u32 g_frames;

/* Provided by libGLESv1_CM: blit the rasteriser's back buffer to the visible
 * framebuffer page. Presenting HERE rather than drawing straight to the panel
 * is what stops animated content smearing across frames. */
extern void tadpole_gl_present(void);

u32 eglSwapBuffers(void *dpy, void *surf)
{
	(void)dpy; (void)surf;
	tadpole_gl_present();
	g_frames++;
	return EGL_TRUE;
}

u32 eglSwapInterval(void *dpy, i32 interval) { (void)dpy; (void)interval; return EGL_TRUE; }
u32 eglWaitGL(void)     { return EGL_TRUE; }
u32 eglWaitNative(i32 e){ (void)e; return EGL_TRUE; }
u32 eglBindAPI(u32 api) { (void)api; return EGL_TRUE; }
u32 eglReleaseThread(void) { return EGL_TRUE; }

/* Frame counter, for the harness to read. */
u32 tadpole_egl_frames(void) { return g_frames; }

/* ---- symbols the rest of the stock GPU stack links against -----------------
 *
 * The stock libEGL.so.1.4 exports more than EGL, and other pieces of the VR5
 * driver link against those extras. Replacing the library without them makes
 * the dynamic loader refuse to start AppManager at all:
 *
 *     symbol '__vr5_set_swap_buffer_callback': can't resolve symbol
 *
 * ...which in turn stops LeapFrogPlugin.so loading, so every LF.CSystem call
 * from ActionScript returns undefined and the UI renders black. The failure
 * looks like a rendering bug and is really a link-time one.
 *
 * These are consumed only by libvr5.so / libGLES.so — parts of the GPU stack
 * this shim supersedes and which are unreachable once our entry points win. So
 * they need to RESOLVE, not to work. Sizes mirror the originals.
 *
 * NOTE the C++ mangled name: EGL::g_pProcList. Spelled via an asm label rather
 * than guessed at, because the loader matches the mangled string exactly.
 */
void  __vr5_set_swap_buffer_callback(void *cb) { (void)cb; }
void *__global_ftn_vg_dispatch_table;
void *tad_egl_proc_list __asm__("_ZN3EGL11g_pProcListE");
