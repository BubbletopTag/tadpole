/* Tadpole — host-side GL replay. See tadpole_hle.c for the design, and
 * shim/tadpole_glcmd.h for the wire format. */
#ifndef TADPOLE_HLE_H
#define TADPOLE_HLE_H

/* Creates the hidden GL context, the offscreen target and the shared ring.
 * Returns 0 if the host cannot replay, in which case the guest sees no
 * heartbeat and keeps rasterising in software. */
int  hle_host_init(const char *dir, int w, int h, int samples, int scale);

/* Picture quality, changeable while a guest is running.
 *
 *   samples  multisampling: 0, 2, 4 or 8
 *   scale    render scale: draw at `scale` times the panel in each axis and
 *            filter back down (supersampling). 1 = off.
 *
 * Anything the driver cannot honour is clamped and reported. Neither is
 * visible to the guest, which receives its 480x272 either way. */
void hle_host_set_quality(int samples, int scale);
int  hle_host_msaa(void);
int  hle_host_scale(void);
void hle_host_shutdown(void);
int  hle_host_ready(void);

/* Replay whatever the guest has queued. Returns 1 if a frame completed, with
 * `out` holding it as ARGB8888 at `pitch_px` pixels per row. */
int  hle_host_pump(unsigned int *out, unsigned int pitch_px);

void hle_host_stats(unsigned long *frames, unsigned long *packets);
unsigned int hle_host_desyncs(void);
int  hle_guest_fell_back(void);

#endif
