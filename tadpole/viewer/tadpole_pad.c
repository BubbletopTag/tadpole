/* Tadpole — the on-screen D-pad and Home button. See tadpole_pad.h for what
 * this is for and why it is only those two controls.
 *
 * SHAPES AND COLOURS ARE BORROWED, NOT REINVENTED. Everything drawn here goes
 * through the handful of routines tadpole_ui.c lends out, so the controls are
 * made of the same rounded, feathered, palette-driven material as the menu bar
 * and the dialogs, and TADPOLE_THEME=green repaints them along with everything
 * else. The only geometry of its own in this file is the triangle, which the
 * chrome had no use for and an arrow needs.
 */
#include "tadpole_pad.h"
#include "tadpole_ui.h"

#include <string.h>

/* ---- configuration ------------------------------------------------------- */

static int g_on = 1;             /* the setting */
static int g_visible;            /* ...and whether there is a guest to send to */
static int g_size = 100;         /* percent */
static int g_opacity = 70;       /* percent */
static int g_left = 1;           /* D-pad on the left */

/* ---- layout --------------------------------------------------------------
 *
 * All of it recomputed from the picture rectangle every frame. It is a dozen
 * integer divisions and it means there is exactly one description of where a
 * control is — the alternative, caching it and invalidating on resize, has one
 * more state to get wrong than this whole file is worth.
 */
static SDL_Rect g_screen;                /* the guest's picture */
static SDL_Rect g_plate;                 /* the D-pad's square footprint */
static SDL_Rect g_arm[4];                /* UP RIGHT DOWN LEFT, as drawn */
static SDL_Rect g_home;                  /* the Home button's square */
static int g_unit;                       /* the D-pad's side */

/* How far outside a control still counts as pressing it. A thumb is wider than
 * the thing it is aiming at and lands low as often as not; without this the
 * bottom row of pixels of a D-pad sitting near the bottom of the picture is
 * the easiest place in the world to miss. */
#define SLOP_NUM  1
#define SLOP_DEN  8

/* The middle of the D-pad, where no direction is meant. Without it a thumb
 * resting dead centre reports whichever way it is a pixel off towards, and
 * flickers between two of them as it settles. */
#define DEADZONE  0.20f

/* tan(67.5 degrees), which splits the pad into eight equal sectors: a
 * direction wins outright inside its own 45, and the two either side of a
 * corner are both pressed inside the 45 between them. Written as the constant
 * rather than computed, because this file does not link libm and a diagonal is
 * not a reason to start. */
#define SECTOR    2.4142136f

/* ---- pointers ------------------------------------------------------------
 *
 * A pointer that goes down on a control owns that control until it lifts, and
 * a pointer that goes down anywhere else is the guest's and is never looked at
 * again. That rule is what lets a finger slide from UP to UP+RIGHT without
 * letting go, and stops a drag that began on the picture from grabbing the
 * D-pad on its way past.
 */
#define PAD_PTRS 8

struct ptr {
	int          used;
	int          is_mouse;
	SDL_FingerID fid;
	unsigned     mask;
};
static struct ptr g_ptr[PAD_PTRS];

/* WHICH FINGER SDL IS ALREADY REPORTING AS THE MOUSE. SDL synthesises mouse
 * events from the first finger down, so that one arrives here twice — once as
 * SDL_FINGERDOWN and once as SDL_MOUSEBUTTONDOWN — and acting on both would
 * press a direction, then press it again from a second pointer, and leave it
 * held when only one of the two lifted. The mouse copy is the one this file
 * acts on, because it is also the one a real mouse sends; the finger copy is
 * dropped. Every OTHER finger produces no mouse events at all and is handled
 * here directly, which is the whole reason the finger path exists.
 *
 * If the synthesis is turned off (SDL_HINT_TOUCH_MOUSE_EVENTS=0) there is no
 * mouse copy to prefer and every finger is handled directly. */
static SDL_FingerID g_mouse_finger;
static int g_mouse_finger_down;
static int g_synth = 1;

static unsigned g_state;
static unsigned g_changed;

/* ---- idle fade -----------------------------------------------------------
 *
 * Controls painted over someone's game are in the way of it, and these are in
 * the way of it all the time — so they stand back when they are not being
 * used. Not all the way: a control that vanishes is one you have to remember
 * the position of, which is worse than one you can see through.
 */
static Uint32 g_last_use;
#define FADE_AFTER_MS  2500
#define FADE_OVER_MS   700
/* Set from a capture over a real title rather than over the idle screen —
 * Alphabet Stew's cover art puts a yellow chip and a bright green star under
 * exactly where the D-pad sits, and at the 38% this started as the left arm
 * was hard to pick out against them. */
#define FADE_TO_PCT    46

void pad_init(void)
{
	const char *h = SDL_GetHint(SDL_HINT_TOUCH_MOUSE_EVENTS);
	g_synth = !(h && h[0] == '0');
	memset(g_ptr, 0, sizeof(g_ptr));
	g_state = g_changed = 0;
	g_mouse_finger_down = 0;
	g_last_use = SDL_GetTicks();
}

void pad_configure(int on, int size_pct, int opacity_pct, int left_handed)
{
	g_on = on;
	g_size = size_pct > 0 ? size_pct : 100;
	g_opacity = opacity_pct > 0 ? opacity_pct : 70;
	g_left = left_handed;
	if (g_size < 50)  g_size = 50;
	if (g_size > 200) g_size = 200;
	if (g_opacity < 15)  g_opacity = 15;
	if (g_opacity > 100) g_opacity = 100;
	if (!g_on)
		pad_release_all();
}

void pad_set_visible(int on)
{
	if (!on && g_visible)
		pad_release_all();
	g_visible = on;
}

int pad_visible(void) { return g_on && g_visible; }

void pad_place(const SDL_Rect *screen)
{
	int m, marg, aw, al, dia, i;

	g_screen = *screen;
	m = screen->w < screen->h ? screen->w : screen->h;

	/* Three tenths of the short side is a control you can hit without
	 * looking and still see most of the game past. The setting scales it
	 * either side of that; the floor is there because a D-pad smaller than
	 * about forty logical pixels cannot be aimed at with a finger at any
	 * window size, and one that big on a tiny window is the lesser problem. */
	g_unit = m * 30 / 100 * g_size / 100;
	if (g_unit < 40) g_unit = 40;
	if (g_unit > m * 3 / 4) g_unit = m * 3 / 4;

	marg = g_unit / 6;
	if (marg < 4) marg = 4;

	g_plate.w = g_plate.h = g_unit;
	g_plate.x = g_left ? screen->x + marg
	                   : screen->x + screen->w - marg - g_unit;
	g_plate.y = screen->y + screen->h - marg - g_unit;

	aw = g_unit * 30 / 100;          /* arm width  */
	al = g_unit * 38 / 100;          /* arm length */

	g_arm[PAD_UP].x    = g_plate.x + (g_unit - aw) / 2;
	g_arm[PAD_UP].y    = g_plate.y;
	g_arm[PAD_UP].w    = aw;  g_arm[PAD_UP].h = al;

	g_arm[PAD_DOWN].x  = g_arm[PAD_UP].x;
	g_arm[PAD_DOWN].y  = g_plate.y + g_unit - al;
	g_arm[PAD_DOWN].w  = aw;  g_arm[PAD_DOWN].h = al;

	g_arm[PAD_LEFT].x  = g_plate.x;
	g_arm[PAD_LEFT].y  = g_plate.y + (g_unit - aw) / 2;
	g_arm[PAD_LEFT].w  = al;  g_arm[PAD_LEFT].h = aw;

	g_arm[PAD_RIGHT].x = g_plate.x + g_unit - al;
	g_arm[PAD_RIGHT].y = g_arm[PAD_LEFT].y;
	g_arm[PAD_RIGHT].w = al;  g_arm[PAD_RIGHT].h = aw;

	/* Home goes in the OTHER bottom corner, and is deliberately much smaller
	 * than the D-pad: it is pressed once in a while and never held, whereas
	 * the D-pad is held for minutes at a time. Sizing them alike would say
	 * they are used alike. */
	dia = g_unit * 42 / 100;
	if (dia < 24) dia = 24;
	g_home.w = g_home.h = dia;
	g_home.x = g_left ? screen->x + screen->w - marg - dia
	                  : screen->x + marg;
	g_home.y = screen->y + screen->h - marg - dia;

	for (i = 0; i < 4; i++)
		if (g_arm[i].w < 1) g_arm[i].w = 1;
}

/* FORCE BUTTONS DOWN, for --ui-shot and for --selftest-pad. The pressed look
 * is half of this control's design — a glow, an accent fill, an inverted arrow
 * — and without a way to ask for it, it is the half that can never be captured
 * or compared, because a screenshot has no finger in it. */
void pad_debug_press(unsigned mask)
{
	g_changed |= mask ^ g_state;
	g_state = mask;
}

int pad_debug_rect(int which, SDL_Rect *out)
{
	if (!pad_visible()) return 0;
	if (which >= 0 && which < 4)   { *out = g_arm[which]; return 1; }
	if (which == PAD_HOME)         { *out = g_home;      return 1; }
	if (which == PAD_N)            { *out = g_plate;     return 1; }
	return 0;
}

/* ---- hit testing --------------------------------------------------------- */

static int in_rect_slop(int x, int y, const SDL_Rect *r)
{
	int sx = r->w * SLOP_NUM / SLOP_DEN, sy = r->h * SLOP_NUM / SLOP_DEN;
	return x >= r->x - sx && x < r->x + r->w + sx &&
	       y >= r->y - sy && y < r->y + r->h + sy;
}

static unsigned hit(int x, int y)
{
	float dx, dy, ax, ay;
	unsigned m = 0;

	if (!pad_visible()) return 0;

	/* Home is tested first. The two never overlap, but it is the smaller
	 * target of the two and a tie should go to the one that is harder to
	 * hit. */
	if (in_rect_slop(x, y, &g_home)) {
		float hx = (float)(x - (g_home.x + g_home.w / 2));
		float hy = (float)(y - (g_home.y + g_home.h / 2));
		float rr = (float)(g_home.w / 2 + g_home.w * SLOP_NUM / SLOP_DEN);
		if (hx * hx + hy * hy <= rr * rr)
			return PAD_BIT(PAD_HOME);
	}

	if (!in_rect_slop(x, y, &g_plate)) return 0;

	dx = (float)(x - (g_plate.x + g_unit / 2)) / (float)(g_unit / 2);
	dy = (float)(y - (g_plate.y + g_unit / 2)) / (float)(g_unit / 2);
	ax = dx < 0 ? -dx : dx;
	ay = dy < 0 ? -dy : dy;

	if (ax * ax + ay * ay < DEADZONE * DEADZONE) return 0;

	if (ax > ay * SECTOR)
		m = PAD_BIT(dx > 0 ? PAD_RIGHT : PAD_LEFT);
	else if (ay > ax * SECTOR)
		m = PAD_BIT(dy > 0 ? PAD_DOWN : PAD_UP);
	else
		m = PAD_BIT(dx > 0 ? PAD_RIGHT : PAD_LEFT) |
		    PAD_BIT(dy > 0 ? PAD_DOWN  : PAD_UP);
	return m;
}

/* ---- pointer bookkeeping ------------------------------------------------- */

static void recompute(void)
{
	unsigned now = 0;
	int i;
	for (i = 0; i < PAD_PTRS; i++)
		if (g_ptr[i].used) now |= g_ptr[i].mask;
	g_changed |= now ^ g_state;
	g_state = now;
}

static struct ptr *find_mouse(void)
{
	int i;
	for (i = 0; i < PAD_PTRS; i++)
		if (g_ptr[i].used && g_ptr[i].is_mouse) return &g_ptr[i];
	return NULL;
}

static struct ptr *find_finger(SDL_FingerID id)
{
	int i;
	for (i = 0; i < PAD_PTRS; i++)
		if (g_ptr[i].used && !g_ptr[i].is_mouse && g_ptr[i].fid == id)
			return &g_ptr[i];
	return NULL;
}

static struct ptr *claim(int is_mouse, SDL_FingerID id, unsigned mask)
{
	int i;
	for (i = 0; i < PAD_PTRS; i++) {
		if (g_ptr[i].used) continue;
		g_ptr[i].used = 1;
		g_ptr[i].is_mouse = is_mouse;
		g_ptr[i].fid = id;
		g_ptr[i].mask = mask;
		return &g_ptr[i];
	}
	return NULL;
}

void pad_release_all(void)
{
	memset(g_ptr, 0, sizeof(g_ptr));
	recompute();
}

unsigned pad_state(void) { return g_state; }

unsigned pad_take_changed(void)
{
	unsigned c = g_changed;
	g_changed = 0;
	return c;
}

/* ---- events -------------------------------------------------------------- */

static void touched(void) { g_last_use = SDL_GetTicks(); }

/* A finger's position is normalised to the WINDOW and is NOT rewritten by
 * SDL's renderer event watch — unlike a mouse event's, which arrives already
 * in logical space (see the long note above event_to_fb in tadpole_view.c).
 * So this is the one place in the viewer where converting window coordinates
 * to logical ones is the right thing to do rather than the classic mistake of
 * doing it twice. */
static void finger_logical(SDL_Renderer *ren, const SDL_TouchFingerEvent *t,
                           int *lx, int *ly)
{
	int ow = 1, oh = 1;
	float fx, fy;
	SDL_GetRendererOutputSize(ren, &ow, &oh);
	SDL_RenderWindowToLogical(ren, (int)(t->x * ow), (int)(t->y * oh),
	                          &fx, &fy);
	*lx = (int)fx;
	*ly = (int)fy;
}

/* IS THIS FINGER ALREADY SPOKEN FOR? Asked by the guest's touchscreen path,
 * which has to know two different things and gets both from here.
 *
 * The finger SDL mirrors as the mouse is handled through its MOUSE copy
 * everywhere — by this file, and by the guest's touchscreen — so its finger
 * copy belongs to nobody. Acting on it as well would press the same point
 * twice and leave it held when only one of the two lifted.
 *
 * A finger this file has claimed for a direction is not a stylus touch, which
 * is the rule the `continue` after pad_event() already enforces for the
 * events pad_event() consumes. Motion is the case it does not cover: a finger
 * that came down on the D-pad and slid off is still ours, and pad_event()
 * says so by returning 1 — but a caller that wants to ask BEFORE dispatching
 * needs this. */
int pad_owns_finger(SDL_FingerID fid)
{
	int i;

	if (g_synth && g_mouse_finger_down && fid == g_mouse_finger)
		return 1;
	for (i = 0; i < PAD_PTRS; i++)
		if (g_ptr[i].used && !g_ptr[i].is_mouse && g_ptr[i].fid == fid)
			return 1;
	return 0;
}

/* Normalised finger coordinates to the logical ones every other pointer event
 * already arrives in. It lives here because this is where it was needed first
 * and where it is exercised; the guest's touchscreen needs exactly the same
 * conversion and must not grow a second copy of it that can drift. */
void pad_finger_logical(SDL_Renderer *ren, const SDL_TouchFingerEvent *t,
                        int *lx, int *ly)
{
	finger_logical(ren, t, lx, ly);
}

int pad_event(const SDL_Event *e, SDL_Renderer *ren)
{
	struct ptr *p;
	unsigned m;
	int x, y;

	if (!pad_visible()) return 0;

	switch (e->type) {
	case SDL_MOUSEBUTTONDOWN:
		if (e->button.button != SDL_BUTTON_LEFT) return 0;
		m = hit(e->button.x, e->button.y);
		if (!m) return 0;
		touched();
		claim(1, 0, m);
		recompute();
		return 1;

	case SDL_MOUSEMOTION:
		p = find_mouse();
		if (!p) {
			/* Not ours. Waking the controls up as the pointer crosses them
			 * still is: they have faded back, and the thing someone does
			 * immediately before pressing one is move towards it. */
			if (hit(e->motion.x, e->motion.y)) touched();
			return 0;
		}
		touched();
		/* Sliding OFF the controls releases, rather than keeping the last
		 * direction held for ever. Sliding from one direction to another
		 * simply reports the new one, which is how a thumb rolls around a
		 * real D-pad. */
		p->mask = hit(e->motion.x, e->motion.y);
		recompute();
		return 1;

	case SDL_MOUSEBUTTONUP:
		if (e->button.button != SDL_BUTTON_LEFT) return 0;
		p = find_mouse();
		if (!p) return 0;
		touched();
		p->used = 0;
		recompute();
		return 1;

	case SDL_FINGERDOWN:
		if (g_synth && !g_mouse_finger_down) {
			/* The one SDL is about to report as a mouse press. Remember it so
			 * its motion and its lift are dropped too, and let the mouse path
			 * do the work. */
			g_mouse_finger_down = 1;
			g_mouse_finger = e->tfinger.fingerId;
			return 0;
		}
		if (g_synth && e->tfinger.fingerId == g_mouse_finger) return 0;
		finger_logical(ren, &e->tfinger, &x, &y);
		m = hit(x, y);
		if (!m) return 0;
		touched();
		claim(0, e->tfinger.fingerId, m);
		recompute();
		return 1;

	case SDL_FINGERMOTION:
		if (g_synth && g_mouse_finger_down &&
		    e->tfinger.fingerId == g_mouse_finger) return 0;
		p = find_finger(e->tfinger.fingerId);
		if (!p) return 0;
		touched();
		finger_logical(ren, &e->tfinger, &x, &y);
		p->mask = hit(x, y);
		recompute();
		return 1;

	case SDL_FINGERUP:
		if (g_synth && g_mouse_finger_down &&
		    e->tfinger.fingerId == g_mouse_finger) {
			g_mouse_finger_down = 0;
			return 0;
		}
		p = find_finger(e->tfinger.fingerId);
		if (!p) return 0;
		touched();
		p->used = 0;
		recompute();
		return 1;

	case SDL_WINDOWEVENT:
		/* A direction held when the window loses focus is one that stays held
		 * for ever, because the release lands somewhere else. */
		if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
		    e->window.event == SDL_WINDOWEVENT_LEAVE)
			pad_release_all();
		return 0;
	}
	return 0;
}

/* ---- drawing ------------------------------------------------------------- */

static void tri(SDL_Renderer *r, float x0, float y0, float x1, float y1,
                float x2, float y2, unsigned col, Uint8 a)
{
	SDL_Vertex v[3];
	SDL_Color c;
	c.r = (Uint8)(col >> 16); c.g = (Uint8)(col >> 8); c.b = (Uint8)col; c.a = a;
	memset(v, 0, sizeof(v));
	v[0].position.x = x0; v[0].position.y = y0; v[0].color = c;
	v[1].position.x = x1; v[1].position.y = y1; v[1].color = c;
	v[2].position.x = x2; v[2].position.y = y2; v[2].color = c;
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

static Uint8 scale_a(int base, int pct)
{
	int v = base * pct / 100;
	if (v < 0) v = 0;
	if (v > 255) v = 255;
	return (Uint8)v;
}

/* Full strength while in use, standing back a couple of seconds after. */
static int fade_pct(void)
{
	Uint32 dt = SDL_GetTicks() - g_last_use;
	if (g_state) return 100;
	if (dt <= FADE_AFTER_MS) return 100;
	dt -= FADE_AFTER_MS;
	if (dt >= FADE_OVER_MS) return FADE_TO_PCT;
	return 100 - (100 - FADE_TO_PCT) * (int)dt / FADE_OVER_MS;
}

static void draw_arm(SDL_Renderer *r, int i, const struct ui_colors *c, int pct)
{
	const SDL_Rect *a = &g_arm[i];
	int down = (g_state & PAD_BIT(i)) != 0;
	float rad = (float)((a->w < a->h ? a->w : a->h)) * 0.34f;
	float cx = a->x + a->w / 2.0f, cy = a->y + a->h / 2.0f;
	float t = (float)(a->w < a->h ? a->w : a->h) * 0.30f;   /* arrow half-width */
	unsigned body = down ? c->accent : c->panel_hi;
	Uint8 abody = down ? scale_a(235, pct) : scale_a(g_opacity * 255 / 100, pct);
	unsigned ink = down ? c->bg : c->text;

	if (down)
		ui_glow(r, (int)cx, (int)cy, (a->w > a->h ? a->w : a->h),
		        c->accent, scale_a(70, pct));

	ui_rr_grad(r, (float)a->x, (float)a->y, (float)a->w, (float)a->h, rad,
	           body, abody, body, (Uint8)(abody * 3 / 4));
	ui_rr_stroke(r, (float)a->x, (float)a->y, (float)a->w, (float)a->h, rad,
	             down ? c->accent : c->edge, scale_a(200, pct),
	             down ? c->accent : c->edge, scale_a(90, pct), 1.0f);

	/* The arrow points away from the middle of the pad, which is the one
	 * thing about a D-pad nobody should have to work out. */
	switch (i) {
	case PAD_UP:
		tri(r, cx, cy - t, cx - t, cy + t * 0.7f, cx + t, cy + t * 0.7f,
		    ink, scale_a(235, pct));
		break;
	case PAD_DOWN:
		tri(r, cx, cy + t, cx - t, cy - t * 0.7f, cx + t, cy - t * 0.7f,
		    ink, scale_a(235, pct));
		break;
	case PAD_LEFT:
		tri(r, cx - t, cy, cx + t * 0.7f, cy - t, cx + t * 0.7f, cy + t,
		    ink, scale_a(235, pct));
		break;
	case PAD_RIGHT:
		tri(r, cx + t, cy, cx - t * 0.7f, cy - t, cx - t * 0.7f, cy + t,
		    ink, scale_a(235, pct));
		break;
	}
}

static void draw_home(SDL_Renderer *r, const struct ui_colors *c, int pct)
{
	int down = (g_state & PAD_BIT(PAD_HOME)) != 0;
	float x = (float)g_home.x, y = (float)g_home.y;
	float w = (float)g_home.w, h = (float)g_home.h;
	float cx = x + w / 2, cy = y + h / 2;
	float s = w * 0.30f;                 /* half the house */
	unsigned body = down ? c->accent : c->panel_hi;
	Uint8 abody = down ? scale_a(235, pct) : scale_a(g_opacity * 255 / 100, pct);
	unsigned ink = down ? c->bg : c->text;
	Uint8 aink = scale_a(235, pct);

	if (down)
		ui_glow(r, (int)cx, (int)cy, (int)w, c->accent, scale_a(80, pct));

	/* A circle is a rounded rect whose corners have run out of straight to
	 * be, so the button needs no shape the chrome did not already have. */
	ui_rr_grad(r, x, y, w, h, w / 2, body, abody, body, (Uint8)(abody * 3 / 4));
	ui_rr_stroke(r, x, y, w, h, w / 2,
	             down ? c->accent : c->edge, scale_a(200, pct),
	             down ? c->accent : c->edge, scale_a(90, pct), 1.0f);

	/* A house: roof, then walls. Not the word HOME — at this size the font
	 * would be four glyphs across a button the width of a fingertip, and a
	 * shape is read faster than a word anyway. */
	tri(r, cx, cy - s, cx - s, cy - s * 0.05f, cx + s, cy - s * 0.05f, ink, aink);
	ui_rr_fill(r, cx - s * 0.62f, cy - s * 0.05f, s * 1.24f, s * 0.95f,
	           s * 0.16f, ink, aink);
	/* The doorway, punched back out in the body colour. */
	ui_rr_fill(r, cx - s * 0.20f, cy + s * 0.30f, s * 0.40f, s * 0.60f,
	           s * 0.08f, body, abody > 200 ? (Uint8)255 : (Uint8)(abody + 55));
}

void pad_draw(SDL_Renderer *r)
{
	struct ui_colors c;
	int pct, i;

	if (!pad_visible()) return;
	ui_colors(&c);
	pct = fade_pct();

	/* The plate under the arms. It is what makes four keys read as one
	 * control, and it gives the arms something to be lit against on a bright
	 * picture — an arm alone over white box art is a pale shape on a pale
	 * ground.
	 *
	 * FAINTER THAN THE ARMS ON PURPOSE. It is the part of this that covers the
	 * most game and does the least: it is not a target, it only groups the
	 * four that are. Measured against TADPOLE_SHOT_BG=FFFFFF, which is the
	 * worst thing it will ever sit on. */
	ui_rr_grad(r, (float)g_plate.x, (float)g_plate.y,
	           (float)g_plate.w, (float)g_plate.h, g_unit * 0.30f,
	           c.panel, scale_a(g_opacity * 95 / 100, pct),
	           c.shadow, scale_a(g_opacity * 135 / 100, pct));
	ui_rr_stroke(r, (float)g_plate.x, (float)g_plate.y,
	             (float)g_plate.w, (float)g_plate.h, g_unit * 0.30f,
	             c.edge, scale_a(110, pct), c.edge, scale_a(40, pct), 1.0f);

	for (i = 0; i < 4; i++)
		draw_arm(r, i, &c, pct);
	draw_home(r, &c, pct);
}
