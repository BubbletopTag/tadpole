/* Tadpole — audible end-to-end test of the fake libasound.
 *
 * A guest-side ARM binary that opens a PCM through our shim and writes a
 * one-second 440 Hz tone. If you hear it, the whole chain is proven:
 *
 *     guest -> tadpole_asound.c -> $TADPOLE_DIR/audio FIFO -> viewer -> SDL
 *
 * That matters because it separates OUR half of the audio path from Brio's.
 * Brio currently streams pure silence, and without this you cannot tell
 * whether the pipe is broken or whether Brio simply is not mixing anything
 * into it.
 *
 *     ./tadpole.sh --run /tone
 */

typedef unsigned int  u32;
typedef unsigned long ulong;
typedef long          slong;

extern int  snd_pcm_open(void **pcm, const char *name, int stream, int mode);
extern int  snd_pcm_set_params(void *pcm, int fmt, int access, u32 ch,
                               u32 rate, int resample, u32 latency);
extern slong snd_pcm_mmap_writei(void *pcm, const void *buf, ulong frames);
extern int  snd_pcm_close(void *pcm);
extern int  printf(const char *fmt, ...);
extern void exit(int code);

#define RATE   32000
#define CHANS  2
#define SECS   2
#define FRAMES (RATE * SECS)

static short buf[FRAMES * CHANS];

/* Integer sine, good enough to recognise by ear: a triangle-ish wave built
 * from a running phase. No libm dependency, so this links against nothing but
 * our shim and libc. */
static short osc(u32 phase)
{
	u32 p = phase & 0xFFFFu;
	int v;
	if (p < 0x4000)        v = (int)p * 2;
	else if (p < 0xC000)   v = 0x8000 - (int)p * 2;
	else                   v = (int)p * 2 - 0x20000;
	return (short)(v / 3);
}

/* Freestanding: no ARM crt1.o or libgcc in the guest lib set, so provide the
 * entry point ourselves and link -nostdlib against libc.so.0 for printf/exit. */
void _start(void);

static int tone_main(void);

void _start(void) { exit(tone_main()); }

static int tone_main(void)
{
	void *pcm = 0;
	u32 phase = 0, step = (u32)(440.0 * 65536.0 / RATE);
	int i;

	if (snd_pcm_open(&pcm, "plugdmix", 0, 0) < 0) {
		printf("tone: snd_pcm_open failed\n");
		return 1;
	}
	snd_pcm_set_params(pcm, 2 /*S16_LE*/, 3 /*RW_INTERLEAVED*/,
	                   CHANS, RATE, 1, 100000);

	for (i = 0; i < FRAMES; i++) {
		short s = osc(phase);
		buf[i * CHANS] = s;
		buf[i * CHANS + 1] = s;
		phase += step;
	}

	printf("tone: writing %d frames of 440 Hz at %d Hz stereo\n", FRAMES, RATE);
	snd_pcm_mmap_writei(pcm, buf, FRAMES);
	snd_pcm_close(pcm);
	printf("tone: done\n");
	return 0;
}
