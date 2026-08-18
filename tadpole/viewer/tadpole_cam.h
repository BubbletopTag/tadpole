/* Tadpole — host side of the LeapPad2 camera.
 *
 * The guest half lives in tadpole/shim/tadpole_v4l2.c and answers the V4L2
 * ioctls; this half produces the pixels. The two meet in state.bin's
 * struct tad_cam_state (shim/tadpole_cam.h) and in $TADPOLE_DIR/camN.bin.
 */
#ifndef TADPOLE_CAM_HOST_H
#define TADPOLE_CAM_HOST_H

#include <stddef.h>
#include "../shim/tadpole_cam.h"

/* `state` is the mapped state.bin; `arena` is the mapped fb0.bin, which is
 * where the viewfinder has to be drawn. Safe to call repeatedly. */
void tad_cam_init(const char *dir, void *state, void *arena, size_t arena_bytes);

/* Once per rendered frame. Starts and stops the platform capture backend as
 * the guest opens and closes the nodes, and draws the still fallback when no
 * backend has produced anything. */
void tad_cam_pump(void);

/* One captured frame, planar I420, from whatever backend is running. May be
 * called from a capture thread. */
void tad_cam_submit(int idx, const unsigned char *i420, int w, int h);

/* Implemented per platform; the generic ones are no-ops. Called from
 * tad_cam_pump() on the render thread only. */
void tad_cam_plat_start(int idx, int w, int h);
void tad_cam_plat_stop(int idx);

/* How many cameras the platform backend can actually offer (0 if none). */
int  tad_cam_plat_count(void);

/* Did the last tad_cam_plat_start() actually leave a camera running? */
int  tad_cam_plat_running(int idx);

#endif
