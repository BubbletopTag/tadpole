/* Tadpole — end-to-end self-test for host-GPU replay.
 *
 * Drives the REAL encoder (shim/tadpole_gles_hle.c) and the REAL replayer
 * (tadpole_hle.c) in one process, with no guest and no visible window, then
 * checks the pixels that come back.
 *
 *   make hle-selftest && ./viewer/hle-selftest
 *
 * WHY IT EXISTS. The first live run failed in two ways that were awkward to
 * diagnose from inside the emulator:
 *
 *   - SDL_GL_CreateContext steals "current", so our glViewport leaked into
 *     SDL_Renderer's context and squeezed the entire viewer UI into a corner.
 *   - one bad packet made the host's tail overrun head; head - tail is unsigned,
 *     so it wrapped to ~4 billion and ran away to 2.6e9 against a head of 84.
 *     The guest saw that as "host stopped draining" and fell back to software.
 *
 * Neither needed the guest to reproduce. Testing both halves of the protocol
 * against each other catches that class of bug in a second rather than a
 * five-minute boot, and it does not need the owner's screen.
 *
 * Both sides are compiled from the shipping sources, so a change to the wire
 * format that only lands on one side fails here.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../shim/tadpole_glcmd.h"
#include "tadpole_hle.h"

#define W 480
#define H 272

/* The encoder's interface. Declared here rather than in a header because the
 * guest side has no header — the core reaches it through externs too. */
extern int  hle_on(void);
extern void hle_present_nowait(void);
extern void hle_clear(unsigned mask, unsigned argb, float depth);
extern void hle_viewport(int x, int y, int w, int h);
extern void hle_enable(unsigned cap);
extern void hle_disable(unsigned cap);
extern void hle_matrixmode(unsigned m);
extern void hle_loadidentity(void);
extern void hle_ortho(float l, float r, float b, float t, float n, float f);
extern void hle_color(float r, float g, float b, float a);
extern void hle_bindtexture(unsigned name);
extern void hle_teximage2d(unsigned name, unsigned w, unsigned h, const unsigned *argb);
extern void hle_texparam(unsigned pname, int v);
extern void hle_bufferdata(unsigned name, unsigned size, const void *data);
extern void hle_arraypointer(unsigned which, unsigned buf, int size,
                             unsigned type, int stride, unsigned off);
extern void hle_clientstate(unsigned which, unsigned on);
extern void hle_drawelements(unsigned mode, int count, unsigned type,
                             unsigned elembuf, unsigned off);

#define GL_TRIANGLES_      0x0004
#define GL_TEXTURE_2D_     0x0DE1
#define GL_PROJECTION_     0x1701
#define GL_MODELVIEW_      0x1700
#define GL_UNSIGNED_SHORT_ 0x1403
#define GL_FIXED_          0x140C
#define GL_NEAREST_        0x2600
#define TEX_MIN_FILTER     0x2801
#define TEX_MAG_FILTER     0x2800
#define COLOUR_BIT         0x4000
#define DEPTH_BIT          0x0100

static int fails;

static void check(int ok, const char *what)
{
	printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) fails++;
}

/* ---- --bench: what a render scale actually costs -------------------------
 *
 * "How far can it scale" has two answers and only one of them is interesting.
 * The driver's limit is enormous — this card accepts 16x, which is 7680x4352
 * for a 480x272 panel — so the real ceiling is the per-frame work: rendering
 * N*N times the pixels, resolving them, and above all TRANSFERRING the result
 * back across the bus, which grows with N*N too.
 *
 * This drives the real replayer with a real frame at each scale and times it,
 * including the full-size readback the viewer does at Tier 3. No guest, no
 * game, no home screen to navigate — just the part that scales.
 */
static void bench_frame(void)
{
	static const int   verts[8] = { 40 << 16,  40 << 16, 440 << 16,  40 << 16,
	                                440 << 16, 232 << 16,  40 << 16, 232 << 16 };
	static const int   uvs[8]   = { 0, 0, 1 << 16, 0, 1 << 16, 1 << 16, 0, 1 << 16 };
	hle_viewport(0, 0, W, H);
	hle_clear(COLOUR_BIT | DEPTH_BIT, 0xFF0D2113u, 1.0f);
	hle_bindtexture(1);
	hle_enable(GL_TEXTURE_2D_);
	hle_color(1, 1, 1, 1);
	hle_clientstate(TADGL_ARR_VERTEX, 1);
	hle_clientstate(TADGL_ARR_TEXCOORD, 1);
	hle_arraypointer(TADGL_ARR_VERTEX,   1, 2, GL_FIXED_, 0, 0);
	hle_arraypointer(TADGL_ARR_TEXCOORD, 2, 2, GL_FIXED_, 0, 0);
	hle_drawelements(GL_TRIANGLES_, 6, GL_UNSIGNED_SHORT_, 3, 0);
	hle_present_nowait();
	(void)verts; (void)uvs;
}

static int bench(void)
{
	static const int scales[] = { 1, 2, 3, 4, 6, 8 };
	static unsigned int fb[W * H];
	unsigned int *big = NULL;
	size_t bigsz = 0;
	int i, k;

	printf("\n  scale     draw buffer     ms/frame   equivalent fps   readback\n");
	for (i = 0; i < (int)(sizeof scales / sizeof *scales); i++) {
		int ss = scales[i], fw = 0, fh = 0;
		Uint32 t0, t1;
		int frames = 30;

		hle_host_set_quality(0, ss);
		if (hle_host_scale() != ss) {
			printf("  %2dx       unavailable\n", ss);
			continue;
		}
		hle_host_want_full(1);
		hle_host_full(&fw, &fh);
		if ((size_t)fw * fh * 4 > bigsz) {
			free(big);
			bigsz = (size_t)fw * fh * 4;
			big = malloc(bigsz);
		}
		/* one warm frame, so texture upload and target creation are not
		 * counted as if they happened every frame */
		bench_frame(); hle_host_pump(fb, W); hle_host_read_full(big);

		t0 = SDL_GetTicks();
		for (k = 0; k < frames; k++) {
			bench_frame();
			hle_host_pump(fb, W);
			hle_host_read_full(big);
		}
		t1 = SDL_GetTicks();
		{
			double ms = (double)(t1 - t0) / frames;
			printf("  %2dx    %5dx%-5d      %6.2f       %6.0f       %5.1f MB\n",
			       ss, W * ss, H * ss, ms, ms > 0 ? 1000.0 / ms : 9999.0,
			       (double)fw * fh * 4 / (1024 * 1024));
		}
	}
	hle_host_want_full(0);
	hle_host_set_quality(0, 1);
	free(big);
	return 0;
}

int main(int argc, char **argv)
{
	static unsigned int fb[W * H];
	unsigned int tex[4];
	int  verts[8];              /* 16.16 fixed point, as the titles use */
	int  uvs[8];
	unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
	unsigned long frames, packets;
	char dir[] = "/tmp/tadpole-hle-selftest";

	setvbuf(stdout, NULL, _IONBF, 0);
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		printf("FAIL SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	mkdir(dir, 0755);
	setenv("TADPOLE_DIR", dir, 1);
	setenv("TADPOLE_GL_HLE", "1", 1);
	unlink("/tmp/tadpole-hle-selftest/glcmd.bin");

	/* Host first: the guest refuses to encode without a live heartbeat. */
	check(hle_host_init(dir, W, H, 0, 1), "host replayer init");
	if (!hle_host_ready()) { printf("FAIL cannot continue\n"); return 1; }
	check(hle_on(), "guest encoder attaches to the ring");
	if (!hle_on()) { printf("FAIL cannot continue\n"); return 1; }

	/* ---- a frame, encoded exactly the way the core forwards one --------- */
	tex[0] = 0xFFFF0000u;       /* ARGB: red    */
	tex[1] = 0xFF00FF00u;       /*       green  */
	tex[2] = 0xFF0000FFu;       /*       blue   */
	tex[3] = 0xFFFFFF00u;       /*       yellow */

	/* A quad covering the middle of the panel, in 16.16 like the real titles.
	 * This is the GL_FIXED conversion path, which desktop GL cannot do itself. */
	verts[0] =  40 << 16; verts[1] =  40 << 16;
	verts[2] = 440 << 16; verts[3] =  40 << 16;
	verts[4] = 440 << 16; verts[5] = 232 << 16;
	verts[6] =  40 << 16; verts[7] = 232 << 16;
	uvs[0] = 0;        uvs[1] = 0;
	uvs[2] = 1 << 16;  uvs[3] = 0;
	uvs[4] = 1 << 16;  uvs[5] = 1 << 16;
	uvs[6] = 0;        uvs[7] = 1 << 16;

	hle_viewport(0, 0, W, H);
	hle_clear(COLOUR_BIT | DEPTH_BIT, 0xFF0D2113u, 1.0f);   /* dark green */

	hle_matrixmode(GL_PROJECTION_);
	hle_loadidentity();
	hle_ortho(0, W, H, 0, -1, 1);
	hle_matrixmode(GL_MODELVIEW_);
	hle_loadidentity();

	hle_bindtexture(1);
	hle_teximage2d(1, 2, 2, tex);
	hle_texparam(TEX_MIN_FILTER, GL_NEAREST_);
	hle_texparam(TEX_MAG_FILTER, GL_NEAREST_);
	hle_enable(GL_TEXTURE_2D_);
	hle_color(1, 1, 1, 1);

	hle_bufferdata(1, sizeof verts, verts);
	hle_bufferdata(2, sizeof uvs,   uvs);
	hle_bufferdata(3, sizeof idx,   idx);

	hle_clientstate(TADGL_ARR_VERTEX, 1);
	hle_clientstate(TADGL_ARR_TEXCOORD, 1);
	hle_arraypointer(TADGL_ARR_VERTEX,   1, 2, GL_FIXED_, 0, 0);
	hle_arraypointer(TADGL_ARR_TEXCOORD, 2, 2, GL_FIXED_, 0, 0);
	hle_drawelements(GL_TRIANGLES_, 6, GL_UNSIGNED_SHORT_, 3, 0);
	hle_present_nowait();

	memset(fb, 0xAB, sizeof fb);              /* poison, so "unwritten" shows */
	check(hle_host_pump(fb, W) == 1, "host replays the frame and presents");
	check(hle_host_desyncs() == 0, "no protocol desync");

	hle_host_stats(&frames, &packets);
	printf("  replayed %lu frame(s), %lu packet(s)\n", frames, packets);
	check(frames == 1, "exactly one frame completed");

	/* ---- did it draw what we asked? ------------------------------------ */
	{
		unsigned int corner = fb[4 * W + 4];        /* outside the quad */
		unsigned int centre = fb[(H / 2) * W + (W / 2)];
		printf("  corner %08X (want the clear colour 0D2113)\n", corner & 0xFFFFFF);
		printf("  centre %08X (want one of the four texel colours)\n", centre & 0xFFFFFF);
		check((corner & 0xFFFFFF) == 0x0D2113u, "clear colour reached the framebuffer");
		check(centre != 0xABABABABu, "the quad wrote pixels (not poison)");
		check((centre & 0xFFFFFF) != (corner & 0xFFFFFF),
		      "quad differs from background — GL_FIXED verts converted");
		{
			unsigned int c = centre & 0xFFFFFF;
			check(c == 0xFF0000u || c == 0x00FF00u ||
			      c == 0x0000FFu || c == 0xFFFF00u,
			      "centre is a texel colour — texture path works");
		}
	}

	/* ---- anti-aliasing, switched on while the replayer is LIVE ----------
	 *
	 * Changing the sample count deletes the framebuffer object and its
	 * attachments and builds new ones underneath a running replay. If that
	 * teardown is wrong the failure is silent and total — every later frame
	 * comes out black, or lands in a default viewport — and it would only be
	 * noticed by someone toggling a settings row mid-game. So toggle it here
	 * and draw the same frame again.
	 */
	hle_host_set_quality(4, 1);
	if (hle_host_msaa() == 0) {
		printf("  (no multisampling on this driver — live-toggle test skipped)\n");
	} else {
		printf("  AA switched to %dx mid-session\n", hle_host_msaa());

		hle_viewport(0, 0, W, H);
		hle_clear(COLOUR_BIT | DEPTH_BIT, 0xFF0D2113u, 1.0f);
		hle_bindtexture(1);
		hle_enable(GL_TEXTURE_2D_);
		hle_color(1, 1, 1, 1);
		hle_clientstate(TADGL_ARR_VERTEX, 1);
		hle_clientstate(TADGL_ARR_TEXCOORD, 1);
		hle_arraypointer(TADGL_ARR_VERTEX,   1, 2, GL_FIXED_, 0, 0);
		hle_arraypointer(TADGL_ARR_TEXCOORD, 2, 2, GL_FIXED_, 0, 0);
		hle_drawelements(GL_TRIANGLES_, 6, GL_UNSIGNED_SHORT_, 3, 0);
		hle_present_nowait();

		memset(fb, 0xAB, sizeof fb);
		check(hle_host_pump(fb, W) == 1, "replays a frame after the AA change");
		check(hle_host_desyncs() == 0, "no desync from rebuilding the target");
		{
			unsigned int corner = fb[4 * W + 4];
			unsigned int centre = fb[(H / 2) * W + (W / 2)];
			printf("  corner %08X  centre %08X (multisampled)\n",
			       corner & 0xFFFFFF, centre & 0xFFFFFF);
			check((corner & 0xFFFFFF) == 0x0D2113u,
			      "clear colour survives the resolve");
			check(centre != 0xABABABABu, "the quad still draws (not poison)");
			{
				unsigned int c = centre & 0xFFFFFF;
				check(c == 0xFF0000u || c == 0x00FF00u ||
				      c == 0x0000FFu || c == 0xFFFF00u,
				      "texture path survives the resolve");
			}
		}
		/* And back off again, which is the other half of a toggle. */
		hle_host_set_quality(0, 1);
		check(hle_host_msaa() == 0, "AA switches back off");
	}

	/* ---- supersampling: draw big, filter down ---------------------------
	 *
	 * The draw buffer is now 3x the panel in each axis, so anything the
	 * replayer expresses in PIXELS has to be scaled with it — the viewport
	 * and the scissor box. Getting either wrong is not subtle: an unscaled
	 * viewport renders the scene into the bottom-left ninth of the buffer, and
	 * an unscaled scissor clips everything outside that same ninth. Both come
	 * back here as a corner that is no longer the clear colour, or a centre
	 * that is no longer a texel.
	 */
	hle_host_set_quality(0, 3);
	if (hle_host_scale() != 3) {
		printf("  (3x render scale unavailable here — skipped)\n");
	} else {
		printf("  render scale now %dx\n", hle_host_scale());

		hle_viewport(0, 0, W, H);
		hle_clear(COLOUR_BIT | DEPTH_BIT, 0xFF0D2113u, 1.0f);
		hle_bindtexture(1);
		hle_enable(GL_TEXTURE_2D_);
		hle_color(1, 1, 1, 1);
		hle_clientstate(TADGL_ARR_VERTEX, 1);
		hle_clientstate(TADGL_ARR_TEXCOORD, 1);
		hle_arraypointer(TADGL_ARR_VERTEX,   1, 2, GL_FIXED_, 0, 0);
		hle_arraypointer(TADGL_ARR_TEXCOORD, 2, 2, GL_FIXED_, 0, 0);
		hle_drawelements(GL_TRIANGLES_, 6, GL_UNSIGNED_SHORT_, 3, 0);
		hle_present_nowait();

		memset(fb, 0xAB, sizeof fb);
		check(hle_host_pump(fb, W) == 1, "replays a frame at 3x render scale");
		{
			unsigned int corner = fb[4 * W + 4];
			unsigned int centre = fb[(H / 2) * W + (W / 2)];
			printf("  corner %08X  centre %08X (supersampled)\n",
			       corner & 0xFFFFFF, centre & 0xFFFFFF);
			check((corner & 0xFFFFFF) == 0x0D2113u,
			      "clear colour survives the downscale — viewport scaled");
			check(centre != 0xABABABABu, "the quad still draws");
			{
				unsigned int c = centre & 0xFFFFFF;
				check(c == 0xFF0000u || c == 0x00FF00u ||
				      c == 0x0000FFu || c == 0xFFFF00u,
				      "centre is still a texel colour — scissor scaled");
			}
		}
		hle_host_set_quality(0, 1);
		check(hle_host_scale() == 1, "render scale returns to 1x");
	}

	if (argc > 1 && !strcmp(argv[1], "--bench")) {
		bench();
		hle_host_shutdown();
		SDL_Quit();
		return 0;
	}

	/* ---- the ring must end level ---------------------------------------- */
	{
		char path[600];
		FILE *f;
		unsigned int hdr[8];
		snprintf(path, sizeof(path), "%s/glcmd.bin", dir);
		f = fopen(path, "rb");
		if (f && fread(hdr, 4, 8, f) == 8) {
			printf("  ring head %u tail %u sent %u done %u\n",
			       hdr[3], hdr[4], hdr[5], hdr[6]);
			check(hdr[3] == hdr[4], "host consumed exactly what the guest wrote");
			check(hdr[5] == hdr[6], "frames sent == frames done");
		} else {
			check(0, "ring header readable");
		}
		if (f) fclose(f);
	}

	hle_host_shutdown();
	SDL_Quit();
	printf("%s\n", fails ? "FAIL" : "PASS host-GPU replay round-trips");
	return fails ? 1 : 0;
}
