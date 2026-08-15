/* The boot presentation: the LeapFrog logo and the startup animation a device
 * shows between the power switch and the home screen.
 *
 * OFF UNLESS ASKED FOR. Options -> System Settings -> Fast Boot is ticked by
 * default and skips all of this; nobody waits for an animation they did not
 * choose. It is also reached from exactly one place — File -> Run System Menu,
 * via guest_launch_ui(). Launching a title directly must never play it, on the
 * device or here: that is not what turning a LeapPad on looks like.
 *
 * WHY THE FRONT END DRAWS IT INSTEAD OF THE GUEST. The device's own path is
 * rcS drawing the logo with imager-fb, then /usr/bin/app asking VideoDaemon for
 * the animation with `vnotify 6`. That path needs three things we do not have:
 * VideoDaemon must survive its own daemonising fork (it now does — see
 * gp_fork() in glasspole), vnotify must reach it over /tmp/video_events_socket
 * (Glasspole's AF_UNIX is in-process, so a second guest cannot), and Windows
 * has no fork at all. Driving it from here works on every engine and on
 * Windows, at the cost of being an imitation rather than the device booting.
 *
 * It is an imitation, and the table in tadpole_boot.c is the shape of that
 * admission: every system this is added to needs its own row — logo, video,
 * how long the logo holds — because none of it is discovered, all of it is
 * declared. Adding a system is a row; forgetting one is a system that boots
 * straight to its home screen.
 */
#ifndef TADPOLE_BOOT_H
#define TADPOLE_BOOT_H

#include <SDL.h>

/* Arm the sequence for the guest about to start. Cheap and SDL-free: the
 * assets are opened on the first boot_draw(), which is the call that has a
 * renderer. `projdir` is the checkout root, as g_projdir holds it. Silently
 * does nothing when no system files match a row in the table. */
void boot_arm(const char *projdir);

/* 1 while the presentation owns the panel. */
int  boot_active(void);

/* Stop now and release everything. Called when the guest's UI comes up, when
 * the guest dies, and at shutdown — the first of those is what VideoDaemon
 * does on /tmp/ui_ready, and it is why this costs no time on a normal boot. */
void boot_stop(void);

/* Paint this frame into `dst`, the same panel rectangle and rotation the guest
 * composite uses. Returns 1 if it painted (so the caller skips the guest's
 * frame), 0 once the sequence is over. */
int  boot_draw(SDL_Renderer *ren, const SDL_Rect *dst, int rotate);

#endif  /* TADPOLE_BOOT_H */
