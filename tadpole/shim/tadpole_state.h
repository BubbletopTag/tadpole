/* Tadpole — state.bin: the geometry the guest publishes and the host reads.
 *
 * ONE DEFINITION. Four separate binaries map this file: the shim that writes
 * it (tadpole_shim.c), the GLES rasteriser that renders into the rectangle it
 * names (tadpole_gles_core.c, a different .so inside the same guest), the
 * viewer that composites with it (tadpole_view.c), and the Python capture
 * tools. They are built at different times, from different branches, and — via
 * the runtime/shimlibs symlinks the worktrees share — a run can genuinely mix
 * a shim from one branch with a GL library from another.
 *
 * THE BUG THIS FILE EXISTS TO END, which has now been fixed four times:
 *
 *     Every Leapster title renders at the full 480x272 panel instead of the
 *     320x240 window its ViewFrame gives it — scaled up and cropped, with the
 *     right-hand side of the game under the bamboo chrome.
 *
 * It has never once been a rendering bug. Each time, the GL rasteriser had
 * decided it could not trust state.bin, and its fallback is "use the whole
 * panel". The reasons it decided that:
 *
 *   1. struct layer_state gained win_x/win_y/win_w/win_h in one file and not
 *      the others, so layer[1] was read out of the middle of layer[0].
 *   2. Windows' CreateFileMapping EXTENDS a file to the section size, so a
 *      272-byte state.bin measured 65536 once mapped (fixed in host_win32.c).
 *   3. The android branch appended a camera block, making state.bin 528 bytes.
 *      main's GL library wanted exactly 272, saw 528, and fell back — on a
 *      shim binary main shares with that branch by symlink.
 *
 * Two rules follow, and both are enforced by tests:
 *
 *   GROW ONLY AT THE END. Everything up to and including layer[NUM_FB-1] is
 *   frozen. A field inserted above the layers moves every layer for every
 *   reader that has not been rebuilt, and nothing warns.
 *
 *   A LONGER state.bin IS NOT AN ERROR. It means the writer is newer than the
 *   reader and appended at the end, which rule one guarantees is harmless.
 *   Only a SHORTER file is unreadable. A reader that demands sizeof equality
 *   turns every future field into the scaling bug — that is precisely how (3)
 *   happened. tad_state_fault() below is the check; use it, do not write your
 *   own.
 *
 * AND IF A READER DOES REFUSE, IT SAYS SO ON stderr. A silent fallback to the
 * full panel is indistinguishable from a rendering bug, which is why this cost
 * four separate investigations that each started by reading the rasteriser.
 */
#ifndef TADPOLE_STATE_H
#define TADPOLE_STATE_H

#define TADPOLE_MAGIC      0x54414450u   /* "TADP" */
/* The layout version. NOT TADPOLE_VERSION — the viewer already uses that name
 * for its own release string, and a shared header cannot take it. */
#define TAD_STATE_VERSION  1
#define NUM_FB             3

/* WHAT THE GUEST IS SHOWING. The panel is portrait and its software is not:
 * the LeapPad UI draws a quarter turn from how the device is held — the same
 * reason the stock boot art is named "...CW.png" — while nearly every title
 * draws landscape into the same buffer. So there is no one right rotation for
 * the window; it depends on what is on screen, and only the guest knows.
 * See screen_note() in tadpole_shim.c for how this is worked out. */
#define TAD_SCREEN_UNKNOWN 0
#define TAD_SCREEN_SYSTEM  1   /* the LeapPad UI — portrait */
#define TAD_SCREEN_TITLE   2   /* an installed title — landscape, nearly always */
#define PKGID_MAX          64

/* Plain `unsigned int` rather than a u32 typedef: this header is included by
 * three files that each declare their own integer types, and by a Windows
 * build. It is 32 bits on every target Tadpole has (ARMv7 guest, x86-64 host,
 * win32 host), which is what the on-disk layout requires. */
struct layer_state {
	unsigned int enabled, xres, yres, bpp, xoffset, yoffset;
	unsigned int nonstd;      /* format/priority/planar bits, see lf1000fb.h */
	unsigned int alpha, blank;
	/* WHERE THIS LAYER LANDS ON THE PANEL.
	 *
	 * A Leapster title does not own the screen. AppManager draws a ViewFrame
	 * (bamboo border, A/B/L/R buttons) on fb0 and gives the game a smaller
	 * window on fb1. For SpongeBob: The Clam Prix, EmeraldTitles/<pkg>/
	 * ViewFrame.json says x=15 y=17 w=320 h=240, and the guest pushes exactly
	 * that down to the driver:
	 *
	 *     fb1 PUTVAR req 320x240 virt 480x2176
	 *     fb1 posioctl 40046d03: f 11 <ptr> 14f 101
	 *                            ^  ^        ^   ^-- bottom 257 = 17+240
	 *                            |  |        `----- right  335 = 15+320
	 *                            |  `-------------- top     17
	 *                            `----------------- left    15
	 *
	 * The hardware MLC composites the layer at that rectangle. We have no MLC,
	 * so the rect is published here for the GL rasteriser to render into and
	 * for the viewer to composite with. Other titles differ (the reading games
	 * use 250x250 at x=76), so this must never be hardcoded.
	 *
	 * Defaults to the full panel, which is what the Flash UI actually uses.
	 */
	unsigned int win_x, win_y, win_w, win_h;

	/* THE VIDEO SCALER'S SOURCE SIZE, for the YUV layer only.
	 *
	 * LF1000FB_IOCSVIDSCALE carries the size of the picture that was actually
	 * decoded; the MLC then stretches it to the layer window. Sneak Peeks
	 * plays 320x240 trailers into a 362x272 window and says so:
	 *
	 *     SetVideoScaler: 0x86498: 320x240 (2)
	 *
	 * Without this the viewer reads a 362x272 rectangle out of a buffer that
	 * only holds 320x240 of picture — cropped, with the remainder garbage.
	 * Zero means "no scaler set": use the window size. */
	unsigned int vid_w, vid_h;
};

struct tadpole_state {
	unsigned int magic, version;
	unsigned int width, height;
	unsigned int vsync_count;
	struct layer_state layer[NUM_FB];

	/* APPENDED AT THE END ON PURPOSE — and everything after this line is the
	 * only part of the file that may ever grow. See the header comment. */
	unsigned int screen;        /* TAD_SCREEN_*: the UI, or a title */
	unsigned int screen_seq;    /* bumped on every change, so a viewer that
	                             * was not looking still sees the transition */
	char screen_pkg[PKGID_MAX]; /* the PackageID when a title is up */
};

/* CAN THIS BUILD READ THAT state.bin? NULL if yes, else why not, in a sentence
 * fit to print. `bytes` is the file's real length — st_size or lseek, NOT our
 * own sizeof.
 *
 * Longer than us is FINE and is the normal case for a newer writer. Shorter is
 * not: the fields we are about to read may not be in the file at all.
 *
 * The layer sanity sweep catches the one thing a length cannot: a struct
 * layer_state that changed SHAPE, which slides layer[1] somewhere else without
 * changing the total by anything a reader could notice. Real layers are panel
 * geometry — a few hundred pixels and a sane depth — so a shifted array reads
 * as garbage here almost immediately.
 */
static inline const char *tad_state_fault(const void *buf, long bytes)
{
	const struct tadpole_state *st = (const struct tadpole_state *)buf;
	int i;

	if (!buf)
		return "state.bin is not mapped";
	if (bytes > 0 && bytes < (long)sizeof(struct tadpole_state))
		return "state.bin is SHORTER than this build expects — the guest "
		       "shim is older than the library reading it; rebuild both";
	if (st->magic != TADPOLE_MAGIC)
		return "state.bin does not start with TADP — it is not ours, or the "
		       "shim was still writing it";
	for (i = 0; i < NUM_FB; i++) {
		const struct layer_state *l = &st->layer[i];
		if (l->xres > 4096u || l->yres > 4096u)
			return "the layer array does not decode as layers — struct "
			       "layer_state has drifted between the writer and this "
			       "build; they must be rebuilt together";
		if (l->bpp && l->bpp != 8u && l->bpp != 16u &&
		    l->bpp != 24u && l->bpp != 32u)
			return "a layer reports an impossible pixel depth — struct "
			       "layer_state has drifted between the writer and this "
			       "build; they must be rebuilt together";
	}
	return 0;
}

/* WHERE THIS LAYER GOES ON THE PANEL, AND HOW MUCH OF IT IS PICTURE.
 *
 * The MLC reads win_w x win_h pixels from the layer's base address, one panel
 * pitch per row, and composites them at (win_x, win_y) — so the layer's buffer
 * is NOT a panel-sized image, and (win_x, win_y) exists only here.
 *
 * Anything that does not describe a rectangle inside the panel means the guest
 * has not told us a window, and the layer IS the panel — which is the Flash
 * UI's normal state and by far the common case.
 *
 * Returns 1 when the guest announced a usable window, 0 when this is the
 * fallback. The compositor treats both the same; the rasteriser only adopts a
 * new viewport on 1, because "no window yet" must not resize a title mid-frame.
 */
static inline int tad_layer_window(const struct layer_state *ls, int panel_w, int panel_h,
                            int *wx, int *wy, int *ww, int *wh)
{
	int x = (int)ls->win_x, y = (int)ls->win_y;
	int cw = (int)ls->win_w, ch = (int)ls->win_h;
	int announced = 1;

	if (cw <= 0 || ch <= 0 || x < 0 || y < 0 ||
	    x + cw > panel_w || y + ch > panel_h) {
		x = 0; y = 0; cw = panel_w; ch = panel_h;
		announced = 0;
	}
	*wx = x; *wy = y; *ww = cw; *wh = ch;
	return announced;
}

#endif /* TADPOLE_STATE_H */
