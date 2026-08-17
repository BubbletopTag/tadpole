/* Tadpole — guest-side GL command encoder for host-GPU replay (HLE).
 *
 * Compiled INTO libGLESv1_CM.so alongside the software rasteriser. The entry
 * points in tadpole_gles_core.c call hle_on() first and, when it is true, encode
 * a packet and return instead of rasterising. See tadpole_glcmd.h for the wire
 * format and for why serialising is the only way to reach the host from here.
 *
 * DESIGN CONSTRAINTS THAT ARE NOT OBVIOUS
 * ---------------------------------------
 * 1. NEVER BLOCK FOREVER. If the encoder waited for space it would wedge on any
 *    host that stopped reading. hle_on() therefore requires a live host
 *    heartbeat and gives up the moment the ring backs up with no reader.
 *    Giving up no longer means quietly rendering the rest of the session in
 *    software: it raises guest_fellback so the viewer can put a dialog on
 *    screen — see the deprecation note above hle_software_wanted(). A run that
 *    genuinely wants no viewer, which is every scripted harness under
 *    `--no-viewer`, asks for the software rasteriser by name with
 *    TADPOLE_GL_SOFTWARE=1.
 *
 * 2. NO POINTERS ON THE WIRE. A guest address is meaningless to the host, so
 *    array references travel as (buffer name, byte offset) and everything else
 *    travels by value.
 *
 * 3. FIXED POINT STAYS OUT OF THE STREAM. GLES 1.x is full of GLfixed because
 *    the platform is soft-float, but desktop GL has no GL_FIXED at all. Matrices
 *    and scalars are converted to float here; vertex ARRAYS keep their declared
 *    type and are converted on the host, which is the only side that knows how
 *    many vertices a draw actually touches.
 */
#include "tadpole_glcmd.h"

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef int            i32;

#define NULL ((void *)0)

extern int   open(const char *path, int flags, ...);
extern void *mmap(void *addr, u32 len, int prot, int flags, int fd, long off);
extern int   close(int fd);
extern void *memcpy(void *d, const void *s, u32 n);
extern char *getenv(const char *name);
extern int   snprintf(char *s, u32 n, const char *fmt, ...);
extern long  write(int fd, const void *buf, u32 n);
extern int   ftruncate(int fd, long length);
extern int   usleep(u32 usec);
extern void  abort(void);

#define O_RDWR   02
#define O_CREAT  0100
#define PROT_RW  3
#define MAP_SHARED 1

static struct tadgl_hdr *g_ring;
static u8  *g_data;
static int  g_state = -1;        /* -1 unknown, 0 disabled, 1 encoding */
static u32  g_stalls;

static void hle_log(const char *msg)
{
	char b[192];
	int n = snprintf(b, sizeof(b), "[hle] %s\n", msg);
	if (n > 0) write(2, b, (u32)n);
}

/* ---- the software rasteriser is deprecated ------------------------------
 *
 * It is years behind the GL core it shares, it is roughly five times slower,
 * and it silently cannot do things the host path does — it samples ONE texture
 * unit, and it ignores the blend factors entirely. Both of those cost a real
 * bug this month: Ben 10's menu text drew black and Pet Pals 2 wore its eye
 * atlas as skin, and NEITHER was visible in software, because in software the
 * wrong state cannot express itself. Every compat verdict in this repository
 * was recorded on that path.
 *
 * So falling back to it is no longer something that happens quietly to a user
 * who did not ask. If HLE was requested and cannot be delivered, the guest
 * says so and raises a flag the viewer turns into a dialog, rather than
 * pretending. Software rasterisation remains available for exactly the case it
 * is still good for — telling whether a fault is in the shared GL core or only
 * in the replay — and now requires being asked for by name:
 *
 *     TADPOLE_GL_SOFTWARE=1
 *
 * which is checked FIRST, so it also forces software when a host is available
 * and would otherwise win.
 */
static int hle_software_wanted(void)
{
	const char *v = getenv("TADPOLE_GL_SOFTWARE");
	return v && v[0] && v[0] != '0';
}

/* THE GPU PATH IS GONE. Say so, and tell the HOST so it can tell the user.
 *
 * NOT an abort. Killing the guest here would take the title down mid-frame and
 * leave a crash report describing a fault the title never committed — and the
 * user, whose save is in that process, would have no idea why. The honest
 * outcome is to stop pretending we are rendering, raise the flag the host
 * already watches (guest_fellback, read by hle_guest_fell_back()), and let the
 * viewer put a dialog on screen asking for a restart.
 *
 * The guest keeps running. Whatever it draws after this point comes from the
 * deprecated software rasteriser and should not be trusted — which is exactly
 * what the dialog says. */
static void hle_lost(const char *why)
{
	if (g_ring) g_ring->guest_fellback = 1;
	hle_log("============================================================");
	hle_log("HOST-GPU REPLAY IS NO LONGER AVAILABLE");
	hle_log(why);
	hle_log("");
	hle_log("The software rasteriser is deprecated: it cannot express");
	hle_log("multitexturing or the blend factors, so anything drawn from");
	hle_log("here on is wrong rather than merely slow. Restart Tadpole.");
	hle_log("");
	hle_log("To use the software rasteriser deliberately, ask for it:");
	hle_log("    TADPOLE_GL_SOFTWARE=1");
	hle_log("============================================================");
	/* THE DEVELOPER'S OPT-IN, still honoured — Options -> Graphics -> "Stop if
	 * HLE falls back" sets it. Default is to tell the user and let the title
	 * carry on, because their unsaved progress is in this process; this is for
	 * when you would rather have the core file and the stack that names the
	 * caller than a running guest. */
	if (getenv("TADPOLE_HLE_STRICT")) {
		hle_log("TADPOLE_HLE_STRICT set: aborting");
		abort();
		*(volatile int *)0 = 0;
	}
}

/* ---- attach ------------------------------------------------------------- */

static void hle_attach(void)
{
	char path[512];
	const char *d;
	int fd;
	void *m;

	g_state = 0;
	/* Asked for by name: software, quietly, exactly as before. */
	if (hle_software_wanted()) {
		hle_log("TADPOLE_GL_SOFTWARE set: using the software rasteriser");
		return;
	}
	/* NOT REQUESTED AT ALL is left alone deliberately. A standalone guest
	 * binary loads this shim without ever asking for replay — tools/glconform
	 * is the one that matters, and it tests state and error codes, not
	 * rendering. Only a request that CANNOT BE HONOURED raises the alarm, and
	 * every real launch makes one: tadpole.sh and the viewer both set it. */
	if (!getenv("TADPOLE_GL_HLE"))
		return;

	d = getenv("TADPOLE_DIR");
	if (!d) d = "/tmp/tadpole";
	snprintf(path, sizeof(path), "%s/glcmd.bin", d);

	fd = open(path, O_RDWR | O_CREAT, 0666);
	if (fd < 0) { hle_lost("cannot open glcmd.bin in TADPOLE_DIR"); return; }
	ftruncate(fd, (long)TADGL_FILE_BYTES);
	m = mmap(NULL, TADGL_FILE_BYTES, PROT_RW, MAP_SHARED, fd, 0);
	close(fd);
	if (m == (void *)-1) { hle_lost("cannot map glcmd.bin"); return; }

	g_ring = m;
	g_data = TADGL_DATA(g_ring);

	/* The HOST owns initialisation: it is the one that knows it can actually
	 * replay. If the header is not stamped, no viewer has attached and there is
	 * no point encoding into a ring nobody reads. */
	if (g_ring->magic != TADGL_MAGIC || g_ring->version != TADGL_VERSION) {
		hle_lost("no host replayer has stamped the command ring "
		         "(is the viewer running?)");
		g_ring = NULL;
		return;
	}
	if (!g_ring->host_alive) {
		hle_lost("the host replayer is not alive");
		g_ring = NULL;
		return;
	}
	g_state = 1;
	hle_log("encoding to host GPU");
}

int hle_on(void)
{
	if (g_state < 0)
		hle_attach();
	return g_state == 1;
}

/* Has the host asked for its mirror to be rebuilt? Consumes the request. */
int hle_want_resync(void)
{
	if (g_state != 1 || !g_ring || !g_ring->want_resync)
		return 0;
	g_ring->want_resync = 0;
	hle_log("host asked for a state resync");
	return 1;
}

/* Abandon GPU rendering MID-RUN — the host stopped draining, or a single packet
 * will not fit the ring.
 *
 * This used to drop to software and merely say so, which looks identical to
 * "HLE never worked" and is how a 17-second success followed by a stall got
 * reported as broken. TADPOLE_HLE_STRICT=1 was the opt-in that made it fatal.
 *
 * IT NOW TELLS THE USER INSTEAD. Continuing in software renders the rest of the
 * session with a rasteriser that cannot express multitexturing or blend
 * factors, so the frames after the fallback are not slower-but-right, they are
 * WRONG — and nothing on screen said so. Aborting was the other extreme: it
 * would take the title, and the player's unsaved progress, down with it. So
 * raise the flag and let the viewer ask for a restart.
 *
 * TADPOLE_GL_SOFTWARE=1 still means software, and a session that asked for it
 * never gets here: hle_attach() returns before any of this. */
static void hle_give_up(const char *why)
{
	g_state = 0;
	hle_lost(why);            /* raises the flag while the ring is still mapped */
	g_ring = NULL;
}

/* Is the host still making progress? The heartbeat is a counter, so a value that
 * has not moved across the whole wait means the viewer is gone — whereas a slow
 * frame still advances it. */
static int host_progressing(u32 *last_beat, int *stuck)
{
	u32 beat = g_ring ? g_ring->host_alive : 0;
	if (beat != *last_beat) { *last_beat = beat; *stuck = 0; return 1; }
	/* ~10s. Sized for the worst BLOCKING operation the viewer performs, not for
	 * a slow frame: rotating the display calls SDL_SetWindowSize, which does a
	 * window-manager round-trip and was measured stalling the pump for 4808 ms.
	 * The owner found it by rotating mid-game — HLE fell back every time. */
	return ++(*stuck) < 20000;
}

/* ---- packet writing ----------------------------------------------------- */

static u32 ring_free(void)
{
	u32 used = g_ring->head - g_ring->tail;   /* unsigned wrap is intended */
	return (used >= TADGL_RING) ? 0 : (TADGL_RING - used);
}

/* PUBLISH THE BYTES BEFORE THE COUNTER.
 *
 * The host reads this ring concurrently, and it decides how much is valid purely
 * from `head`. If the store to head becomes visible before the payload does, the
 * host reads whatever was in the ring previously, treats it as a packet header,
 * and advances tail by a garbage length — after which head - tail underflows
 * (both unsigned) and tail runs away. That is precisely the first live failure:
 * tail 2634025991 against a head of 84, which the guest then saw as "host
 * stopped draining".
 *
 * volatile alone does not order a volatile store against the preceding non-
 * volatile memcpy, so make it explicit. */
static void ring_put(const void *src, u32 n)
{
	u32 off = g_ring->head % TADGL_RING;
	u32 first = TADGL_RING - off;
	if (first > n) first = n;
	memcpy(g_data + off, src, first);
	if (n > first)
		memcpy(g_data, (const u8 *)src + first, n - first);
	__sync_synchronize();               /* data, THEN the counter */
	g_ring->head += n;
}

/* Reserve space for one packet, waiting only as long as the host is visibly
 * making progress. Returns 0 if HLE has been abandoned. */
static int pkt_begin(u32 op, u32 payload)
{
	u32 need = (u32)sizeof(struct tadgl_pkt) + ((payload + 3u) & ~3u);
	struct tadgl_pkt p;
	int spins = 0;

	if (!g_ring) return 0;
	/* Stated against the payload, using the constant the HOST validates with,
	 * so the two ends cannot drift apart — they did, and it cost a white
	 * screen. See TADGL_MAX_PAYLOAD. */
	if (payload > TADGL_MAX_PAYLOAD) {
		hle_give_up("packet larger than the ring; using software raster");
		return 0;
	}
	{
		u32 beat = g_ring->host_alive;
		while (ring_free() < need) {
			__sync_synchronize();       /* re-read the host's cursors */
			usleep(500);
			if (!host_progressing(&beat, &spins)) {
				hle_give_up("host stopped draining the command ring");
				return 0;
			}
			g_stalls++;
		}
	}
	p.op = (unsigned short)op;
	p.pad = 0;
	p.len = payload;
	ring_put(&p, sizeof p);
	return 1;
}

/* Payload padding, so the next header lands 4-byte aligned. */
static void pkt_pad(u32 payload)
{
	static const u8 zero[4] = { 0, 0, 0, 0 };
	u32 slack = ((payload + 3u) & ~3u) - payload;
	if (slack) ring_put(zero, slack);
}

static void enc0(u32 op)
{
	if (pkt_begin(op, 0)) pkt_pad(0);
}

static void enc_u32(u32 op, const u32 *v, u32 count)
{
	u32 n = count * 4u;
	if (pkt_begin(op, n)) { ring_put(v, n); pkt_pad(n); }
}

static void enc_blob(u32 op, const u32 *hdr, u32 hdrcount,
                     const void *blob, u32 blobn)
{
	u32 n = hdrcount * 4u + blobn;
	if (!pkt_begin(op, n)) return;
	ring_put(hdr, hdrcount * 4u);
	if (blobn) ring_put(blob, blobn);
	pkt_pad(n);
}

/* ---- the calls the core forwards --------------------------------------- */

void hle_present(void)
{
	if (!g_ring) return;
	enc0(TADGL_PRESENT);
	if (!g_ring) return;
	g_ring->frames_sent++;
	/* Wait for the host to finish this frame. This is what keeps the guest one
	 * frame ahead at most; without it the guest would run away and the ring
	 * would only ever be full. It also paces the guest the same way the panel
	 * used to, so pace_frame() has nothing left to do in HLE mode. */
	{
		u32 want = g_ring->frames_sent;
		u32 beat = g_ring->host_alive;
		int spins = 0;
		while (g_ring->frames_done < want) {
			usleep(500);
			/* A SLOW FRAME IS NOT A REASON TO ABANDON GPU RENDERING. Loading a
			 * title floods the ring with texture uploads and the host can take
			 * well over a second to work through them; the old code treated
			 * that as a dead host and disabled HLE permanently, which is why a
			 * run would render on the GPU for ~17s and then crawl. Only a
			 * heartbeat that has stopped entirely counts as dead. */
			if (!host_progressing(&beat, &spins)) {
				hle_give_up("host heartbeat stopped while awaiting a frame");
				return;
			}
		}
	}
}

/* Test seam: encode PRESENT without waiting for the host.
 *
 * hle_present() blocks until the host has replayed the frame, which is correct
 * in the emulator (it paces the guest) but makes a SINGLE-THREADED test
 * impossible — the test would block before it could pump. GL contexts are
 * per-thread, so the replayer cannot simply be moved to another thread either.
 * viewer/hle_selftest.c uses this instead. */
void hle_present_nowait(void)
{
	if (!g_ring) return;
	enc0(TADGL_PRESENT);
	if (g_ring) g_ring->frames_sent++;
}

void hle_clear(u32 mask, u32 argb, float depth)
{ u32 v[3]; v[0]=mask; v[1]=argb; memcpy(&v[2], &depth, 4); enc_u32(TADGL_CLEAR, v, 3); }

void hle_viewport(i32 x, i32 y, i32 w, i32 h)
{ u32 v[4]; v[0]=(u32)x; v[1]=(u32)y; v[2]=(u32)w; v[3]=(u32)h;
  enc_u32(TADGL_VIEWPORT, v, 4); }

void hle_enable(u32 cap)  { enc_u32(TADGL_ENABLE,  &cap, 1); }
void hle_disable(u32 cap) { enc_u32(TADGL_DISABLE, &cap, 1); }

void hle_blendfunc(u32 s, u32 d) { u32 v[2]={s,d}; enc_u32(TADGL_BLENDFUNC, v, 2); }
void hle_depthfunc(u32 f)        { enc_u32(TADGL_DEPTHFUNC, &f, 1); }
void hle_depthmask(u32 on)       { enc_u32(TADGL_DEPTHMASK, &on, 1); }
void hle_cullface(u32 m)         { enc_u32(TADGL_CULLFACE, &m, 1); }
void hle_frontface(u32 m)        { enc_u32(TADGL_FRONTFACE, &m, 1); }
void hle_shademodel(u32 m)       { enc_u32(TADGL_SHADEMODEL, &m, 1); }

void hle_alphafunc(u32 f, float ref)
{ u32 v[2]; v[0]=f; memcpy(&v[1], &ref, 4); enc_u32(TADGL_ALPHAFUNC, v, 2); }

void hle_texenv(u32 target, u32 pname, i32 value)
{ u32 v[3]={target,pname,(u32)value}; enc_u32(TADGL_TEXENV, v, 3); }

/* GL_TEXTURE_ENV_COLOR only. Its own opcode because it is the one TexEnv
 * parameter that is a vector; everything else is an enum and rides TADGL_TEXENV
 * as a single i32. */
void hle_texenvcolor(const float *rgba)
{ enc_u32(TADGL_TEXENVCOLOR, (const u32 *)rgba, 4); }

void hle_scissor(i32 x, i32 y, i32 w, i32 h)
{ u32 v[4]={(u32)x,(u32)y,(u32)w,(u32)h}; enc_u32(TADGL_SCISSOR, v, 4); }

void hle_colormask(u32 r, u32 g, u32 b, u32 a)
{ u32 v[4]={r,g,b,a}; enc_u32(TADGL_COLORMASK, v, 4); }

void hle_linewidth(float w)  { enc_u32(TADGL_LINEWIDTH,  (const u32 *)&w, 1); }
void hle_pointsize(float s)  { enc_u32(TADGL_POINTSIZE,  (const u32 *)&s, 1); }

void hle_polygonoffset(float factor, float units)
{ float v[2]={factor,units}; enc_u32(TADGL_POLYGONOFFSET, (const u32 *)v, 2); }

/* Lighting. The header travels as u32 and the values as float in the same
 * packet, which enc_blob already does — the alternative, two packets, could be
 * split by a ring wrap and read as one. */
void hle_light(u32 light, u32 pname, const float *v, u32 count)
{ u32 hd[3]={light,pname,count};
  enc_blob(TADGL_LIGHT, hd, 3, v, count * 4u); }

void hle_material(u32 face, u32 pname, const float *v, u32 count)
{ u32 hd[3]={face,pname,count};
  enc_blob(TADGL_MATERIAL, hd, 3, v, count * 4u); }

void hle_lightmodel(u32 pname, const float *v, u32 count)
{ u32 hd[2]={pname,count};
  enc_blob(TADGL_LIGHTMODEL, hd, 2, v, count * 4u); }

void hle_normal(float x, float y, float z)
{ float v[3]={x,y,z}; enc_u32(TADGL_NORMAL, (const u32 *)v, 3); }

void hle_color(float r, float g, float b, float a)
{ float v[4]={r,g,b,a}; enc_u32(TADGL_COLOR, (const u32 *)v, 4); }

void hle_matrixmode(u32 m) { enc_u32(TADGL_MATRIXMODE, &m, 1); }
/* Tell the host to drop every mirrored texture, buffer and array. Sent when the
 * guest tears its context down, so a recycled texture name in the next title
 * cannot resolve to the previous title's image. */
void hle_reset(void) { if (g_ring) enc0(TADGL_RESET); }

void hle_loadidentity(void){ enc0(TADGL_LOADIDENTITY); }
void hle_pushmatrix(void)  { enc0(TADGL_PUSHMATRIX); }
void hle_popmatrix(void)   { enc0(TADGL_POPMATRIX); }

void hle_loadmatrix(const float *m) { enc_u32(TADGL_LOADMATRIX, (const u32 *)m, 16); }
void hle_multmatrix(const float *m) { enc_u32(TADGL_MULTMATRIX, (const u32 *)m, 16); }

void hle_ortho(float l, float r, float b, float t, float n, float f)
{ float v[6]={l,r,b,t,n,f}; enc_u32(TADGL_ORTHO, (const u32 *)v, 6); }
void hle_frustum(float l, float r, float b, float t, float n, float f)
{ float v[6]={l,r,b,t,n,f}; enc_u32(TADGL_FRUSTUM, (const u32 *)v, 6); }

void hle_translate(float x, float y, float z)
{ float v[3]={x,y,z}; enc_u32(TADGL_TRANSLATE, (const u32 *)v, 3); }
void hle_scale(float x, float y, float z)
{ float v[3]={x,y,z}; enc_u32(TADGL_SCALE, (const u32 *)v, 3); }
void hle_rotate(float a, float x, float y, float z)
{ float v[4]={a,x,y,z}; enc_u32(TADGL_ROTATE, (const u32 *)v, 4); }

void hle_bindtexture(u32 name)   { enc_u32(TADGL_BINDTEXTURE, &name, 1); }
void hle_activetexture(u32 unit) { enc_u32(TADGL_ACTIVETEXTURE, &unit, 1); }
void hle_deletetexture(u32 name) { enc_u32(TADGL_DELETETEXTURE, &name, 1); }
void hle_texparam(u32 pname, i32 v)
{ u32 a[2]={pname,(u32)v}; enc_u32(TADGL_TEXPARAM, a, 2); }

/* Textures cross already converted to ARGB8888 — the core's upload path has
 * done the format work, and sending one canonical layout keeps every pixel
 * format decision on the guest side where it is already tested. */
void hle_teximage2d(u32 name, u32 w, u32 h, const u32 *argb)
{ u32 hd[3]={name,w,h}; enc_blob(TADGL_TEXIMAGE2D, hd, 3, argb, w*h*4u); }

void hle_texsubimage2d(u32 name, u32 x, u32 y, u32 w, u32 h, const u32 *argb)
{ u32 hd[5]={name,x,y,w,h}; enc_blob(TADGL_TEXSUBIMAGE2D, hd, 5, argb, w*h*4u); }

void hle_bufferdata(u32 name, u32 size, const void *data)
{ u32 hd[2]={name,size}; enc_blob(TADGL_BUFFERDATA, hd, 2, data, data ? size : 0); }

void hle_buffersubdata(u32 name, u32 off, u32 size, const void *data)
{ u32 hd[3]={name,off,size}; enc_blob(TADGL_BUFFERSUBDATA, hd, 3, data, size); }

void hle_deletebuffer(u32 name) { enc_u32(TADGL_DELETEBUFFER, &name, 1); }

void hle_arraypointer(u32 which, u32 buf, i32 size, u32 type, i32 stride, u32 off)
{ u32 v[6]={which,buf,(u32)size,type,(u32)stride,off};
  enc_u32(TADGL_ARRAYPOINTER, v, 6); }

void hle_clientstate(u32 which, u32 on)
{ u32 v[2]={which,on}; enc_u32(TADGL_CLIENTSTATE, v, 2); }

void hle_drawarrays(u32 mode, i32 first, i32 count)
{ u32 v[3]={mode,(u32)first,(u32)count}; enc_u32(TADGL_DRAWARRAYS, v, 3); }

void hle_drawelements(u32 mode, i32 count, u32 type, u32 elembuf, u32 off)
{ u32 v[5]={mode,(u32)count,type,elembuf,off}; enc_u32(TADGL_DRAWELEMENTS, v, 5); }
