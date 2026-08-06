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

/* ---- the frame at FULL draw resolution ----------------------------------
 *
 * hle_host_pump hands back a panel-sized frame because that is what the
 * guest's framebuffer is. When the replay is drawing at 3x, squeezing it into
 * 480x272 is the last thing that happens to it — and the only reason to do
 * that is that the guest's layer has to hold something.
 *
 * The viewer draws the game layer itself, so it can have the big one. Call
 * hle_host_want_full(1) and the downscale is skipped; hle_host_full() then
 * reports the size to allocate and hle_host_read_full() fills it after a
 * frame completes.
 */
void hle_host_want_full(int on);
void hle_host_full(int *w, int *h);          /* draw-buffer size */
/* The layer rectangle in PANEL coordinates — where the picture belongs. */
void hle_host_rect(int *x, int *y, int *w, int *h);
int  hle_host_read_full(unsigned int *out);  /* 1 if it produced pixels */

void hle_host_stats(unsigned long *frames, unsigned long *packets);
unsigned int hle_host_desyncs(void);
int  hle_guest_fell_back(void);

#endif
