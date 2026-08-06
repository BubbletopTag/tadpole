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
 * and turned into environment variables when a guest is launched.
 *
 * DEBUG IS ONE DIAL, NOT FIVE SWITCHES. It used to be a pair of unrelated
 * checkboxes — "GL debug logging" and the shim's own --debug — which between
 * them could not express either thing anyone actually wants: silence, or
 * everything. The level means:
 *
 *   0  quiet     the guest's own output goes nowhere
 *   1  normal    AppManager's serial log, exactly what the device prints
 *   2  verbose   + the shim's file/audio tracing, + every GL stub and error
 *   3  trace     + every guest syscall (qemu -strace). Enormous, and the only
 *                thing that answers "did it even try to open that file"
 *
 * Anything that writes FILES rather than lines stays a separate switch, since
 * volume is not the question there.
 */
#define UI_GAMESDIR_MAX 512

struct ui_settings {
	int gl;                  /* TADPOLE_GL — the software GLES1 rasteriser */
	int gl_hle;              /* TADPOLE_GL_HLE — replay on the host GPU */
	int debug_level;         /* 0..3, see above */
	int log_to_file;         /* also write the guest's output to a log file */
	int gl_dumpframe;        /* TADPOLE_GL_DUMPFRAME */
	int gl_dumptex;          /* TADPOLE_GL_DUMPTEX */
	int rotate;              /* 0/90/180/270, clockwise */
	int scale;               /* window scale, 1..4 */
	int touch_debug;         /* TADPOLE_TOUCH_DEBUG */
	int audio_on;
	int audio_latency_ms;    /* cap on how far ahead of the speaker we run */
	int audio_pace;          /* TADPOLE_AUDIO_PACE — hold the guest to realtime */
	int frame_cap;           /* TADPOLE_HZ; 0 = uncapped */
	int hle_strict;          /* TADPOLE_HLE_STRICT — die instead of falling back */
	int msaa;                /* host-GPU anti-aliasing: 0 off, or 2/4/8 samples */
	int io_delay_us;         /* TADPOLE_IO_DELAY_US — pretend to be NAND */
	int tslib;               /* TADPOLE_TSLIB — the device's own touch library */
	int boot_on_start;       /* run the system menu as soon as Tadpole opens */
	char games_dir[UI_GAMESDIR_MAX];   /* the folder the library was read from */
};

enum ui_action {
	UI_ACT_NONE = 0,
	UI_ACT_RUN_UI,           /* boot AppManager — the system menu */
	UI_ACT_RUN_SWF,          /* path filled in */
	UI_ACT_INSTALL_PKG,      /* path filled in */
	UI_ACT_SETUP_FIRMWARE,   /* path filled in */
	UI_ACT_ERASE_FW,         /* wipe the installed system files */
	UI_ACT_BUILD_SYSROOT,    /* regenerate runtime/sysroot from the rootfs */
	UI_ACT_SCAN_GAMES,       /* path = folder of .tar backups to read */
	UI_ACT_INSTALL_GAMES,    /* path = file listing the archives to install */
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

/* Re-read the game index written by tools/scan-games.sh, and re-test which
 * titles are installed. Call after a scan or an install finishes. */
void  ui_games_reload(void);

/* Told by the viewer so the bar can show it and File can offer Stop. */
void  ui_set_running(int running);

/* Idle backdrop, drawn when no guest is running. */
void  ui_draw_idle(SDL_Renderer *ren, int lw, int lh);

/* Put the UI into a named state without synthetic input — see the note in
 * tadpole_ui.c. Used by --ui-shot. */
void  ui_debug_state(const char *spec);

#endif /* TADPOLE_UI_H */
