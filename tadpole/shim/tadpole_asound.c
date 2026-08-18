/* Tadpole — fake libasound.so.2 for the guest.
 *
 * WHY A FULL REPLACEMENT RATHER THAN PASSTHROUGH
 * ----------------------------------------------
 * The real libasound talks to the kernel through ioctls on the /dev/snd nodes, and
 * qemu-user does not forward the ALSA control ioctls: SNDRV_CTL_IOCTL_CARD_INFO
 * works natively (returns card 0, id 'PCH') but comes back ENOTTY through qemu.
 * So there is nothing to pass through TO. We implement the API instead.
 *
 * Until now /etc/asound.conf pointed everything at a null sink, because
 * AppManager's UI never renders if audio init FAILS (see HANDOVER 4.8) — it
 * had to succeed, silently. This replaces the silence with real output.
 *
 * WHAT IT HAS TO IMPLEMENT
 * ------------------------
 * The UNION of the snd_* symbols LF/Base/Brio/Module/libAudio.so imports on
 * every device we boot — they are not the same list, see the note above
 * snd_pcm_hw_params_get_buffer_time. Brio uses the mmap_writei path, not
 * snd_pcm_writei.
 *
 * WHO REACHES IT, AND ON WHICH DEVICE
 * -----------------------------------
 * LeapPad2:  AppManager -> libAudioMPI.so -> dlopen(libAudio.so) -> here.
 * LeapPad3 / Ultra (Qt): AppServer -> libQtAppServer.so.1 -> libAudioMPI.so ->
 *   dlopen(libAudio.so) -> here. Confirmed from DT_NEEDED, not assumed: nothing
 *   in the Qt shell's own dependency tree links libasound, and the three other
 *   things in the image that do (libgstalsa.so, libQtMultimediaE, libsiimpl.so)
 *   are not loaded by the shell. So the Brio module is the whole audio path on
 *   both, and the module is dlopened — which is why LD_LIBRARY_PATH still
 *   decides which libasound it gets.
 *
 * WHERE THE SAMPLES GO
 * --------------------
 * Straight into $TADPOLE_DIR/audio, a FIFO the viewer drains into SDL. Same
 * shape as the rest of Tadpole: a plain host file, no protocol. The negotiated
 * format is left in $TADPOLE_DIR/audio.fmt as one line of text ("rate channels
 * bits"), so the viewer can open its device to match without us having to
 * touch the shared state struct.
 *
 * The device runs 32000 Hz S16_LE stereo, buffer 4096 / period 1024 (from its
 * own /etc/asound.conf, pcm.dmixbrio). We do not assume that — every setter
 * records what it is actually told.
 *
 * PACING: writes stay non-blocking but retry, so the guest is throttled by how
 * fast the viewer consumes (which SDL paces) instead of racing ahead and
 * garbling. If the viewer is absent or wedged we give up after a bounded wait
 * and claim the frames were written — dropping audio is always better than
 * hanging the UI thread.
 */

typedef unsigned int   u32;
typedef unsigned long  ulong;
typedef long           slong;
typedef unsigned int   size_t_;

#define NULL ((void *)0)

extern int    open(const char *path, int flags, ...);
extern int    close(int fd);
extern slong  write(int fd, const void *buf, size_t_ n);
extern int    mkfifo(const char *path, u32 mode);
extern char  *getenv(const char *name);
extern int    snprintf(char *s, size_t_ n, const char *fmt, ...);
extern void  *memset(void *s, int c, size_t_ n);
extern int    usleep(u32 usec);
extern int   *__errno_location(void);

/* Monotonic clock for the virtual playback position. struct timespec is two
 * longs on this 32-bit target. */
struct tad_ts { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_ts *tp);
extern int getpid(void);
#define CLOCK_MONOTONIC_ 1

static unsigned long long now_us(void)
{
	struct tad_ts t;
	if (clock_gettime(CLOCK_MONOTONIC_, &t) != 0)
		return 0;
	return (unsigned long long)t.tv_sec * 1000000ULL
	     + (unsigned long long)t.tv_nsec / 1000ULL;
}

#define O_WRONLY   01
#define O_NONBLOCK 04000
#define O_CREAT    0100
#define O_TRUNC    01000
#define EAGAIN     11

/* ALSA opaque types. The guest only ever passes these around as pointers. */
typedef struct tad_pcm        snd_pcm_t;
typedef struct tad_hw_params  snd_pcm_hw_params_t;
typedef struct tad_sw_params  snd_pcm_sw_params_t;

/* snd_pcm_hw_params_alloca() does alloca(snd_pcm_hw_params_sizeof()), so the
 * caller gives us exactly as much room as we ask for and not a byte more. Ask
 * for something comfortably larger than we need and never grow past it. */
#define PARAMS_SIZE 512

struct tad_hw_params {
	u32 magic;
	u32 rate, channels, format, access;
	ulong buffer_size, period_size;
};

struct tad_sw_params {
	u32 magic;
	ulong start_threshold, avail_min;
};

struct tad_pcm {
	int   fd;             /* FIFO to the viewer, -1 if unavailable */
	int   stream;         /* 0 = playback, 1 = capture */
	u32   rate, channels, format;
	u32   frame_bytes;
	ulong buffer_size, period_size;
	/* Virtual playback clock — see pace_pcm(). t0_us is 0 when idle. */
	unsigned long long t0_us, written;
};

#define HW_MAGIC 0x48575041u
#define SW_MAGIC 0x53575041u

/* One handle is enough: Brio opens a single playback PCM. A second open gets
 * its own struct but shares the FIFO, which is what dmix did on hardware. */
static struct tad_pcm g_pcm[4];
static int g_debug_audio = -1;
static int g_npcm;

static const char *tad_dir(void)
{
	const char *d = getenv("TADPOLE_DIR");
	return d ? d : "/tmp/tadpole";
}

/* Bytes per sample for the ALSA format ids Brio might pick. Anything we do not
 * recognise is treated as 16-bit, which is what the hardware uses. */
static u32 fmt_bits(u32 format)
{
	switch (format) {
	case 0: case 1:   return 8;    /* S8, U8         */
	case 2: case 3:   return 16;   /* S16_LE, S16_BE */
	case 4: case 5:   return 16;   /* U16_LE, U16_BE */
	case 10: case 11: return 32;   /* S32_LE, S32_BE */
	default:          return 16;
	}
}

static int strlen_(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static void publish_format(struct tad_pcm *p)
{
	char path[512], line[64];
	int fd;

	snprintf(path, sizeof(path), "%s/audio.fmt", tad_dir());
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return;
	snprintf(line, sizeof(line), "%u %u %u %lu\n",
	         p->rate, p->channels, fmt_bits(p->format),
	         (ulong)p->period_size);
	write(fd, line, (size_t_)strlen_(line));
	close(fd);
}

/* BACK OFF when there is no reader.
 *
 * With no viewer the FIFO has no reader, open() fails, and this used to be
 * retried on every single write — 2.1 million attempts in one headless boot,
 * enough I/O to stop the guest making progress. Retry occasionally instead: a
 * viewer that starts later is picked up within a second, and one that never
 * starts costs nothing. */
static void open_fifo(struct tad_pcm *p)
{
	char path[512];
	static unsigned tries;

	if (p->fd < 0 && (tries++ & 0x3F))
		return;

	if (g_debug_audio > 0) {
		char b[96];
		int n = snprintf(b, sizeof(b),
		                 "[tadpole] PCM stream %d opening FIFO\n",
		                 (int)(p - g_pcm));
		if (n > 0) write(2, b, (size_t_)n);
	}
	snprintf(path, sizeof(path), "%s/audio", tad_dir());
	mkfifo(path, 0666);                       /* harmless if it exists */
	/* O_NONBLOCK so a missing reader gives -1 instead of blocking here. */
	p->fd = open(path, O_WRONLY | O_NONBLOCK);
}

/* ---- lifecycle ---------------------------------------------------------- */

int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode)
{
	struct tad_pcm *p;

	(void)mode;
	if (!pcmp)
		return -22;
	if (g_debug_audio < 0)
		{ const char *d = getenv("TADPOLE_DEBUG");
		  g_debug_audio = (d && d[0] && d[0] != '0'); }
	if (g_npcm >= 4)
		return -24;

	p = &g_pcm[g_npcm++];
	memset(p, 0, sizeof(*p));
	p->fd          = -1;
	p->stream      = stream;
	p->rate        = 32000;
	p->channels    = 2;
	p->format      = 2;                       /* S16_LE */
	p->frame_bytes = 4;
	p->buffer_size = 4096;
	p->period_size = 1024;

	/* Capture (microphone) is not wired up; report success but never hand
	 * back samples, so Brio's mic path fails soft instead of erroring out
	 * during init and taking the UI with it. */
	if (stream == 0)
		open_fifo(p);

	if (g_debug_audio > 0) {
		char b[160];
		int n = snprintf(b, sizeof(b),
		                 "[tadpole] snd_pcm_open slot=%d stream=%d name=%s"
		                 " (live=%d)\n",
		                 (int)(p - g_pcm), stream, name ? name : "?", g_npcm);
		if (n > 0) write(2, b, (size_t_)n);
	}
	*pcmp = p;
	return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
	if (pcm && pcm->fd >= 0) {
		close(pcm->fd);
		pcm->fd = -1;
	}
	return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
	if (pcm && pcm->fd < 0 && pcm->stream == 0)
		open_fifo(pcm);           /* viewer may have started since open */
	if (pcm) { pcm->t0_us = 0; pcm->written = 0; }   /* restart the clock */
	return 0;
}

int snd_pcm_start(snd_pcm_t *pcm) { if (pcm) { pcm->t0_us = 0; pcm->written = 0; } return 0; }
/* drop discards buffered audio, so the virtual buffer is empty again. */
int snd_pcm_drop(snd_pcm_t *pcm)    { if (pcm) { pcm->t0_us = 0; pcm->written = 0; } return 0; }
int snd_pcm_resume(snd_pcm_t *pcm)  { (void)pcm; return 0; }
int snd_pcm_drain(snd_pcm_t *pcm)   { (void)pcm; return 0; }

/* ---- the one that actually moves audio ---------------------------------- */

/* Bytes discarded because the reader never caught up. Non-zero means the guest
 * is outrunning real time; see the note at the end of the write loop. */
static unsigned long g_dropped;

static void dbg_drop(size_t_ now, unsigned long total)
{
	char b[96];
	int n = snprintf(b, sizeof(b),
	                 "[tadpole] audio DROPPED %u bytes (total %lu)\n",
	                 (unsigned)now, total);
	if (n > 0) write(2, b, (size_t_)n);
}

/* PACE THE WRITER AGAINST A REAL-TIME PLAYBACK CLOCK.
 *
 * libAudio.so imports no snd_pcm_wait, no snd_pcm_avail_update and no
 * snd_pcm_delay — checked with `strings`. Its entire pacing comes from
 * snd_pcm_mmap_writei BLOCKING once the device buffer is full, which is how a
 * real ALSA playback loop is throttled.
 *
 * Ours never blocked in any meaningful way: it dumped into a FIFO that the
 * viewer drains into a deep ring, so Brio's mixer thread rendered as fast as
 * the emulated CPU allowed. Audio was therefore GENERATED far faster than it
 * could be played — "running EXTREMELY FAST and sounds terrible" — and when the
 * pipe finally did back up we spun for 500 ms and threw the remainder away,
 * which is what clipped short sounds like the sign-in tap.
 *
 * So model the device: bytes drain at exactly rate * channels * bytes-per-frame
 * per second. Hold the writer until the in-flight amount fits inside one device
 * buffer, which for the negotiated 4096 frames is about 128 ms. Brio is then
 * paced to real time no matter how fast or slow qemu is running.
 *
 * If the guest stops writing long enough for the buffer to drain we resync
 * rather than accumulate credit, otherwise a quiet passage would earn the right
 * to dump a burst afterwards and sound fast all over again.
 */
static int g_pace_on = -1;
static int g_pace_dbg = -1;
static unsigned long long g_dbg_bytes, g_dbg_slept, g_dbg_t0;

static void pace_pcm(struct tad_pcm *p, size_t_ bytes)
{
	unsigned long long rate_b, played, inflight, cap, now;
	int guard = 0;

	if (g_pace_on < 0) {
		const char *e = getenv("TADPOLE_AUDIO_PACE");
		g_pace_on = (e && e[0] == '0') ? 0 : 1;
	}
	if (!g_pace_on)
		return;                       /* for measuring the unpaced rate */
	rate_b = (unsigned long long)p->rate * p->frame_bytes;
	if (!rate_b)
		return;
	cap = (unsigned long long)p->buffer_size * p->frame_bytes;
	if (cap < 4096) cap = 4096;

	now = now_us();
	if (!now)
		return;                       /* no clock: do not throttle */
	if (!p->t0_us) { p->t0_us = now; p->written = 0; }

	for (;;) {
		played = (now - p->t0_us) * rate_b / 1000000ULL;
		if (played >= p->written) {   /* drained: restart the clock */
			p->t0_us = now;
			p->written = 0;
			inflight = 0;
		} else {
			inflight = p->written - played;
		}
		if (inflight + bytes <= cap)
			break;
		{
			unsigned long long over = inflight + bytes - cap;
			unsigned long long us = over * 1000000ULL / rate_b;
			if (us < 1000) us = 1000;
			if (us > 100000) us = 100000;     /* never wedge the thread */
			g_dbg_slept += us;
			usleep((u32)us);
		}
		now = now_us();
		if (!now || ++guard > 40)
			break;
	}
	p->written += bytes;

	/* WHAT THE PACER ACTUALLY DID, once a second. The arithmetic above looks
	 * correct on paper — simulate it and a 4096-byte period comes out at one
	 * write per 32 ms, exactly 128000 B/s — yet the viewer measures the guest
	 * producing 200% of realtime, flat. When a model and a measurement disagree
	 * that precisely, the model is missing a caller, so report the inputs
	 * rather than re-deriving the same result. */
	if (g_pace_dbg < 0) {
		const char *e = getenv("TADPOLE_AUDIO_DEBUG");
		g_pace_dbg = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	if (g_pace_dbg) {
		g_dbg_bytes += bytes;
		if (!g_dbg_t0) g_dbg_t0 = now;
		if (now - g_dbg_t0 >= 1000000ULL) {
			char b[192];
			int n = snprintf(b, sizeof(b),
			        "[tadpole] pace[pid %d]: %llu B/s in (rate=%u fb=%u -> %llu B/s"
			        " expected) cap=%llu slept=%llu ms/s\n",
			        (int)getpid(),
			        g_dbg_bytes * 1000000ULL / (now - g_dbg_t0),
			        p->rate, p->frame_bytes, rate_b, cap,
			        g_dbg_slept / 1000ULL);
			if (n > 0) write(2, b, (size_t_)n);
			g_dbg_bytes = 0; g_dbg_slept = 0; g_dbg_t0 = now;
		}
	}
}

slong snd_pcm_mmap_writei(snd_pcm_t *pcm, const void *buffer, ulong frames)
{
	const char *p = buffer;
	size_t_ left;
	int spins = 0;

	if (!pcm || !buffer)
		return -22;
	if (g_debug_audio < 0)
		{ const char *d = getenv("TADPOLE_DEBUG");
		  g_debug_audio = (d && d[0] && d[0] != '0'); }
	left = (size_t_)(frames * pcm->frame_bytes);

	/* PACE FIRST, EVEN WHEN THERE IS NOWHERE TO PUT IT.
	 *
	 * This used to return early when the FIFO had no reader, which skipped
	 * pacing altogether — and the guest's own sense of time comes from this
	 * call blocking. With nothing draining the pipe, Brio's audio clock ran
	 * as fast as qemu could generate samples, and everything synced to it ran
	 * away too: a 4.416 s video finished in 0.27 s with 46 of its 49 frames
	 * binned as "late" (VideoModule::GetVideoFrame: Dropped frame N).
	 *
	 * Discarding the bytes is fine. Discarding the TIMING is not — a device
	 * with the speaker unplugged still takes a second to play a second. */
	pace_pcm(pcm, left);

	if (pcm->fd < 0) {
		open_fifo(pcm);
		if (pcm->fd < 0)
			return (slong)frames;      /* no reader: discard, but on time */
	}

	while (left) {
		slong n = write(pcm->fd, p, left);
		if (n > 0) {
			p += n;
			left -= (size_t_)n;
			spins = 0;
			continue;
		}
		if (n < 0 && *__errno_location() == EAGAIN) {
			/* Reader is behind. Waiting here is CORRECT: it paces the
			 * guest against real playback. Bounded so a dead viewer
			 * cannot wedge Brio's audio thread. */
			/* With pace_pcm() ahead of us the pipe should rarely be
			 * full; allow much longer before giving up so a transient
			 * hiccup does not clip the sound. */
			if (++spins > 1500)
				break;
			usleep(2000);
			continue;
		}
		break;                                 /* reader gone */
	}
	/* Anything still unwritten is THROWN AWAY while we tell the caller every
	 * frame landed. That truncates whatever was playing — a narrator line cut
	 * mid-word is this, not a decoder bug. It should only happen if the guest
	 * is generating audio faster than real time, which frame pacing exists to
	 * prevent, so make it countable instead of silent. */
	if (left) {
		g_dropped += left;
		if (g_debug_audio)
			dbg_drop(left, g_dropped);
	}
	return (slong)frames;
}

slong snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, ulong frames)
{
	return snd_pcm_mmap_writei(pcm, buffer, frames);
}

slong snd_pcm_avail_update(snd_pcm_t *pcm)
{
	return pcm ? (slong)pcm->period_size : 0;
}

int snd_pcm_set_params(snd_pcm_t *pcm, int format, int access, u32 channels,
                       u32 rate, int soft_resample, u32 latency)
{
	(void)access; (void)soft_resample; (void)latency;
	if (!pcm)
		return -22;
	pcm->format      = (u32)format;
	pcm->channels    = channels;
	pcm->rate        = rate;
	pcm->frame_bytes = channels * (fmt_bits((u32)format) / 8);
	publish_format(pcm);
	return 0;
}

/* ---- hw params ---------------------------------------------------------- */

size_t_ snd_pcm_hw_params_sizeof(void) { return PARAMS_SIZE; }
size_t_ snd_pcm_sw_params_sizeof(void) { return PARAMS_SIZE; }

int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **p)
{
	static struct tad_hw_params pool[4];
	static int n;
	if (!p || n >= 4)
		return -12;
	*p = &pool[n++];
	memset(*p, 0, sizeof(**p));
	return 0;
}
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *p) { (void)p; }

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw)
{
	if (!hw)
		return -22;
	memset(hw, 0, sizeof(*hw));
	hw->magic = HW_MAGIC;
	if (pcm) {
		hw->rate        = pcm->rate;
		hw->channels    = pcm->channels;
		hw->format      = pcm->format;
		hw->buffer_size = pcm->buffer_size;
		hw->period_size = pcm->period_size;
	}
	return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, int a)
{ (void)pcm; if (hw) hw->access = (u32)a; return 0; }

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, int f)
{ (void)pcm; if (hw) hw->format = (u32)f; return 0; }

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, u32 c)
{ (void)pcm; if (hw) hw->channels = c; return 0; }

int snd_pcm_hw_params_set_rate_resample(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, u32 v)
{ (void)pcm; (void)hw; (void)v; return 0; }

/* The _near setters take the requested value BY POINTER and write back what
 * the hardware could actually do. We can do exactly what was asked, so echo
 * it back unchanged — and leave *dir at 0 meaning "exact". */
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                    u32 *val, int *dir)
{
	(void)pcm;
	if (hw && val) hw->rate = *val;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                           u32 *val, int *dir)
{
	(void)pcm;
	if (hw && val && hw->rate)
		hw->buffer_size = (ulong)((*val / 1000) * (hw->rate / 1000));
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                           u32 *val, int *dir)
{
	(void)pcm;
	if (hw && val && hw->rate)
		hw->period_size = (ulong)((*val / 1000) * (hw->rate / 1000));
	if (dir) *dir = 0;
	return 0;
}

/* THE TIME GETTERS ARE NOT OPTIONAL ON THE LEAPPAD3, and their absence is a
 * load-time failure rather than a quiet one.
 *
 * The LeapPad2's libAudio.so imports 29 snd_* symbols; the LeapPad3's imports
 * 29 too, but not the same 29. Diffing the two DT_SYMTABs:
 *
 *   only on the LeapPad2:  snd_pcm_drain  snd_pcm_hw_params_free
 *                          snd_pcm_hw_params_get_sbits
 *                          snd_pcm_hw_params_malloc  snd_pcm_writei
 *   only on the LeapPad3:  snd_pcm_hw_params_get_buffer_time
 *                          snd_pcm_hw_params_get_period_time
 *
 * A replacement libasound that is missing one of them does not degrade — the
 * guest's loader refuses the whole module the moment Brio dlopens it:
 *     libAudio.so: can't resolve symbol 'snd_pcm_hw_params_get_buffer_time'
 * and the shell then runs on with no audio module at all, which looks exactly
 * like the null sink it replaced. So both are defined here even though the
 * device we know needs them is only one of the two.
 *
 * The value is a period/buffer expressed in MICROSECONDS. The device's own log
 * line is the check on the arithmetic: at 32000 Hz it prints
 *     set_hwparams: buffer time=128000, size=4096
 *     set_hwparams: period time=32000, size=1024
 * and 4096/32000 s is 128000 us, 1024/32000 s is 32000 us. 64-bit intermediate
 * because 4096 * 1000000 is within a few percent of overflowing 32 bits and a
 * larger buffer would wrap silently. */
static int params_time(ulong frames, u32 rate, u32 *val, int *dir)
{
	if (!rate)
		rate = 32000;
	if (val)
		*val = (u32)((unsigned long long)frames * 1000000ULL / rate);
	if (dir)
		*dir = 0;
	return 0;
}

int snd_pcm_hw_params_get_buffer_time(const snd_pcm_hw_params_t *hw, u32 *val, int *dir)
{ return params_time(hw ? hw->buffer_size : 4096, hw ? hw->rate : 0, val, dir); }

int snd_pcm_hw_params_get_period_time(const snd_pcm_hw_params_t *hw, u32 *val, int *dir)
{ return params_time(hw ? hw->period_size : 1024, hw ? hw->rate : 0, val, dir); }

int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *hw, ulong *val)
{ if (val) *val = hw ? hw->buffer_size : 4096; return 0; }

int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *hw, ulong *val, int *dir)
{ if (val) *val = hw ? hw->period_size : 1024; if (dir) *dir = 0; return 0; }

int snd_pcm_hw_params_get_sbits(const snd_pcm_hw_params_t *hw)
{ return hw ? (int)fmt_bits(hw->format) : 16; }

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw)
{
	if (!pcm || !hw)
		return -22;
	if (hw->rate)     pcm->rate     = hw->rate;
	if (hw->channels) pcm->channels = hw->channels;
	pcm->format      = hw->format;
	pcm->frame_bytes = pcm->channels * (fmt_bits(pcm->format) / 8);
	if (!pcm->frame_bytes)
		pcm->frame_bytes = 4;
	if (hw->buffer_size) pcm->buffer_size = hw->buffer_size;
	if (hw->period_size) pcm->period_size = hw->period_size;
	if (pcm->stream == 0)
		publish_format(pcm);
	return 0;
}

/* ---- sw params ---------------------------------------------------------- */

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw)
{
	(void)pcm;
	if (!sw)
		return -22;
	memset(sw, 0, sizeof(*sw));
	sw->magic = SW_MAGIC;
	return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw, ulong v)
{ (void)pcm; if (sw) sw->start_threshold = v; return 0; }

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw, ulong v)
{ (void)pcm; if (sw) sw->avail_min = v; return 0; }

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw)
{ (void)pcm; (void)sw; return 0; }

/* ---- misc --------------------------------------------------------------- */

const char *snd_strerror(int e)
{
	(void)e;
	return "tadpole-asound";
}
