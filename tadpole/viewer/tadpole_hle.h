/* Tadpole — host-side GL replay. See tadpole_hle.c for the design, and
 * shim/tadpole_glcmd.h for the wire format. */
#ifndef TADPOLE_HLE_H
#define TADPOLE_HLE_H

/* Creates the hidden GL context, the offscreen target and the shared ring.
 * Returns 0 if the host cannot replay, in which case the guest sees no
 * heartbeat and keeps rasterising in software. */
int  hle_host_init(const char *dir, int w, int h);
void hle_host_shutdown(void);
int  hle_host_ready(void);

/* Replay whatever the guest has queued. Returns 1 if a frame completed, with
 * `out` holding it as ARGB8888 at `pitch_px` pixels per row. */
int  hle_host_pump(unsigned int *out, unsigned int pitch_px);

void hle_host_stats(unsigned long *frames, unsigned long *packets);
unsigned int hle_host_desyncs(void);
int  hle_guest_fell_back(void);

#endif
