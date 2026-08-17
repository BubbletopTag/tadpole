/* Tadpole — the boot presentation. See tadpole_boot.h for what this is and,
 * more to the point, what it is not.
 *
 * Two steps, in the order a device performs them: the logo, then the startup
 * animation over the top of a guest that is already booting. Overlapping them
 * is not a shortcut, it is what the device does — VideoDaemon watches for
 * /tmp/ui_ready and stops the animation the moment AppManager raises it
 * ("UI is ready!! Stopping video early!"). We watch the same file for the same
 * reason, so turning Fast Boot off fills the wait that was already there
 * rather than adding to it.
 */

#include "tadpole_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tadpole_ui.h"

#ifdef TADPOLE_THEORA
#include <ogg/ogg.h>
#include <theora/theoradec.h>
#include <vorbis/codec.h>
#endif

#define BPATH 1100

/* ---- what each system shows ----------------------------------------------
 *
 * DECLARED, NOT DISCOVERED, and that is the honest cost of drawing this from
 * the front end. rcS picks its assets from /sys/devices/system/board/platform,
 * which is the kernel's answer about the hardware it is running on; we have no
 * kernel and no hardware, so we recognise a system by the files its firmware
 * installs and take the first row that matches.
 *
 * The video path is the one rcS symlinks to /var/sounds/StartupVideo.ogg on
 * that platform, named directly rather than through the symlink: the link is
 * made at boot by a script we never run, so in a sysroot it is whatever
 * tools/setup-sysroot.sh last pointed it at.
 *
 * logo_ms is how long the logo holds before the animation starts. It is a
 * judgement, not a measurement — on the device that gap is however long rcS
 * takes to get from imager-fb to /usr/bin/app, which depends on how many
 * services started and how cold the flash was. A beat over a second reads as
 * deliberate without being a wait.
 */
struct boot_show {
	const char *name;
	const char *logo;      /* sysroot-relative, as the guest would name it */
	const char *video;
	unsigned    logo_ms;
};

static const struct boot_show SHOWS[] = {
	/* LeapPad2 / LeapPad Explorer. "CW" is not a choice between orientations:
	 * the panel is portrait, so everything the device draws is stored a
	 * quarter turn from how it is held, this logo no more than the home
	 * screen. Presentation is the viewer's -r and nothing else's, which is
	 * why this is drawn exactly as stored. */
	{ "Valencia", "/var/screens/Valencia-Boot-logoCW.png",
	              "/LF/Base/LpadAssets/Video/StartupVideo.ogg", 1200 },
	/* LeapsterGS. Present for the same reason the table exists at all; the
	 * paths are rcS's LEX branch and its LucyAssets tree. Untested — no
	 * LeapsterGS firmware has been through here — so it is a starting point
	 * for whoever adds that system, not a claim that it works. */
	{ "Lucy",     "/var/screens/Lucy-Boot-logo.png",
	              "/LF/Base/LucyAssets/Video/StartupVideo.ogg", 1200 },
};
#define NSHOWS ((int)(sizeof SHOWS / sizeof *SHOWS))

enum { B_OFF = 0, B_LOGO, B_VIDEO };

static struct {
	int          state;
	int          opened;          /* assets tried; do not try twice a frame */
	Uint32       t0;              /* ticks when the sequence started */
	Uint32       tvideo;          /* ticks when the video started */
	const struct boot_show *show;
	char         logo_path[BPATH];
	char         video_path[BPATH];
	char         ui_ready[BPATH];
	SDL_Texture *logo;
	int          logo_w, logo_h;
	SDL_Texture *frame;
	int          fw, fh;          /* the video's visible picture size */
	SDL_AudioDeviceID adev;
} B;

static int file_exists(const char *p)
{
	struct stat st;
	return p && p[0] && stat(p, &st) == 0;
}

/* ---- the video ----------------------------------------------------------
 *
 * Ogg with three logical streams in it — skeleton, theora, vorbis — so the
 * headers have to be sorted out by asking each codec whether a packet is
 * theirs, rather than by trusting an order. The skeleton stream answers "no"
 * to both and is dropped, which is the correct treatment: it carries timing
 * metadata for streaming, and we have the whole file.
 */
#ifdef TADPOLE_THEORA

static struct {
	FILE            *f;
	ogg_sync_state   oy;
	ogg_stream_state to, vo;
	int              have_t, have_v;
	th_info          ti;
	th_comment       tc;
	th_setup_info   *tsi;
	th_dec_ctx      *td;
	vorbis_info      vi;
	vorbis_comment   vc;
	vorbis_dsp_state vd;
	vorbis_block     vb;
	int              vd_ready;
	double           held;        /* presentation time of the frame we hold */
	int              eof;
} V;

/* Returns bytes read; 0 at end of file. */
static int v_feed(void)
{
	char *b;
	size_t n;
	if (!V.f) return 0;
	b = ogg_sync_buffer(&V.oy, 8192);
	if (!b) return 0;
	n = fread(b, 1, 8192, V.f);
	ogg_sync_wrote(&V.oy, (long)n);
	return (int)n;
}

static void v_close(void)
{
	if (V.td)  { th_decode_free(V.td); V.td = NULL; }
	if (V.tsi) { th_setup_free(V.tsi); V.tsi = NULL; }
	if (V.have_t) { ogg_stream_clear(&V.to); th_comment_clear(&V.tc);
	                th_info_clear(&V.ti); }
	if (V.vd_ready) { vorbis_block_clear(&V.vb); vorbis_dsp_clear(&V.vd); }
	if (V.have_v) { ogg_stream_clear(&V.vo); vorbis_comment_clear(&V.vc);
	                vorbis_info_clear(&V.vi); }
	ogg_sync_clear(&V.oy);
	if (V.f) fclose(V.f);
	memset(&V, 0, sizeof V);
}

/* Read every header packet of every stream. 1 when there is a theora stream
 * ready to decode; 0 (and everything released) when there is not.
 *
 * COUNTED TO THREE, NOT RUN UNTIL IT STOPS, and the difference cost the first
 * keyframe. th_decode_headerin() returns >0 for "that was a header", but 0
 * means "that was NOT a header — it is your first video packet, and I have not
 * consumed it". A loop that treats 0 as its exit condition has already pulled
 * that packet out of the stream and drops it on the floor. The animation then
 * decodes from libtheora's initial reference frame, which is mid-grey: every
 * block an inter frame touched came out right and everything it did not stayed
 * grey, so the picture was legible and the background was wrong.
 *
 * vorbis_synthesis_headerin() is the same trap the other way up — it returns 0
 * for SUCCESS — so the same loop written the same way ran zero times, vorbis
 * got one header out of three, and there was no audio at all.
 *
 * Both codecs have exactly three header packets. Counting them says what is
 * meant and cannot mistake a data packet for the end of anything.
 */
static int v_open(const char *path)
{
	ogg_page   og;
	ogg_packet op;
	int t_hdr = 0, v_hdr = 0, done = 0;

	memset(&V, 0, sizeof V);
	V.f = fopen(path, "rb");
	if (!V.f) return 0;
	ogg_sync_init(&V.oy);
	th_info_init(&V.ti);
	th_comment_init(&V.tc);
	vorbis_info_init(&V.vi);
	vorbis_comment_init(&V.vc);

	while (!done) {
		if (ogg_sync_pageout(&V.oy, &og) != 1) {
			if (v_feed() == 0) break;      /* truncated file */
			continue;
		}
		if (ogg_page_bos(&og)) {
			/* A stream begins. Which codec it is can only be settled by
			 * offering its first packet to each of them in turn — this file
			 * leads with a skeleton stream, which answers no to both and is
			 * dropped. */
			ogg_stream_state test;
			ogg_stream_init(&test, ogg_page_serialno(&og));
			ogg_stream_pagein(&test, &og);
			if (ogg_stream_packetout(&test, &op) != 1) {
				ogg_stream_clear(&test);
				continue;
			}
			if (!V.have_t &&
			    th_decode_headerin(&V.ti, &V.tc, &V.tsi, &op) > 0) {
				memcpy(&V.to, &test, sizeof test);
				V.have_t = 1;
				t_hdr = 1;
			} else if (!V.have_v &&
			           vorbis_synthesis_headerin(&V.vi, &V.vc, &op) == 0) {
				memcpy(&V.vo, &test, sizeof test);
				V.have_v = 1;
				v_hdr = 1;
			} else {
				ogg_stream_clear(&test);   /* skeleton, or a second of a kind */
			}
			continue;
		}
		/* A continuation page: hand it to whichever stream owns it and take
		 * the two remaining headers, and not one packet more. */
		if (V.have_t && ogg_page_serialno(&og) == V.to.serialno)
			ogg_stream_pagein(&V.to, &og);
		if (V.have_v && ogg_page_serialno(&og) == V.vo.serialno)
			ogg_stream_pagein(&V.vo, &og);

		while (V.have_t && t_hdr < 3 &&
		       ogg_stream_packetout(&V.to, &op) == 1) {
			if (th_decode_headerin(&V.ti, &V.tc, &V.tsi, &op) <= 0)
				break;                     /* malformed; give up on video */
			t_hdr++;
		}
		while (V.have_v && v_hdr < 3 &&
		       ogg_stream_packetout(&V.vo, &op) == 1) {
			if (vorbis_synthesis_headerin(&V.vi, &V.vc, &op) != 0) {
				V.have_v = 0;              /* no audio; the picture is fine */
				break;
			}
			v_hdr++;
		}
		done = V.have_t && t_hdr == 3 && (!V.have_v || v_hdr == 3);
	}

	if (!V.have_t || t_hdr != 3) { v_close(); return 0; }
	V.td = th_decode_alloc(&V.ti, V.tsi);
	if (!V.td) { v_close(); return 0; }
	th_setup_free(V.tsi); V.tsi = NULL;

	if (V.have_v && v_hdr == 3 &&
	    vorbis_synthesis_init(&V.vd, &V.vi) == 0) {
		vorbis_block_init(&V.vd, &V.vb);
		V.vd_ready = 1;
	}
	V.held = -1.0;
	return 1;
}

/* Decode audio until SDL is holding about `ahead_ms` of it. Queued rather than
 * mixed through the guest's callback device: the guest has published no audio
 * format yet at boot — audio.fmt is written when it first opens ALSA — so
 * there is nothing to mix with and nothing to take a rate from. */
static void v_audio(SDL_AudioDeviceID dev, int ahead_ms)
{
	ogg_packet op;
	float    **pcm;
	int        n, i, c;
	Uint32     want;

	if (!dev || !V.vd_ready) return;
	want = (Uint32)((long)V.vi.rate * V.vi.channels * 2 * ahead_ms / 1000);

	while (SDL_GetQueuedAudioSize(dev) < want) {
		/* Anything already synthesised comes out first. */
		if ((n = vorbis_synthesis_pcmout(&V.vd, &pcm)) > 0) {
			Sint16 *out = malloc((size_t)n * V.vi.channels * 2);
			if (!out) return;
			for (i = 0; i < n; i++)
				for (c = 0; c < V.vi.channels; c++) {
					float s = pcm[c][i];
					if (s >  1.0f) s =  1.0f;
					if (s < -1.0f) s = -1.0f;
					out[i * V.vi.channels + c] = (Sint16)(s * 32767.0f);
				}
			SDL_QueueAudio(dev, out, (Uint32)n * V.vi.channels * 2);
			free(out);
			vorbis_synthesis_read(&V.vd, n);
			continue;
		}
		if (ogg_stream_packetout(&V.vo, &op) != 1)
			return;                      /* v_pump refills the streams */
		if (vorbis_synthesis(&V.vb, &op) == 0)
			vorbis_synthesis_blockin(&V.vd, &V.vb);
	}
}

/* Move pages from the file into whichever stream they belong to. 0 at EOF. */
static int v_pages(void)
{
	ogg_page og;
	while (ogg_sync_pageout(&V.oy, &og) != 1) {
		if (v_feed() == 0) { V.eof = 1; return 0; }
	}
	if (V.have_t && ogg_page_serialno(&og) == V.to.serialno)
		ogg_stream_pagein(&V.to, &og);
	else if (V.have_v && ogg_page_serialno(&og) == V.vo.serialno)
		ogg_stream_pagein(&V.vo, &og);
	return 1;
}

/* YUV to the ARGB the rest of the viewer speaks.
 *
 * BT.601 WITH STUDIO SWING, and the difference from the full-range formula is
 * not subtle enough to ignore or large enough to notice by eye — which is why
 * it took a diff to find. Theora carries Y in [16,235], so white is 235 and
 * has to be stretched to 255; reading Y as if it were already full range makes
 * every pixel in the picture exactly 20 too dark. Measured against ffmpeg's
 * decode of the same file: mean |delta| 19.2 per channel before, under 1 after.
 *
 * 16.16 fixed point, no floats and no table: 480x272 at 12 fps is 1.5 Mpx a
 * second and this is four seconds of it.
 *
 * NOTE FOR blit_layer_yuv420() IN tadpole_view.c, which converts the guest's
 * own video plane: it still uses the full-range coefficients, so the device's
 * in-title FMV is washed out by the same 20. Same bug, different path, and not
 * changed here — it would move every FMV capture in the compatibility sweep.
 */
static void v_convert(const th_ycbcr_buffer yc, Uint32 *dst, int w, int h,
                      int px0, int py0, int subx, int suby)
{
	int x, y;
	for (y = 0; y < h; y++) {
		const unsigned char *yr = yc[0].data + (size_t)(y + py0) * yc[0].stride;
		const unsigned char *cb = yc[1].data
		                        + (size_t)((y + py0) / suby) * yc[1].stride;
		const unsigned char *cr = yc[2].data
		                        + (size_t)((y + py0) / suby) * yc[2].stride;
		for (x = 0; x < w; x++) {
			int Y = (yr[x + px0] - 16) * 76309;          /* 1.164 */
			int U = cb[(x + px0) / subx] - 128;
			int Vv = cr[(x + px0) / subx] - 128;
			int r = (Y + 104597 * Vv) >> 16;             /* 1.596 */
			int g = (Y -  25675 * U - 53279 * Vv) >> 16; /* .392 .813 */
			int b = (Y + 132201 * U) >> 16;              /* 2.017 */
			if (r < 0) r = 0; else if (r > 255) r = 255;
			if (g < 0) g = 0; else if (g > 255) g = 255;
			if (b < 0) b = 0; else if (b > 255) b = 255;
			dst[(size_t)y * w + x] = 0xFF000000u |
			        ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
		}
	}
}

/* Advance to the frame that should be on screen at `now` seconds, and keep the
 * audio queue fed. Returns 0 when the stream has run out. */
static int v_pump(SDL_Renderer *ren, SDL_AudioDeviceID dev, double now)
{
	ogg_packet op;

	for (;;) {
		int dup;

		v_audio(dev, 300);

		if (V.held > now)               /* the frame we hold is still current */
			return 1;

		if (ogg_stream_packetout(&V.to, &op) != 1) {
			if (V.eof) break;
			if (!v_pages()) break;
			continue;
		}
		/* A header packet inside the data run is not ours to decode; theora
		 * says so with TH_EBADPACKET rather than by refusing to be asked.
		 *
		 * TH_DUPFRAME IS NOT AN ERROR AND NOT A FRAME TO SKIP. Theora codes a
		 * still moment as "the last picture again", and this clip is four
		 * seconds of mostly-still logo at 12 fps, so most of its packets are
		 * duplicates. Treating them as failures dropped their time as well as
		 * their picture, which runs the animation at several times speed. */
		{
			ogg_int64_t gp = 0;
			int r = th_decode_packetin(V.td, &op, &gp);
			if (r < 0)
				continue;
			dup    = (r == TH_DUPFRAME);
			V.held = th_granule_time(V.td, gp);
		}
		if (dup)
			continue;                   /* the texture already holds it */
		{
			th_ycbcr_buffer yc;
			Uint32 *px;
			int subx = 1, suby = 1;

			if (th_decode_ycbcr_out(V.td, yc) != 0)
				continue;
			switch (V.ti.pixel_fmt) {
			case TH_PF_420: subx = 2; suby = 2; break;
			case TH_PF_422: subx = 2; suby = 1; break;
			default:        subx = 1; suby = 1; break;   /* 4:4:4 */
			}
			if (!B.frame) {
				B.fw = (int)V.ti.pic_width;
				B.fh = (int)V.ti.pic_height;
				B.frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
				                            SDL_TEXTUREACCESS_STREAMING,
				                            B.fw, B.fh);
				if (!B.frame) break;
			}
			px = malloc((size_t)B.fw * B.fh * 4);
			if (!px) break;
			v_convert(yc, px, B.fw, B.fh,
			          (int)V.ti.pic_x, (int)V.ti.pic_y, subx, suby);
			SDL_UpdateTexture(B.frame, NULL, px, B.fw * 4);
			free(px);
		}
		/* Keep going while we are behind: a frame whose time has already
		 * passed is decoded and replaced rather than shown late, which keeps
		 * the animation on its own timeline instead of drifting off it. */
	}

	/* OUT OF PICTURES, WHICH IS NOT YET THE END. The video track is 4.08s and
	 * the audio is 4.42s — the chime outlasts the last frame by a third of a
	 * second — so hold the final picture until the queue has drained rather
	 * than cutting the animation off mid-note. */
	return dev && SDL_GetQueuedAudioSize(dev) > 0;
}

#else   /* no theora at build time */

static int  v_open(const char *path) { (void)path; return 0; }
static void v_close(void) { }
static int  v_pump(SDL_Renderer *ren, SDL_AudioDeviceID dev, double now)
{ (void)ren; (void)dev; (void)now; return 0; }

#endif  /* TADPOLE_THEORA */

/* ---- the sequence -------------------------------------------------------- */

void boot_arm(const char *projdir)
{
	int i;

	boot_stop();
	if (!projdir || !projdir[0])
		return;

	for (i = 0; i < NSHOWS; i++) {
		char logo[BPATH], video[BPATH];
		snprintf(logo,  sizeof logo,  "%s/runtime/sysroot%s",
		         projdir, SHOWS[i].logo);
		snprintf(video, sizeof video, "%s/runtime/sysroot%s",
		         projdir, SHOWS[i].video);
		/* EITHER is enough to claim the row. A firmware with the logo and no
		 * animation still gets a logo, which is most of the effect and all of
		 * the honesty. */
		if (!file_exists(logo) && !file_exists(video))
			continue;
		B.show = &SHOWS[i];
		snprintf(B.logo_path,  sizeof B.logo_path,  "%s", logo);
		snprintf(B.video_path, sizeof B.video_path, "%s", video);
		snprintf(B.ui_ready, sizeof B.ui_ready,
		         "%s/runtime/sysroot/tmp/ui_ready", projdir);
		B.state = B_LOGO;
		B.t0    = SDL_GetTicks();
		return;
	}
}

int boot_active(void) { return B.state != B_OFF; }

void boot_stop(void)
{
	if (B.state != B_OFF)
		v_close();
	if (B.logo)  { SDL_DestroyTexture(B.logo);  B.logo = NULL; }
	if (B.frame) { SDL_DestroyTexture(B.frame); B.frame = NULL; }
	if (B.adev)  { SDL_CloseAudioDevice(B.adev); B.adev = 0; }
	memset(&B, 0, sizeof B);
}

/* The guest's own "I am up" flag, written by AppManager through Brio's
 * CAtomicFile. Watching it is what makes this cost nothing: on a slow boot the
 * animation plays out, on a fast one it is cut exactly where the device would
 * cut it. */
static int guest_ui_ready(void)
{
	return file_exists(B.ui_ready);
}

int boot_draw(SDL_Renderer *ren, const SDL_Rect *dst, int rotate)
{
	Uint32 now;

	if (B.state == B_OFF)
		return 0;
	if (guest_ui_ready()) { boot_stop(); return 0; }

	now = SDL_GetTicks();

	if (!B.opened) {
		B.opened = 1;
		B.logo = ui_png_texture(ren, B.logo_path, &B.logo_w, &B.logo_h, NULL);
	}

	if (B.state == B_LOGO && now - B.t0 >= B.show->logo_ms) {
		B.state  = B_VIDEO;
		B.tvideo = now;
		if (v_open(B.video_path)) {
#ifdef TADPOLE_THEORA
			if (V.vd_ready) {
				SDL_AudioSpec want, got;
				memset(&want, 0, sizeof want);
				want.freq     = V.vi.rate;
				want.channels = (Uint8)V.vi.channels;
				want.format   = AUDIO_S16SYS;
				want.samples  = 1024;
				want.callback = NULL;          /* queued, not called back */
				B.adev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
				if (B.adev) SDL_PauseAudioDevice(B.adev, 0);
			}
#endif
		} else {
			/* No animation for this firmware. The logo has had its beat, so
			 * there is nothing left to show. */
			boot_stop();
			return 0;
		}
	}

	if (B.state == B_VIDEO) {
		double t = (double)(now - B.tvideo) / 1000.0;
		if (!v_pump(ren, B.adev, t)) {
			boot_stop();
			return 0;
		}
		if (!B.frame) { boot_stop(); return 0; }
	}

	/* Black behind it, not the idle screen: the panel is off until the device
	 * lights it, and the logo is a small mark on a white field that would sit
	 * oddly on anything else. */
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderFillRect(ren, dst);

	{
		SDL_Texture *t = (B.state == B_VIDEO && B.frame) ? B.frame : B.logo;
		if (!t) { boot_stop(); return 0; }
		if (rotate)
			SDL_RenderCopyEx(ren, t, NULL, dst, (double)rotate, NULL,
			                 SDL_FLIP_NONE);
		else
			SDL_RenderCopy(ren, t, NULL, dst);
	}
	return 1;
}
