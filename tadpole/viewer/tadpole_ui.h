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
#define UI_DEVICE_MAX    32
/* How many device profiles the wizard will list. runtime/devices/ holds one
 * .conf per supported tablet; this only bounds what fits on the page. */
#define UI_DEVICES_MAX    8

struct ui_settings {
	int gl;                  /* TADPOLE_GL — the software GLES1 rasteriser */
	int gl_hle;              /* TADPOLE_GL_HLE — replay on the host GPU */
	int debug_level;         /* 0..3, see above */
	int log_to_file;         /* also write the guest's output to a log file */
	int update_check;        /* look for a newer release at startup */
	int gl_dumpframe;        /* TADPOLE_GL_DUMPFRAME */
	int gl_dumptex;          /* TADPOLE_GL_DUMPTEX */
	int rotate;              /* 0/90/180/270, clockwise */
	/* Turn with the guest: portrait for the LeapPad UI, landscape for a
	 * title. `rotate` above then holds whatever is on screen NOW; what gets
	 * saved is the last orientation chosen by hand. See rotate_for_screen()
	 * in tadpole_view.c, which owns the policy. */
	int auto_rotate;
	int scale;               /* window scale, 1..4 */
	int touch_debug;         /* TADPOLE_TOUCH_DEBUG */
	int audio_on;
	int audio_latency_ms;    /* cap on how far ahead of the speaker we run */
	int audio_pace;          /* TADPOLE_AUDIO_PACE — hold the guest to realtime */
	int frame_cap;           /* TADPOLE_HZ; 0 = uncapped */
	int hle_strict;          /* TADPOLE_HLE_STRICT — die instead of falling back */
	int msaa;                /* host-GPU anti-aliasing: 0 off, or 2/4/8 samples */
	int render_scale;        /* draw at N x the panel and filter down; 1 = off */
	int io_delay_us;         /* TADPOLE_IO_DELAY_US — pretend to be NAND */
	int tslib;               /* TADPOLE_TSLIB — the device's own touch library */
	int boot_on_start;       /* run the system menu as soon as Tadpole opens */
	/* Let the device nag you to connect to LeapFrog Connect. OFF: the service
	 * is gone, and it is a tap between the user and their games every boot.
	 * tadpole.sh writes the field Parent Settings writes — see the note there. */
	int connect_nag;
	/* Skip the logo and the startup animation. TICKED BY DEFAULT: the device's
	 * boot sequence is something to opt into, not something to sit through, and
	 * an emulator that made everyone watch four seconds of branding to reach a
	 * menu would have got that backwards. See viewer/tadpole_boot.c. */
	int fast_boot;
	char games_dir[UI_GAMESDIR_MAX];   /* the folder the library was read from */
	/* WHICH DEVICE TO EMULATE — a DEV_ID from a runtime/devices profile.
	 * Empty means "work it out from the installed firmware", which is what
	 * runtime/device.sh does by reading Firmware/meta.inf. This is an
	 * OVERRIDE, so leaving it empty is the right default: the firmware
	 * already knows what it is, and a stale setting here would boot a
	 * LeapPad2 rootfs with a LeapPad Ultra's screen geometry. */
	char device[UI_DEVICE_MAX];
};

/* The viewer's own PNG decoder: 8-bit RGBA or RGB, non-interlaced. Shared
 * because the boot logos are read by tadpole_boot.c and adding SDL2_image for
 * them would undo the point of having written it. `out_px` is optional and
 * hands the caller the pixels to own. */
SDL_Texture *ui_png_texture(SDL_Renderer *ren, const char *path,
                            int *out_w, int *out_h, Uint32 **out_px);

enum ui_action {
	UI_ACT_NONE = 0,
	UI_ACT_RUN_UI,           /* boot AppManager — the system menu */
	UI_ACT_RUN_SWF,          /* path filled in */
	UI_ACT_RUN_APP,          /* path = PackageID of an installed app */
	UI_ACT_INSTALL_PKG,      /* path filled in */
	UI_ACT_CONVERT_CART,     /* path = a raw cartridge dump to turn into .tar */
	UI_ACT_SETUP_FIRMWARE,   /* path filled in */
	UI_ACT_SETUP_DIDJ,       /* path = DIDJ.zip, the Didj compatibility files */
	UI_ACT_SETUP_DIDJ_OVERLAY, /* path = ControlOverlay.zip */
	UI_ACT_FETCH_DIDJ,       /* download the compatibility files, then install */
	UI_ACT_FETCH_DIDJ_OVERLAY, /* download the controller overlay */
	UI_ACT_ERASE_FW,         /* wipe the installed system files */
	UI_ACT_BUILD_SYSROOT,    /* regenerate runtime/sysroot from the rootfs */
	UI_ACT_ONLINE_UPDATE,    /* fetch the system files from LeapFrog */
	UI_ACT_MAKE_PROFILE,     /* create a player profile; fields via ui_profile_get */
	UI_ACT_SCAN_GAMES,       /* path = folder of .tar backups to read */
	UI_ACT_INSTALL_GAMES,    /* path = file listing the archives to install */
	UI_ACT_CHECK_UPDATE,     /* ask GitHub whether a newer release exists */
	UI_ACT_DO_UPDATE,        /* download it; path = where to write */
	/* path = a ProductID (0x........). Ask LeapFrog which micromods that
	 * title has; downloads them to the cache and lists them, installs
	 * nothing. See the note in tadpole_ui.c. */
	UI_ACT_MICROMODS_SCAN,
	/* path = a ProductID, ui_action_arg() = the ticked slots, comma
	 * separated. Installs exactly those. */
	UI_ACT_MICROMODS_INSTALL,
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

/* The second argument of the action just taken. Only the micromod install
 * sets it; read it together with the action. */
const char *ui_action_arg(void);

/* Re-read the Micromods screen after a scan or install changed what is on
 * disk, and the ticked slots as a comma-separated list. */
void  ui_micromods_reload(void);
int   ui_micromods_picked(char *out, size_t n);

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
/* A REAL percentage, when one is honestly knowable. The downloader knows the
 * byte total before it starts; the extractor does not, and keeps the
 * marquee. Pass -1 to go back to "still working". */
void ui_progress_pct(int pct);
int  ui_progress_active(void);

/* ---- update check ------------------------------------------------------
 *
 * The viewer runs tools/check-update.py and feeds its output here a line at a
 * time; this side parses and presents it. Keeping the parse in the UI rather
 * than the caller means the dialog owns its own state and there is one place
 * that knows the script's output format.
 *
 * `silent` marks the check that runs by itself at startup: it must never open
 * a dialog unless there is genuinely something newer, because an update check
 * that interrupts someone who is up to date — or offline — is a bug. A check
 * the user asked for always reports back, including "you are up to date".
 */
void ui_update_begin(int silent);
void ui_update_line(const char *line);
void ui_update_finish(void);
const char *ui_update_asset(void);
int  ui_update_pending(void);

/* One-line status shown at the right of the bar. */
void  ui_status(const char *fmt, ...);
/* A modal the user has to dismiss, raised from code rather than from a menu.
 * -> 0 if something else was already on screen and it was NOT shown, so the
 * caller can try again on a later frame instead of losing the message. */
int   ui_alert(const char *title, const char *body);

/* Re-test whether the system files exist. Call after anything that could
 * have installed or removed them. */
void  ui_invalidate_prereqs(void);

/* Re-read the game index written by tools/scan-games.sh, and re-test which
 * titles are installed. Call after a scan or an install finishes. */
void  ui_games_reload(void);

/* The profile the wizard is composing. The viewer turns these into arguments
 * for tools/make-profile.sh when UI_ACT_MAKE_PROFILE comes back. */
void  ui_profile_get(char *name, size_t namesz, int *grade,
                     char *picture, size_t picsz);

/* Told by the viewer so the bar can show it and File can offer Stop. */
void  ui_set_running(int running);

/* Idle backdrop, drawn when no guest is running. */
void  ui_draw_idle(SDL_Renderer *ren, int lw, int lh);

/* Put the UI into a named state without synthetic input — see the note in
 * tadpole_ui.c. Used by --ui-shot. */
void  ui_debug_state(const char *spec);

/* Render everything settled, with no entrance animations. A --ui-shot draws
 * ONE frame; without this it would catch whatever fraction of a panel's fade
 * that frame landed on, and every capture would differ from the last for no
 * reason at all. Call before drawing a shot; there is no way back. */
void  ui_anim_disable(void);

/* The app launcher's list position, for --selftest-apps. Reading it from
 * outside is the only way to check that a wheel or an arrow key actually
 * moved the list: "the code looks right" is how it came to ship not
 * scrolling in the first place. */
void  ui_debug_apps(int *n, int *top, int *sel, int *rows);
char  ui_debug_app_initial(int index);

/* WHICH PRODUCT THIS IS PRESENTING ITSELF AS, and what is running the guest.
 * They are two different questions and they used to be one: the chrome was
 * green for Tadpole-on-qemu and blue for Glasspole, so the colour was a claim
 * about the engine. Glasspole is the default engine now, on every platform, so
 * the colour says nothing about it — the chrome is blue either way and the
 * program is Tadpole except in the Windows installer's build, which ships
 * under the other name. ui_engine_name() is the honest answer about the
 * engine, and the About box is where it is given.
 *
 * Call ui_brand_apply() before drawing anything; it selects the palette
 * (TADPOLE_THEME=green for the original). */
int         ui_brand_is_glasspole(void);
const char *ui_brand_name(void);
const char *ui_engine_name(void);
void        ui_brand_apply(void);

#endif /* TADPOLE_UI_H */