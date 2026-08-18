/* Tadpole — fake libasound.so.2 for the LeapPad2 guest.
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
 * Exactly the 26 snd_* symbols LF/Base/Brio/Module/libAudio.so imports. Brio
 * uses the mmap_writei path, not snd_pcm_writei.
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

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned long  ulong;
typedef long           slong;
typedef unsigned int   size_t_;

#define NULL ((void *)0)

extern int    open(const char *path, int flags, ...);
extern int    close(int fd);
extern slong  write(int fd, const void *buf, size_t_ n);
extern int    mkfifo(const char *path, u32 mode);
extern int    unlink(const char *path);
extern slong  read(int fd, void *buf, size_t_ n);
extern char  *getenv(const char *name);
extern int    snprintf(char *s, size_t_ n, const char *fmt, ...);
extern void  *memset(void *s, int c, size_t_ n);
extern void  *memmove(void *d, const void *s, size_t_ n);
extern int    usleep(u32 usec);
extern int   *__errno_location(void);

/* Monotonic clock for the virtual playback position. struct timespec is two
 * longs on this 32-bit target. */
struct tad_ts { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_ts *tp);
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
#define O_RDWR     02
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

/* CAPTURE STAGING. Two seconds at the 16 kHz mono the microphone module asks
 * for, which is far more than the period it reads in; it only has to cover the
 * gap between the viewer's writes and the guest's reads.
 *
 * Linear rather than circular, and compacted on commit: snd_pcm_mmap_begin has
 * to hand back ONE contiguous run, so a ring would need either two areas (the
 * API allows it, the caller may not expect it) or a copy anyway. The memmove
 * is a few kilobytes per period. */
#define CAP_BUF 65536

struct tad_pcm {
	int   fd;             /* FIFO to the viewer, -1 if unavailable */
	int   stream;         /* 0 = playback, 1 = capture */
	u32   rate, channels, format;
	u32   frame_bytes;
	ulong buffer_size, period_size;
	/* Virtual playback clock — see pace_pcm(). t0_us is 0 when idle. */
	unsigned long long t0_us, written;
	/* Capture only. */
	u8   *cap;            /* &g_capbuf[slot][0]                          */
	u32   cap_len;        /* valid bytes                                 */
	u32   cap_taken;      /* handed out by mmap_begin, not yet committed */
	int   running;        /* between snd_pcm_start and snd_pcm_drop      */
	unsigned long long cap_t0_us, cap_read;
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

/* THE MICROPHONE, WHICH RUNS THE OTHER WAY.
 *
 * Same shape as the playback FIFO and the same reasoning behind it: a named
 * pipe in TADPOLE_DIR, no protocol, one process writing and one reading. Only
 * the direction changes — the viewer writes what the tablet's microphone heard
 * and the guest reads it.
 *
 * O_RDWR rather than O_RDONLY, and that is not a typo. Opening a FIFO read-only
 * blocks until a writer appears, and O_RDONLY|O_NONBLOCK succeeds but then
 * reports end-of-file the moment the writer closes — so a viewer that restarts
 * would leave the guest's microphone permanently at EOF. Holding a write
 * descriptor of our own means there is always a writer and read() simply
 * returns EAGAIN when there is nothing to hear. The same trick the input event
 * nodes use in tadpole_shim.c.
 *
 * AND IT IS OPENED ONLY WHILE RECORDING, which is what tells the host to turn
 * the microphone on. A FIFO with no reader cannot be opened for writing —
 * open() gives ENXIO — so "the viewer can open $TADPOLE_DIR/mic" means exactly
 * "the guest is capturing", with no second channel to keep in step and nothing
 * for the two sides to disagree about. It also matters that the host does not
 * hold the microphone when nobody is listening: on Android that is a battery
 * cost and, on some ROMs, a notification.
 *
 * THE HOST MAKES THE NODE, NOT US. A FIFO created here belongs to root — the
 * guest runs as root inside the chroot — and lands with the plain
 * app_data_file:s0 context, which an app may read but may not write. The
 * viewer creates it so it carries the app's own SELinux categories; root can
 * open anything either way. We only mkfifo as a fallback for the headless case
 * where there is no viewer to have done it. */
static void open_mic_fifo(struct tad_pcm *p)
{
	char path[512];
	static unsigned tries;

	if (p->fd < 0 && (tries++ & 0x3F))
		return;
	snprintf(path, sizeof(path), "%s/mic", tad_dir());
	p->fd = open(path, O_RDWR | O_NONBLOCK);
	if (p->fd < 0) {
		mkfifo(path, 0666);
		p->fd = open(path, O_RDWR | O_NONBLOCK);
	}
}

/* What the host has to record, published the same way audio.fmt publishes what
 * it has to play. Removed on close so the viewer knows to let the microphone
 * go — an app that keeps a recorder open costs battery and, on some ROMs, puts
 * a notification in the shade. */
static void publish_mic(struct tad_pcm *p, int on)
{
	char path[512], line[64];
	int fd;

	snprintf(path, sizeof(path), "%s/mic.fmt", tad_dir());
	if (!on) {
		unlink(path);
		return;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return;
	snprintf(line, sizeof(line), "%u %u %u %lu\n",
	         p->rate, p->channels, fmt_bits(p->format),
	         (ulong)p->period_size);
	write(fd, line, (size_t_)strlen_(line));
	close(fd);
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

	if (stream == 0) {
		open_fifo(p);
	} else {
		/* Capture: 16 kHz mono is what libMicrophone.so asks for, but every
		 * setter below records what it is actually told. */
		static u8 g_capbuf[4][CAP_BUF];
		p->rate        = 16000;
		p->channels    = 1;
		p->frame_bytes = 2;
		p->period_size = 1024;
		p->cap = g_capbuf[(p - g_pcm) & 3];
		/* The FIFO is opened by snd_pcm_start, not here: holding it open is
		 * the signal that recording is happening. */
	}

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
	if (pcm && pcm->stream == 1) {
		publish_mic(pcm, 0);
		pcm->running = 0;
	}
	if (pcm && pcm->fd >= 0) {
		close(pcm->fd);
		pcm->fd = -1;
	}
	return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
	if (!pcm)
		return 0;
	if (pcm->fd < 0 && pcm->stream == 0)
		open_fifo(pcm);           /* viewer may have started since open */
	pcm->t0_us = 0; pcm->written = 0;                /* restart the clock */
	pcm->cap_len = pcm->cap_taken = 0;
	pcm->cap_t0_us = 0; pcm->cap_read = 0;
	return 0;
}

int snd_pcm_start(snd_pcm_t *pcm)
{
	if (!pcm) return 0;
	pcm->t0_us = 0; pcm->written = 0;
	pcm->cap_t0_us = 0; pcm->cap_read = 0;
	if (pcm->stream == 1) {
		pcm->running = 1;
		if (pcm->fd < 0) open_mic_fifo(pcm);
		publish_mic(pcm, 1);       /* tell the host to start recording */
	}
	return 0;
}
/* drop discards buffered audio, so the virtual buffer is empty again. */
int snd_pcm_drop(snd_pcm_t *pcm)
{
	if (!pcm) return 0;
	pcm->t0_us = 0; pcm->written = 0;
	pcm->cap_len = pcm->cap_taken = 0;
	if (pcm->stream == 1) {
		pcm->running = 0;
		publish_mic(pcm, 0);
		if (pcm->fd >= 0) { close(pcm->fd); pcm->fd = -1; }
	}
	return 0;
}
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
			usleep((u32)us);
		}
		now = now_us();
		if (!now || ++guard > 40)
			break;
	}
	p->written += bytes;
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

/* ---- capture ------------------------------------------------------------
 *
 * Brio's microphone module reads with the mmap interface, not snd_pcm_readi:
 *
 *     avail_update()                   how much is there
 *     mmap_begin(&areas,&off,&frames)  where it is
 *     ... copy it out, or sf_write_raw it ...
 *     mmap_commit(off, frames)         done with it
 *
 * so we have to hand back a real memory area, not a count. The staging buffer
 * is that area; the FIFO is drained into it here.
 *
 * PACED AGAINST REAL TIME, for the same reason the playback side is. A capture
 * that reported everything available the instant it arrived would let the
 * guest's recording task spin as fast as it could read, and the video recorder
 * uses the audio clock to decide how many video frames to write — so an
 * unpaced microphone makes a recording that plays back at the wrong speed.
 * Never report more than real time has produced. */
static void cap_fill(struct tad_pcm *p)
{
	slong n;

	if (p->fd < 0) {
		open_mic_fifo(p);
		if (p->fd < 0)
			return;
	}
	while (p->cap_len < CAP_BUF) {
		n = read(p->fd, p->cap + p->cap_len, CAP_BUF - p->cap_len);
		if (n <= 0)
			break;
		p->cap_len += (u32)n;
	}
	/* Overrun: the guest stopped reading. Keep the NEWEST samples — a
	 * recording that drops a moment in the middle is better than one that
	 * falls further and further behind. */
	if (p->cap_len >= CAP_BUF) {
		u32 keep = CAP_BUF / 2;
		memmove(p->cap, p->cap + (p->cap_len - keep), keep);
		p->cap_len = keep;
	}
}

slong snd_pcm_avail_update(snd_pcm_t *pcm)
{
	unsigned long long el, due;
	u32 frames;

	if (!pcm)
		return 0;
	if (pcm->stream == 0)
		return (slong)pcm->period_size;

	cap_fill(pcm);
	frames = pcm->frame_bytes ? pcm->cap_len / pcm->frame_bytes : 0;

	if (!pcm->cap_t0_us)
		pcm->cap_t0_us = now_us();
	el  = now_us() - pcm->cap_t0_us;
	due = (unsigned long long)pcm->rate * el / 1000000ULL;
	if (due <= pcm->cap_read)
		return 0;
	due -= pcm->cap_read;
	if (frames > due)
		frames = (u32)due;
	return (slong)frames;
}

/* struct snd_pcm_channel_area: where one channel's samples are, in BITS.
 * Interleaved S16 stereo is addr=base, first=16*ch, step=32. */
struct tad_area { void *addr; u32 first; u32 step; };
static struct tad_area g_areas[8];

int snd_pcm_mmap_begin(snd_pcm_t *pcm, const struct tad_area **areas,
                       ulong *offset, ulong *frames)
{
	u32 have, want, ch, bits;

	if (!pcm || !areas || !offset || !frames)
		return -22;
	if (pcm->stream != 1)
		return -77;                            /* -EBADFD: playback uses writei */

	cap_fill(pcm);
	bits = fmt_bits(pcm->format);
	have = pcm->frame_bytes ? pcm->cap_len / pcm->frame_bytes : 0;
	want = (u32)*frames;
	if (want > have)
		want = have;

	for (ch = 0; ch < pcm->channels && ch < 8; ch++) {
		g_areas[ch].addr  = pcm->cap;
		g_areas[ch].first = ch * bits;
		g_areas[ch].step  = pcm->channels * bits;
	}
	*areas  = g_areas;
	*offset = 0;                               /* always the start: see commit */
	*frames = want;
	pcm->cap_taken = want * pcm->frame_bytes;
	return 0;
}

slong snd_pcm_mmap_commit(snd_pcm_t *pcm, ulong offset, ulong frames)
{
	u32 n;

	(void)offset;
	if (!pcm || pcm->stream != 1)
		return (slong)frames;
	n = (u32)frames * pcm->frame_bytes;
	if (n > pcm->cap_len)
		n = pcm->cap_len;
	if (n) {
		memmove(pcm->cap, pcm->cap + n, pcm->cap_len - n);
		pcm->cap_len -= n;
		pcm->cap_read += frames;
	}
	pcm->cap_taken = 0;
	return (slong)frames;
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

/* THE ONE THAT WAS MISSING, AND WHAT IT COST. libMicrophone.so imports
 * snd_pcm_sw_params_malloc and this library did not define it, so the guest
 * loader refused the whole module:
 *
 *     /LF/Base/bin/AppManager: can't resolve symbol 'snd_pcm_sw_params_malloc'
 *
 * one line, in the middle of a boot that otherwise looks fine, and the
 * consequence is a Brio with no microphone at all — which the camera then
 * reports as a camera problem. Every symbol libMicrophone.so imports is
 * defined here now, whether or not it does anything. */
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **p)
{
	static struct tad_sw_params pool[4];
	static int n;
	if (!p || n >= 4)
		return -12;
	*p = &pool[n++];
	memset(*p, 0, sizeof(**p));
	(*p)->magic = SW_MAGIC;
	return 0;
}

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

/* The _near setters take a POINTER and write back what they settled on; the
 * caller reads it. Handing back the requested value is honest here — there is
 * no hardware to refuse it, and the host records at whatever we publish. */
int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                        u32 *val)
{
	(void)pcm;
	if (!hw || !val) return -22;
	if (*val == 0) *val = 1;
	hw->channels = *val;
	return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm,
                                           snd_pcm_hw_params_t *hw,
                                           ulong *val, int *dir)
{
	(void)pcm;
	if (!hw || !val) return -22;
	if (*val == 0) *val = 1024;
	hw->period_size = *val;
	if (dir) *dir = 0;
	return 0;
}

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

void snd_pcm_sw_params_free(snd_pcm_sw_params_t *p) { (void)p; }

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw, ulong v)
{ (void)pcm; if (sw) sw->start_threshold = v; return 0; }

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw, ulong v)
{ (void)pcm; if (sw) sw->avail_min = v; return 0; }

int snd_pcm_sw_params_set_tstamp_mode(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw,
                                      int mode)
{ (void)pcm; (void)sw; (void)mode; return 0; }

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw)
{ (void)pcm; (void)sw; return 0; }

/* ---- status ------------------------------------------------------------
 *
 * The microphone module asks for the TRIGGER TIMESTAMP — when capture actually
 * started — and uses it to line the audio up with the video. A monotonic
 * reading of our own clock is the right answer: it is the same clock the
 * capture pacing uses, so the two agree with each other, which is what
 * matters. */
struct tad_status { u32 magic; long trig_sec, trig_nsec; };
#define ST_MAGIC 0x53544154u

size_t_ snd_pcm_status_sizeof(void) { return PARAMS_SIZE; }

int snd_pcm_status_malloc(void **p)
{
	static struct tad_status pool[4];
	static int n;
	if (!p || n >= 4)
		return -12;
	*p = &pool[n++];
	memset(*p, 0, sizeof(struct tad_status));
	((struct tad_status *)*p)->magic = ST_MAGIC;
	return 0;
}
void snd_pcm_status_free(void *p) { (void)p; }

int snd_pcm_status(snd_pcm_t *pcm, void *status)
{
	struct tad_status *st = status;
	struct tad_ts t;

	if (!st)
		return -22;
	st->magic = ST_MAGIC;
	if (pcm && pcm->cap_t0_us) {
		st->trig_sec  = (long)(pcm->cap_t0_us / 1000000ULL);
		st->trig_nsec = (long)((pcm->cap_t0_us % 1000000ULL) * 1000ULL);
	} else if (clock_gettime(CLOCK_MONOTONIC_, &t) == 0) {
		st->trig_sec = t.tv_sec; st->trig_nsec = t.tv_nsec;
	}
	return 0;
}

/* struct timeval out-parameter: two longs on this 32-bit target. */
void snd_pcm_status_get_trigger_tstamp(const void *status, void *tstamp)
{
	const struct tad_status *st = status;
	long *tv = tstamp;

	if (!tv)
		return;
	if (st && st->magic == ST_MAGIC) {
		tv[0] = st->trig_sec;
		tv[1] = st->trig_nsec / 1000;
	} else {
		tv[0] = 0; tv[1] = 0;
	}
}

/* ---- misc --------------------------------------------------------------- */

const char *snd_strerror(int e)
{
	(void)e;
	return "tadpole-asound";
}
