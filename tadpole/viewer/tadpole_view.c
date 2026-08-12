/* Tadpole — LeapPad2 (NXP3200 / VALENCIA) emulator
 *
 * tadpole_view.c — native host-side viewer.
 *
 * Runs as an ordinary x86-64 SDL2 program. It mmaps the same framebuffer
 * files the guest mmaps through the shim, so pixels cross the boundary with
 * no copying and no protocol: the guest writes, we read the same pages.
 *
 * Input goes the other way, as struct input_event written into the FIFOs the
 * shim hands the guest as /dev/input/eventN.
 *
 * Build: cc -O2 tadpole_view.c $(pkg-config --cflags --libs sdl2) -o tadpole-view
 */

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
/* Guest supervision (fork/wait/kill) and the shared mappings — the two things
 * with no Windows spelling. Their users are stubbed out below on Windows;
 * see "guest supervision" for why they are stubs and not ports. */
#include <sys/wait.h>
#include <signal.h>
#include <sys/mman.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>       /* map_file: MapViewOfFile of the guest's arena */
#include <shellapi.h>      /* ShellExecuteA: hand over to the update installer */
#include <limits.h>        /* INT_PTR's companions */
#include <io.h>            /* _open_osfhandle: pipe HANDLEs as CRT fds */

/* The FIFO stand-ins, viewer end. glasspole's gp_mkfifo maps a FIFO path to
 * the named pipe \\.\pipe\tadpole-<basename>; this is the other half of that
 * contract (see host_win32.c for the whole of it). Reader creates the pipe,
 * writer connects — the viewer reads audio and writes events, the guest the
 * reverse, and whichever end is late retries: the shim per audio period, us
 * through ev_open_missing() every frame. The HANDLE is wrapped into a CRT fd
 * so every read()/write() above stays exactly as Linux wrote it. */
static int tp_fifo_fd(const char *path, int want_read)
{
	char name[128];
	const char *base = strrchr(path, '/');
	HANDLE h;

	base = base ? base + 1 : path;
	snprintf(name, sizeof(name), "\\\\.\\pipe\\tadpole-%s", base);
	h = CreateFileA(name,
	                want_read ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE,
	                0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		if (!want_read)
			return -1;             /* no reader yet: retry, as on Linux */
		h = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
		                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
		                     1, 64 << 10, 64 << 10, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
			return -1;
		ConnectNamedPipe(h, NULL);     /* NOWAIT: arms, never blocks */
	}
	return _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
}
#endif
#include "tadpole_ui.h"
#include "tadpole_hle.h"
#include "tadpole_port.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>              /* guest_sweep_stragglers walks /proc */
#endif

#define TADPOLE_MAGIC 0x54414450u   /* "TADP" */
#define NUM_FB 3
#define NUM_EV 6

/* How long the viewer keeps drawing the game layer after the last HLE frame.
 * See the drop in the render loop — this is "the title has gone", not a
 * frame-pacing tolerance. */
#define GL_STALE_MS 1000u

/* Set by the Makefile; "dev" in a working copy. The update checker treats
 * anything that is not a release tag as an unreleased build and offers the
 * newest release without claiming you are behind. */
#ifndef TADPOLE_VERSION
#define TADPOLE_VERSION "dev"
#endif

/* Mirrors struct tadpole_state in the shim. Keep the two in sync. */
struct layer_state {
	uint32_t enabled, xres, yres, bpp, xoffset, yoffset;
	uint32_t nonstd, alpha, blank;
	/* On-panel rectangle for this layer — see the long note in
	 * tadpole_shim.c. Must stay in step with the shim's copy: both map the
	 * same state.bin, so a mismatch silently shifts every field after it. */
	uint32_t win_x, win_y, win_w, win_h;
	/* Video-scaler source size for the YUV layer; 0 = unset. */
	uint32_t vid_w, vid_h;
};

struct tadpole_state {
	uint32_t magic, version;
	uint32_t width, height;
	uint32_t vsync_count;
	struct layer_state layer[NUM_FB];
};

/* struct input_event as the 32-bit ARM guest sees it: 32-bit time_t. NOT the
 * host layout, which has a 64-bit timeval. Hard-coded deliberately. */
struct guest_input_event {
	uint32_t tv_sec;
	uint32_t tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t  value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_PRESSURE 0x18
#define BTN_TOUCH 0x14a

/* Pressure to report while the stylus is down. Real hardware emits 10..70
 * (evtest on the device), NOT the advertised 1023 and certainly not 255 —
 * pick something inside the range the guest would actually see. */
#define TOUCH_PRESSURE 60

/* evdev node indices — MUST match g_ev_names[] in the shim, which follows
 * the real device's /proc/bus/input/devices order. Getting these wrong sends
 * keypresses to the USB node and nothing responds. */
#define EV_USB        0
#define EV_GPIO_KEYS  1
#define EV_TOUCH      2
#define EV_TOUCH_RAW  3
#define EV_ACCEL      4
#define EV_POWER      5

/* Keycodes. LeapPad1's driver mapped the D-pad and buttons this way; LeapPad2
 * has fewer buttons but the codes carry over. Refine against the LF2000
 * keypad driver once the guest is far enough along to report what it wants. */
#define KEY_ESC_       1
#define KEY_MINUS_     12
#define KEY_EQUAL_     13
#define KEY_A_         30
#define KEY_B_         48
#define KEY_H_         35
#define KEY_L_         38
#define KEY_M_         50
#define KEY_P_         25
#define KEY_R_         19
#define KEY_X_         45
#define KEY_UP_        103
#define KEY_LEFT_      105
#define KEY_RIGHT_     106
#define KEY_DOWN_      108
#define KEY_VOLUMEDOWN_ 114
#define KEY_VOLUMEUP_   115
#define KEY_POWER_      116

static const char *g_dir = "/tmp/tadpole";

/* The touchscreen coordinate space the GUEST expects.
 *
 * Verified: the events we send arrive at the guest byte-for-byte intact, so
 * any offset you see on screen is Brio's own transform — i.e. we are sending
 * the right numbers in the WRONG SPACE. Brio's non-tslib fallback does not
 * necessarily treat ABS_X/ABS_Y as screen pixels.
 *
 * Defaults to screen pixels (0..w-1, 0..h-1). Override to experiment:
 *   TADPOLE_TS_MAX_X=4095 TADPOLE_TS_MAX_Y=4095 ./tadpole.sh
 * The authoritative values come from `evtest /dev/input/event2` on real
 * hardware — that prints the driver's true ABS_X/ABS_Y min and max. */
static int g_ts_max_x = 0;   /* 0 = use screen width  */
static int g_ts_max_y = 0;   /* 0 = use screen height */

/* Last touch point, in FRAMEBUFFER coordinates, for the debug crosshair.
 * Drawn into the same pixel buffer as the guest's content, so it is rotated
 * and scaled by exactly the same path — if the cross lands under the pointer,
 * the window->framebuffer mapping is right, and if it does not, the error is
 * visible and measurable instead of being argued about. */
static int g_touch_mark_x = -1;
static int g_touch_mark_y = -1;
static int g_touch_debug;
static int   g_evfd[NUM_EV];
static void *g_fb[NUM_FB];
static size_t g_fbsz[NUM_FB];
static struct tadpole_state *g_state;

/* ---- audio -------------------------------------------------------------- *
 *
 * The guest's fake libasound (shim/tadpole_asound.c) writes PCM into
 * $TADPOLE_DIR/audio and leaves the negotiated format in audio.fmt. We drain
 * the FIFO into a ring here and let SDL's callback pull from it.
 *
 * Why a ring rather than reading in the callback: the callback runs on SDL's
 * audio thread and must never block, and a FIFO read can return short. Reading
 * in the main loop and copying in the callback keeps the blocking side on the
 * thread that is allowed to wait.
 *
 * Underrun is filled with silence rather than stalling — the guest is paced by
 * the FIFO filling up, so a brief gap is self-correcting.
 *
 * LATENCY. The ring is deliberately large, but DEPTH is capped separately —
 * they are different things and conflating them cost about five seconds of lag.
 * The chain the guest's audio travels is:
 *
 *     guest snd_pcm_writei -> FIFO (64 KB) -> this ring (512 KB) -> SDL
 *
 * At the negotiated 32 kHz stereo 16-bit that is 128000 bytes/sec, so a full
 * ring alone is 4.10 s and the FIFO adds 0.51 s: 4.61 s worst case, which is
 * what "roughly 5 seconds" of delay was. Nothing drains it either, because the
 * cursors only ever converge if the guest UNDER-produces. The guest bursts
 * ahead while a title loads, the ring pins to full, and the lag is permanent.
 *
 * The capacity is fine — a deep ring absorbs jitter. What was missing is a
 * bound on how far AHEAD of the speaker we are willing to run. audio_cb trims
 * the backlog when it exceeds ADEPTH_MAX, dropping the OLDEST audio down to
 * ADEPTH_TARGET. Trimming in the callback is what makes it race-free: the
 * callback is the only writer of g_atail.
 *
 * Hysteresis (trim only above MAX, but trim all the way to TARGET) keeps this
 * rare — one small discontinuity when the guest has run ahead, rather than a
 * continuous stutter. Underrun risk is unchanged: TARGET still holds several
 * SDL periods, and short gaps already fill with silence.
 */
#define ARING_BYTES (1u << 19)          /* 512 KB capacity — NOT the latency */
/* Runtime, not compile-time: Options -> Audio Settings changes it live. The
 * target trails the cap so the hysteresis survives whatever the user picks. */
static volatile int g_adepth_max_ms    = 260;
static volatile int g_adepth_target_ms = 120;

static uint8_t  g_aring[ARING_BYTES];
static volatile uint32_t g_ahead;       /* write cursor (main thread) */
static volatile uint32_t g_atail;       /* read cursor  (audio thread) */
static SDL_AudioDeviceID g_adev;
static int      g_afd = -1;
static int      g_arate, g_achans, g_abits;
static volatile uint32_t g_abps;        /* bytes/sec, 0 until a device opens */
static volatile uint32_t g_atrims;      /* how often we have had to trim */

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	uint32_t head = g_ahead, tail = g_atail;
	uint32_t avail = head - tail;          /* unsigned wrap is intended */
	uint32_t bps = g_abps;
	int n, i;

	(void)ud;

	/* Drop the oldest backlog if we have run too far ahead of the speaker.
	 * Only this callback writes g_atail, so moving it here needs no lock. */
	if (bps) {
		uint32_t max = bps * (uint32_t)g_adepth_max_ms / 1000u;
		if (avail > max) {
			tail = head - bps * (uint32_t)g_adepth_target_ms / 1000u;
			avail = head - tail;
			g_atrims++;
		}
	}

	n = (int)(avail < (uint32_t)len ? avail : (uint32_t)len);
	for (i = 0; i < n; i++)
		stream[i] = g_aring[(tail + i) % ARING_BYTES];
	if (n < len)
		memset(stream + n, 0, (size_t)(len - n));   /* underrun: silence */
	g_atail = tail + (uint32_t)n;
}

static void audio_open_fifo(void)
{
#ifndef _WIN32
	char path[512];

	snprintf(path, sizeof(path), "%s/audio", g_dir);
	mkfifo(path, 0666);
	/* O_RDWR on a FIFO never blocks and never sees EOF, so we can hold the
	 * read end open whether or not the guest has started writing. */
	g_afd = open(path, O_RDWR | O_NONBLOCK);
#else
	/* The reader end of the audio pipe — we create it, the shim's writer
	 * connects when it starts (or retries per period if we were late). */
	char path[512];
	snprintf(path, sizeof(path), "%s/audio", g_dir);
	g_afd = tp_fifo_fd(path, 1);
#endif
}

/* Open (or reopen) the SDL device when the guest publishes a format. */
static void audio_poll_fmt(void)
{
	char path[512], buf[64];
	int fd, n, rate = 0, chans = 0, bits = 0, period = 0;
	SDL_AudioSpec want, got;

	snprintf(path, sizeof(path), "%s/audio.fmt", g_dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	if (sscanf(buf, "%d %d %d %d", &rate, &chans, &bits, &period) < 3)
		return;
	if (rate <= 0 || chans <= 0)
		return;
	if (rate == g_arate && chans == g_achans && bits == g_abits)
		return;                                  /* unchanged */

	if (g_adev) {
		SDL_CloseAudioDevice(g_adev);
		g_adev = 0;
	}
	memset(&want, 0, sizeof(want));
	want.freq     = rate;
	want.channels = (Uint8)chans;
	want.format   = (bits == 8) ? AUDIO_U8 : (bits == 32 ? AUDIO_S32LSB
	                                                     : AUDIO_S16LSB);
	want.samples  = (Uint16)(period > 0 ? period : 1024);
	want.callback = audio_cb;

	g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
	if (!g_adev) {
		fprintf(stderr, "tadpole-view: audio open failed: %s\n",
		        SDL_GetError());
		return;
	}
	g_arate = rate; g_achans = chans; g_abits = bits;
	/* Publish bytes/sec last: audio_cb treats 0 as "no bound yet". */
	g_abps = (uint32_t)(rate * chans * (bits ? bits : 16) / 8);
	printf("tadpole-view: audio %d Hz, %d ch, %d-bit (period %d), "
	       "latency capped at %d ms\n", rate, chans, bits, period,
	       g_adepth_max_ms);
	fflush(stdout);
	SDL_PauseAudioDevice(g_adev, 0);
}

/* Move whatever the guest has written into the ring. Called once per frame. */
/* SOMEONE HAS TO READ THE FIFO EVEN WHEN NOBODY IS LISTENING.
 *
 * audio_pump is the only reader of the guest's audio FIFO, and it used to run
 * only when audio was enabled. Switch audio off and the FIFO fills; the
 * guest's audio thread then blocks in write() forever, and any title that
 * waits for a sound to finish before moving on simply stops.
 *
 * That is not a quiet degradation — it is a hang. Clam Prix sits on its title
 * screen and never reaches the menu, with the renderer still running at 60 fps
 * so everything looks alive. Read and throw the bytes away instead.
 */
static void audio_discard(void)
{
	uint8_t tmp[16384];
	int rounds;

	if (g_afd < 0)
		return;
	for (rounds = 0; rounds < 8; rounds++)
		if (read(g_afd, tmp, sizeof tmp) <= 0)
			return;
}

static void audio_pump(void)
{
	uint8_t tmp[16384];
	int rounds;

	if (g_afd < 0)
		return;
	for (rounds = 0; rounds < 8; rounds++) {
		uint32_t space = ARING_BYTES - (g_ahead - g_atail);
		size_t want = sizeof(tmp);
		ssize_t n;
		uint32_t i;

		if (space < sizeof(tmp))
			want = space;
		if (want == 0)
			return;                       /* ring full; guest will wait */
		n = read(g_afd, tmp, want);
		if (n <= 0)
			return;
		for (i = 0; i < (uint32_t)n; i++)
			g_aring[(g_ahead + i) % ARING_BYTES] = tmp[i];
		g_ahead += (uint32_t)n;
	}
}

static void *map_file(const char *path, size_t *len_out)
{
#ifndef _WIN32
	struct stat st;
	void *p;
	int fd = open(path, O_RDWR);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0 || st.st_size == 0) {
		close(fd);
		return NULL;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return NULL;
	if (len_out)
		*len_out = (size_t)st.st_size;
	return p;
#else
	/* The guest (glasspole) maps these files as REAL shared views since its
	 * chunked-reservation change, so their pages are live — and mapping them
	 * here needs none of the placement gymnastics the emulator needed: our
	 * own address space, any address, plain MapViewOfFile, Windows 7 API.
	 * This is the Linux architecture reproduced, not a new one; the
	 * one-process design remains the plan of record and deletes this too. */
	HANDLE f, m;
	LARGE_INTEGER sz;
	void *p;

	f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
	                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return NULL;
	if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) {
		CloseHandle(f);
		return NULL;
	}
	m = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, NULL);
	CloseHandle(f);
	if (!m)
		return NULL;
	p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	CloseHandle(m);                     /* the view keeps the section alive */
	if (p && len_out)
		*len_out = (size_t)sz.QuadPart;
	return p;
#endif
}

static void send_event(int dev, uint16_t type, uint16_t code, int32_t value)
{
	struct guest_input_event ev;
	struct timeval tv;

	if (dev < 0 || dev >= NUM_EV || g_evfd[dev] < 0)
		return;

	gettimeofday(&tv, NULL);
	ev.tv_sec  = (uint32_t)tv.tv_sec;
	ev.tv_usec = (uint32_t)tv.tv_usec;
	ev.type    = type;
	ev.code    = code;
	ev.value   = value;

	if (write(g_evfd[dev], &ev, sizeof(ev)) < 0)
		/* guest not reading yet; dropping is correct, not an error */;
}

static void send_key(int dev, uint16_t code, int down)
{
	send_event(dev, EV_KEY, code, down ? 1 : 0);
	send_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* THE D-PAD ROTATES WITH THE DISPLAY.
 *
 * The LeapPad2's D-pad codes assume the device is held PORTRAIT. A Leapster
 * title is played turned on its side, so the physical "up" button is the game's
 * "left" — press the arrow that looks like up and the selection moves sideways.
 *
 * The display rotation the user picks is exactly the rotation needed to undo
 * that, so derive it rather than adding a separate setting: whatever makes the
 * picture upright also makes the arrow keys agree with it. Rotating with the ROT
 * button therefore fixes the controls in the same click.
 *
 * Directions clockwise: up, right, down, left. At rotate=270 — the orientation
 * Leapster titles are played in — visual "up" must send physical RIGHT, which is
 * one step clockwise, so the shift is (4 - rotate/90) % 4.
 *
 * TADPOLE_RAW_DPAD=1 sends the physical codes unrotated.
 */
static const uint16_t DPAD_CW[4] = { KEY_UP_, KEY_RIGHT_, KEY_DOWN_, KEY_LEFT_ };
static int g_raw_dpad;
static int g_dpad_shift = -1;   /* -1 = derive from the rotation */

/* A LEAPSTER TITLE IS LANDSCAPE ON A PORTRAIT DEVICE, so the game's axes are a
 * quarter turn from the D-pad's REGARDLESS of how we are displaying it. That
 * constant was missing: at rotate=0 the mapping was identity, and the owner
 * measured visual up arriving as the game's LEFT, right as UP, left as DOWN —
 * precisely one step out.
 *
 * THEN IT WAS MEASURED AGAIN and found to be three, not one: with 1, both axes
 * came out reversed — up gave down, left gave right — which is a half turn, so
 * the mapping was two steps out and 1 + 2 = 3. The earlier value was set from a
 * single observation of one axis, which cannot distinguish a quarter turn from
 * three quarters; two axes are needed to pin the direction of rotation, and
 * that is what the second measurement supplied.
 *
 * So the shift is the display correction PLUS that constant. TADPOLE_DPAD_SHIFT
 * (0-3) overrides it, because controls cannot be verified without someone
 * holding the keyboard and a wrong guess is worse than an adjustable default.
 */
#define DPAD_GAME_TURN 3

/* TADPOLE_DPAD_MIRROR=1 reflects left/right, leaving up/down alone.
 *
 * THE ROTATION MODEL CANNOT EXPRESS EVERY WRONG MAPPING. Four rotations move
 * both axes together, so "left and right are swapped but up and down are
 * correct" is unreachable by any shift — that is a reflection, and if the
 * panel's axes are mirrored relative to the D-pad's no rotation will ever fix
 * it. Two guesses at the constant have now been wrong, and neither could have
 * been right if the true relationship is a reflection.
 *
 * Together the two knobs cover all eight possibilities (4 rotations x 2
 * reflections), so one sitting at the keyboard settles it for good.
 */
static int g_dpad_mirror;

static uint16_t rotate_dpad(int visual_idx, int rotate)
{
	int shift;
	if (g_raw_dpad)
		return DPAD_CW[visual_idx];
	/* Reflect BEFORE rotating: swapping indices 1 and 3 exchanges RIGHT and
	 * LEFT while UP (0) and DOWN (2) stay put. */
	if (g_dpad_mirror && (visual_idx & 1))
		visual_idx ^= 2;
	shift = (g_dpad_shift >= 0)
	      ? g_dpad_shift
	      : ((4 - ((rotate / 90) & 3)) + DPAD_GAME_TURN) & 3;
	return DPAD_CW[(visual_idx + shift) & 3];
}

static uint16_t map_key(SDL_Keycode k, int rotate)
{
	switch (k) {
	case SDLK_UP:     return rotate_dpad(0, rotate);
	case SDLK_RIGHT:  return rotate_dpad(1, rotate);
	case SDLK_DOWN:   return rotate_dpad(2, rotate);
	case SDLK_LEFT:   return rotate_dpad(3, rotate);
	case SDLK_x:      return KEY_A_;
	case SDLK_z:      return KEY_B_;
	case SDLK_h:      return KEY_H_;
	case SDLK_p:      return KEY_P_;
	/* THE SHOULDER BUTTONS WERE NEVER WIRED UP. KEY_L_ and KEY_R_ have been
	 * defined since the keycode table was written and nothing ever returned
	 * them, so a title that wanted L or R could not be given one. */
	case SDLK_q:      return KEY_L_;
	case SDLK_w:      return KEY_R_;
	case SDLK_HOME:   return KEY_M_;
	case SDLK_ESCAPE: return KEY_ESC_;
	case SDLK_EQUALS: return KEY_VOLUMEUP_;
	case SDLK_MINUS:  return KEY_VOLUMEDOWN_;
	default:          return 0;
	}
}

/* Map a MOUSE EVENT position to framebuffer coordinates.
 *
 * The coordinates in SDL_MOUSEMOTION / SDL_MOUSEBUTTON* are ALREADY LOGICAL.
 * When a renderer has a logical size, SDL's own renderer event watch rewrites
 * those events in place, applying both the render scale and the letterbox
 * viewport offset before the application ever sees them. So there is nothing
 * left to convert here — only the display rotation to undo.
 *
 * This cost a long hunt. The old code called SDL_RenderWindowToLogical on
 * coordinates that were already logical, dividing by the render scale a SECOND
 * time. The error is therefore zero at the viewport origin and grows linearly
 * with distance from it, scaled by (1 - 1/scale): invisible at -s 1, exactly
 * 2x at -s 2. That is precisely the reported symptom — "it works if I shrink
 * the window to about 1:1, and gets worse the bigger I make it".
 *
 * Measured on this display, logical 272x480 in a pillarboxed 700x900 window
 * (scale 1.875, viewport offset 95):
 *
 *     true window (300,400) -> hand-computed logical (109.3,213.3)
 *                           -> SDL delivered event(109,213)
 *
 * Note SDL_GetMouseState() is NOT converted and still returns true window
 * coordinates; only the event structs are rewritten. Do not mix the two.
 *
 * ROTATION: rotating an image R degrees clockwise sends source (fx,fy) to
 * logical (H-1-fy, fx) for R=90, so the inverse is fx=ly, fy=H-1-lx.
 */
static void event_to_fb(int rotate, int w, int h, int lx, int ly,
                        int *fx, int *fy)
{
	switch (rotate) {
	case 90:  *fx = ly;         *fy = h - 1 - lx; break;
	case 180: *fx = w - 1 - lx; *fy = h - 1 - ly; break;
	case 270: *fx = w - 1 - ly; *fy = lx;         break;
	default:  *fx = lx;         *fy = ly;         break;
	}
	if (*fx < 0) *fx = 0; else if (*fx >= w) *fx = w - 1;
	if (*fy < 0) *fy = 0; else if (*fy >= h) *fy = h - 1;

	/* scale screen pixels into the guest's expected touch range */
	if (g_ts_max_x > 0) *fx = (int)((long)*fx * g_ts_max_x / (w - 1));
	if (g_ts_max_y > 0) *fy = (int)((long)*fy * g_ts_max_y / (h - 1));
}

/* --selftest: prove window_to_fb is right, using the REAL function.
 *
 * The touch-offset hunt kept stalling on tests that re-implemented this
 * mapping and then agreed with themselves. This drives the actual shipping
 * code against a real window at a spread of sizes, including deliberately
 * awkward aspect ratios. The centre of the window must land on the centre of
 * the framebuffer at EVERY size; a size-dependent error is the whole bug.
 */
static void plot(uint32_t *px, int w, int h, int x, int y, uint32_t col)
{
	if (x >= 0 && y >= 0 && x < w && y < h)
		px[y * w + x] = col;
}

static void cross(uint32_t *px, int w, int h, int x, int y, int arm, uint32_t col)
{
	int i;
	for (i = -arm; i <= arm; i++) {
		plot(px, w, h, x + i, y, col);
		plot(px, w, h, x, y + i, col);
	}
}

/* Reference grid at KNOWN framebuffer coordinates, 60 px apart.
 *
 * The point is to make the test independent of everything I cannot measure
 * from a screenshot: window decoration sizes, where the pointer drifted to
 * after the click, compositor scaling. Click a green dot; the red cross shows
 * where the viewer thinks you clicked. If they coincide the mapping is right,
 * and if they do not, the error reads directly off the picture in grid units —
 * and, crucially, whether it GROWS with distance from the origin (a scale
 * error) or stays constant (an offset error).
 */
static void draw_touch_grid(uint32_t *px, int w, int h)
{
	const uint32_t green  = 0xFF00C000u;
	const uint32_t bright = 0xFF00FF40u;
	int x, y;

	for (y = 30; y < h; y += 60)
		for (x = 30; x < w; x += 60)
			cross(px, w, h, x, y, (x == 30 && y == 30) ? 10 : 5,
			      (x == 30 && y == 30) ? bright : green);

	/* Outline the framebuffer's exact edge. Makes the extent of the guest's
	 * screen unmistakable in a screenshot, so "does the content fill the
	 * window, and how far apart are the dots on screen?" can be answered by
	 * looking rather than by inferring from click coordinates. */
	for (x = 0; x < w; x++) {
		plot(px, w, h, x, 0, bright);
		plot(px, w, h, x, h - 1, bright);
	}
	for (y = 0; y < h; y++) {
		plot(px, w, h, 0, y, bright);
		plot(px, w, h, w - 1, y, bright);
	}
}

/* Draw a crosshair at the last touch point, in framebuffer space. */
static void draw_touch_mark(uint32_t *px, int w, int h)
{
	if (g_touch_mark_x < 0 || g_touch_mark_y < 0)
		return;
	cross(px, w, h, g_touch_mark_x, g_touch_mark_y, 8, 0xFFFF0000u);
}

/* Forward transform: where does framebuffer pixel (fx,fy) LAND in the window?
 *
 * This is the render path in reverse of window_to_fb, derived from what the
 * draw code below actually does: blit the w x h texture into a dst rect
 * centred in the logical area, spun by `rotate` about its own centre, then let
 * SDL_RenderSetLogicalSize letterbox that into the output, then output pixels
 * scale to window pixels.
 *
 * Round-tripping through this is the only honest check of the rotated cases.
 * Testing the window CENTRE proves nothing about rotation: the centre is a
 * fixed point of every rotation, so it maps to itself even when the transform
 * is completely wrong.
 */
static void fb_to_window(SDL_Renderer *ren, SDL_Window *win, int rotate,
                         int w, int h, int fx, int fy, int *wx, int *wy)
{
	int lw = (rotate == 90 || rotate == 270) ? h : w;
	int lh = (rotate == 90 || rotate == 270) ? w : h;
	float cx = (lw - w) / 2.0f + w / 2.0f;   /* dst centre, logical coords */
	float cy = (lh - h) / 2.0f + h / 2.0f;
	float X  = (lw - w) / 2.0f + fx + 0.5f;  /* unrotated, logical coords */
	float Y  = (lh - h) / 2.0f + fy + 0.5f;
	float rx, ry, sx, sy;
	int ww, wh, ow, oh;
	SDL_Rect vp;

	switch (rotate) {
	case 90:  rx = cx - (Y - cy); ry = cy + (X - cx); break;
	case 180: rx = 2 * cx - X;    ry = 2 * cy - Y;    break;
	case 270: rx = cx + (Y - cy); ry = cy - (X - cx); break;
	default:  rx = X;             ry = Y;             break;
	}

	SDL_RenderGetViewport(ren, &vp);
	SDL_RenderGetScale(ren, &sx, &sy);
	SDL_GetWindowSize(win, &ww, &wh);
	SDL_GetRendererOutputSize(ren, &ow, &oh);

	/* SDL_RenderGetViewport reports the viewport in LOGICAL units — it
	 * divides the stored output-pixel rect by the render scale. So the
	 * letterbox offset has to be added BEFORE scaling, not after. Adding it
	 * after understates the offset by vp.x*(scale-1), which looks exactly
	 * like a size-dependent touch offset and sent me chasing a viewer bug
	 * that was really in this test. */
	(void)sx; (void)sy; (void)vp; (void)ww; (void)wh; (void)ow; (void)oh;
	*wx = (int)rx;                 /* LOGICAL coords: that is what SDL hands */
	*wy = (int)ry + UI_BAR_H;      /* the app. The chrome shifts y down.     */
}

static int selftest(int rotate, int scale)
{
	static const int sizes[][2] = {
		{960,544}, {1000,600}, {800,400}, {1200,544},
		{640,363}, {1440,816}, {1101,623}, {469,339},
	};
	static const int pts[][2] = {           /* framebuffer coords to probe */
		{240,136}, {40,30}, {440,30}, {40,240}, {440,240}, {120,68}, {360,204},
	};
	const int w = 480, h = 272;
	SDL_Window *win;
	SDL_Renderer *ren;
	int lw = (rotate == 90 || rotate == 270) ? h : w;
	int lh = (rotate == 90 || rotate == 270) ? w : h;
	int i, j, k, bad = 0;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("tadpole selftest", SDL_WINDOWPOS_CENTERED,
	                       SDL_WINDOWPOS_CENTERED, lw * scale,
	                       (lh + UI_BAR_H) * scale, SDL_WINDOW_RESIZABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	SDL_RenderSetLogicalSize(ren, lw, lh + UI_BAR_H);

	printf("driver=%s rotate=%d — round-trip fb -> logical -> fb"
	       " (with the %dpx menu bar)\n\n",
	       SDL_GetCurrentVideoDriver(), rotate, UI_BAR_H);

	for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
		int ww, wh, worst = 0;

		SDL_SetWindowSize(win, sizes[i][0], sizes[i][1]);
		for (j = 0; j < 30; j++) { SDL_PumpEvents(); SDL_Delay(10); }
		SDL_RenderClear(ren);
		SDL_RenderPresent(ren);
		for (j = 0; j < 10; j++) { SDL_PumpEvents(); SDL_Delay(10); }
		SDL_GetWindowSize(win, &ww, &wh);

		printf("  win %4dx%-4d ", ww, wh);
		for (k = 0; k < (int)(sizeof(pts) / sizeof(pts[0])); k++) {
			int mx, my, gx, gy, ex, ey;

			fb_to_window(ren, win, rotate, w, h, pts[k][0], pts[k][1],
			             &mx, &my);
			event_to_fb(rotate, w, h, mx, my - UI_BAR_H, &gx, &gy);
			ex = gx - pts[k][0];
			ey = gy - pts[k][1];
			if (ex < 0) ex = -ex;
			if (ey < 0) ey = -ey;
			if (ex > worst) worst = ex;
			if (ey > worst) worst = ey;
			if (worst > 2)
				printf("[fb(%d,%d)->win(%d,%d)->fb(%d,%d)] ",
				       pts[k][0], pts[k][1], mx, my, gx, gy);
		}
		if (worst > 2)
			bad++;
		printf("worst err %d px %s\n", worst, worst > 2 ? "FAIL" : "ok");
	}

	printf("\n%s\n", bad ? "FAILED — touch does not land where pixels are drawn"
	                     : "PASS — touch lands where pixels are drawn");
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return bad ? 1 : 0;
}

/* WHERE THIS LAYER GOES ON THE PANEL, AND HOW MUCH OF IT IS PICTURE.
 *
 * A LAYER'S BUFFER IS NOT A PANEL-SIZED IMAGE. The MLC reads win_w x win_h
 * pixels from the layer's base address, `line_length` bytes per row, and
 * composites them at (win_x, win_y). The guest writes accordingly — for
 * Digging for Dinosaurs, whose ViewFrame window is 250x250 at (76,11), Brio's
 * own log says
 *
 *     AllocBuffer: new buf offset 0017E800, length 00075300
 *     CreateHandle: 0x86728: 250x250 (1920) @ 0x8217e800
 *     SetWindowPosition: 0x86728: 0,0 .. 250,250
 *
 * 0x75300 is 250 x 1920: two hundred and fifty rows of PANEL stride, starting
 * at the pan base with no offset of any kind. The (76,11) exists only in the
 * IOCSPOSTION the shim records into state.bin.
 *
 * Copying the buffer 1:1 onto the panel therefore drew every such title in the
 * top-left corner, where the ViewFrame art then cropped off its left and top
 * edges — "the game is cut off and is not centred". It went unnoticed for the
 * 3D titles because the GL rasteriser used to compensate by rendering AT the
 * window, which is why Clam Prix looked right and every Flash-rendered Leapster
 * title did not. Placement belongs here, once, for all of them.
 *
 * Anything that does not describe a rectangle inside the panel means the guest
 * has not told us a window, and the layer IS the panel — which is the Flash
 * UI's normal state and by far the common case.
 */
static void layer_window(const struct layer_state *ls, int w, int h,
                         int *wx, int *wy, int *ww, int *wh)
{
	int x = (int)ls->win_x, y = (int)ls->win_y;
	int cw = (int)ls->win_w, ch = (int)ls->win_h;

	if (cw <= 0 || ch <= 0 || x < 0 || y < 0 || x + cw > w || y + ch > h) {
		x = 0; y = 0; cw = w; ch = h;
	}
	*wx = x; *wy = y; *ww = cw; *wh = ch;
}

/* SAY WHERE A LAYER IS BEING COMPOSITED, once, and again whenever it moves.
 *
 * A misplaced layer is invisible in a log and ambiguous in a screenshot: the
 * ViewFrame art crops whatever spills out of the window, so "drawn in the
 * corner" and "drawn correctly but with the wrong content" look identical.
 * Three numbers per title settle it without a pixel ruler. */
static void say_layer(int idx, int wx, int wy, int ww, int wh)
{
	static int last[NUM_FB][4];
	static int seen[NUM_FB];

	if (seen[idx] && last[idx][0] == wx && last[idx][1] == wy &&
	    last[idx][2] == ww && last[idx][3] == wh)
		return;
	seen[idx] = 1;
	last[idx][0] = wx; last[idx][1] = wy;
	last[idx][2] = ww; last[idx][3] = wh;
	fprintf(stderr, "[tadpole] fb%d layer %dx%d at %d,%d\n",
	        idx, ww, wh, wx, wy);
}

/* Convert one layer into the ARGB8888 SDL texture. */
/* Layer format, out of fb_var_screeninfo.nonstd — include/linux/lf1000/
 * lf1000fb.h in the LF2 kernel drop:
 *
 *   #define LF1000_NONSTD_FORMAT       20
 *   #define LF1000_NONSTD_FORMAT_MASK  0x7
 *   enum { LAYER_FORMAT_RGB = 0, LAYER_FORMAT_YUV420 = 1, LAYER_FORMAT_YUV422 = 2 };
 *
 * fb0 and fb1 always report 0. fb2 is the MLC's video plane and reports
 * YUV420 the moment anything starts a video. */
#define LF_FORMAT(nonstd) (((nonstd) >> 20) & 0x7)
#define LF_FMT_RGB     0
#define LF_FMT_YUV420  1
#define LF_FMT_YUV422  2

/* THE VIDEO PLANE, WHICH IS NOT RGB AND NEVER WAS.
 *
 * This is why FMV showed nothing. The decoder worked the whole time — Brio's
 * libVideo.so decodes Theora in software with libtheora and writes the result
 * into fb2 — but fb2 holds YUV420, and every layer went through the packed-RGB
 * path above. A correct frame composited as noise or as nothing, and the only
 * visible symptom was "video plays audio, shows no picture".
 *
 * The plane layout is the driver's, not a guess. lf2000fb.c, nxfb_ops_set_par:
 *
 *   soc_dpc_set_vid_address(module, pbase,                        4096,
 *                                   pbase + 2048,                 4096,
 *                                   pbase + 2048 + 4096*yres/2,   4096, 0);
 *
 * so with P the layer pitch: Y at 0, Cb at P/2, Cr at P/2 + P*(yres/2), every
 * plane strided by P. The driver hardcodes P = 4096 (fix.line_length for the
 * YUV layer is always 4096, whatever the mode); we pass the pitch in because
 * our shim reports fb2's line_length by the ordinary RGB formula and Brio
 * lays the planes out from what we told it. Both agree on the SHAPE, which is
 * what this arithmetic depends on — see the note in docs about making the
 * shim report 4096 and why that is a separate, riskier change.
 *
 * 4:2:0, so one chroma sample covers a 2x2 luma block. */
static void blit_layer_yuv420(uint32_t *dst, int w, int h,
                              const struct layer_state *ls, const void *src)
{
	const unsigned char *base = src;
	size_t pitch = (size_t)w * (ls->bpp ? ls->bpp : 32) / 8;
	int x, y, wx, wy, ww, wh, sw, sh;

	if (!src)
		return;
	layer_window(ls, w, h, &wx, &wy, &ww, &wh);

	/* THE SCALER. The decoded picture is sw x sh; the MLC stretches it to the
	 * layer window. Sneak Peeks plays 320x240 trailers into a 362x272 window,
	 * so without this the right 42 columns and bottom 32 rows of every frame
	 * are read from a part of the buffer the decoder never wrote. Nearest
	 * neighbour: this is a 480x272 panel being upscaled again by the viewer,
	 * and a second filtering pass here would only smear it. */
	sw = ls->vid_w ? (int)ls->vid_w : ww;
	sh = ls->vid_h ? (int)ls->vid_h : wh;
	if (sw > (int)ls->xres) sw = (int)ls->xres;
	if (sh > (int)ls->yres) sh = (int)ls->yres;
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	for (y = 0; y < wh; y++) {
		int sy = wh == sh ? y : y * sh / wh;
		const unsigned char *yr = base + (size_t)sy * pitch;
		const unsigned char *cb = base + (size_t)(sy / 2) * pitch + pitch / 2;
		const unsigned char *cr = base + ((size_t)(sh / 2) + (size_t)(sy / 2))
		                        * pitch + pitch / 2;
		for (x = 0; x < ww; x++) {
			int sx = ww == sw ? x : x * sw / ww;
			int Y = yr[sx];
			int U = cb[sx / 2] - 128;
			int V = cr[sx / 2] - 128;
			int r = Y + ((91881 * V) >> 16);
			int g = Y - ((22554 * U + 46802 * V) >> 16);
			int b = Y + ((116130 * U) >> 16);
			if (r < 0) r = 0; else if (r > 255) r = 255;
			if (g < 0) g = 0; else if (g > 255) g = 255;
			if (b < 0) b = 0; else if (b > 255) b = 255;
			dst[(wy + y) * w + wx + x] = 0xFF000000u |
			        ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
		}
	}
}

static void blit_layer_rgb(uint32_t *dst, int w, int h,
                           const struct layer_state *ls,
                           const void *src, int first);

static void blit_layer(uint32_t *dst, int w, int h, const struct layer_state *ls,
                       const void *src, int first)
{
	if (LF_FORMAT(ls->nonstd) == LF_FMT_YUV420) {
		/* The video plane is the bottom layer and opaque where it is
		 * enabled, so `first` never has anything useful to say about it. */
		blit_layer_yuv420(dst, w, h, ls, src);
		return;
	}
	blit_layer_rgb(dst, w, h, ls, src, first);
}

static void blit_layer_rgb(uint32_t *dst, int w, int h,
                           const struct layer_state *ls,
                           const void *src, int first)
{
	int x, y, wx, wy, ww, wh;

	if (!src)
		return;
	layer_window(ls, w, h, &wx, &wy, &ww, &wh);

	if (ls->bpp == 16) {
		const uint16_t *s = src;
		for (y = 0; y < wh; y++) {
			for (x = 0; x < ww; x++) {
				uint16_t p = s[y * w + x];
				uint32_t r = (uint32_t)((p >> 11) & 0x1F) << 3;
				uint32_t g = (uint32_t)((p >> 5)  & 0x3F) << 2;
				uint32_t b = (uint32_t)( p        & 0x1F) << 3;
				dst[(wy + y) * w + wx + x] =
					0xFF000000u | (r << 16) | (g << 8) | b;
			}
		}
	} else {
		const uint32_t *s = src;
		for (y = 0; y < wh; y++) {
			for (x = 0; x < ww; x++) {
				uint32_t p = s[y * w + x];
				if (first) {
					dst[(wy + y) * w + wx + x] =
						0xFF000000u | (p & 0x00FFFFFFu);
				} else {
					uint32_t a = (p >> 24) & 0xFF;
					if (a)
						dst[(wy + y) * w + wx + x] =
							0xFF000000u | (p & 0x00FFFFFFu);
				}
			}
		}
	}
}

/* The same layer, but KEEPING its transparency instead of flattening it.
 *
 * The alpha in these buffers is a key, not a blend factor: the guest writes
 * zero where the layer below should show through and anything non-zero where
 * it should not. Turning that into a real 0/255 alpha channel lets the layer
 * be drawn as its own texture over whatever the GPU has already put on screen
 * — which is how a 1440x816 game picture gets underneath a 480x272 frame of
 * chrome without either being resampled to meet the other. */
static void blit_layer_keyed(uint32_t *dst, int w, int h,
                             const struct layer_state *ls, const void *src)
{
	int x, y, wx, wy, ww, wh;

	if (!src)
		return;
	if (ls->bpp == 16) {
		/* 16bpp has no alpha channel, so it is opaque everywhere. */
		blit_layer(dst, w, h, ls, src, 1);
		return;
	}
	layer_window(ls, w, h, &wx, &wy, &ww, &wh);
	{
		const uint32_t *s = src;
		for (y = 0; y < wh; y++)
			for (x = 0; x < ww; x++) {
				uint32_t p = s[y * w + x];
				dst[(wy + y) * w + wx + x] = ((p >> 24) & 0xFF)
				               ? (0xFF000000u | (p & 0x00FFFFFFu))
				               : 0u;
			}
	}
}

/* --selftest-layers: prove a layer lands on the panel where its window says.
 *
 * Needs no window and no GL, because the thing under test is arithmetic: the
 * layer's buffer holds win_w x win_h at the PANEL pitch starting at its base,
 * and the compositor has to put that at (win_x, win_y) and touch nothing else.
 * Getting this wrong by (win_x, win_y) is the whole of "the game is cut off and
 * is not centred", and it is invisible in a screenshot because the ViewFrame
 * art crops whatever spills out.
 *
 * The cases are the shapes that actually ship: the reading titles' 250x250 at
 * (76,11), a Leapster game's 320x240 at (15,17), the full-panel Flash UI, and a
 * layer whose window is nonsense — which must fall back to the whole panel
 * rather than drop the picture. */
static int selftest_layers(void)
{
	static const struct { int x, y, w, h, ex, ey, ew, eh; const char *what; } cases[] = {
		{  76, 11, 250, 250,  76, 11, 250, 250, "reading title 250x250" },
		{  15, 17, 320, 240,  15, 17, 320, 240, "Leapster game 320x240" },
		{   0,  0, 480, 272,   0,  0, 480, 272, "full panel"            },
		{   0,  0,   0,   0,   0,  0, 480, 272, "no window announced"   },
		{ 400,  0, 250, 250,   0,  0, 480, 272, "window off the panel"  },
	};
	const int w = 480, h = 272;
	uint32_t *src = malloc((size_t)w * h * 4);
	uint32_t *dst = malloc((size_t)w * h * 4);
	int bad = 0, k;

	if (!src || !dst) { free(src); free(dst); return 1; }
	printf("layer window -> panel placement\n\n");

	for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
		struct layer_state ls;
		int x, y, wx, wy, ww, wh, wrong = 0, missing = 0, spill = 0;

		memset(&ls, 0, sizeof(ls));
		ls.enabled = 1; ls.bpp = 32;
		ls.win_x = (uint32_t)cases[k].x; ls.win_y = (uint32_t)cases[k].y;
		ls.win_w = (uint32_t)cases[k].w; ls.win_h = (uint32_t)cases[k].h;

		layer_window(&ls, w, h, &wx, &wy, &ww, &wh);

		/* A source whose every pixel encodes its own position, so a shifted
		 * copy is caught rather than merely a blank one. */
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				src[y * w + x] = 0xFF000000u | (uint32_t)(y << 9) | (uint32_t)x;
		memset(dst, 0, (size_t)w * h * 4);
		blit_layer(dst, w, h, &ls, src, 1);

		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				int inside = x >= wx && x < wx + ww && y >= wy && y < wy + wh;
				uint32_t got = dst[y * w + x];
				if (!inside) {
					if (got) spill++;
					continue;
				}
				if (!got) { missing++; continue; }
				/* Source row (y-wy), column (x-wx) — the layer's own origin. */
				if (got != (0xFF000000u | (uint32_t)((y - wy) << 9)
				                        | (uint32_t)(x - wx)))
					wrong++;
			}

		if (wx != cases[k].ex || wy != cases[k].ey ||
		    ww != cases[k].ew || wh != cases[k].eh || wrong || missing || spill)
			bad++;
		printf("  %-24s window %d,%d %dx%d -> %d,%d %dx%d  "
		       "wrong %d missing %d spill %d  %s\n",
		       cases[k].what, cases[k].x, cases[k].y, cases[k].w, cases[k].h,
		       wx, wy, ww, wh, wrong, missing, spill,
		       (wx == cases[k].ex && wy == cases[k].ey &&
		        ww == cases[k].ew && wh == cases[k].eh &&
		        !wrong && !missing && !spill) ? "ok" : "FAIL");
	}

	free(src); free(dst);
	printf("\n%s\n", bad ? "FAILED — layers are not composited at their window"
	                     : "PASS — layers land where their window says");
	return bad ? 1 : 0;
}

static int guest_external(void);   /* defined with the guest controls below */

/* ---- --ui-shot: render one frame of a named UI state to a PNG -------------
 *
 * Companion to --selftest. The interface is now a real part of the program, so
 * it needs a way to be captured and compared that does not depend on a working
 * pointer (see the Wayland note in ui_debug_state).
 */
extern int compress2(unsigned char *dst, unsigned long *dstLen,
                     const unsigned char *src, unsigned long srcLen, int level);

static void put_be32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static unsigned crc32_of(const unsigned char *d, size_t n)
{
	static unsigned tab[256];
	unsigned c = 0xFFFFFFFFu;
	size_t i;
	int k;
	if (!tab[1])
		for (i = 0; i < 256; i++) {
			c = (unsigned)i;
			for (k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	c = 0xFFFFFFFFu;
	for (i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

static void png_chunk(FILE *f, const char *type, const unsigned char *d, unsigned n)
{
	unsigned char hdr[8];
	unsigned char *buf = malloc(n + 4);
	put_be32(hdr, n);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (n) fwrite(d, 1, n, f);
	if (buf) {
		memcpy(buf, type, 4);
		if (n) memcpy(buf + 4, d, n);
		put_be32(hdr, crc32_of(buf, n + 4));
		free(buf);
	} else {
		put_be32(hdr, crc32_of((const unsigned char *)type, 4));
	}
	fwrite(hdr, 1, 4, f);
}

static int write_png(const char *path, int w, int h, const unsigned char *rgb)
{
	FILE *f = fopen(path, "wb");
	unsigned char ihdr[13];
	unsigned char *raw, *z;
	unsigned long zn;
	int y;

	if (!f) return 0;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	put_be32(ihdr, (unsigned)w); put_be32(ihdr + 4, (unsigned)h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
	png_chunk(f, "IHDR", ihdr, 13);

	raw = malloc((size_t)h * (w * 3 + 1));
	for (y = 0; y < h; y++) {
		raw[(size_t)y * (w * 3 + 1)] = 0;              /* filter: none */
		memcpy(raw + (size_t)y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3,
		       (size_t)w * 3);
	}
	zn = (unsigned long)h * (w * 3 + 1) + 1024;
	z = malloc(zn);
	if (compress2(z, &zn, raw, (unsigned long)h * (w * 3 + 1), 6) == 0)
		png_chunk(f, "IDAT", z, (unsigned)zn);
	png_chunk(f, "IEND", NULL, 0);
	free(raw); free(z);
	fclose(f);
	return 1;
}

static int ui_shot(SDL_Renderer *ren, SDL_Window *win, const char *state,
                   const char *out, int rotate, int w, int h)
{
	int ow, oh, ok;
	unsigned char *rgb;
	int lw = (rotate == 90 || rotate == 270) ? h : w;
	int lh = (rotate == 90 || rotate == 270) ? w : h;

	/* Evaluate the SAME guest state the main loop does. Without this a shot
	 * always renders the idle case, so it cannot show a greyed-out File menu —
	 * exactly the thing being debugged when Run System Menu stuck disabled. */
	ui_set_running(guest_external());
	ui_debug_state(state);
	SDL_SetRenderDrawColor(ren, 12, 30, 20, 255);
	SDL_RenderClear(ren);
	ui_draw_idle(ren, lw, lh + UI_BAR_H);
	ui_draw(ren, lw, lh + UI_BAR_H);

	SDL_GetRendererOutputSize(ren, &ow, &oh);
	rgb = malloc((size_t)ow * oh * 3);
	if (!rgb) return 0;
	if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, ow * 3) != 0) {
		fprintf(stderr, "read pixels: %s\n", SDL_GetError());
		free(rgb);
		return 0;
	}
	ok = write_png(out, ow, oh, rgb);
	free(rgb);
	(void)win;
	printf("ui-shot %s -> %s (%dx%d)\n", state, out, ow, oh);
	return ok;
}

/* The input FIFOs are made by the SHIM, so on a cold start they do not exist
 * until a guest has booted. Opening them once at startup — which is all the
 * viewer used to need, because tadpole.sh created them before launching it —
 * leaves every fd at -1 forever and silently discards all input.
 *
 * That is not a subtle failure: AppManager arms a 12-second shutdown timer at
 * boot and expects to be told it is on external power. With the FIFOs never
 * opened the message is dropped, the timer fires, and the guest writes
 * /tmp/shutdown and exits on its own. Symptom: "it boots and then quits".
 */
static void ev_open_missing(void)
{
	char path[600];
	int i;
	for (i = 0; i < NUM_EV; i++) {
		if (g_evfd[i] >= 0)
			continue;
		snprintf(path, sizeof(path), "%s/ev%d", g_dir, i);
#ifndef _WIN32
		g_evfd[i] = open(path, O_RDWR | O_NONBLOCK);
#else
		g_evfd[i] = tp_fifo_fd(path, 0);   /* writer end; guest serves */
#endif
	}
}

/* Map the shim's shared files if they exist yet. Safe to call every frame:
 * once mapped it does nothing. */
/* The chrome lives INSIDE the logical space, above the guest's picture — see
 * the coordinate note in tadpole_ui.h. Every place that sets the logical size
 * has to agree, so they all come here. */
static void set_logical(SDL_Renderer *ren, int rotate, int w, int h)
{
	int lw = (rotate == 90 || rotate == 270) ? h : w;
	int lh = (rotate == 90 || rotate == 270) ? w : h;
	SDL_RenderSetLogicalSize(ren, lw, lh + UI_BAR_H);
}

static int try_map(void)
{
	char path[600];
	int i;

	if (g_state) return 1;
	snprintf(path, sizeof(path), "%s/state.bin", g_dir);
	g_state = map_file(path, NULL);
	if (!g_state) return 0;
	if (g_state->magic != TADPOLE_MAGIC) {   /* half-written; try again later */
		g_state = NULL;
		return 0;
	}
	snprintf(path, sizeof(path), "%s/fb0.bin", g_dir);
	g_fb[0] = map_file(path, &g_fbsz[0]);
	for (i = 1; i < NUM_FB; i++) {
		g_fb[i]   = g_fb[0];
		g_fbsz[i] = g_fbsz[0];
	}
	return g_fb[0] != NULL;
}

/* ---- running the guest --------------------------------------------------
 *
 * The viewer used to be something tadpole.sh started. Now it is the front end,
 * so it starts tadpole.sh instead — with --no-viewer, because the window it
 * would otherwise open is this one.
 *
 * The child gets its own process GROUP. tadpole.sh execs qemu as a grandchild
 * and backgrounds VideoDaemon beside it, so killing just the script leaves the
 * guest holding the input FIFOs — two readers then split the event stream,
 * which looks exactly like flaky touch. Signalling the group takes the lot.
 */
static pid_t g_guest;                 /* the emulator */
static pid_t g_tool;                  /* short-lived helper (install, etc) */
static char  g_tool_what[64];
static char  g_projdir[1024];

/* <proj>/tadpole/viewer/tadpole-view -> <proj>: strip three components. */
static void find_project_dir(const char *argv0)
{
	char buf[1024];
	int i;

	/* An explicit override WINS. Deriving the project from argv[0] works for a
	 * normal checkout, but not when the binary lives somewhere else entirely —
	 * inside an AppImage mount, or when testing one tree's viewer against
	 * another tree's data. */
	{
		const char *e = getenv("TADPOLE_PROJECT");
		if (e && *e) {
			snprintf(g_projdir, sizeof(g_projdir), "%s", e);
			/* Forward slashes here too, not only on the argv[0] path
			 * below. tadpole.exe hands this over in Windows spelling, and
			 * a backslash reaching the guest's LD_LIBRARY_PATH is not a
			 * separator to uClibc — every library then fails to load, with
			 * "can't load library 'libVideoMPI.so'" as the only clue. */
			for (i = 0; g_projdir[i]; i++)
				if (g_projdir[i] == '\\') g_projdir[i] = '/';
			return;
		}
	}
	if (!tp_realpath(argv0, buf, sizeof(buf))) {
		snprintf(g_projdir, sizeof(g_projdir), "%s", ".");
		return;
	}
	/* One separator convention below this line. Win32 APIs accept forward
	 * slashes everywhere, so normalise rather than teach every strrchr
	 * about backslashes. A no-op on Linux. */
	for (i = 0; buf[i]; i++)
		if (buf[i] == '\\') buf[i] = '/';
	for (i = 0; i < 3; i++) {
		char *slash = strrchr(buf, '/');
		if (!slash || slash == buf) break;
		*slash = 0;
	}
	/* Strip-three is right for <proj>/tadpole/viewer/tadpole-view and one
	 * short for the CMake layout's viewer/build/tadpole-view.exe — where it
	 * lands on <proj>/tadpole, every prerequisite looks missing, and the
	 * wizard tells a fully-installed machine to start over. So verify the
	 * guess the way tadpole.exe does: the project root is wherever
	 * tadpole.sh is, walking up if need be. */
	for (i = 0; i < 3; i++) {
		char probe[1100];
		char *slash;
		snprintf(probe, sizeof(probe), "%s/tadpole.sh", buf);
		if (access(probe, F_OK) == 0) break;
		slash = strrchr(buf, '/');
		if (!slash || slash == buf) break;
		*slash = 0;
	}
	snprintf(g_projdir, sizeof(g_projdir), "%s", buf);
}

/* qemu-user hands the guest OUR environment and then applies its own -E
 * overrides on top, so exporting a variable here is enough to reach the shim
 * inside the guest — tadpole.sh does not have to forward each one by hand.
 *
 * THE DEBUG LEVEL EXPANDS HERE, in one place, so that "level 2" has exactly
 * one meaning. See the table in tadpole_ui.h.
 */
#ifndef _WIN32
static void guest_setenv(const struct ui_settings *c)
{
	char buf[32];
	int lv = c->debug_level;

	if (c->gl)           setenv("TADPOLE_GL", "1", 1);          else unsetenv("TADPOLE_GL");
	/* TADPOLE_GL_SOFTWARE COMES FROM THE USER'S SHELL, not from the settings —
	 * it is the deliberate way to the deprecated software rasteriser, and on
	 * Windows setting it before launching the viewer is the ONLY way to reach
	 * it, since the checkbox that used to is now locked on. The guest inherits
	 * our environment, so it arrives on its own; the point of naming it here is
	 * that TADPOLE_GL_HLE must not be sent alongside it. The shim prefers
	 * software when both are present, but "both are present" is a contradiction
	 * to have to reason about at the far end. */
	if (getenv("TADPOLE_GL_SOFTWARE") || !c->gl_hle)
		unsetenv("TADPOLE_GL_HLE");
	else
		setenv("TADPOLE_GL_HLE", "1", 1);
	/* This guest is launched WITH --no-viewer, but not headless: we are the
	 * viewer, already running, and we read the shared arena this guest writes
	 * into for host-GPU replay same as if it had opened its own window.
	 * tadpole.sh's --no-viewer refusal exists for the OTHER case — a caller
	 * with no viewer anywhere in the picture — and cannot tell the two apart
	 * on its own, so we say so. */
	setenv("TADPOLE_SUPERVISED", "1", 1);
	if (c->gl_dumpframe) setenv("TADPOLE_GL_DUMPFRAME", "1", 1);else unsetenv("TADPOLE_GL_DUMPFRAME");
	if (c->gl_dumptex)   setenv("TADPOLE_GL_DUMPTEX", "1", 1);  else unsetenv("TADPOLE_GL_DUMPTEX");
	if (c->touch_debug)  setenv("TADPOLE_TOUCH_DEBUG", "1", 1); else unsetenv("TADPOLE_TOUCH_DEBUG");
	/* NOT unset when absent from the settings, unlike its neighbours. The
	 * "Stop if HLE falls back" row is gone — replay dying raises a dialog now,
	 * and the only remaining choice was between that and killing the title,
	 * which is a debugging preference rather than a setting. So the environment
	 * is the only way to ask for it, and clearing it here would take that away
	 * from anyone who did. */
	if (c->hle_strict) setenv("TADPOLE_HLE_STRICT", "1", 1);
	if (c->tslib)        setenv("TADPOLE_TSLIB", "1", 1);       else unsetenv("TADPOLE_TSLIB");

	/* Level 2 turns on the shim's own tracing and the GL layer's; level 3 adds
	 * every guest syscall, which tadpole.sh turns into qemu -strace. */
	if (lv >= 2) { setenv("TADPOLE_DEBUG", "1", 1); setenv("TADPOLE_GL_DEBUG", "1", 1); }
	else         { unsetenv("TADPOLE_DEBUG"); unsetenv("TADPOLE_GL_DEBUG"); }
	if (lv >= 3) setenv("TADPOLE_STRACE", "1", 1); else unsetenv("TADPOLE_STRACE");

	/* The shim reads TADPOLE_HZ's PRESENCE as "the user has an opinion", so an
	 * explicit 0 is how you ask for uncapped and unset is the 60 Hz default.
	 * Always write it: leaving it unset would let a stale value from whatever
	 * shell launched the viewer decide the frame rate. */
	snprintf(buf, sizeof(buf), "%d", c->frame_cap);
	setenv("TADPOLE_HZ", buf, 1);

	if (c->audio_pace) unsetenv("TADPOLE_AUDIO_PACE");
	else               setenv("TADPOLE_AUDIO_PACE", "0", 1);

	if (c->io_delay_us > 0) {
		snprintf(buf, sizeof(buf), "%d", c->io_delay_us);
		setenv("TADPOLE_IO_DELAY_US", buf, 1);
	} else {
		unsetenv("TADPOLE_IO_DELAY_US");
	}
	setenv("TADPOLE_DIR", g_dir, 1);
}
#endif  /* !_WIN32 — only spawn_script's child calls this */

/* ---- where the guest's output goes ---------------------------------------
 *
 * AppManager is talkative — four hundred lines to reach the home screen — and
 * until now every one of them went to the viewer's stdout. That is fine when
 * you started it from a terminal and useless the rest of the time: launched
 * from a desktop icon there is no terminal at all, so the single most useful
 * artefact for working out why a title died was simply discarded.
 *
 * The guest now writes down a pipe that we pump: to the log file when one is
 * wanted, and to stdout when there is a terminal to read it. At level 0 there
 * is no pipe and the output goes to /dev/null, which is the difference between
 * "quiet" and "hidden".
 */
static int   g_glog_fd = -1;
static FILE *g_glog_file;

static void guest_log_path(char *out, size_t n)
{
	/* XDG override, then the platform's app-data directory, then ~/.local.
	 * LOCALAPPDATA is just an environment variable — set on every Windows,
	 * never set on Linux — so this chain needs no #ifdef to be right on
	 * both. */
	const char *x = getenv("XDG_STATE_HOME");
	const char *la = getenv("LOCALAPPDATA");
	const char *home = getenv("HOME");
	char *p;
	if (x && *x)        snprintf(out, n, "%s/tadpole", x);
	else if (la && *la) snprintf(out, n, "%s/Tadpole/state", la);
	else                snprintf(out, n, "%s/.local/state/tadpole", home ? home : "/tmp");
	/* EVERY level of it. ~/.local/state does not exist on a fresh account, and
	 * one mkdir() of a path whose parent is missing fails with ENOENT — which
	 * is exactly how the first log file went nowhere, silently, while the
	 * emulator carried on as though it had been written. */
	for (p = out + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		tp_mkdir(out);
		*p = '/';
	}
	tp_mkdir(out);
	{
		size_t l = strlen(out);
		snprintf(out + l, n - l, "/tadpole.log");
	}
}

static void guest_log_open(void)
{
	char p[1100], old[1160];
	if (g_glog_file) { fclose(g_glog_file); g_glog_file = NULL; }
	if (!ui_cfg()->log_to_file || ui_cfg()->debug_level < 1) return;
	guest_log_path(p, sizeof(p));
	/* Keep exactly one previous run. The log you want is nearly always the one
	 * from the boot that just went wrong, and starting the next boot would
	 * otherwise erase it. */
	snprintf(old, sizeof(old), "%s.1", p);
	rename(p, old);
	g_glog_file = fopen(p, "w");
	if (g_glog_file)
		setvbuf(g_glog_file, NULL, _IOLBF, 0);
}

static void guest_log_pump(void)
{
	char chunk[1024];
	ssize_t n;
	if (g_glog_fd < 0) return;
	while ((n = read(g_glog_fd, chunk, sizeof chunk)) > 0) {
		if (g_glog_file) fwrite(chunk, 1, (size_t)n, g_glog_file);
		if (isatty(1))   fwrite(chunk, 1, (size_t)n, stdout);
	}
	if (n == 0) {                       /* the guest closed it */
		close(g_glog_fd);
		g_glog_fd = -1;
	}
}

static void guest_log_close(void)
{
	guest_log_pump();
	if (g_glog_fd >= 0) { close(g_glog_fd); g_glog_fd = -1; }
	if (g_glog_file) { fclose(g_glog_file); g_glog_file = NULL; }
}

/* A guest we did NOT start still counts as running — tadpole.sh writes its pid
 * to $TADPOLE_DIR/.lock and refuses a second instance on the same dir. Without
 * this the menu would offer "Run System Menu" against a live guest and the
 * launch would just fail on the lock. */
#ifdef _WIN32
/* ---- guest supervision, Windows --------------------------------------------
 *
 * Not fork-shaped after all. On Linux tadpole.sh assembles an emulator
 * command and the viewer forks the script; here the same knowledge assembles
 * the same command for CreateProcess — glasspole IS the emulator on Windows,
 * and starting a process is the one part of supervision Win32 does natively.
 * What stays unported are the SCRIPTS: the tools written in bash and python
 * (updates, installs) still answer honestly that they need the full build.
 *
 * The guest environment mirrors what tools/tadpole-win.ps1 proved out, with
 * paths spelled drive-relative: they must survive a colon-separated
 * LD_LIBRARY_PATH, and glasspole's sysroot fallthrough opens them natively
 * against the current drive. */
static HANDLE g_guest_h;
static HANDLE g_tool_h;

/* Run one of the project's Python tools and stream its output to the progress
 * panel.
 *
 * OUTPUT GOES THROUGH A FILE, NOT A PIPE, and that is the point. The panel is
 * drained by tool_drain() from the UI thread with plain read() calls that
 * expect a non-blocking descriptor; an anonymous pipe on Windows has no such
 * mode, so a read between the tool's lines would freeze the window. A file
 * has exactly the semantics wanted for free: reads return what has been
 * written so far and 0 at the end, over and over, until the writer finishes.
 * Same trick as the guest's log, for the same reason.
 *
 * Which Python: TADPOLE_PYTHON if set, then the py launcher, then python3 and
 * python. First one that starts wins; if none do, the caller's "could not
 * start" message is the truth. */
/* Where Python actually is, in the order worth trying. PATH is the LEAST
 * reliable of these on Windows: python.org's per-user installer does not add
 * itself by default, so "python" resolving is the exception rather than the
 * rule, and a tool that only tries PATH tells most users they have no Python
 * while it sits in their profile. */
static const char *find_python(char *buf, size_t n)
{
	const char *e = getenv("TADPOLE_PYTHON");
	char probe[900];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	if (e && *e && access(e, F_OK) == 0) {
		snprintf(buf, n, "\"%s\"", e);
		return buf;
	}
	/* Project-local, the Windows spelling of tools/fetch-deps.sh's
	 * build/deps/python — a Python that came with Tadpole beats one that
	 * happens to be installed. */
	snprintf(probe, sizeof(probe), "%s/build/deps/python/python.exe", g_projdir);
	if (access(probe, F_OK) == 0) {
		snprintf(buf, n, "\"%s\"", probe);
		return buf;
	}
	e = getenv("LOCALAPPDATA");
	if (e && *e) {
		snprintf(probe, sizeof(probe), "%s\\Programs\\Python\\Python3*", e);
		h = FindFirstFileA(probe, &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				snprintf(probe, sizeof(probe),
				         "%s\\Programs\\Python\\%s\\python.exe", e, fd.cFileName);
				if (access(probe, F_OK) == 0) {
					FindClose(h);
					snprintf(buf, n, "\"%s\"", probe);
					return buf;
				}
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}
	return NULL;              /* the PATH candidates are tried after this */
}

static pid_t spawn_python(const char *rel, char *const argv[], int *outfd)
{
	static const char *cand[] = { NULL, "py -3", "python3", "python" };
	char found[1024];
	char cmd[4096], logp[700];
	SECURITY_ATTRIBUTES sa;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HANDLE lh;
	unsigned c;
	int i, n, ok = 0;

	cand[0] = find_python(found, sizeof(found));

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	snprintf(logp, sizeof(logp), "%s/tool.log", g_dir);
	lh = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                 &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (lh == INVALID_HANDLE_VALUE)
		return -1;

	for (c = 0; c < sizeof(cand) / sizeof(cand[0]) && !ok; c++) {
		if (!cand[c] || !cand[c][0])
			continue;
		n = snprintf(cmd, sizeof(cmd), "%s \"%s/%s\"", cand[c], g_projdir, rel);
		for (i = 1; argv[i] && n < (int)sizeof(cmd) - 4; i++)
			n += snprintf(cmd + n, sizeof(cmd) - n, " \"%s\"", argv[i]);
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		si.dwFlags    = STARTF_USESTDHANDLES;
		si.hStdOutput = lh;
		si.hStdError  = lh;
		memset(&pi, 0, sizeof(pi));
		if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
		                   NULL, g_projdir, &si, &pi))
			ok = 1;
	}
	CloseHandle(lh);
	if (!ok) {
		fprintf(stderr, "tadpole-view: no Python found for %s — install "
		        "Python 3 from python.org, or set TADPOLE_PYTHON\n", rel);
		ui_status("this needs Python 3 installed");
		return -1;
	}
	CloseHandle(pi.hThread);
	if (g_tool_h)
		CloseHandle(g_tool_h);
	g_tool_h = pi.hProcess;
	if (outfd)
		*outfd = open(logp, O_RDONLY);
	return (pid_t)pi.dwProcessId;
}

/* C:\x/y -> /x/y (drop_drive) or C:/x/y (keep it), because this is the only
 * place host spelling becomes guest spelling.
 *
 * The separator conversion is never optional: a backslash is not a separator
 * to uClibc OR to glasspole's HostPath, so a TADPOLE_DIR of "C:\Users\...\run"
 * would reach the guest as a single opaque name, the shim's framebuffer files
 * would go somewhere the guest never looks, and the title would die inside its
 * display module with "No framebuffer allocation available". %LOCALAPPDATA%
 * hands us exactly that spelling, so this is not defensive: it is the common
 * case.
 *
 * THE DRIVE LETTER IS A DIFFERENT QUESTION, AND GETTING IT WRONG COST AN
 * INSTALL. Dropping it was the rule for everything, because a colon splits a
 * colon-separated LD_LIBRARY_PATH and the resulting driveless path resolves
 * against the guest process's current drive — which the viewer sets to the
 * install tree, so the install's own paths come out right by construction.
 *
 * TADPOLE_DIR is the one path that is NOT in the install tree. It lives under
 * %LOCALAPPDATA%, i.e. on the user profile's drive. On the C: install that
 * everyone including the installer's default uses, the two drives are the same
 * one and the missing letter is refilled correctly by accident. Install to E:
 * and the guest is told the runtime directory is at /Users/<name>/AppData/...
 * — which it dutifully resolves on E:, where there is no such directory. Every
 * fb0.bin, state.bin and event node then fails to open, and the log reads:
 *
 *     [0x5] CreateHandle: No framebuffer allocation available
 *     <ASSERT>: [0x0] Unsupported destination PixelFormat used 0
 *
 * — a framebuffer complaint with no framebuffer code behind it.
 *
 * So: keep the drive for the single-value variables that may name another
 * volume, drop it only where a colon genuinely cannot survive. LD_LIBRARY_PATH
 * is that one place, and every entry in it is inside the install tree anyway.
 * glasspole anchors what is left on the drive its own .exe was loaded from
 * rather than on the current directory — see the note above widen() in
 * host_win32.c — so the driveless spelling no longer depends on how the
 * process was started either. */
static void drel(const char *in, char *out, size_t n, int drop_drive)
{
	size_t i;
	if (drop_drive && in[0] && in[1] == ':')
		in += 2;
	for (i = 0; i + 1 < n && in[i]; i++)
		out[i] = in[i] == '\\' ? '/' : in[i];
	out[i] = 0;
}

static pid_t spawn_script(const char *script, char *const argv[], int as_guest,
                          int *outfd, int silent)
{
	char R[1024], dir[600], cmd[4096];
	const char *prog = NULL;
	int i, n;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	(void)silent;
	if (outfd) *outfd = -1;

	/* TOOLS RUN AS PYTHON HERE. Every tool the viewer spawns exists in both
	 * spellings now: the .sh ones are Linux's entry points and the .py ones
	 * are what Windows runs, with no bash anywhere. The Linux path is
	 * untouched — it still spawns the scripts.
	 *
	 * FIRMWARE WAS THE HOLE, and it was the whole product on Windows. The
	 * setup wizard could download system files and then had nothing to
	 * install them with, so the honest refusal below left a user who had
	 * done everything right staring at a first-run screen. install-firmware,
	 * online-update, make-profile and erase-firmware are the four that
	 * closes. A profile matters as much as the firmware: without one the
	 * system boots to Create Profile and stops, so "installed" and
	 * "playable" are different states and both need a tool here.
	 *
	 * The refusal stays for anything genuinely unported, because a tool that
	 * half-works is worse than one that says it cannot. */
	if (!as_guest) {
		static const struct { const char *sh, *py; } port[] = {
			{ "tools/install-game.sh",     "tools/install-game.py"     },
			{ "tools/scan-games.sh",       "tools/scan-games.py"       },
			{ "tools/install-firmware.sh", "tools/install-firmware.py" },
			{ "tools/install-didj.sh",     "tools/install-didj.py"     },
			{ "tools/online-update.sh",    "tools/online-update.py"    },
			{ "tools/make-profile.sh",     "tools/make-profile.py"     },
			{ "tools/erase-firmware.sh",   "tools/erase-firmware.py"   },
		};
		const char *py = NULL;
		unsigned k;
		for (k = 0; k < sizeof(port) / sizeof(port[0]); k++)
			if (!strcmp(script, port[k].sh)) { py = port[k].py; break; }
		if (!py && strstr(script, ".py"))               py = script;
		if (!py) {
			fprintf(stderr, "tadpole-view: cannot run %s — that tool is a "
			        "shell script and Windows has no shell for it\n", script);
			return -1;
		}
		return spawn_python(py, argv, outfd);
	}
	if (strcmp(script, "tadpole.sh") != 0) {
		fprintf(stderr, "tadpole-view: cannot run %s — guests launch "
		        "natively on Windows\n", script);
		return -1;
	}
	/* argstart is where the GUEST's own argv begins, and it is tracked
	 * explicitly rather than left in the loop variable: falling out of the
	 * scan with i on the NULL terminator and then stepping past it reads off
	 * the end of argv, which crashes the viewer the moment anyone picks Run
	 * System Menu. */
	int argstart = 0;
	for (i = 1; argv[i]; i++) {
		if (!strcmp(argv[i], "--boot")) {
			prog = "/LF/Base/bin/AppManager";
			break;                  /* AppManager takes no arguments here */
		}
		if (!strcmp(argv[i], "--run") && argv[i + 1]) {
			prog = argv[i + 1];
			argstart = i + 2;       /* everything after is the guest's */
			break;
		}
		if (!strcmp(argv[i], "--app")) {
			ui_status("launch titles from the home screen on Windows");
			return -1;
		}
	}
	if (!prog) {
		ui_status("nothing to launch");
		return -1;
	}

	/* R: the install tree, drive filed off. Everything derived from it stays
	 * that way — LD_LIBRARY_PATH because a colon there is a separator, and
	 * TADPOLE_SYSROOT because the shim prepends it to guest paths and then
	 * chdir()s into the result, which glasspole canonicalises as a guest path.
	 * All of those live on the install's own drive, so the drive glasspole
	 * anchors them on is the right one.
	 *
	 * dir: TADPOLE_DIR, WITH ITS DRIVE, because it is the one path in the run
	 * that is not in the install tree — %LOCALAPPDATA% is on the user profile's
	 * volume, which is only the install's volume by coincidence. See drel().
	 *
	 * --sysroot is given absolutely rather than as "runtime/sysroot". It was
	 * relative, which quietly made the sysroot lookup depend on the child's
	 * current directory being the one we pass below; when that assumption
	 * fails nothing says so — every guest path falls through to the literal
	 * branch and the rootfs appears to be empty.
	 *
	 * Quoted, all of them: "C:\Program Files\..." and any user whose name has
	 * a space in it would otherwise end the argument early. */
	drel(g_projdir, R, sizeof(R), 1);
	drel(g_dir, dir, sizeof(dir), 0);
	n = snprintf(cmd, sizeof(cmd),
	    "\"%s/glasspole/build/glasspole.exe\" --sysroot \"%s/runtime/sysroot\""
	    " -E \"LD_LIBRARY_PATH=%s/runtime/shimlibs-gl:%s/runtime/shimlibs-z:"
	    "%s/runtime/shimlibs:%s/runtime/libs\""
	    " -E \"TADPOLE_DIR=%s\""
	    " -E TSLIB_CONFFILE=/nonexistent-ts.conf"
	    " -E \"TADPOLE_SYSROOT=%s/runtime/sysroot\"",
	    g_projdir, g_projdir, R, R, R, R, dir, R);
	if (ui_cfg()->gl || ui_cfg()->gl_hle)
		n += snprintf(cmd + n, sizeof(cmd) - n, " -E TADPOLE_GL=1");
	/* HLE REPLAY GOES TO THE GUEST ON WINDOWS AGAIN — THE BLOCKER IS GONE.
	 *
	 * It was held back because AppManager died before its first frame with
	 *     [0x5] CreateHandle: No framebuffer allocation available
	 *     <ASSERT> Unsupported destination PixelFormat used 0
	 * while TADPOLE_GL=1 alone stayed up.
	 *
	 * RE-MEASURED, and it no longer happens. AppManager launched with
	 * TADPOLE_GL=1 TADPOLE_GL_HLE=1 against a viewer holding the ring boots
	 * to "UI entered" with three successful CreateHandle calls at
	 * 480x272 (1920). Confirmed as not-my-doing by rebuilding glasspole from
	 * this same commit with the host_win32.c view fix reverted: THAT boots
	 * too. Something between the original measurement and here repaired it —
	 * the TADPOLE_DIR spelling fix and the syscall/errno/chdir work are all
	 * candidates — so this note deliberately does not claim a cause.
	 *
	 * WHAT WITHHOLDING IT COST, which is the reason this matters more than a
	 * crash: it did not turn rendering off, it silently selected the SOFTWARE
	 * RASTERISER. tadpole.sh's own note describes that path as drawing simple
	 * screens correctly and "visibly mangling busy ones". So every 3D title on
	 * Windows was being judged on the fallback, and "content too large for its
	 * frame, a banner drawn flipped, objects missing" is what that fallback
	 * looks like — a Windows-only rendering bug with no Windows-only rendering
	 * code behind it.
	 *
	 * If it regresses, the honest move is to re-measure and say so here, not
	 * to withhold silently: a user cannot tell the fallback from the real
	 * thing except by the picture being wrong. */
	if (ui_cfg()->gl_hle)
		n += snprintf(cmd + n, sizeof(cmd) - n, " -E TADPOLE_GL_HLE=1");
	/* THE DIAGNOSIS VARIABLES, which guest_env() sets on POSIX and this path
	 * did not set at all. Windows spells its guest environment by hand, one
	 * -E at a time, so a variable not named here simply never reaches the
	 * shim — and TADPOLE_GL_DEBUG was never named here. Debug level 2 in the
	 * viewer therefore turned on the GL trace on Linux and did NOTHING on
	 * Windows, which is precisely the platform where the 3D output is wrong
	 * and the trace is the thing you want.
	 *
	 * It looked like it was working, which is why it survived: tad_gl_warn()
	 * and tad_gl_report() write to gl-warnings.log whatever the level is, so
	 * a Windows run still produced a GL error tally. What it could not
	 * produce was the per-call trace naming the enum that raised them. */
	if (ui_cfg()->debug_level >= 2)
		n += snprintf(cmd + n, sizeof(cmd) - n,
		              " -E TADPOLE_DEBUG=1 -E TADPOLE_GL_DEBUG=1");
	if (ui_cfg()->gl_dumpframe)
		n += snprintf(cmd + n, sizeof(cmd) - n, " -E TADPOLE_GL_DUMPFRAME=1");
	if (ui_cfg()->gl_dumptex)
		n += snprintf(cmd + n, sizeof(cmd) - n, " -E TADPOLE_GL_DUMPTEX=1");
	n += snprintf(cmd + n, sizeof(cmd) - n, " %s", prog);
	if (argstart)
		for (i = argstart; argv[i] && n < (int)sizeof(cmd) - 2; i++)
			n += snprintf(cmd + n, sizeof(cmd) - n, " %s", argv[i]);

	/* THE GUEST MUST HAVE SOMEWHERE TO WRITE. CREATE_NO_WINDOW alone leaves
	 * the child with no console and no standard handles, so every guest
	 * write() to stdout fails — and AppManager, four hundred log lines
	 * chattier than anything else here, gives up and exits within seconds.
	 * That looks exactly like "the emulator cannot start" and is not.
	 * So hand it a real file: $TADPOLE_DIR/guest.log, inheritable, as both
	 * stdout and stderr. It is the Windows spelling of the POSIX path's
	 * pipe-or-/dev/null, and it doubles as the log a user can send us. */
	{
		SECURITY_ATTRIBUTES sa;
		char logp[700];
		HANDLE lh;

		memset(&sa, 0, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		snprintf(logp, sizeof(logp), "%s/guest.log", g_dir);
		lh = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                 &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (lh == INVALID_HANDLE_VALUE)
			lh = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);

		/* WITH DEBUG ON, THE GUEST GETS A CONSOLE OF ITS OWN. AppManager is
		 * four hundred lines of boot chatter and the single most useful
		 * artefact when a title misbehaves; watching it arrive live is worth
		 * a window, and it is what "debug level 1" means everywhere else in
		 * this program. Quiet by default: a console per launch would be
		 * noise for someone who only wants to play. The log file is written
		 * either way, so nothing is lost by choosing the quiet one. */
		int console = ui_cfg()->debug_level >= 1;
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		if (!console) {
			si.dwFlags    = STARTF_USESTDHANDLES;
			si.hStdInput  = NULL;
			si.hStdOutput = lh;
			si.hStdError  = lh;
		}
		memset(&pi, 0, sizeof(pi));
		if (!CreateProcessA(NULL, cmd, NULL, NULL, !console,
		                    console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
		                    NULL, g_projdir, &si, &pi)) {
			if (lh != INVALID_HANDLE_VALUE) CloseHandle(lh);
			fprintf(stderr, "tadpole-view: glasspole would not start "
			        "(GetLastError %lu)\n", (unsigned long)GetLastError());
			ui_status("glasspole would not start — is it built?");
			return -1;
		}
		if (lh != INVALID_HANDLE_VALUE) CloseHandle(lh);
	}
	CloseHandle(pi.hThread);
	if (g_guest_h)
		CloseHandle(g_guest_h);
	g_guest_h = pi.hProcess;
	return (pid_t)pi.dwProcessId;
}

static int guest_external(void) { return 0; }

static int guest_alive(void)
{
	if (!g_guest_h)
		return 0;
	if (WaitForSingleObject(g_guest_h, 0) == WAIT_TIMEOUT)
		return 1;
	CloseHandle(g_guest_h);
	g_guest_h = NULL;
	return 0;
}

static void guest_stop(void)
{
	if (!g_guest_h)
		return;
	/* TerminateProcess without ceremony: since the shared-view change there
	 * is no write-back to lose, and the guest holds no state outside its
	 * TADPOLE_DIR files, which are already coherent. */
	TerminateProcess(g_guest_h, 0);
	WaitForSingleObject(g_guest_h, 2000);
	CloseHandle(g_guest_h);
	g_guest_h = NULL;
}

/* Reap a finished Python tool, so the progress panel closes and reports.
 * Without this the panel would sit at "starting..." for ever, which is how a
 * working installer still looks broken. */
static int tool_reap(pid_t pid, int *exited_ok)
{
	DWORD code = 1;
	(void)pid;
	if (!g_tool_h)
		return 0;
	if (WaitForSingleObject(g_tool_h, 0) == WAIT_TIMEOUT)
		return 0;
	GetExitCodeProcess(g_tool_h, &code);
	CloseHandle(g_tool_h);
	g_tool_h = NULL;
	*exited_ok = (code == 0);
	return 1;
}

#else  /* POSIX guest supervision */

static int guest_external(void)
{
	char path[600];
	FILE *f;
	int pid = 0;

	snprintf(path, sizeof(path), "%s/.lock", g_dir);
	if (!(f = fopen(path, "r"))) return 0;
	if (fscanf(f, "%d", &pid) != 1) pid = 0;
	fclose(f);
	if (pid <= 0 || kill(pid, 0) != 0)
		return 0;
	/* The lock means "this TADPOLE_DIR is in use", NOT "a guest is running".
	 * Since tadpole.sh now launches us and waits, the pid in there is usually
	 * the script that started us — and treating that as a running guest greys
	 * out Run System Menu and Launch .swf permanently, on a perfectly good
	 * install. tadpole.sh exports its own pid so we can tell them apart. */
	{
		const char *own = getenv("TADPOLE_LOCK_PID");
		if (own && atoi(own) == pid)
			return 0;
	}
	return 1;
}

static int guest_alive(void)
{
	int st;
	if (g_guest <= 0) return 0;
	if (waitpid(g_guest, &st, WNOHANG) == g_guest) { g_guest = 0; return 0; }
	return 1;
}

/* THE ONE THAT LEFT THE GROUP. VideoDaemon daemonizes the textbook way — fork,
 * setsid, close every descriptor (see tadpole_shim.c) — so it is in ITS OWN
 * SESSION and no kill(-pgid) can ever reach it. Measured on a close:
 *
 *     pid     ppid   pgid    sid
 *     172353  172322 172322  172290   the viewer's session — killed
 *     172348  3853   172346  172346   its own — outlived every close
 *
 * tadpole.sh already knows this happens and reaps by TADPOLE_DIR, but it does
 * so at STARTUP, which is a launch too late: between closing and next opening,
 * a guest nobody can see is still running, holding the arena and the audio
 * FIFO. Do it here, at the moment of closing, which is where it belongs.
 *
 * MATCHED EXACTLY, and ancestors skipped, for the reasons tadpole.sh gives at
 * length: a prefix match makes /tmp/tadpole reap /tmp/tadpole-2, and our own
 * launcher exports the same TADPOLE_DIR, so killing "anything that matches"
 * kills the shell that started us.
 */
#ifdef __linux__
static int is_our_ancestor(pid_t cand)
{
	pid_t p = getpid();
	int hops;
	for (hops = 0; p > 1 && hops < 64; hops++) {
		char path[64];
		FILE *f;
		int ppid = 0;
		if (p == cand) return 1;
		snprintf(path, sizeof(path), "/proc/%d/stat", (int)p);
		if (!(f = fopen(path, "r"))) return 0;
		/* comm can contain spaces and parentheses; ppid is the field after
		 * the last ')'. */
		{
			char buf[512], *rp;
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			buf[n] = 0;
			rp = strrchr(buf, ')');
			if (!rp || sscanf(rp + 1, " %*c %d", &ppid) != 1) ppid = 0;
		}
		fclose(f);
		p = ppid;
	}
	return 0;
}

/* THE ENVIRONMENT VARIABLE ALONE IS NOT ENOUGH TO KILL SOMETHING OVER.
 *
 * TADPOLE_DIR is exported, so EVERY child of the launcher inherits it — during
 * testing this sweep cheerfully killed a `sleep` that happened to be a sibling
 * of the viewer. Anything the user ran from the same shell would go the same
 * way. So the environment says WHICH session a process belongs to, and this
 * says whether it is a guest at all: only the emulator binary is ever swept.
 *
 * TADPOLE_QEMU first, because that is what actually got launched — a user
 * running Glasspole, or a bring-your-own build, is not covered by a hardcoded
 * list. The two names are the fallback for when it is unset. */
static int looks_like_a_guest(int pid)
{
	char path[64], comm[64], *nl;
	const char *emu = getenv("TADPOLE_QEMU");
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	if (!(f = fopen(path, "r"))) return 0;
	n = fread(comm, 1, sizeof(comm) - 1, f);
	fclose(f);
	comm[n] = 0;
	if ((nl = strchr(comm, '\n'))) *nl = 0;
	if (!comm[0]) return 0;

	if (emu && *emu) {
		const char *b = strrchr(emu, '/');
		b = b ? b + 1 : emu;
		/* comm is truncated to 15 characters; compare on that. */
		if (!strncmp(comm, b, 15)) return 1;
	}
	return !strcmp(comm, "qemu-arm") || !strcmp(comm, "glasspole");
}

static void guest_sweep_stragglers(void)
{
	char want[600];
	DIR *d;
	struct dirent *e;
	size_t wlen;

	if (!g_dir || !*g_dir) return;
	wlen = (size_t)snprintf(want, sizeof(want), "TADPOLE_DIR=%s", g_dir);
	if (wlen >= sizeof(want)) return;
	if (!(d = opendir("/proc"))) return;

	while ((e = readdir(d))) {
		char path[64], buf[8192];
		int pid = atoi(e->d_name);
		FILE *f;
		size_t n, i;
		int hit = 0;

		if (pid <= 1 || pid == (int)getpid()) continue;
		snprintf(path, sizeof(path), "/proc/%d/environ", pid);
		if (!(f = fopen(path, "r"))) continue;      /* gone, or not ours */
		n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		/* environ is NUL-separated: compare whole entries, never a prefix. */
		for (i = 0; i < n; i += strlen(buf + i) + 1) {
			if (!strcmp(buf + i, want)) { hit = 1; break; }
			if (!buf[i]) break;
		}
		if (!hit || is_our_ancestor((pid_t)pid)) continue;
		if (!looks_like_a_guest(pid)) continue;
		kill((pid_t)pid, SIGKILL);
	}
	closedir(d);
}
#else
static void guest_sweep_stragglers(void) { }
#endif

/* SWEEP THE WHOLE GROUP, EVEN WHEN THE LEADER WENT QUIETLY.
 *
 * A guest is not one process. AppManager starts VideoDaemon, and both run in
 * the process group this function signals. The SIGTERM goes to the group, but
 * the wait loop only ever watched the LEADER — and returned the moment it was
 * reaped, before the SIGKILL below. So a sibling that does not act on SIGTERM
 * outlived every close, orphaned, holding $TADPOLE_DIR and its .lock.
 *
 * That is not theoretical: four VideoDaemon processes from a session hours
 * earlier were still running when this was found, and one survives every
 * `--boot` and close, measured. The next launch then meets
 *
 *     tadpole: another instance (pid NNN) is using /tmp/tadpole
 *
 * which is how "I closed it" and "it is still running" end up both true.
 *
 * So the SIGKILL is now unconditional. It costs one signal to an empty group
 * in the common case, and kill(2) on a group with no members simply fails with
 * ESRCH, which is the correct outcome and needs no test. */
static void guest_stop(void)
{
	int i, st, reaped = 0;

	if (g_guest > 0) {
		kill(-g_guest, SIGTERM);
		for (i = 0; i < 40; i++) {             /* up to 2s to go quietly */
			if (waitpid(g_guest, &st, WNOHANG) == g_guest) { reaped = 1; break; }
			SDL_Delay(50);
		}
		kill(-g_guest, SIGKILL);
		if (!reaped) waitpid(g_guest, &st, 0);
		g_guest = 0;
	}
	/* UNCONDITIONALLY, because the guest is very often not ours to wait on.
	 * `./tadpole.sh --app X` — the normal way to start a title — launches the
	 * emulator from the SCRIPT and the viewer beside it, so g_guest is 0 here
	 * and the early return this used to take meant closing the window left the
	 * whole guest running. The window went away and the emulator did not,
	 * which is as close to "closing it does nothing" as makes no difference. */
	guest_sweep_stragglers();
}

/* argv must be NULL-terminated; runs <proj>/<script> with the settings applied.
 *
 * `outfd`, when non-NULL, receives a read end carrying the child's stdout AND
 * stderr. Installing firmware takes minutes, and without its output the UI has
 * nothing to show but a spinner — the progress panel exists to display exactly
 * these lines. */
static pid_t spawn_script(const char *script, char *const argv[], int as_guest,
                          int *outfd, int silent)
{
	char path[1100];
	pid_t pid;
	int pfd[2] = { -1, -1 };

	snprintf(path, sizeof(path), "%s/%s", g_projdir, script);
	if (outfd && pipe(pfd) != 0) { *outfd = -1; outfd = NULL; }
	pid = fork();
	if (pid < 0) {
		if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
		return -1;
	}
	if (pid == 0) {
		setpgid(0, 0);
		if (outfd) {
			close(pfd[0]);
			dup2(pfd[1], 1);
			dup2(pfd[1], 2);
			close(pfd[1]);
		} else if (silent) {
			/* Debug level 0: the guest says nothing. Opened here rather than
			 * dropped later so the writes never happen at all. */
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0) { dup2(null, 1); dup2(null, 2); close(null); }
		}
		if (as_guest) guest_setenv(ui_cfg());
		if (chdir(g_projdir) != 0) _exit(126);
		execv(path, argv);
		_exit(127);
	}
	setpgid(pid, pid);          /* also set in the parent: avoids the race */
	if (outfd) {
		close(pfd[1]);
		/* Non-blocking: the UI loop must keep drawing while the tool works. */
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
		*outfd = pfd[0];
	}
	return pid;
}

/* Nonblocking reap: 1 with *exited_ok set when pid has finished, else 0. */
static int tool_reap(pid_t pid, int *exited_ok)
{
	int st;
	if (waitpid(pid, &st, WNOHANG) != pid) return 0;
	*exited_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	return 1;
}

#endif  /* guest supervision */

static void guest_launch_ui(void)
{
	char *av[8];
	int n = 0, quiet = ui_cfg()->debug_level < 1;
	av[n++] = (char *)"tadpole.sh";
	av[n++] = (char *)"--no-viewer";
	/* --boot because tadpole.sh no longer starts the system on its own: its
	 * default is now front-end-only, and WE are the front end asking for it. */
	av[n++] = (char *)"--boot";
	av[n] = NULL;
	guest_stop();
	guest_log_close();
	guest_log_open();
	g_guest = spawn_script("tadpole.sh", av, 1, quiet ? NULL : &g_glog_fd, quiet);
	ui_set_running(g_guest > 0);
	ui_status(g_guest > 0 ? "booting..." : "launch failed");
}

/* A .swf is run by the guest's Flash player, so the path has to be one the
 * GUEST can see: sysroot-relative, not a host path. */
/* Start an installed app by PackageID.
 *
 * tadpole.sh --app resolves the entry point out of the package's meta.inf and
 * takes both kinds: a .swf goes to saplayer, a native App.so goes to
 * AppManager with TADPOLE_LAUNCH set. That distinction is exactly what the
 * old "Launch .swf" browser could not make, so it could only ever start half
 * the library. */
static void guest_launch_app(const char *pkg)
{
	char *av[6];
	int i = 0;

	if (!pkg || !pkg[0])
		return;
	av[i++] = (char *)"tadpole.sh";
	av[i++] = (char *)"--app";
	av[i++] = (char *)pkg;
	av[i] = NULL;
	guest_stop();
	guest_log_close();
	guest_log_open();
	g_guest = spawn_script("tadpole.sh", av, 1, &g_glog_fd, 0);
	if (g_guest > 0) ui_status("%s", pkg);
	else             ui_status("could not start %s", pkg);
}

static void guest_launch_swf(const char *hostpath)
{
	char sysroot[1100], *av[8];
	const char *g;
	size_t n;
	int i = 0;

	snprintf(sysroot, sizeof(sysroot), "%s/runtime/sysroot", g_projdir);
	n = strlen(sysroot);
	if (strncmp(hostpath, sysroot, n) != 0 || hostpath[n] != '/') {
		ui_status("swf must live inside runtime/sysroot");
		return;
	}
	g = hostpath + n;
	av[i++] = (char *)"tadpole.sh";
	av[i++] = (char *)"--no-viewer";
	av[i++] = (char *)"--run";
	av[i++] = (char *)"/LF/Base/Flash/bin/saplayer";
	av[i++] = (char *)g;
	av[i] = NULL;
	guest_stop();
	{
		int quiet = ui_cfg()->debug_level < 1;
		guest_log_close();
		guest_log_open();
		g_guest = spawn_script("tadpole.sh", av, 1,
		                       quiet ? NULL : &g_glog_fd, quiet);
	}
	ui_set_running(g_guest > 0);
	ui_status(g_guest > 0 ? "running swf" : "launch failed");
}

static int  g_tool_fd = -1;
static char g_tool_buf[512];
static int  g_tool_len;

/* Up to two arguments, either of which may be NULL — enough for every tool
 * here, and it keeps the "one argument" callers unchanged. */
/* The update check reuses the tool runner, but its output is a report to be
 * parsed rather than a log to be shown, and it must not open a progress panel.
 * g_tool_update marks which kind of run is in flight. */
static int  g_tool_update;
static char g_update_dest[1100];

static void tool_run2(const char *what, const char *script,
                      const char *a1, const char *a2)
{
	char *av[4];
	int n = 0;
	av[n++] = (char *)script;
	if (a1) av[n++] = (char *)a1;
	if (a2) av[n++] = (char *)a2;
	av[n] = NULL;
	if (g_tool > 0) { ui_status("busy: %s", g_tool_what); return; }
	snprintf(g_tool_what, sizeof(g_tool_what), "%s", what);
	g_tool_len = 0;
	g_tool = spawn_script(script, av, 0, &g_tool_fd, 0);
	ui_status("%s...", what);
	if (g_tool > 0) {
		ui_progress_begin(what);
		ui_progress_line("starting...");
	} else {
		ui_status("%s could not start", what);
	}
}

static void tool_run(const char *what, const char *script, const char *arg)
{
	tool_run2(what, script, arg, NULL);
}

/* Up to six arguments, for the tools that take flags rather than a path. */
static void tool_runv(const char *what, const char *script, char *const av[])
{
	char *argv[8];
	int n = 0;
	argv[n++] = (char *)script;
	while (av[n - 1] && n < 7) { argv[n] = av[n - 1]; n++; }
	argv[n] = NULL;
	if (g_tool > 0) { ui_status("busy: %s", g_tool_what); return; }
	snprintf(g_tool_what, sizeof(g_tool_what), "%s", what);
	g_tool_len = 0;
	g_tool = spawn_script(script, argv, 0, &g_tool_fd, 0);
	ui_status("%s...", what);
	/* No progress panel for the update check: it is a background errand, and
	 * covering the window with a box every launch to say "still nothing new"
	 * is exactly the behaviour that makes people turn updaters off. */
	if (g_tool > 0 && !g_tool_update) {
		ui_progress_begin(what); ui_progress_line("starting...");
	} else if (g_tool <= 0) {
		g_tool_update = 0;
		ui_status("%s could not start", what);
	}
}

/* Drain whatever the tool has written, a line at a time. Called every frame. */
static void tool_drain(void)
{
	char chunk[256];
	ssize_t n;

	if (g_tool_fd < 0) return;
	while ((n = read(g_tool_fd, chunk, sizeof chunk)) > 0) {
		ssize_t i;
		for (i = 0; i < n; i++) {
			if (chunk[i] == '\n' || g_tool_len == (int)sizeof(g_tool_buf) - 1) {
				g_tool_buf[g_tool_len] = 0;
				/* A tool that knows its total says so, and the panel draws a
				 * measured bar instead of a moving one. Anything else is an
				 * ordinary log line. */
				if (!strncmp(g_tool_buf, "@@PROGRESS ", 11)) {
					long a2 = 0, b2 = 0;
					if (sscanf(g_tool_buf + 11, "%ld %ld", &a2, &b2) == 2 && b2 > 0)
						ui_progress_pct((int)((a2 * 100) / b2));
				} else if (g_tool_update) {
					ui_update_line(g_tool_buf);
				} else if (!strncmp(g_tool_buf, "pct ", 4)) {
					ui_progress_pct(atoi(g_tool_buf + 4));
				} else if (g_tool_len) {
					ui_progress_line(g_tool_buf);
				}
				g_tool_len = 0;
			} else if (chunk[i] != '\r') {
				g_tool_buf[g_tool_len++] = chunk[i];
			}
		}
	}
}

static void tool_poll(void)
{
	int ok = 0;
	if (g_tool <= 0) return;
	tool_drain();
	if (!tool_reap(g_tool, &ok)) return;
	tool_drain();                      /* anything written just before exit */
	if (g_tool_len) {
		g_tool_buf[g_tool_len] = 0;
		if (g_tool_update) ui_update_line(g_tool_buf);
		else               ui_progress_line(g_tool_buf);
	}
	g_tool_len = 0;
	if (g_tool_fd >= 0) { close(g_tool_fd); g_tool_fd = -1; }

	/* THE UPDATE CHECK OWNS ITS OWN ENDING. It never opened a progress panel
	 * — a check that flashes a dialog at every launch is worse than no check —
	 * so it must not close one, and its report is shown by the UI instead. */
	if (g_tool_update) {
		g_tool_update = 0;
		g_tool = 0;
		ui_update_finish();
		ui_status(" ");
		return;
	}
	if (g_update_dest[0] && ok) {
		/* INSTALL IT AND RESTART, rather than leaving homework.
		 *
		 * "Saved as <path> — now quit, swap the files and chmod +x" was a
		 * correct instruction and a bad update: it asks someone to do by hand
		 * the one step that is easy to get wrong, and until they do, the thing
		 * they downloaded is not the thing they are running.
		 *
		 * A running AppImage is a mounted image, so it cannot be overwritten
		 * in place — but it CAN be replaced by rename, which is atomic and
		 * leaves the running mount alone: the old inode stays alive until this
		 * process exits, which is exactly the moment we stop needing it.
		 * Then re-exec the new path.
		 */
		const char *img = getenv("APPIMAGE");
		char dest[1100];
		snprintf(dest, sizeof(dest), "%s", g_update_dest);
		g_update_dest[0] = 0;

#ifdef _WIN32
		/* Hand over to the installer and get out of its way. It has to
		 * replace files this process is holding open, so the running program
		 * closing itself IS the last step of the update rather than an
		 * inconvenience to work around.
		 *
		 * "/S" — SILENTLY, AND THAT IS THE POINT OF THE WHOLE EXERCISE.
		 *
		 * Updating here used to mean: the window disappears, an installer
		 * appears, agree to the introduction, confirm the directory, press
		 * Install, then find Tadpole and open it again. Six steps and a
		 * closed program. On Linux the same button renames the new AppImage
		 * over the running one and execv()s it — the window blinks and comes
		 * back on the new version, and nothing is asked. That gap is what
		 * "Windows users cannot update as easily" means.
		 *
		 * A silent NSIS install skips every page, keeps the directory the
		 * previous install recorded in the registry, and the installer's
		 * .onInstSuccess starts tadpole.exe again when it is done. The user
		 * presses Download and the program comes back updated, which is what
		 * the Linux side has always done. Running it by hand from Explorer is
		 * unaffected and still asks everything it used to. */
		ui_progress_line("installing the update");
		ui_progress_done(1);
		guest_stop();
		guest_log_close();
		if ((INT_PTR)ShellExecuteA(NULL, "open", dest, "/S", NULL, SW_SHOWNORMAL) > 32) {
			SDL_Quit();
			exit(0);
		}
		ui_progress_line("downloaded, but the installer would not start —");
		ui_progress_line(dest);
		return;
#endif
		if (img && img[0] && !strcmp(dest + strlen(dest) - 4, ".new")) {
			char target[1100];
			snprintf(target, sizeof(target), "%s", img);
			if (rename(dest, target) == 0) {
				chmod(target, 0755);
				ui_progress_line("installed - restarting Tadpole");
				ui_progress_done(1);
				guest_stop();
				guest_log_close();
				SDL_Quit();
				/* execv, not fork: the user asked for the new version, and
				 * leaving the old one running beside it is how you end up
				 * with two windows and no idea which is which. */
				{
					char *av[2];
					av[0] = target;
					av[1] = NULL;
					execv(target, av);
				}
				/* Only reached if exec failed — say so plainly, do not
				 * pretend the update worked. */
				ui_progress_line("installed, but could not restart - "
				                 "close Tadpole and open it again");
				return;
			}
			{
				char note[1200];
				snprintf(note, sizeof(note),
				         "Downloaded, but could not replace %s - move %s "
				         "over it yourself.", target, dest);
				ui_progress_line(note);
			}
		} else {
			char note[1200];
			snprintf(note, sizeof(note),
			         "Saved as %s - this is not an AppImage install, so "
			         "nothing was replaced.", dest);
			ui_progress_line(note);
		}
	}
	ui_status("%s %s", g_tool_what, ok ? "done" : "FAILED");
	ui_invalidate_prereqs();      /* it may have installed or erased things */
	/* A scan writes a new index, and an install changes which titles are
	 * marked as already there — both are what the library is looking at. */
	ui_games_reload();
	ui_progress_done(ok);
	g_tool = 0;
}

int main(int argc, char **argv)
{
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	uint32_t *pixels;
	/* THE PICTURE IS BUILT IN THREE PIECES, not one.
	 *
	 * The guest's layers composite bottom-up: fb2, then fb1, then fb0 on top,
	 * and for a 3D title fb1 IS the game viewport — its layer window is
	 * 320x240 at +15+17 and everything outside that is transparent, with the
	 * bamboo frame and the A/B/L/R buttons all living on fb0 above it.
	 *
	 * That is what makes a high-resolution game picture possible at all: the
	 * GL image occupies one rectangle, with guest 2D strictly above and below
	 * it. So the two halves are composited separately on the CPU as before,
	 * and the GL layer is drawn between them at whatever size we have it in —
	 * 1440x816 rather than 320x240. Nothing else in the frame changes.
	 *
	 * With HLE off, `top` is still split out and drawn second, which composites
	 * to exactly what the single buffer used to produce. */
	SDL_Texture *tex_top = NULL;      /* fb0: chrome, alpha-keyed, drawn last */
	uint32_t *pixels_top = NULL;
	SDL_Texture *tex_gl = NULL;       /* the game viewport at draw resolution */
	uint32_t *gl_px = NULL;
	int gl_tw = 0, gl_th = 0;         /* size of tex_gl */
	int top_drawn = 0;                /* fb0 had something to show this frame */
	int n;                            /* index into the composite order */
	int vid_over_fb1 = 0;             /* MLC video priority puts fb2 above fb1 */
	int said_vid_order = -1;
	int g_upd_checked = 0;            /* the silent update check has run */
	int gl_have = 0;                  /* tex_gl holds a current frame */
	Uint32 gl_stamp = 0;              /* when that frame arrived */
	int gl_rx = 0, gl_ry = 0, gl_rw = 0, gl_rh = 0;   /* where it belongs */
	Uint32 fps_at = 0;                /* clock at the last frame-rate sample */
	unsigned long fps_frames = 0, fps_packets = 0;    /* counters at that sample */
	int fps_primed = 0;               /* fps_at/fps_frames hold a real reading */
	int fps_shown = 0;                /* the status line is ours to clear */
	int gpu_lost_told = 0;            /* the replay-died dialog, once a session */
	char path[512];
	int scale = 2, w, h, i, running = 1, touching = 0;
	int rotate = 0;   /* degrees CW; portrait apps need 90 */
	char actpath[1024];
	int selftest_want = 0;
	const char *shot_state = NULL, *shot_out = NULL;
	int boot_now = 0, power_announced = 0;
	const char *env;

	/* --version, ANSWERED BEFORE ANYTHING ELSE HAPPENS.
	 *
	 * "Which build am I running?" is the first question on every bug report and
	 * the only place it could be answered was Help -> About, which needs a
	 * window, a GPU and a working install to reach. It is also the only way to
	 * check a Windows build without a Windows desktop in front of you, which is
	 * how the "released binaries report dev" complaint went unmeasured for as
	 * long as it did: the string is in the executable, but nothing would say it
	 * out loud.
	 *
	 * Deliberately the very first thing in main — before find_project_dir(),
	 * before settings are read and long before SDL — so it answers on a machine
	 * where the rest of the program cannot start at all. */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			printf("%s\n", TADPOLE_VERSION);
			return 0;
		}
	}

	find_project_dir(argv[0]);
	/* Saved settings are the defaults; command-line flags still win, so the
	 * scripted harnesses (probe-launch.sh et al) keep behaving as before. */
	ui_preload_settings();
	scale  = ui_cfg()->scale;
	rotate = ui_cfg()->rotate;
	/* The compiled-in default is /tmp/tadpole, which is meaningless on
	 * Windows. LOCALAPPDATA is the Windows spelling of "per-user writable app
	 * data" and is never set on Linux, so checking the environment — rather
	 * than the platform — keeps this one chain correct on both. An explicit
	 * TADPOLE_DIR still wins, as it always has. */
	{
		static char defdir[600];
		const char *la = getenv("LOCALAPPDATA");
		if (la && *la) {
			snprintf(defdir, sizeof(defdir), "%s/Tadpole", la);
			tp_mkdir(defdir);
			snprintf(defdir + strlen(defdir), sizeof(defdir) - strlen(defdir),
			         "/run");
			tp_mkdir(defdir);
			g_dir = defdir;
		}
	}
	if ((env = getenv("TADPOLE_DIR")) != NULL)
		g_dir = env;
	g_touch_debug = getenv("TADPOLE_TOUCH_DEBUG") != NULL;
	g_raw_dpad    = getenv("TADPOLE_RAW_DPAD") != NULL;
	if ((env = getenv("TADPOLE_DPAD_SHIFT")) != NULL) g_dpad_shift = atoi(env) & 3;
	g_dpad_mirror = getenv("TADPOLE_DPAD_MIRROR") != NULL;
	if ((env = getenv("TADPOLE_TS_MAX_X")) != NULL) g_ts_max_x = atoi(env);
	if ((env = getenv("TADPOLE_TS_MAX_Y")) != NULL) g_ts_max_y = atoi(env);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			scale = ui_cfg()->scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			g_dir = argv[++i];
		else if (!strcmp(argv[i], "-r") && i + 1 < argc)
			rotate = ui_cfg()->rotate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--selftest"))
			selftest_want = 1;
		else if (!strcmp(argv[i], "--selftest-layers"))
			return selftest_layers();
		else if (!strcmp(argv[i], "--boot"))
			boot_now = 1;
		else if (!strcmp(argv[i], "--ui-shot") && i + 2 < argc) {
			shot_state = argv[++i];
			shot_out   = argv[++i];
		}
	}
	if (selftest_want)
		return selftest(rotate, scale);

	/* NOT fatal any more. The front end has to come up with nothing running
	 * so File -> Run System Menu is reachable; the shim creates these files
	 * when a guest starts, and try_map() picks them up then. */
	try_map();
	w = g_state ? (int)g_state->width  : 480;
	h = g_state ? (int)g_state->height : 272;
	if (w <= 0 || h <= 0) { w = 480; h = 272; }
	printf("tadpole-view: %dx%d, scale %dx, dir %s\n", w, h, scale, g_dir);
	/* O_RDWR on a FIFO never blocks and never fails with ENXIO, so we can
	 * hold the write end open whether or not the guest is reading yet. */
	for (i = 0; i < NUM_EV; i++) {
		snprintf(path, sizeof(path), "%s/ev%d", g_dir, i);
#ifndef _WIN32
		g_evfd[i] = open(path, O_RDWR | O_NONBLOCK);
#else
		g_evfd[i] = tp_fifo_fd(path, 0);   /* writer end; guest serves */
#endif
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	{
		int ww = (rotate == 90 || rotate == 270) ? h : w;
		int wh = (rotate == 90 || rotate == 270) ? w : h;
		ui_brand_apply();
		win = SDL_CreateWindow(ui_brand_name(), SDL_WINDOWPOS_CENTERED,
		                       SDL_WINDOWPOS_CENTERED,
		                       ww * scale, (wh + UI_BAR_H) * scale,
		                       SDL_WINDOW_RESIZABLE);
	}
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	set_logical(ren, rotate, w, h);
	ui_init(ren, g_projdir);
	{
		SDL_Surface *ico = ui_icon_surface();
		if (ico) { SDL_SetWindowIcon(win, ico); SDL_FreeSurface(ico); }
	}
	if (shot_state) {
		int rc = ui_shot(ren, win, shot_state, shot_out, rotate, w, h) ? 0 : 1;
		ui_shutdown();
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return rc;
	}
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
	                        SDL_TEXTUREACCESS_STREAMING, w, h);
	pixels = malloc((size_t)w * h * 4);
	tex_top = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
	                            SDL_TEXTUREACCESS_STREAMING, w, h);
	if (tex_top) SDL_SetTextureBlendMode(tex_top, SDL_BLENDMODE_BLEND);
	pixels_top = malloc((size_t)w * h * 4);

	/* "Boot the system menu at startup" — for people who set Tadpole up once
	 * and afterwards only ever want the LeapPad. Not while the wizard is up:
	 * with no firmware there is nothing to boot, and starting a doomed guest
	 * underneath the panel that explains why would be its own puzzle. */
	if (ui_cfg()->boot_on_start && !ui_modal())
		guest_launch_ui();

	/* HOST-GPU REPLAY. Brought up before any guest starts, because the guest
	 * checks for a live host heartbeat when it first decides whether to encode
	 * — if we are not ready it silently keeps rasterising in software. */
	/* Env var as well as the setting, so a one-line command is self-contained:
	 *   TADPOLE_GL_HLE=1 ./tadpole-view -d DIR --boot                        */
	if (getenv("TADPOLE_GL_HLE"))
		ui_cfg()->gl_hle = 1;
	if (ui_cfg()->gl_hle) {
		/* Anti-aliasing costs nothing the guest can see: its own glViewport is
		 * discarded, so the sample count is ours to pick. TADPOLE_GL_MSAA
		 * overrides the setting for a one-line comparison run. */
		int aa = ui_cfg()->msaa;
		int ss = ui_cfg()->render_scale;
		const char *e = getenv("TADPOLE_GL_MSAA");
		const char *e2 = getenv("TADPOLE_GL_SCALE");
		if (e) aa = atoi(e);
		if (e2) ss = atoi(e2);
		if (!hle_host_init(g_dir, w, h, aa, ss))
			ui_status("HLE unavailable; software raster");
	}

	audio_open_fifo();

	/* --boot: start the system menu immediately, so the front end can still
	 * be a single command for anyone who wants it that way. */
	if (boot_now)
		guest_launch_ui();

	/* Deferred until the FIFO is open — see ev_open_missing(). */
	power_announced = 0;

	while (running) {
		SDL_Event e;

		while (SDL_PollEvent(&e)) {
			/* Chrome first. A click on the bar, an open menu or any modal
			 * belongs to the front end and must NOT also reach the guest. */
			{
				int lw = (rotate == 90 || rotate == 270) ? h : w;
				int lh = (rotate == 90 || rotate == 270) ? w : h;
				if (ui_event(&e, lw, lh + UI_BAR_H)) {
					if (touching) {   /* don't strand a held stylus */
						touching = 0;
						send_event(EV_TOUCH, EV_ABS, ABS_PRESSURE, 0);
						send_event(EV_TOUCH, EV_KEY, BTN_TOUCH, 0);
						send_event(EV_TOUCH, EV_SYN, SYN_REPORT, 0);
					}
					continue;
				}
			}
			switch (e.type) {
			case SDL_WINDOWEVENT:
				/* Re-assert the logical size whenever the window is
				 * resized.
				 *
				 * SDL is supposed to recompute the render scale and
				 * letterbox viewport itself on SIZE_CHANGED, and it
				 * demonstrably does under SDL_SetWindowSize (see
				 * --selftest). A user drag-resize is a different path,
				 * and a scale left over from the previous size gives
				 * an error that is zero at the viewport origin and
				 * grows linearly with distance from it — which is
				 * precisely the reported symptom, and would also
				 * explain touch being accurate at exactly one window
				 * size. Cheap to make certain: if SDL already handled
				 * it this is a no-op.
				 */
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
				    e.window.event == SDL_WINDOWEVENT_RESIZED)
					set_logical(ren, rotate, w, h);
				break;
			case SDL_QUIT:
				running = 0;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				uint16_t code;
				if (e.key.repeat)
					break;
				/* KEYDOWN only, like the Ctrl+R handler below it. Q is the
				 * guest's L button now, and swallowing its key-UP because
				 * Ctrl happened to be held would leave L stuck down inside
				 * the guest with nothing to release it. */
				if (e.key.keysym.sym == SDLK_q && e.type == SDL_KEYDOWN &&
				    (e.key.keysym.mod & KMOD_CTRL)) {
					running = 0;
					break;
				}
				/* Many LeapPad titles are portrait and draw rotated into
				 * the landscape framebuffer — the same reason the stock
				 * boot art is named "...CW.png". R cycles orientation. */
				if (e.key.keysym.sym == SDLK_r && e.type == SDL_KEYDOWN &&
				    (e.key.keysym.mod & KMOD_CTRL)) {
					rotate = (rotate + 90) % 360;
					ui_cfg()->rotate = rotate;
					ui_cfg_save();
					set_logical(ren, rotate, w, h);
					printf("rotate = %d\n", rotate);
					break;
				}
				code = map_key(e.key.keysym.sym, rotate);
				if (code)
					send_key(EV_GPIO_KEYS, code, e.type == SDL_KEYDOWN);
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
				int fx, fy;
				event_to_fb(rotate, w, h, e.button.x,
				            e.button.y - UI_BAR_H, &fx, &fy);
				touching = (e.type == SDL_MOUSEBUTTONDOWN);
				g_touch_mark_x = fx; g_touch_mark_y = fy;
				send_event(EV_TOUCH, EV_ABS, ABS_X, fx);
				send_event(EV_TOUCH, EV_ABS, ABS_Y, fy);
				send_event(EV_TOUCH, EV_ABS, ABS_PRESSURE, touching ? TOUCH_PRESSURE : 0);
				send_event(EV_TOUCH, EV_KEY, BTN_TOUCH, touching);
				send_event(EV_TOUCH, EV_SYN, SYN_REPORT, 0);
				if (g_touch_debug)
				{
					int ww, wh, ow, oh;
					float rsx, rsy;
					SDL_Rect vp;
					SDL_GetWindowSize(win, &ww, &wh);
					SDL_GetRendererOutputSize(ren, &ow, &oh);
					/* A STALE render scale after a resize is the one
					 * fault that produces error growing linearly with
					 * distance from the viewport origin, so print it. */
					SDL_RenderGetScale(ren, &rsx, &rsy);
					SDL_RenderGetViewport(ren, &vp);
					printf("touch %s win(%d,%d) [win %dx%d out %dx%d]"
					       " -> fb(%d,%d) rot=%d  rscale %.4f vp %d,%d %dx%d"
					       "  nearest grid dot (%d,%d) off by (%+d,%+d)\n",
					       touching ? "down" : "up",
					       e.button.x, e.button.y, ww, wh, ow, oh,
					       fx, fy, rotate, rsx, vp.x, vp.y, vp.w, vp.h,
					       30 + 60 * ((fx - 30 + 30) / 60),
					       30 + 60 * ((fy - 30 + 30) / 60),
					       fx - (30 + 60 * ((fx - 30 + 30) / 60)),
					       fy - (30 + 60 * ((fy - 30 + 30) / 60)));
				}
				break;
			}
			case SDL_MOUSEMOTION:
				if (touching) {
					int fx, fy;
					event_to_fb(rotate, w, h, e.motion.x,
					            e.motion.y - UI_BAR_H, &fx, &fy);
					g_touch_mark_x = fx; g_touch_mark_y = fy;
					send_event(EV_TOUCH, EV_ABS, ABS_X, fx);
					send_event(EV_TOUCH, EV_ABS, ABS_Y, fy);
					send_event(EV_TOUCH, EV_ABS, ABS_PRESSURE, TOUCH_PRESSURE);
					send_event(EV_TOUCH, EV_SYN, SYN_REPORT, 0);
				}
				break;
			}
		}

		/* Replay whatever the guest queued. The finished frame lands in the
		 * SAME place the software rasteriser writes — fb1's page in the shared
		 * arena — so the three-layer compositor below needs no changes. */
		if (hle_host_ready()) {
			uint32_t *dst = NULL;
			if (g_state && g_fb[1]) {
				const struct layer_state *ls = &g_state->layer[1];
				size_t pitch = (size_t)w * (ls->bpp ? ls->bpp : 32) / 8;
				size_t off = (size_t)ls->yoffset * pitch;
				int wx, wy, ww, wh;
				layer_window(ls, w, h, &wx, &wy, &ww, &wh);
				dst = (off + pitch * (size_t)wh <= g_fbsz[1])
				    ? (uint32_t *)((unsigned char *)g_fb[1] + off)
				    : (uint32_t *)g_fb[1];
			}
			if (!dst) {
				/* Drain into scratch when the shared arena is not mapped yet.
				 * The guest BLOCKS on present waiting for frames_done, so
				 * skipping the pump would stall it for a second and then make
				 * it fall back to software for the rest of the session — the
				 * ring must keep moving even when we have nowhere to put the
				 * result. */
				static uint32_t *scratch;
				if (!scratch) scratch = malloc((size_t)w * h * 4);
				dst = scratch;
			}
			/* SAY WHERE THE FRAME IS GOING, once, and again if it changes.
			 *
			 * Falling back to scratch is indistinguishable from working: the
			 * replay reports a full scene ("59% non-black") because it counts
			 * the buffer it just wrote, while the arena the compositor reads
			 * stays black. A capture then shows an empty screen next to
			 * statistics claiming a rendered frame, and nothing says which of
			 * the two is lying. */
			if (dst) {
				static const void *last_dst;
				static int said_scratch = -1;
				int to_scratch = (g_state && g_fb[1])
				               ? ((const void *)dst < g_fb[1] ||
				                  (const char *)dst >= (const char *)g_fb[1] + g_fbsz[1])
				               : 1;
				if (dst != last_dst || to_scratch != said_scratch) {
					last_dst = dst;
					said_scratch = to_scratch;
					if (getenv("TADPOLE_HLE_DEBUG"))
						fprintf(stderr, "hle: presenting to %s "
						        "(state=%p fb1=%p yoff=%u)\n",
						        to_scratch ? "SCRATCH — the compositor will "
						                     "never see this" : "the fb1 arena",
						        (void *)g_state, g_fb[1],
						        g_state ? g_state->layer[1].yoffset : 0);
				}
				hle_host_pump(dst, (unsigned)w);

				/* THE GAME LAYER, AT DRAW RESOLUTION.
				 *
				 * Only worth doing when there is more resolution to have; at
				 * 1x the panel-sized frame the guest already received is the
				 * same picture, and going round again would cost a second
				 * transfer for nothing. */
				if (ui_cfg()->render_scale > 1 && hle_host_scale() > 1) {
					int fw = 0, fh = 0;
					hle_host_full(&fw, &fh);
					if (fw > 0 && fh > 0) {
						if (fw != gl_tw || fh != gl_th) {
							/* The layer rectangle can change when a title
							 * switches screens, so the texture follows it. */
							if (tex_gl) SDL_DestroyTexture(tex_gl);
							free(gl_px);
							tex_gl = SDL_CreateTexture(ren,
							             SDL_PIXELFORMAT_ARGB8888,
							             SDL_TEXTUREACCESS_STREAMING, fw, fh);
							gl_px = malloc((size_t)fw * fh * 4);
							gl_tw = fw; gl_th = fh;
							if (tex_gl)
								SDL_SetTextureBlendMode(tex_gl, SDL_BLENDMODE_NONE);
						}
						if (tex_gl && gl_px && hle_host_read_full(gl_px)) {
							SDL_UpdateTexture(tex_gl, NULL, gl_px, fw * 4);
							hle_host_rect(&gl_rx, &gl_ry, &gl_rw, &gl_rh);
							/* SAY WHICH PATH IS DRAWING THE GAME, once.
							 * "Looks the same" is the expected outcome of a
							 * resolution change that silently did not happen,
							 * and the two are otherwise indistinguishable
							 * from a screenshot. */
							if (!gl_have)
								fprintf(stderr, "hle: viewer is drawing the game "
								        "layer itself, %dx%d into %dx%d at %d,%d\n",
								        fw, fh, gl_rw, gl_rh, gl_rx, gl_ry);
							gl_have = 1;
							gl_stamp = SDL_GetTicks();
						} else if (!gl_have) {
							static int said;
							if (!said++)
								fprintf(stderr, "hle: no full-size frame yet "
								        "(scale %d, %dx%d) — still showing the "
								        "guest's own layer\n",
								        hle_host_scale(), fw, fh);
						}
					}
				} else if (gl_have) {
					gl_have = 0;      /* back to the guest's own picture */
				}
			}
		}
		/* THE LAST FRAME OF A TITLE MUST NOT OUTLIVE IT.
		 *
		 * Leaving a game does NOT kill the guest — AppManager returns to the
		 * home screen and carries on drawing into its own layers — so there
		 * is no process death to hang this on. What does happen is that the
		 * HLE ring goes quiet: hle_host_read_full() stops producing frames
		 * and `gl_have` stays set, painting the title's final frame over the
		 * home screen until the render scale is cycled (which resets it via
		 * the branch above).
		 *
		 * Nothing announces "the title exited", so use the absence of frames
		 * as the signal. A second is far longer than any legitimate gap — a
		 * stalled frame at 12 fps is 83 ms — and if it ever fires early the
		 * next frame sets gl_have straight back. */
		if (gl_have && (!hle_host_ready() ||
		                SDL_GetTicks() - gl_stamp > GL_STALE_MS)) {
			gl_have = 0;
			if (getenv("TADPOLE_HLE_DEBUG"))
				fprintf(stderr, "hle: no game-layer frame for %ums — "
				        "dropping it so the guest's own picture shows\n",
				        (unsigned)GL_STALE_MS);
		}

		/* THE CHECK THAT RUNS BY ITSELF — once, a few seconds in.
		 *
		 * Not at startup proper: the first seconds belong to opening the
		 * window and, for a first-time user, the setup wizard, and a network
		 * round trip competing with that is how a launch feels slow. Delayed,
		 * silent, and it only ever surfaces if there really is something
		 * newer — ui_update_finish() drops the "you are up to date" and the
		 * "no connection" cases when the check was not asked for.
		 */
		if (!g_upd_checked && SDL_GetTicks() > 4000 && g_tool <= 0 &&
		    !ui_modal() && ui_cfg()->update_check) {
			char *av[3];
			g_upd_checked = 1;
			av[0] = (char *)"--current";
			av[1] = (char *)TADPOLE_VERSION;
			av[2] = NULL;
			ui_update_begin(1);
			g_tool_update = 1;
			tool_runv("Checking for updates", "tools/check-update.py", av);
		}

		hle_host_want_full(ui_cfg()->render_scale > 1);

		memset(pixels, 0, (size_t)w * h * 4);
		top_drawn = 0;
		if (g_state) {
			int drawn = 0;
			/* Z-ORDER: fb0 is the TOPMOST layer, not the bottom.
			 * The guest leaves fb_var_screeninfo.nonstd (and so the
			 * LF1000_NONSTD_PRIORITY field) at 0, meaning it relies on the
			 * MLC's fixed layer order. AppManager puts native widgets on
			 * fb0 and Flash content on fb1 — and a native widget like the
			 * on-screen keyboard has to appear OVER the Flash UI, which is
			 * fully opaque. So composite bottom-up: fb2, fb1, then fb0.
			 *
			 * EXCEPT THAT THE VIDEO PLANE MOVES. fb2 is not fixed at the
			 * bottom; the MLC lets the video plane sit at any of four depths
			 * and the guest picks one. soc_dpc_set_vid_priority (dpc.c):
			 *
			 *   0  video>0>1>2      video above every RGB layer
			 *   1  0>video>1>2      under fb0, OVER fb1
			 *   2  0>1>video>2      what we always assumed
			 *   3  0>1>2>video      the very bottom
			 *
			 * Sneak Peeks asks for priority 1 (nonstd 0x01100000), and it
			 * draws an opaque background on fb1 — so compositing fb2 at the
			 * bottom buried a perfectly good trailer under the app's own
			 * artwork. That is the whole of "plays audio, shows no picture".
			 *
			 * Priority 0 is folded in with 1 here: fb0 is composited in its
			 * own later pass so the Tier 3 game layer can go between it and
			 * the rest, and putting the video above THAT needs more than a
			 * reorder. Nothing has asked for it yet; make it say so if it
			 * ever does. */
			{
				const struct layer_state *v = &g_state->layer[2];
				int vp = (v->nonstd >> 24) & 0x3;
				vid_over_fb1 = LF_FORMAT(v->nonstd) == LF_FMT_YUV420 && vp <= 1;
				if (vid_over_fb1 != said_vid_order) {
					said_vid_order = vid_over_fb1;
					fprintf(stderr, "[tadpole] video plane priority %d — "
					        "drawing it %s fb1\n", vp,
					        vid_over_fb1 ? "OVER" : "under");
				}
			}
			for (n = 0; n < NUM_FB; n++) {
				const struct layer_state *ls;
				/* Bottom-up. Swap the two lower slots when the video plane
				 * outranks fb1. fb0 stays last either way. */
				static const int base_order[NUM_FB] = { 2, 1, 0 };
				static const int over_order[NUM_FB] = { 1, 2, 0 };
				i = (vid_over_fb1 ? over_order : base_order)[n];
				ls = &g_state->layer[i];
				const unsigned char *base;
				size_t pitch, off;
				int wx, wy, ww, wh;

				if (!ls->enabled || ls->blank || !g_fb[i])
					continue;

				/* Brio double/triple-buffers inside one framebuffer and
				 * flips with FBIOPAN_DISPLAY, so the visible screen starts
				 * yoffset lines in — not at byte 0. Only the layer's own
				 * rows have to fit: demanding a full panel's worth of them
				 * would reject a legal window sitting near the end of the
				 * arena and silently rewind it to offset 0. */
				layer_window(ls, w, h, &wx, &wy, &ww, &wh);
				pitch = (size_t)w * (ls->bpp ? ls->bpp : 32) / 8;
				/* xoffset IS PART OF THE ADDRESS, not a decoration.
				 *
				 * Brio hands out layer buffers from one arena at byte
				 * granularity and then expresses the result as a pan: the
				 * whole rows go in yoffset and the remainder in xoffset. For
				 * the video layer that remainder is not zero —
				 *
				 *   DeAllocBuffer: remove offset 002FC000
				 *   fb2 PUTVAR ... off 416,1629
				 *   1629 * 1920 + 416 * 4 = 0x2FC000
				 *
				 * — so reading from yoffset alone starts 416 pixels early and
				 * every row of the video lands shifted. fb0 and fb1 have only
				 * ever had xoffset 0, which is why this went unnoticed. */
				off   = (size_t)ls->yoffset * pitch
				      + (size_t)ls->xoffset * (ls->bpp ? ls->bpp : 32) / 8;
				if (off + pitch * (size_t)wh > g_fbsz[i])
					off = 0;
				base = (const unsigned char *)g_fb[i] + off;
				say_layer(i, wx, wy, ww, wh);

				/* fb0 is the TOP layer and goes into its own buffer, so the
				 * game picture can be drawn between the two at its own
				 * resolution. Everything else composites as before. */
				if (i == 0 && pixels_top) {
					memset(pixels_top, 0, (size_t)w * h * 4);
					blit_layer_keyed(pixels_top, w, h, ls, base);
					top_drawn = 1;
				} else {
					blit_layer(pixels, w, h, ls, base, drawn == 0);
					drawn++;
				}
			}
		}

		if (g_touch_debug) {
			draw_touch_grid(pixels, w, h);
			draw_touch_mark(pixels, w, h);
		}
		{
			int lw = (rotate == 90 || rotate == 270) ? h : w;
			int lh = (rotate == 90 || rotate == 270) ? w : h;
			SDL_Rect dst = { (lw - w) / 2, UI_BAR_H + (lh - h) / 2, w, h };

			SDL_SetRenderDrawColor(ren, 6, 15, 10, 255);
			SDL_RenderClear(ren);
			if (g_state) {
				/* Bottom: everything below the game layer. */
				SDL_UpdateTexture(tex, NULL, pixels, w * 4);
				if (rotate)
					SDL_RenderCopyEx(ren, tex, NULL, &dst, (double)rotate,
					                 NULL, SDL_FLIP_NONE);
				else
					SDL_RenderCopy(ren, tex, NULL, &dst);

				/* Middle: the game, at whatever resolution it was drawn at.
				 * Its destination is the layer rectangle mapped into the same
				 * place on screen the panel-sized version would have occupied,
				 * so nothing moves — it simply arrives with more detail. */
				if (gl_have && tex_gl) {
					SDL_Rect g = { dst.x + gl_rx, dst.y + gl_ry, gl_rw, gl_rh };
					if (getenv("TADPOLE_GL_TINT"))
						SDL_SetTextureColorMod(tex_gl, 255, 80, 80);
					if (rotate)
						SDL_RenderCopyEx(ren, tex_gl, NULL, &g, (double)rotate,
						                 NULL, SDL_FLIP_NONE);
					else
						SDL_RenderCopy(ren, tex_gl, NULL, &g);
				}

				/* Top: fb0's chrome, alpha-keyed over both. */
				if (top_drawn && tex_top) {
					SDL_UpdateTexture(tex_top, NULL, pixels_top, w * 4);
					if (rotate)
						SDL_RenderCopyEx(ren, tex_top, NULL, &dst, (double)rotate,
						                 NULL, SDL_FLIP_NONE);
					else
						SDL_RenderCopy(ren, tex_top, NULL, &dst);
				}
			} else {
				ui_draw_idle(ren, lw, lh + UI_BAR_H);
			}
			ui_draw(ren, lw, lh + UI_BAR_H);
		}
		SDL_RenderPresent(ren);

		/* ---- screenshot what is ACTUALLY ON SCREEN ----------------------
		 *
		 * tools/fbshot.py composites the guest's shared framebuffers, which
		 * was the whole picture right up until the game layer stopped being
		 * squeezed back into them. With a high-resolution game layer the
		 * arena holds a stale copy, so an arena screenshot would show the old
		 * picture and every capture comparison would be measuring nothing.
		 *
		 * A trigger file rather than a key, because the thing that wants a
		 * screenshot is a script: write the destination path into
		 * $TADPOLE_DIR/shot.req and the next frame lands there.
		 */
		{
			char req[600], want[512];
			FILE *rf;
			snprintf(req, sizeof(req), "%s/shot.req", g_dir);
			if ((rf = fopen(req, "r"))) {
				if (fgets(want, sizeof(want), rf)) {
					int ow = 0, oh = 0;
					size_t n = strlen(want);
					while (n && (want[n-1] == '\n' || want[n-1] == '\r'))
						want[--n] = 0;
					SDL_GetRendererOutputSize(ren, &ow, &oh);
					if (n && ow > 0 && oh > 0) {
						unsigned char *rgb = malloc((size_t)ow * oh * 3);
						if (rgb && SDL_RenderReadPixels(ren, NULL,
						        SDL_PIXELFORMAT_RGB24, rgb, ow * 3) == 0)
							write_png(want, ow, oh, rgb);
						free(rgb);
					}
					/* The game layer as the viewer RECEIVED it, beside the
					 * window shot. When a resolution change makes no visible
					 * difference, the first question is whether the detail
					 * ever arrived — and this answers it without argument. */
					if (n && gl_have && gl_px && gl_tw > 0 && gl_th > 0) {
						char gp[600];
						unsigned char *rgb = malloc((size_t)gl_tw * gl_th * 3);
						snprintf(gp, sizeof(gp), "%s.layer.png", want);
						if (rgb) {
							int i2, np = gl_tw * gl_th;
							for (i2 = 0; i2 < np; i2++) {
								rgb[i2*3]     = (gl_px[i2] >> 16) & 0xFF;
								rgb[i2*3 + 1] = (gl_px[i2] >> 8)  & 0xFF;
								rgb[i2*3 + 2] =  gl_px[i2]        & 0xFF;
							}
							write_png(gp, gl_tw, gl_th, rgb);
							free(rgb);
						}
					}
				}
				fclose(rf);
				unlink(req);
			}
		}

		if (ui_cfg()->audio_on) {
			static Uint32 last_fmt_poll;
			Uint32 now = SDL_GetTicks();

			audio_pump();
			/* POLL ON A CLOCK, NOT ON vsync_count.
			 *
			 * This used to run only when (vsync_count & 0x1F) == 0. That
			 * counter is advanced by the guest's flips, and a title that
			 * renders through our GL never pans at all — measured: it stays
			 * put for the whole run. So the test was either never true or
			 * always true depending on where it stopped, and a title that
			 * opens the PCM at a DIFFERENT rate from the home screen never
			 * got the device reopened. Its audio then played at whatever
			 * rate the previous app negotiated: too fast and garbled, while
			 * the home screen sounded fine. */
			if (now - last_fmt_poll >= 200) {
				last_fmt_poll = now;
				audio_poll_fmt();
			}
		} else {
			/* Audio off still means the guest's FIFO must be emptied. */
			audio_discard();
		}
		g_adepth_max_ms    = ui_cfg()->audio_latency_ms;
		g_adepth_target_ms = ui_cfg()->audio_latency_ms / 2;

		if (g_state) {
			/* Stand in for the panel's vsync so anything blocking on
			 * FBIO_WAITFORVSYNC gets a plausible cadence. */
			g_state->vsync_count++;
		} else {
			/* RETRY REGARDLESS OF WHO STARTED THE GUEST.
			 *
			 * This used to be gated on `g_guest > 0` — only when the VIEWER
			 * spawned the guest. The initial try_map() at startup runs before
			 * any guest has created state.bin, so it always fails; without the
			 * retry the viewer then runs the whole session unmapped.
			 *
			 * `tadpole.sh --boot` starts the viewer and the guest as SIBLINGS,
			 * so g_guest is 0 and the retry never fired. The result was silent
			 * and total: HLE rendered every frame correctly into a scratch
			 * buffer, reported a full scene in its own statistics, and the
			 * window stayed black — because the arena the compositor reads was
			 * never mapped at all. The front-end path escaped it only because
			 * "Run System Menu" spawns the guest FROM the viewer.
			 *
			 * A failed map is one open() that returns ENOENT; doing it per
			 * frame until it succeeds costs nothing and stops immediately. */
			if (try_map())
				ui_status("running");
		}
		ev_open_missing();
		/* ANNOUNCE EXTERNAL POWER REPEATEDLY, NOT ONCE.
		 *
		 * AppManager arms a 12-second shutdown timer at boot and cancels it
		 * when told it is on external power. Sending that once, as soon as the
		 * FIFO opens, is a RACE: if it lands before AppManager starts reading,
		 * the guest never sees it, the timer fires, and the boot ends in the
		 * shutdown screen.
		 *
		 * It used to win that race by accident — tadpole.sh passed
		 * TADPOLE_DEBUG=0, which the shim read as "on" (it tested presence, not
		 * value), and the resulting log flood slowed the guest enough. Fixing
		 * the debug flag made the guest faster and exposed this.
		 *
		 * Repeat for the first ~15 seconds, which covers the timer with room to
		 * spare. KEY_UP_ is idle-poll traffic to a guest that is already awake,
		 * so repeating it is harmless — unlike KEY_POWER, which must never be
		 * sent unless a shutdown is actually wanted.
		 */
		/* THREE, CLOSE TOGETHER — NOT THIRTY SPREAD OVER FIFTEEN SECONDS.
		 *
		 * "Repeating it is harmless" above was wrong, and it cost the home
		 * screen. Every one of these makes the guest log
		 *
		 *     kPowerExternal event = user switched to external power
		 *
		 * and dispatch it libEvent -> LeapFrogPlugin -> ACTIONSCRIPT. Thirty at
		 * half-second intervals is thirty re-entries into the interpreter spread
		 * across the first fifteen seconds — exactly the window in which the
		 * picker sweeps its icons, loading each asynchronously and polling for
		 * it ("waiting for load of the image"). An event landing between the
		 * request and the load completing left a reference undefined, and
		 * libflashlite dereferenced it:
		 *
		 *     pc  libflashlite.so+0x000dda14    ldr r6, [r3, #0x8]   with r3 = 0
		 *     stack: libEvent.so -> LeapFrogPlugin.so -> libflashlite.so
		 *
		 * Four captured crashes, all inside that sweep, at icon 21/24/32/36 —
		 * the index is random because the collision is a matter of timing. About
		 * one boot in two, and two of the four reports have the kPowerExternal
		 * line printed at the fault itself.
		 *
		 * The repeats only ever existed because the FIFO may not be open when we
		 * first want to send: the shim creates it, so on a cold start there is
		 * nothing to write to yet. That is a startup race, not a reason to keep
		 * talking for fifteen seconds. The guard already requires the fd, so
		 * once a write has gone out the message has landed; two more in quick
		 * succession cover a guest that had not yet installed its handler, and
		 * all three are done long before the picker exists. AppManager's
		 * 12-second shutdown timer is still beaten with room to spare. */
		if (g_evfd[EV_POWER] >= 0 && power_announced < 3) {
			static Uint32 last_power;
			Uint32 now3 = SDL_GetTicks();
			if (!power_announced || now3 - last_power >= 250) {
				send_key(EV_POWER, KEY_UP_, 1);
				send_key(EV_POWER, KEY_UP_, 0);
				last_power = now3;
				power_announced++;
			}
		}
		tool_poll();
		guest_log_pump();
		/* Anti-aliasing, applied the moment it is changed rather than at the
		 * next launch. One int compare per frame; the rebuild only happens on
		 * an actual change. */
		if (hle_host_ready() &&
		    (ui_cfg()->msaa != hle_host_msaa() ||
		     ui_cfg()->render_scale != hle_host_scale())) {
			hle_host_set_quality(ui_cfg()->msaa, ui_cfg()->render_scale);
			if (hle_host_scale() > 1 && hle_host_msaa())
				ui_status("%dx scale + %dx AA", hle_host_scale(), hle_host_msaa());
			else if (hle_host_scale() > 1)
				ui_status("%dx render scale", hle_host_scale());
			else if (hle_host_msaa())
				ui_status("AA %dx", hle_host_msaa());
			else
				ui_status("AA off");
		}
		if (g_guest > 0 && !guest_alive()) {
			ui_status("stopped");
			fps_shown = 0;         /* "stopped" says more than "HLE idle" would */
			guest_log_close();     /* flush the tail of a boot that just died */
		}
		ui_set_running(g_guest > 0 || guest_external());
		g_touch_debug = ui_cfg()->touch_debug;
		/* A FRAME RATE IS A MEASUREMENT — ONLY PRINT ONE THAT WAS TAKEN.
		 *
		 * "Never show a bare frame rate once the guest has given up" was only
		 * half the problem. The other half is that the replayer is brought up
		 * before any guest exists and stays up after one dies, so the bar sat
		 * at "HLE 0 fps" whenever nothing was being rendered through it — and
		 * that reads as "the emulator is broken" rather than "nothing is being
		 * measured right now" (reported by FairPlay137).
		 *
		 * COMPLETED FRAMES ARE THE EVIDENCE, not a live guest. g_guest and
		 * guest_external() are no use here: under `tadpole.sh --boot` the guest
		 * is our SIBLING and the lock holds the script's own pid, so the viewer
		 * believes nothing is running while a title renders at 60 fps. Gating
		 * on those would blank the number in the commonest boot path.
		 *
		 * Frames also beat the fallback flag, which lives in the ring and is
		 * only cleared by hle_host_init() — one title giving up would otherwise
		 * leave the banner there for every title after it, viewer-lifetime.
		 *
		 * Packets tell "producing nothing" apart from "not being asked for
		 * anything": commands arriving with no frame out of them in a whole
		 * second is a real zero, and hiding that would hide a genuine stall. */
		if (!hle_host_ready()) {
			fps_primed = 0;
		} else {
			Uint32 now2 = SDL_GetTicks();
			if (!fps_primed) {
				/* g_frames is cumulative and starts wherever the replayer
				 * happens to be, so the first tick only takes a baseline —
				 * subtracting from zero would report a whole session's frames
				 * as one second of them. */
				hle_host_stats(&fps_frames, &fps_packets);
				fps_at = now2;
				fps_primed = 1;
			} else if (now2 - fps_at >= 1000) {
				unsigned long f, pk;
				/* Divide by the window we actually got. The pump is paced by
				 * the frame cap and by whatever the guest is doing, so a tick
				 * that lands at 1400 ms would otherwise overstate by 40%. */
				Uint32 dt = now2 - fps_at;
				hle_host_stats(&f, &pk);
				if (f > fps_frames) {
					ui_status("HLE %lu fps",
					          ((f - fps_frames) * 1000 + dt / 2) / dt);
					fps_shown = 1;
				} else if (hle_guest_fell_back()) {
					ui_status("HLE FELL BACK - software");
					fps_shown = 1;
					/* A line on the status bar was how this was reported for
					 * months, and it is the one place nobody looks while a
					 * game is on screen. The software rasteriser cannot
					 * express what the titles ask for, so every frame after
					 * this point is wrong rather than slow — that deserves
					 * something the user has to dismiss.
					 *
					 * Once per session, and only when nothing else is up:
					 * ui_alert() declines while a menu or dialog is open, so
					 * retry on later ticks until it lands. */
					if (!gpu_lost_told) {
						char body[160];
						snprintf(body, sizeof(body),
						         "GPU render engine CRASHED. "
						         "Please restart %s.", ui_brand_name());
						if (ui_alert("Graphics", body))
							gpu_lost_told = 1;
					}
				} else if (pk > fps_packets) {
					ui_status("HLE 0 fps");
					fps_shown = 1;
				} else if (fps_shown) {
					/* Nothing came through in that second. Say so rather than
					 * leaving the last number on the bar, where it would be
					 * read as current. */
					ui_status("HLE idle");
					fps_shown = 0;
				}
				fps_frames = f;
				fps_packets = pk;
				fps_at = now2;
			}
		}

		switch (ui_take_action(actpath, sizeof(actpath))) {
		case UI_ACT_RUN_UI:  guest_launch_ui();  power_announced = 0; break;
		case UI_ACT_RUN_SWF: guest_launch_swf(actpath); power_announced = 0; break;
		case UI_ACT_RUN_APP: guest_launch_app(actpath); power_announced = 0; break;
		case UI_ACT_STOP:
			guest_stop();
			ui_set_running(0);
			ui_status("stopped");
			fps_shown = 0;
			break;
		case UI_ACT_QUIT: running = 0; break;
		case UI_ACT_INSTALL_PKG:
			tool_run("install", "tools/install-game.sh", actpath);
			break;
		case UI_ACT_CONVERT_CART: {
			/* THE .tar GOES TO THE GAMES FOLDER, not beside the .bin. The
			 * point of converting is to install it, and the Game Library
			 * reads that folder — so the result appears where the user is
			 * already looking rather than next to a 128 MB image they now
			 * have to go and find. */
			/* ARGUMENTS ONLY — tool_runv() puts the script in argv[0] itself.
			 *
			 * This listed the script here as well, so the command that actually
			 * ran was
			 *     tools/cart2tar.py tools/cart2tar.py -o <games> <dump.bin>
			 * and argparse bound its `images` positional to the FIRST contiguous
			 * run of positionals — the duplicated script name — consumed -o, and
			 * then had the real .bin left over with nowhere to put it:
			 *     error: unrecognized arguments: /home/…/dump.bin
			 *
			 * So Convert Cartridge Dump could never have worked from the menu,
			 * and had argparse been more permissive it would have been worse:
			 * the tool would have tried to convert its own source file. Every
			 * other tool_runv caller here passes arguments only; this was the
			 * one that did not. */
			char *av[6];
			char gd[600];
			const char *dir = ui_cfg()->games_dir;
			int n = 0;
			if (dir && *dir) {
				snprintf(gd, sizeof(gd), "%s", dir);
				av[n++] = (char *)"-o";
				av[n++] = gd;
			}
			av[n++] = (char *)actpath;
			av[n]   = NULL;
			tool_runv("converting cartridge", "tools/cart2tar.py", av);
			break;
		}
		case UI_ACT_SCAN_GAMES:
			tool_run("reading games", "tools/scan-games.sh", actpath);
			break;
		case UI_ACT_INSTALL_GAMES:
			/* actpath is a FILE listing the archives, not an archive: a batch
			 * of thirty titles is a perfectly ordinary thing to ask for, and
			 * thirty paths do not belong on a command line. */
			tool_run2("install", "tools/install-game.sh",
			          "--from-list", actpath);
			break;
		case UI_ACT_SETUP_FIRMWARE:
			tool_run("firmware", "tools/install-firmware.sh", actpath);
			break;
		/* Two buttons, two halves of the same setup — see the Didj page in
		 * tadpole_ui.c. Split because they are two separate downloads and the
		 * file browser hands back one path at a time. */
		case UI_ACT_SETUP_DIDJ:
			tool_run2("didj", "tools/install-didj.sh", "--setup", actpath);
			break;
		case UI_ACT_SETUP_DIDJ_OVERLAY:
			tool_run2("didj", "tools/install-didj.sh", "--overlay", actpath);
			break;
		/* Fetch-and-install, one button each. No path: the URLs live in
		 * install-didj.py beside the note on where each piece comes from. */
		case UI_ACT_FETCH_DIDJ:
			tool_run("didj", "tools/install-didj.sh", "--fetch-compat");
			break;
		case UI_ACT_FETCH_DIDJ_OVERLAY:
			tool_run("didj", "tools/install-didj.sh", "--fetch-overlay");
			break;
		case UI_ACT_BUILD_SYSROOT:
			tool_run("sysroot", "runtime/setup-sysroot.sh", NULL);
			break;
		case UI_ACT_CHECK_UPDATE: {
			char *av[3];
			av[0] = (char *)"--current";
			av[1] = (char *)TADPOLE_VERSION;
			av[2] = NULL;
			ui_update_begin(0);
			g_tool_update = 1;
			tool_runv("Checking for updates", "tools/check-update.py", av);
			break;
		}
		case UI_ACT_DO_UPDATE: {
			/* WHERE THE NEW IMAGE GOES. Next to the running AppImage when
			 * there is one — that is where the user keeps it and what their
			 * launcher points at — otherwise the current directory. It is
			 * written to <name>.new rather than over the running file: a
			 * running AppImage is a mounted image, and overwriting it under
			 * itself is how you get a half-updated program that cannot
			 * explain itself. */
			const char *img = getenv("APPIMAGE");
			char dest[1100];
			char *av[3];
#ifdef _WIN32
			/* A Windows release is an INSTALLER, not an image to swap under
			 * ourselves: it goes to the user's own data directory and is run
			 * when the download finishes (see tool_poll), which is the
			 * convention every Windows updater follows and the only one that
			 * can replace files this process is holding open. */
			(void)img;
			snprintf(dest, sizeof(dest), "%s/Glasspole-Setup.exe",
			         g_dir[0] ? g_dir : ".");
#else
			if (img && img[0])
				snprintf(dest, sizeof(dest), "%s.new", img);
			else
				snprintf(dest, sizeof(dest), "%s/Tadpole-x86_64.AppImage",
				         g_projdir[0] ? g_projdir : ".");
#endif
			av[0] = (char *)"--download";
			av[1] = dest;
			av[2] = NULL;
			snprintf(g_update_dest, sizeof(g_update_dest), "%s", dest);
			g_tool_update = 0;
			tool_runv("Downloading update", "tools/check-update.py", av);
			break;
		}
		case UI_ACT_ONLINE_UPDATE:
			tool_run("online update", "tools/online-update.sh", NULL);
			break;
		case UI_ACT_MAKE_PROFILE: {
			static char nm[64], gr[16], pic[1024];
			char *av[7];
			int grade = 1, n = 0;
			ui_profile_get(nm, sizeof(nm), &grade, pic, sizeof(pic));
			snprintf(gr, sizeof(gr), "%d", grade);
			av[n++] = (char *)"--name"; av[n++] = nm;
			av[n++] = (char *)"--grade"; av[n++] = gr;
			if (pic[0]) { av[n++] = (char *)"--picture"; av[n++] = pic; }
			av[n] = NULL;
			tool_runv("profile", "tools/make-profile.sh", av);
			break;
		}
		case UI_ACT_ERASE_FW:
			/* Stop the guest first: it has the old sysroot mapped, and pulling
			 * that out from under a running emulator is a poor way to find out
			 * what breaks. */
			guest_stop();
			ui_set_running(0);
			tool_run("erase", "tools/erase-firmware.sh", NULL);
			break;
		case UI_ACT_RELAYOUT:
			rotate = ui_cfg()->rotate;
			scale  = ui_cfg()->scale;
			/* SDL_SetWindowSize blocks on a window-manager round trip — measured
			 * at 4.8 seconds here. The guest is meanwhile waiting for us to
			 * replay its frame, so drain the ring on BOTH sides of the resize;
			 * otherwise rotating reliably starves it into falling back to
			 * software, which is exactly what was reported. */
			if (hle_host_ready() && g_state && g_fb[1])
				hle_host_pump((uint32_t *)g_fb[1], (unsigned)w);
			set_logical(ren, rotate, w, h);
			SDL_SetWindowSize(win,
			        ((rotate == 90 || rotate == 270) ? h : w) * scale,
			        (((rotate == 90 || rotate == 270) ? w : h) + UI_BAR_H) * scale);
			if (hle_host_ready() && g_state && g_fb[1])
				hle_host_pump((uint32_t *)g_fb[1], (unsigned)w);
			break;
		default: break;
		}
	}

	guest_stop();
	guest_log_close();
	hle_host_shutdown();
	ui_shutdown();
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
