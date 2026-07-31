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
#define GL_GLEXT_PROTOTYPES 1
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../shim/tadpole_glcmd.h"
#include "tadpole_hle.h"

/* GLES1 enums that differ from, or are absent on, the desktop. */
#define GLES_FIXED  0x140C

#define MAX_TEX  256
#define MAX_BUF  128

struct hbuf { unsigned char *data; unsigned int size; };
struct harr { unsigned int buf, type, on; int size, stride; unsigned int off; };

static struct tadgl_hdr *g_ring;
static unsigned char    *g_data;
static SDL_Window       *g_win;
static SDL_GLContext     g_ctx;
static GLuint            g_fbo, g_colour, g_depth;
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
/* The layer rectangle the guest told us about. Only this region is read back:
 * blanket-writing the whole panel filled the REST of fb1 with the FBO's
 * untouched black, and since the compositor draws fb2 (video) beneath fb1, an
 * opaque fb1 hides it. The software rasteriser only ever wrote inside the
 * window, so matching that is also what keeps compositing behaviour identical. */
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
static unsigned int      g_gl_err;     /* first GL error seen, 0 if none */
static unsigned long     g_err_count;

/* GL ERRORS ARE THE FIRST THING TO CHECK when a replay produces nothing. A
 * single rejected call (bad enum, unsupported combination) silently draws
 * nothing and every later call still "works", so the frame comes back empty with
 * no other symptom. Sample once per frame — glGetError is a pipeline flush point
 * and calling it per command would dominate the cost. */
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

static int make_target(int w, int h)
{
	glGenFramebuffers(1, &g_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	glGenTextures(1, &g_colour);
	glBindTexture(GL_TEXTURE_2D, g_colour);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, g_colour, 0);
	glGenRenderbuffers(1, &g_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                          GL_RENDERBUFFER, g_depth);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

int hle_host_init(const char *dir, int w, int h)
{
	char path[600];
	int fd;
	void *m;

	g_w = w; g_h = h;
	snprintf(path, sizeof(path), "%s/glcmd.bin", dir);
	fd = open(path, O_RDWR | O_CREAT, 0666);
	if (fd < 0) { fprintf(stderr, "hle: cannot open %s\n", path); return 0; }
	if (ftruncate(fd, (off_t)TADGL_FILE_BYTES) != 0) { close(fd); return 0; }
	m = mmap(NULL, TADGL_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) { fprintf(stderr, "hle: cannot map glcmd.bin\n"); return 0; }

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

	if (!make_target(w, h)) {
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
	g_verbose = getenv("TADPOLE_HLE_DEBUG") != NULL;
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

static void setup_arrays(unsigned int nverts)
{
	static const GLenum client[TADGL_ARR_COUNT] = {
		GL_VERTEX_ARRAY, GL_COLOR_ARRAY, GL_TEXTURE_COORD_ARRAY,
		GL_NORMAL_ARRAY };
	int i;

	for (i = 0; i < TADGL_ARR_COUNT; i++) {
		const void *p = bind_array(i, nverts);
		struct harr *a = &g_arr[i];
		unsigned int stride;

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
		case TADGL_ARR_TEXCOORD:
			glTexCoordPointer(a->size, a->type == GLES_FIXED ? GL_FLOAT : a->type,
			                  (GLsizei)stride, p);
			break;
		case TADGL_ARR_NORMAL:
			glNormalPointer(a->type == GLES_FIXED ? GL_FLOAT : a->type,
			                (GLsizei)stride, p);
			break;
		}
	}
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
			 * crawling forward would only corrupt more. */
			if (p.op >= TADGL_OP_COUNT || padded > TADGL_RING / 2u) {
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
			 * origin is bottom-left and the panel's is top-left, so flip. */
			int rx = g_vw ? g_vx : 0, ry = g_vw ? g_vy : 0;
			int rw = g_vw ? g_vw : g_w, rh = g_vh ? g_vh : g_h;
			int y;
			check_gl("frame");
			glFinish();
			for (y = 0; y < rh; y++)
				glReadPixels(rx, g_h - 1 - (ry + y), rw, 1, GL_BGRA,
				             GL_UNSIGNED_BYTE,
				             out + (size_t)(ry + y) * pitch_px + rx);
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
			/* The guest's y is measured from the top; GL's is from the bottom. */
			glViewport(v[0], g_h - v[1] - v[3], v[2], v[3]);
			glScissor(v[0], g_h - v[1] - v[3], v[2], v[3]);
			glEnable(GL_SCISSOR_TEST);
			g_vx = v[0]; g_vy = v[1]; g_vw = v[2]; g_vh = v[3];
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
			if (c == 0x0B44 /* GL_CULL_FACE */ || c == 0x0B50 /* GL_LIGHTING */ ||
			    c == 0x0B60 /* GL_FOG */) {
				if (!g_filtered++)
					fprintf(stderr, "hle: ignoring glEnable(0x%04X) — the"
					        " software reference ignores it too\n", c);
				break;
			}
			if (c == 0x0DE1 /* GL_TEXTURE_2D */) g_tex_enabled = 1;
			glEnable(c);
			break;
		}
		case TADGL_DISABLE: {
			unsigned int c;
			ring_get(&c, 4);
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
			/* The titles call glTexEnv with values that are NOT valid modes —
			 * the software path already notes this and ignores them. Forwarding
			 * them verbatim is a guaranteed GL_INVALID_ENUM, and one rejected
			 * call leaves the error flag set for everything after it. Only pass
			 * the combination we actually honour. */
			if (v[1] == 0x2200 /* GL_TEXTURE_ENV_MODE */ &&
			    (v[2] == 0x2100 /* MODULATE */ || v[2] == 0x1E01 /* REPLACE */))
				glTexEnvi(0x2300 /* GL_TEXTURE_ENV */, 0x2200, (GLint)v[2]);
			break;
		}
		case TADGL_COLOR:     { float v[4]; ring_get(v,16);
		                        glColor4f(v[0],v[1],v[2],v[3]); break; }

		case TADGL_MATRIXMODE:  { unsigned int m; ring_get(&m,4); glMatrixMode(m); break; }
		case TADGL_LOADIDENTITY: glLoadIdentity(); break;
		case TADGL_PUSHMATRIX:   glPushMatrix();   break;
		case TADGL_POPMATRIX:    glPopMatrix();    break;
		case TADGL_LOADMATRIX:  { float m[16]; ring_get(m,64); glLoadMatrixf(m); break; }
		case TADGL_MULTMATRIX:  { float m[16]; ring_get(m,64); glMultMatrixf(m); break; }
		case TADGL_ORTHO:       { float v[6]; ring_get(v,24);
		                          glOrtho(v[0],v[1],v[2],v[3],v[4],v[5]); break; }
		case TADGL_FRUSTUM:     { float v[6]; ring_get(v,24);
		                          glFrustum(v[0],v[1],v[2],v[3],v[4],v[5]); break; }
		case TADGL_TRANSLATE:   { float v[3]; ring_get(v,12); glTranslatef(v[0],v[1],v[2]); break; }
		case TADGL_SCALE:       { float v[3]; ring_get(v,12); glScalef(v[0],v[1],v[2]); break; }
		case TADGL_ROTATE:      { float v[4]; ring_get(v,16); glRotatef(v[0],v[1],v[2],v[3]); break; }

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
		case TADGL_TEXPARAM:    { unsigned int v[2]; ring_get(v,8);
		                          glTexParameteri(GL_TEXTURE_2D, v[0], (GLint)v[1]); break; }
		case TADGL_DELETETEXTURE:{ unsigned int n; ring_get(&n,4);
		                          if (n < MAX_TEX && g_tex[n]) {
		                              glDeleteTextures(1, &g_tex[n]); g_tex[n] = 0; }
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
