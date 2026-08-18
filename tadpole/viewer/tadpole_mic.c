/* Tadpole — host side of the LeapPad2's microphone.
 *
 * The guest half is in tadpole/shim/tadpole_asound.c, which is a complete
 * replacement for libasound and now serves capture as well as playback. Brio's
 * microphone module reads with the ALSA mmap interface — avail_update,
 * mmap_begin, mmap_commit — so the shim has to hand back real memory; this
 * side fills it.
 *
 * THE FIFO IS THE PROTOCOL AND ALSO THE SWITCH. $TADPOLE_DIR/mic is a named
 * pipe, the same arrangement the playback path has used from the beginning,
 * running the other way. What is new is that its existence as an OPEN pipe is
 * the control signal: a FIFO with no reader cannot be opened for writing —
 * open() gives ENXIO — so "we can open it" means exactly "the guest is
 * recording". There is no second channel to keep in step, nothing to go stale
 * if a guest dies mid-recording, and the microphone is not held open when
 * nothing is listening.
 *
 * WE CREATE THE NODE. A FIFO made by the guest belongs to root and carries the
 * bare app_data_file:s0 context, which an Android app may read but may not
 * write — the same MLS category rule that made root-created framebuffer files
 * unmappable and needed the helper's chcon pass. Making it here gives it the
 * app's own categories, and root can open it either way.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "tadpole_mic.h"

static char g_dir[512];
static int  g_fd = -1;          /* write end of the mic FIFO, -1 when idle */
static int  g_running;
static int  g_rate = 16000, g_channels = 1;
static int  g_wait;             /* pumps to wait before probing again */
static unsigned long g_dropped;

void tad_mic_init(const char *dir)
{
#ifndef _WIN32
	char path[600];

	snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "");
	snprintf(path, sizeof(path), "%s/mic", g_dir);
	if (mkfifo(path, 0666) != 0 && errno != EEXIST)
		fprintf(stderr, "mic: cannot create %s: %s\n", path, strerror(errno));
#else
	(void)dir;
#endif
}

/* What the guest negotiated, if it has said. Optional: the microphone module
 * asks for 16 kHz mono and that is the default, so a mic.fmt we cannot read
 * costs nothing. */
static void read_fmt(void)
{
	char path[600], line[64];
	FILE *f;
	unsigned rate = 0, ch = 0, bits = 0;

	snprintf(path, sizeof(path), "%s/mic.fmt", g_dir);
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(line, sizeof(line), f) &&
	    sscanf(line, "%u %u %u", &rate, &ch, &bits) >= 2) {
		if (rate >= 4000 && rate <= 96000) g_rate = (int)rate;
		if (ch == 1 || ch == 2)            g_channels = (int)ch;
	}
	fclose(f);
}

void tad_mic_pump(void)
{
#ifndef _WIN32
	char path[600];

	if (!g_dir[0])
		return;

	if (g_running) {
		if (g_fd >= 0)
			return;
		tad_mic_plat_stop();
		g_running = 0;
	}
	if (g_wait > 0) { g_wait--; return; }
	g_wait = 30;                       /* about twice a second at 60 fps */

	snprintf(path, sizeof(path), "%s/mic", g_dir);
	g_fd = open(path, O_WRONLY | O_NONBLOCK);
	if (g_fd < 0)
		return;                        /* ENXIO: nobody is recording */

	read_fmt();
	g_dropped = 0;
	tad_mic_plat_start(g_rate, g_channels);
	g_running = 1;
	fprintf(stderr, "mic: recording at %d Hz, %d channel(s)\n",
	        g_rate, g_channels);
#endif
}

void tad_mic_data(const void *pcm, int bytes)
{
#ifndef _WIN32
	const char *p = pcm;
	int fd = g_fd;

	if (fd < 0 || !p || bytes <= 0)
		return;
	while (bytes > 0) {
		ssize_t n = write(fd, p, (size_t)bytes);
		if (n > 0) { p += n; bytes -= (int)n; continue; }
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN) {
			/* THE GUEST IS NOT KEEPING UP. Dropping is right: this is live
			 * audio and there is nowhere to put it. The shim's own staging
			 * buffer already covers two seconds of jitter, so reaching here
			 * means the recording task has stopped reading altogether. */
			g_dropped += (unsigned long)bytes;
			return;
		}
		/* EPIPE: the guest closed its end. Stop until it asks again. */
		close(fd);
		g_fd = -1;
		return;
	}
#else
	(void)pcm; (void)bytes;
#endif
}

/* Generic no-op backend; Android overrides these. */
#if !defined(__ANDROID__)
void tad_mic_plat_start(int rate, int channels) { (void)rate; (void)channels; }
void tad_mic_plat_stop(void) { }
#endif
