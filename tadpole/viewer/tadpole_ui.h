/* Tadpole — pixel-art UI chrome for the viewer.
 *
 * The viewer used to be a bare framebuffer window driven entirely from
 * tadpole.sh's flags. This adds the menu bar, dialogs and file browser that
 * make it a normal emulator front end, so the shell command is no longer the
 * only way in.
 *
 * COORDINATE SPACE. Everything here is drawn in the renderer's LOGICAL space,
 * the same one SDL rewrites mouse events into. The viewer sets that space to
 *
 *     width  = the panel width  (rotated)
 *     height = UI_BAR_H + the panel height (rotated)
 *
 * so the bar sits above the guest's picture instead of over it, and the whole
 * interface scales as pixel art with the window. A click at logical y < UI_BAR_H
 * belongs to the chrome; anything lower is the guest's, shifted up by UI_BAR_H.
 * Keeping the bar inside the logical space is what lets event_to_fb() — which
 * took a long time to get right — stay exactly as it was.
 */
#ifndef TADPOLE_UI_H
#define TADPOLE_UI_H

#include <SDL.h>
#include <stddef.h>

#define UI_BAR_H 13          /* logical pixels; 7px font + padding */

/* Everything the front end can change. Persisted to ~/.config/tadpole/ui.cfg
 * and turned into environment variables when a guest is launched. */
struct ui_settings {
	int gl;                  /* TADPOLE_GL — the software GLES1 rasteriser */
	int gl_hle;              /* TADPOLE_GL_HLE — replay on the host GPU */
	int gl_debug;            /* TADPOLE_GL_DEBUG */
	int gl_dumpframe;        /* TADPOLE_GL_DUMPFRAME */
	int gl_dumptex;          /* TADPOLE_GL_DUMPTEX */
	int shim_debug;          /* tadpole.sh --debug */
	int rotate;              /* 0/90/180/270, clockwise */
	int scale;               /* window scale, 1..4 */
	int touch_debug;         /* TADPOLE_TOUCH_DEBUG */
	int audio_on;
	int audio_latency_ms;    /* cap on how far ahead of the speaker we run */
};

enum ui_action {
	UI_ACT_NONE = 0,
	UI_ACT_RUN_UI,           /* boot AppManager — the system menu */
	UI_ACT_RUN_SWF,          /* path filled in */
	UI_ACT_INSTALL_PKG,      /* path filled in */
	UI_ACT_SETUP_FIRMWARE,   /* path filled in */
	UI_ACT_ERASE_FW,         /* wipe the installed system files */
	UI_ACT_BUILD_SYSROOT,    /* regenerate runtime/sysroot from the rootfs */
	UI_ACT_STOP,
	UI_ACT_QUIT,
	UI_ACT_RELAYOUT          /* rotate/scale changed; viewer must resize */
};

void  ui_preload_settings(void);
void  ui_init(SDL_Renderer *ren, const char *project_dir);
SDL_Surface *ui_icon_surface(void);
void  ui_shutdown(void);

/* Returns 1 if the event belonged to the chrome and must not reach the guest. */
int   ui_event(const SDL_Event *e, int lw, int lh);

void  ui_draw(SDL_Renderer *ren, int lw, int lh);

/* Pops the pending action, if any. `path` receives the argument when the
 * action carries one. */
enum ui_action ui_take_action(char *path, size_t pathsz);

struct ui_settings *ui_cfg(void);
void  ui_cfg_save(void);

/* A modal is up: the guest must not receive input, and the emulator can pause. */
int   ui_modal(void);

/* ---- progress ----------------------------------------------------------
 *
 * Installing firmware takes a minute or more: unpacking a zip, scanning 70
 * packages, extracting a 53 MB UBIFS volume. Running that silently in the
 * background made the wizard simply vanish, with no way to tell whether it was
 * working, finished, or had failed — the caller feeds the tool's output here
 * instead, and the modal stays up until the user dismisses it.
 */
void ui_progress_begin(const char *title);
void ui_progress_line(const char *line);
void ui_progress_done(int ok);
int  ui_progress_active(void);

/* One-line status shown at the right of the bar. */
void  ui_status(const char *fmt, ...);

/* Re-test whether the system files exist. Call after anything that could
 * have installed or removed them. */
void  ui_invalidate_prereqs(void);

/* Told by the viewer so the bar can show it and File can offer Stop. */
void  ui_set_running(int running);

/* Idle backdrop, drawn when no guest is running. */
void  ui_draw_idle(SDL_Renderer *ren, int lw, int lh);

/* Put the UI into a named state without synthetic input — see the note in
 * tadpole_ui.c. Used by --ui-shot. */
void  ui_debug_state(const char *spec);

#endif /* TADPOLE_UI_H */
