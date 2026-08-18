/* Tadpole — host side of the LeapPad2 camera.
 *
 * WHY THE VIEWFINDER IS DRAWN HERE AND NOT IN THE SHIM
 *
 * The obvious place for it is the guest side, next to the V4L2 emulation, and
 * that is where it was first written. It cannot work there, and the reason is
 * a measurement rather than an opinion: on the VIP path the preview is not a
 * stream the application reads. CVIPCameraModule::StartVideoCapture points
 * VIDIOC_S_FBUF at a display surface, turns on VIDIOC_OVERLAY, and then the
 * hardware DMAs into the MLC's video plane while the application waits for a
 * button. With the viewfinder open on the tablet:
 *
 *     AppManager                        3% CPU
 *     state.bin vsync_count             13, 13, 13 — frozen
 *     cam0 n_qbuf=1 n_dqbuf=0           one buffer queued, none ever taken
 *
 * The guest issues no framebuffer ioctl of any kind, so a shim-side pump has
 * nothing to hang a clock on. The viewer has one: it is already running a
 * render loop, and on Android it is already the process the camera frames
 * arrive in. So the shim publishes WHERE the surface is — translating the
 * physical base out of S_FBUF into an offset in the arena both sides map — and
 * this file writes into it.
 *
 * THE PLANE LAYOUT is the video plane's, the same one blit_layer_yuv420() in
 * tadpole_view.c already reads, from lf2000fb.c's soc_dpc_set_vid_address:
 * with P the surface pitch, Y row y at y*P, Cb row y at y*P + P/2, and Cr row
 * y at (h/2 + y)*P + P/2. Luma in the first half of each row, chroma in the
 * second.
 *
 * THE STILL FALLBACK. $TADPOLE_DIR/camN.raw, one frame of packed I420 at the
 * negotiated size, is served when no backend has produced anything. It is what
 * the whole path was brought up against — the first working viewfinder was a
 * colour-bar test pattern pushed as a file — and it is what a user who has
 * refused the camera permission should see instead of a green rectangle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "tadpole_cam.h"

/* Mirrors the head of struct tadpole_state in tadpole_view.c. Only the camera
 * block is reached from here, and it is at the END of that struct, so rather
 * than duplicate the layer array this takes the offset from the caller. */
struct cam_ctx {
	struct tad_cam_state *cam;      /* &state->cam[0]                     */
	unsigned char        *arena;
	size_t                arena_bytes;
	char                  dir[512];

	struct {
		int            running;     /* platform backend started for this  */
		int            w, h;        /* what it was started for            */
		unsigned       frames;      /* submitted by the backend           */
		unsigned char *buf;         /* newest I420, own copy              */
		size_t         cap;
		int            bw, bh;
		int            still_tried; /* camN.raw looked for already        */
		int            failed;      /* platform start failed: back off    */
		int            retry;       /* pumps to wait before trying again  */
		unsigned char *still;
		size_t         still_len;
	} c[TAD_CAM_N];
};

static struct cam_ctx g;
static int g_ready;

/* The camera block sits at the end of struct tadpole_state; tadpole_view.c
 * passes the address of state->cam[0] rather than re-deriving the offset. */
void tad_cam_init(const char *dir, void *cam0, void *arena, size_t arena_bytes)
{
	if (g_ready)
		return;
	g_ready = 1;
	g.cam = (struct tad_cam_state *)cam0;
	g.arena = (unsigned char *)arena;
	g.arena_bytes = arena_bytes;
	snprintf(g.dir, sizeof(g.dir), "%s", dir ? dir : "");
}

/* ---- the viewfinder ----------------------------------------------------- */

static void preview_blit(int idx, const unsigned char *src, int sw, int sh)
{
	struct tad_cam_state *st = &g.cam[idx];
	unsigned dw = st->ov_w, dh = st->ov_h, P = st->ov_pitch, off = st->ov_off;
	const unsigned char *sy = src;
	const unsigned char *su = src + (size_t)sw * sh;
	const unsigned char *sv = su + (size_t)((sw + 1) / 2) * ((sh + 1) / 2);
	unsigned cw = (unsigned)((sw + 1) / 2);
	unsigned char *dst;
	unsigned x, y;

	if (!g.arena || !st->ov_on || !dw || !dh || !P || sw <= 0 || sh <= 0)
		return;
	/* The last row written is the bottom Cr row, at (dh/2 + dh/2 - 1)*P. */
	if ((size_t)off + (size_t)(dh + dh / 2) * P > g.arena_bytes)
		return;
	dst = g.arena + off;

	for (y = 0; y < dh; y++) {
		const unsigned char *r = sy + (size_t)(y * (unsigned)sh / dh) * sw;
		unsigned char *o = dst + (size_t)y * P;
		for (x = 0; x < dw; x++)
			o[x] = r[x * (unsigned)sw / dw];
	}
	for (y = 0; y < dh / 2; y++) {
		unsigned cy = y * (unsigned)(sh / 2) / (dh / 2);
		const unsigned char *ru = su + (size_t)cy * cw;
		const unsigned char *rv = sv + (size_t)cy * cw;
		unsigned char *ou = dst + (size_t)y * P + P / 2;
		unsigned char *ov = dst + (size_t)(dh / 2 + y) * P + P / 2;
		for (x = 0; x < dw / 2; x++) {
			unsigned cx = x * (unsigned)(sw / 2) / (dw / 2);
			ou[x] = ru[cx];
			ov[x] = rv[cx];
		}
	}
}

/* ---- staging, for the frames the guest DOES read ------------------------
 *
 * Photos and video recording go through DQBUF, and the shim serves those out
 * of camN.bin. Two slots, the host writing the one the guest is not reading,
 * so neither side needs a lock: fill slot (seq+1)&1, set its length, then bump
 * seq. See tadpole/shim/tadpole_cam.h. */
static void stage_frame(int idx, const unsigned char *i420, size_t len)
{
#ifndef _WIN32
	struct tad_cam_state *st = &g.cam[idx];
	char path[600];
	unsigned slot = (st->seq + 1u) & 1u;
	int fd;

	if (!st->slot_size || len > st->slot_size)
		return;
	snprintf(path, sizeof(path), "%s/cam%d.bin", g.dir, idx);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	if (lseek(fd, (off_t)slot * st->slot_size, SEEK_SET) >= 0) {
		size_t put = 0;
		while (put < len) {
			ssize_t w = write(fd, i420 + put, len - put);
			if (w <= 0) break;
			put += (size_t)w;
		}
		if (put == len) {
			st->bytes[slot] = (unsigned)len;
			st->seq++;
		}
	}
	close(fd);
#else
	(void)idx; (void)i420; (void)len;
#endif
}

/* ---- what the backend calls --------------------------------------------- */

void tad_cam_submit(int idx, const unsigned char *i420, int w, int h)
{
	struct tad_cam_state *st;
	size_t need;

	if (!g_ready || !g.cam || idx < 0 || idx >= TAD_CAM_N || !i420)
		return;
	if (w <= 0 || h <= 0)
		return;
	st = &g.cam[idx];
	need = (size_t)w * h + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2);

	if (g.c[idx].cap < need) {
		unsigned char *n = realloc(g.c[idx].buf, need);
		if (!n) return;
		g.c[idx].buf = n;
		g.c[idx].cap = need;
	}
	memcpy(g.c[idx].buf, i420, need);
	g.c[idx].bw = w;
	g.c[idx].bh = h;
	g.c[idx].frames++;
	st->host = 1;

	preview_blit(idx, g.c[idx].buf, w, h);
	/* The guest asked for a specific size; only stage a frame that is it. */
	if ((int)st->width == w && (int)st->height == h)
		stage_frame(idx, g.c[idx].buf, need);
}

/* ---- the still fallback -------------------------------------------------- */

static const unsigned char *still_for(int idx, size_t want)
{
	char path[600];
	FILE *f;
	long n;

	if (g.c[idx].still && g.c[idx].still_len == want)
		return g.c[idx].still;
	if (g.c[idx].still_tried && !g.c[idx].still)
		return NULL;
	g.c[idx].still_tried = 1;
	snprintf(path, sizeof(path), "%s/cam%d.raw", g.dir, idx);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || (size_t)n != want) { fclose(f); return NULL; }
	free(g.c[idx].still);
	g.c[idx].still = malloc((size_t)n);
	if (!g.c[idx].still) { fclose(f); return NULL; }
	if (fread(g.c[idx].still, 1, (size_t)n, f) != (size_t)n) {
		free(g.c[idx].still);
		g.c[idx].still = NULL;
	} else {
		g.c[idx].still_len = (size_t)n;
	}
	fclose(f);
	return g.c[idx].still;
}

/* ---- once a frame -------------------------------------------------------- */

void tad_cam_pump(void)
{
	int i, want = -1;

	if (!g_ready || !g.cam)
		return;

	/* ONE CAMERA AT A TIME, because the hardware means it. Both of this
	 * tablet's are HAL 1.0 and android.hardware.Camera.open() on the second
	 * while the first is still held throws
	 *
	 *     java.lang.RuntimeException: Fail to connect to camera service
	 *
	 * which is exactly what happened on the first press of CameraWidget's
	 * switch button: the guest opens /dev/video1 before it closes /dev/video0,
	 * so for one pass both looked wanted. Pick the one to run, release the
	 * other, and only then start it. The guest never uses both at once — the
	 * switch button is a switch — so nothing is given up.
	 *
	 * Later index wins, because a switch opens the new node first. */
	for (i = 0; i < TAD_CAM_N; i++)
		if (g.cam[i].open && g.cam[i].width && g.cam[i].height)
			want = i;

	for (i = 0; i < TAD_CAM_N; i++) {
		if (i == want)
			continue;
		if (g.c[i].running || g.c[i].failed) {
			g.c[i].running = 0;
			g.c[i].failed = 0;
			g.c[i].retry = 0;
			g.cam[i].host = 0;
			tad_cam_plat_stop(i);
		}
	}

	if (want >= 0) {
		struct tad_cam_state *st = &g.cam[want];
		struct { int w, h; } cur = { g.c[want].w, g.c[want].h };
		int restart = g.c[want].running &&
		              (cur.w != (int)st->width || cur.h != (int)st->height);

		if (restart) {
			tad_cam_plat_stop(want);
			g.c[want].running = 0;
		}
		if (!g.c[want].running) {
			if (g.c[want].retry > 0) {
				g.c[want].retry--;
			} else {
				g.c[want].w = (int)st->width;
				g.c[want].h = (int)st->height;
				g.c[want].frames = 0;
				g.c[want].running = 1;
				tad_cam_plat_start(want, g.c[want].w, g.c[want].h);
				if (!tad_cam_plat_running(want)) {
					/* BACK OFF, DO NOT GIVE UP. A start can fail because the
					 * other camera has not finished releasing yet — half a
					 * second later it works. Retrying every frame would turn
					 * that into sixty stack traces a second; never retrying
					 * would leave the viewfinder dead for the rest of the
					 * session. */
					g.c[want].running = 0;
					g.c[want].failed = 1;
					g.c[want].retry = 30;
				}
			}
		}

		/* Nothing live yet: show the still, if there is one, so the viewfinder
		 * is not the flat green an unwritten YUV buffer gives — and so that a
		 * photo taken now is that still rather than nothing. */
		if (!g.c[want].frames) {
			size_t need = (size_t)st->width * st->height
			            + 2 * (size_t)((st->width + 1) / 2)
			                * ((st->height + 1) / 2);
			const unsigned char *sfr = still_for(want, need);
			if (sfr) {
				preview_blit(want, sfr, (int)st->width, (int)st->height);
				if (st->seq == 0)
					stage_frame(want, sfr, need);
			}
		}
	}
}

/* Generic no-op backend. Android overrides these in tadpole_cam_android.c. */
#if !defined(__ANDROID__)
void tad_cam_plat_start(int idx, int w, int h) { (void)idx; (void)w; (void)h; }
void tad_cam_plat_stop(int idx) { (void)idx; }
int  tad_cam_plat_running(int idx) { (void)idx; return 0; }
#endif
