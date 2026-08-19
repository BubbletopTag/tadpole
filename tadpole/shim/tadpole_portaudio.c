/* Tadpole — fake libportaudio.so for the Didj.
 *
 * WHY A FULL REPLACEMENT RATHER THAN A WORKING /dev/dsp
 * ----------------------------------------------------
 * There IS a working /dev/dsp: the shim answers its format ioctls, takes its
 * writes and paces them, and the guest's own portaudio drove it correctly —
 * negotiating 32000 Hz stereo S16_LE and producing 16 KB buffers on time.
 *
 * And AppManager still never got past audio init. Brio's main thread sat in a
 * futex wait from the moment portaudio created its callback thread, while that
 * thread ran happily forever: PaUnixThread_New posts the child and waits to be
 * told it has started, and on this 2008 build the telling never happened. From
 * outside there is nothing to fix — the device is behaving, the callback is
 * being called, and the parent is waiting on a condition variable inside a
 * library we do not have the source of.
 *
 * So do what tadpole_asound.c did for ALSA and implement the API instead. The
 * surface is nine symbols, which is the whole reason this is a better trade
 * than emulating a sound card well enough for a particular old copy of
 * portaudio to be happy with it:
 *
 *     Pa_Initialize  Pa_Terminate  Pa_GetErrorText
 *     Pa_GetDefaultOutputDevice   Pa_GetDeviceInfo
 *     Pa_OpenStream  Pa_StartStream  Pa_StopStream  Pa_CloseStream
 *
 * WHERE THE SAMPLES GO: straight out through /dev/dsp, which the shim already
 * owns. Nothing here talks to the viewer directly — the pacing, the FIFO and
 * the audio.fmt handshake are all on the other side of that write().
 *
 * THE ABI IS PORTAUDIO V19's PUBLIC ONE and none of it is guessed: the structs
 * below are portaudio.h's, and the only target-specific part is that a double
 * is 8-byte aligned on ARM EABI, which the compiler applies for us.
 */

typedef unsigned int   u32;
typedef unsigned long  ulong;
typedef long           slong;
typedef __SIZE_TYPE__  size_t;

#define NULL ((void *)0)

extern int   open(const char *path, int flags, ...);
extern int   close(int fd);
extern slong write(int fd, const void *buf, size_t n);
extern int   ioctl(int fd, ulong req, ...);
extern void *memset(void *s, int c, size_t n);
extern int   pthread_create(ulong *th, const void *attr,
                            void *(*fn)(void *), void *arg);

#define O_WRONLY 01

/* OSS, as in tadpole_shim.c — the same numbers, and the shim answers them. */
#define SNDCTL_DSP_SETFMT     0xC0045005ul
#define SNDCTL_DSP_CHANNELS   0xC0045006ul
#define SNDCTL_DSP_SPEED      0xC0045002ul
#define SNDCTL_DSP_SETFRAGMENT 0xC004500Aul
#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010

/* ---- portaudio.h ------------------------------------------------------- */

typedef int    PaError;
typedef int    PaDeviceIndex;
typedef int    PaHostApiIndex;
typedef double PaTime;
typedef ulong  PaSampleFormat;
typedef ulong  PaStreamFlags;
typedef ulong  PaStreamCallbackFlags;
typedef void   PaStream;

#define paNoError      0
#define paContinue     0

#define paFloat32 0x00000001ul
#define paInt32   0x00000002ul
#define paInt24   0x00000004ul
#define paInt16   0x00000008ul
#define paInt8    0x00000010ul
#define paUInt8   0x00000020ul

typedef struct PaStreamCallbackTimeInfo {
	PaTime inputBufferAdcTime;
	PaTime currentTime;
	PaTime outputBufferDacTime;
} PaStreamCallbackTimeInfo;

typedef int PaStreamCallback(const void *input, void *output,
                             ulong frameCount,
                             const PaStreamCallbackTimeInfo *timeInfo,
                             PaStreamCallbackFlags statusFlags,
                             void *userData);

typedef struct PaStreamParameters {
	PaDeviceIndex device;
	int           channelCount;
	PaSampleFormat sampleFormat;
	PaTime        suggestedLatency;
	void         *hostApiSpecificStreamInfo;
} PaStreamParameters;

typedef struct PaDeviceInfo {
	int         structVersion;
	const char *name;
	PaHostApiIndex hostApi;
	int         maxInputChannels;
	int         maxOutputChannels;
	PaTime      defaultLowInputLatency;
	PaTime      defaultLowOutputLatency;
	PaTime      defaultHighInputLatency;
	PaTime      defaultHighOutputLatency;
	double      defaultSampleRate;
} PaDeviceInfo;

/* ---- one device, one stream -------------------------------------------- */
/*
 * The Didj has one sound device and Brio opens one stream on it. Making
 * either of those an array would be inventing a generality nothing asks for.
 */
static const PaDeviceInfo g_dev = {
	2,                    /* structVersion */
	"Didj",
	0,                    /* hostApi */
	0,                    /* maxInputChannels — no capture on this device */
	2,
	0.020, 0.020,         /* low latency, in and out */
	0.080, 0.080,         /* high */
	32000.0               /* what its /etc/asound.conf-equivalent runs at */
};

#define DSP_MAX_FRAME 8               /* 2 ch * 4 bytes, the widest we make */
#define DSP_MAX_BUF   (4096 * DSP_MAX_FRAME)

static struct {
	int   open;
	int   running;
	int   fd;                          /* /dev/dsp */
	PaStreamCallback *cb;
	void *user;
	ulong frames;                      /* per callback */
	u32   rate, channels, bytes;       /* bytes = per sample, per channel */
	PaTime clock;                      /* fed to the callback */
	unsigned char buf[DSP_MAX_BUF];
} g_s;

static u32 fmt_bytes(PaSampleFormat f)
{
	/* Only the formats an OSS device can actually take. Anything else is
	 * converted by nobody, so it is refused at Pa_OpenStream rather than
	 * played as noise. */
	if (f & paInt16) return 2;
	if (f & paInt8)  return 1;
	if (f & paUInt8) return 1;
	return 0;
}

PaError Pa_Initialize(void)
{
	memset(&g_s, 0, sizeof(g_s));
	g_s.fd = -1;
	return paNoError;
}

PaError Pa_Terminate(void)
{
	g_s.running = 0;
	return paNoError;
}

PaDeviceIndex Pa_GetDefaultOutputDevice(void) { return 0; }

const PaDeviceInfo *Pa_GetDeviceInfo(PaDeviceIndex i)
{
	return i == 0 ? &g_dev : NULL;
}

const char *Pa_GetErrorText(PaError e)
{
	return e == paNoError ? "Success" : "Tadpole portaudio: unsupported request";
}

PaError Pa_OpenStream(PaStream **stream,
                      const PaStreamParameters *in,
                      const PaStreamParameters *out,
                      double rate, ulong frames,
                      PaStreamFlags flags,
                      PaStreamCallback *cb, void *user)
{
	u32 b;

	(void)in; (void)flags;
	if (!stream || !out)
		return -1;
	b = fmt_bytes(out->sampleFormat);
	if (!b || out->channelCount < 1 || out->channelCount > 2)
		return -1;
	/* paFramesPerBufferUnspecified is 0: pick something, and pick the buffer
	 * the shim's fragment size already implies so nothing has to re-chunk. */
	if (!frames || frames * (ulong)out->channelCount * b > DSP_MAX_BUF)
		frames = 1024;

	g_s.cb       = cb;
	g_s.user     = user;
	g_s.frames   = frames;
	g_s.rate     = (u32)rate;
	g_s.channels = (u32)out->channelCount;
	g_s.bytes    = b;
	g_s.clock    = 0.0;
	g_s.open     = 1;
	*stream = (PaStream *)&g_s;
	return paNoError;
}

/* CALL THE CALLBACK, WRITE WHAT IT PRODUCED, REPEAT.
 *
 * No pacing here on purpose. write() lands in the shim's /dev/dsp, which
 * either fills a FIFO the viewer drains — and is throttled by that — or, with
 * no viewer, sleeps for exactly as long as the samples would have taken. Doing
 * it a second time here would halve the rate. */
static void *pump(void *arg)
{
	ulong bytes = g_s.frames * g_s.channels * g_s.bytes;

	(void)arg;
	while (g_s.running) {
		PaStreamCallbackTimeInfo t;
		int r;

		t.inputBufferAdcTime = 0.0;
		t.currentTime = g_s.clock;
		t.outputBufferDacTime = g_s.clock;

		memset(g_s.buf, 0, bytes);
		r = g_s.cb ? g_s.cb(NULL, g_s.buf, g_s.frames, &t, 0, g_s.user)
		           : paContinue;
		if (g_s.fd >= 0)
			write(g_s.fd, g_s.buf, bytes);
		g_s.clock += (PaTime)g_s.frames / (PaTime)(g_s.rate ? g_s.rate : 1);
		if (r != paContinue)
			break;
	}
	g_s.running = 0;
	return NULL;
}

PaError Pa_StartStream(PaStream *s)
{
	ulong th;
	int v;

	if (s != (PaStream *)&g_s || !g_s.open)
		return -1;
	if (g_s.running)
		return paNoError;

	g_s.fd = open("/dev/dsp", O_WRONLY);
	if (g_s.fd >= 0) {
		/* Tell the shim what we settled on, so it publishes the right
		 * audio.fmt and paces at the right rate. Failure is not fatal: a
		 * silent stream beats a dead AppManager. */
		v = (g_s.bytes == 2) ? AFMT_S16_LE : AFMT_U8;
		ioctl(g_s.fd, SNDCTL_DSP_SETFMT, &v);
		v = (int)g_s.channels;   ioctl(g_s.fd, SNDCTL_DSP_CHANNELS, &v);
		v = (int)g_s.rate;       ioctl(g_s.fd, SNDCTL_DSP_SPEED, &v);
	}

	g_s.running = 1;
	/* AND RETURN, WITHOUT WAITING FOR THE CHILD. That wait is the entire
	 * reason this file exists: the stock library's parent blocks until the
	 * callback thread says it has started, and here that never arrived. There
	 * is nothing to synchronise with — the thread's first act is to call the
	 * callback, and a caller that wants to know it is running has
	 * Pa_IsStreamActive, which Brio does not use. */
	if (pthread_create(&th, NULL, pump, NULL) != 0) {
		g_s.running = 0;
		return -1;
	}
	return paNoError;
}

PaError Pa_StopStream(PaStream *s)
{
	if (s != (PaStream *)&g_s)
		return -1;
	g_s.running = 0;
	return paNoError;
}

PaError Pa_CloseStream(PaStream *s)
{
	if (s != (PaStream *)&g_s)
		return -1;
	g_s.running = 0;
	if (g_s.fd >= 0) {
		close(g_s.fd);
		g_s.fd = -1;
	}
	g_s.open = 0;
	return paNoError;
}
