/* Tadpole — the on-screen D-pad and Home button.
 *
 * WHY THIS EXISTS. The LeapPad2 has physical buttons either side of its
 * screen, and a desktop has a keyboard standing in for them (see map_key in
 * tadpole_view.c). A touchscreen has neither, so on a tablet — or on a desktop
 * being driven by a finger, or by someone who simply does not want to hold a
 * keyboard to play — the D-pad and the Home button are unreachable and the
 * emulator is half usable. These draw them onto the picture instead.
 *
 * WHY ONLY THOSE TWO. The device's A and B are not here on purpose. LeapPad2
 * titles that need them run under LeapFrog's own Leapster emulator, which
 * draws its own A and B onto the touchscreen — so the guest already provides
 * them, and a second pair of ours would sit on top of the real ones. What the
 * guest cannot provide is the hardware D-pad and the Home key, because those
 * are wired to GPIO and not to the panel.
 *
 * COORDINATE SPACE. Logical, the same space the chrome uses and the same one
 * SDL rewrites mouse events into — see the note at the top of tadpole_ui.h.
 * The controls are placed relative to the guest's picture rather than to the
 * window, so they stay on the picture when it is letterboxed or rotated. That
 * also means "up" here is up as SEEN, and the rotation from a seen direction
 * to the keycode the guest expects is rotate_dpad()'s job, exactly as it is
 * for the arrow keys.
 *
 * POINTERS, PLURAL. A finger holding a direction while another taps the
 * guest's own on-screen buttons is the ordinary way to play, so this tracks
 * several pointers at once and each one owns whatever it pressed for as long
 * as it is down. See the note above pad_event().
 */
#ifndef TADPOLE_PAD_H
#define TADPOLE_PAD_H

#include <SDL.h>

/* The four directions come FIRST and in clockwise order from up, because that
 * is the order rotate_dpad() indexes: the caller passes one of these straight
 * to it and gets back the keycode for the guest. Changing the order here
 * silently rotates the D-pad. */
enum {
	PAD_UP = 0, PAD_RIGHT, PAD_DOWN, PAD_LEFT,
	PAD_HOME,
	PAD_N
};

#define PAD_BIT(b)     (1u << (b))
#define PAD_DPAD_MASK  0x0Fu

void pad_init(void);

/* Where the controls go: `screen` is the guest's picture inside the logical
 * space, and the controls sit on it. Cheap, and called every frame — the
 * picture moves when the window is resized, when the orientation changes, and
 * when a title draws at a different size. */
void pad_place(const SDL_Rect *screen);

/* Feed EVERY event through this before the guest sees it; returns 1 when the
 * overlay took it and it must go no further.
 *
 * WHICH EVENTS, AND WHY BOTH KINDS. Mouse events are handled because a mouse
 * click is the desktop way in, and because SDL synthesises them from the FIRST
 * finger of a touch — so single-touch arrives here as a mouse and needs no
 * special case. Real SDL_FINGER* events are handled only for the fingers that
 * are NOT that first one, which is what makes a second finger work without
 * counting the first one twice. `ren` converts a finger's normalised window
 * position into the logical space; mouse events are already in it.
 */
int pad_event(const SDL_Event *e, SDL_Renderer *ren);

/* 1 when the pad is holding this finger, or when it is the finger SDL mirrors
 * as the mouse — in both cases the guest's touchscreen must leave it alone.
 * See the long note above the definition. */
int pad_owns_finger(SDL_FingerID fid);

/* Normalised finger coordinates -> logical, the space mouse events arrive in. */
void pad_finger_logical(SDL_Renderer *ren, const SDL_TouchFingerEvent *t,
                        int *lx, int *ly);

/* Which buttons are held now, and which bits have changed since the last call
 * to pad_take_changed() — that one clears as it reports, so the caller sends
 * one key event per real transition and no more. Read the state AFTER taking
 * the changes to know which way each one went. */
unsigned pad_state(void);
unsigned pad_take_changed(void);

/* Let go of everything, reporting the releases through pad_take_changed().
 * For losing window focus, and for a guest that has just stopped: a direction
 * still held when the guest goes away is one the next guest inherits. */
void pad_release_all(void);

void pad_draw(SDL_Renderer *r);

/* Drawn and hit-tested only while there is a guest to send the keys to. The
 * idle screen has no use for a D-pad, and controls that do nothing are worse
 * than no controls. */
void pad_set_visible(int on);
int  pad_visible(void);

/* Settings, read from struct ui_settings at init and whenever the Controller
 * dialog changes them. Kept as a call rather than a header dependency so this
 * file does not have to know what a ui_settings is. */
void pad_configure(int on, int size_pct, int opacity_pct, int left_handed);

/* The hit rectangles, for --selftest-pad. Testing this by eye is how a control
 * that draws in the right place and answers in the wrong one gets shipped.
 * `which` is one of the button numbers above, or PAD_N for the D-pad's whole
 * square footprint. */
int pad_debug_rect(int which, SDL_Rect *out);

/* Hold these down without anyone touching anything, so the pressed look can be
 * captured by --ui-shot and the key path exercised by --selftest-pad. The next
 * real event replaces it. */
void pad_debug_press(unsigned mask);

#endif /* TADPOLE_PAD_H */
