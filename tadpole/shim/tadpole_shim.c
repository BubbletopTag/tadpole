/* Tadpole — LeapPad2 (NXP3200 / VALENCIA) emulator
 *
 * tadpole_shim.c — guest-side device shim.
 *
 * Cross-compiled for ARMv7 softfp and LD_PRELOADed into the guest under
 * qemu-user. It fakes the hardware AppManager expects:
 *
 *   /dev/fb0..2            three framebuffers (libDisplay.so opens all three)
 *   /dev/input/event0..4   five evdev nodes, matched BY NAME via EVIOCGNAME
 *
 * The pixel data path deliberately involves no interception at all: open()
 * hands back a descriptor onto a plain host file, so the guest's mmap() is a
 * real shared mapping of that file. The native viewer mmaps the same file and
 * both sides see the same pages. Only the control path (ioctl) is emulated.
 *
 * Input events arrive through FIFOs the viewer writes struct input_event into.
 *
 * We declare everything by hand rather than including kernel headers: this is
 * cross-compiled with the host's clang and no ARM sysroot, and struct
 * input_event in particular is NOT layout-compatible between 32-bit ARM
 * (32-bit time_t) and the x86-64 host.
 */

#define _GNU_SOURCE
#include <stdarg.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed int         s32;
typedef unsigned long      ulong;

/* ---- libc, declared by hand (no ARM sysroot at build time) -------------
 * Use the compiler's own __SIZE_TYPE__ so these match the builtin
 * declarations exactly for the target ABI. */
typedef __SIZE_TYPE__ size_t;

extern void *dlsym(void *handle, const char *symbol);
extern int   snprintf(char *s, size_t n, const char *fmt, ...);
extern int   strncmp(const char *a, const char *b, size_t n);
extern int   strcmp(const char *a, const char *b);
extern size_t strlen(const char *s);
extern void *memcpy(void *d, const void *s, size_t n);
extern void *memset(void *s, int c, size_t n);
extern char *getenv(const char *name);
extern int   mkdir(const char *path, u32 mode);
extern int   mkfifo(const char *path, u32 mode);
extern int   ftruncate(int fd, long length);
extern long  write(int fd, const void *buf, size_t n);
extern int   getpid(void);
extern int   fcntl(int fd, int cmd, ...);

/* For the vsync timebase. Declared by hand like the rest — no ARM sysroot at
 * build time. struct timespec on this 32-bit target is two longs. */
struct tad_timespec { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_timespec *tp);
extern int nanosleep(const struct tad_timespec *req, struct tad_timespec *rem);
#define CLOCK_MONOTONIC_ 1

/* tadpole_crash.c — catches SIGSEGV/BUS/ILL/FPE/ABRT and writes a report that
 * names the faulting library and offset, then re-raises so qemu still cores. */
extern void tad_crash_install(const char *dir,
                              int (*real_open)(const char *, int, ...));
/* Returns the guest's previous handler, or (void(*)(int))-1 for a signal the
 * crash reporter does not manage — in which case signal() falls through. */
extern void (*tad_crash_take_signal(int sig, void (*h)(int)))(int);
extern void (*tad_crash_take_sigaction(int sig, void (*h)(int), unsigned long flags))(int);
/* tadpole_v4l2.c: the LeapTV's camera, a USB device that is not there. */
extern int  tad_cam_match(const char *path);
extern int  tad_cam_open(const char *path, const char *dir,
                         int (*real_open)(const char *, int, ...),
                         void *(*real_mmap)(void *, size_t, int, int, int, long));
extern int  tad_cam_is(int fd);
extern int  tad_cam_ioctl(int fd, ulong req, void *arg);
extern void tad_cam_close(int fd);

#define RTLD_NEXT ((void *)-1L)
/* uClibc's value for "search the global scope, from the beginning" — which is
 * how the two-shims check below asks who actually owns a symbol. */
#define RTLD_DEFAULT ((void *)0)

#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR   02
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#define O_NONBLOCK 04000

#define F_DUPFD  0

/* ---- fb ioctls --------------------------------------------------------- */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
/* _IOW('F', 0x20, u32) */
#define FBIO_WAITFORVSYNC   0x40044620

/* LeapFrog extensions, from include/linux/lf1000/lf1000fb.h (magic 'm') */
#define LF1000FB_IOCSALPHA    0x40046D01
#define LF1000FB_IOCGALPHA    0x80046D02
#define LF1000FB_IOCSPOSTION  0x40046D03   /* [sic] typo is in the header */
#define LF1000FB_IOCGPOSTION  0x80046D04
#define LF1000FB_IOCSVIDSCALE 0x40046D05
#define LF1000FB_IOCGVIDSCALE 0x80046D06

/* evdev: match on dir+type+nr, ignore the size field baked into the cmd */
#define EV_MASK          0xC000FFFFu
#define EVIOCGVERSION_ID 0x80004501u
#define EVIOCGID_ID      0x80004502u
#define EVIOCGNAME_ID    0x80004506u
#define EVIOCGPHYS_ID    0x80004507u
#define EVIOCGBIT_BASE   0x80004520u   /* +ev type */

struct fb_bitfield { u32 offset, length, msb_right; };

struct fb_var_screeninfo {
	u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
	u32 bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	u32 nonstd, activate, height, width, accel_flags;
	u32 pixclock, left_margin, right_margin, upper_margin, lower_margin;
	u32 hsync_len, vsync_len, sync, vmode, rotate, colorspace;
	u32 reserved[4];
};

struct fb_fix_screeninfo {
	char  id[16];
	ulong smem_start;
	u32   smem_len;
	u32   type, type_aux, visual;
	u16   xpanstep, ypanstep, ywrapstep;
	u32   line_length;
	ulong mmio_start;
	u32   mmio_len;
	u32   accel;
	u16   capabilities;
	u16   reserved[2];
};

/* ---- shared state, mirrored to the viewer ------------------------------ */
#define TADPOLE_MAGIC   0x54414450u   /* "TADP" */
#define TADPOLE_VERSION 1
#define NUM_FB          3
#define NUM_EV          6

/* WHAT THE GUEST IS SHOWING. The panel is portrait and its software is not:
 * the LeapPad UI draws a quarter turn from how the device is held — the same
 * reason the stock boot art is named "...CW.png" — while nearly every title
 * draws landscape into the same buffer. So there is no one right rotation for
 * the window; it depends on what is on screen, and only the guest knows.
 * See screen_note() for how this is worked out and PKGID_MAX for the name. */
#define TAD_SCREEN_UNKNOWN 0
#define TAD_SCREEN_SYSTEM  1   /* the LeapPad UI — portrait */
#define TAD_SCREEN_TITLE   2   /* an installed title — landscape, nearly always */
#define PKGID_MAX          64

struct layer_state {
	u32 enabled, xres, yres, bpp, xoffset, yoffset;
	u32 nonstd;      /* format/priority/planar bits, see lf1000fb.h */
	u32 alpha, blank;
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
	u32 win_x, win_y, win_w, win_h;

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
	u32 vid_w, vid_h;
};

/* WHAT EACH INPUT NODE IS FOR, so the viewer can find the keyboard by
 * purpose instead of by a slot number it assumed. The slot numbers differ per
 * device family (see g_ev_lf2000 and g_ev_lf1000 below) and the viewer used
 * to hard-code the LeapPad2's: on the Didj that sent every keypress to the
 * Power Button node, and nothing responded. TAD_EV_SLOTS is fixed rather than
 * NUM_EV so the on-disk layout does not move if NUM_EV ever does. */
#define TAD_EV_NONE       0
#define TAD_EV_USB        1
#define TAD_EV_KEYS       2
#define TAD_EV_TOUCH      3
#define TAD_EV_TOUCH_RAW  4
#define TAD_EV_ACCEL      5
#define TAD_EV_POWER      6
#define TAD_EV_SLOTS      8

struct tadpole_state {
	u32 magic, version;
	u32 width, height;
	u32 vsync_count;
	struct layer_state layer[NUM_FB];

	/* APPENDED AT THE END ON PURPOSE. tools/fbshot.py reads the header and
	 * the layer array out of this same file by offset, so anything inserted
	 * above would silently shift every layer it decodes and the capture would
	 * come out of the wrong page. Grow this struct here, never in the middle.
	 */
	u32 screen;                 /* TAD_SCREEN_*: the UI, or a title */
	u32 screen_seq;             /* bumped on every change, so a viewer that
	                             * was not looking still sees the transition */
	char screen_pkg[PKGID_MAX]; /* the PackageID when a title is up */
	/* Which evdev node is which: TAD_EV_* per slot, TAD_EV_NONE past the
	 * device's count. Appended, so a reader built before it sees a longer
	 * file than it expects — which is fine, and every reader must treat it
	 * so (tadpole_gles_core.c, tadpole_view.c, tools/fbshot.py, tools/burst.py).
	 * A reader that demands an exact length turns this into the "Leapster
	 * title fills the whole panel" bug, which is the rasteriser refusing the
	 * file and falling back. */
	u32 ev_role[TAD_EV_SLOTS];
};

/* ---- geometry ---------------------------------------------------------- */
static u32 g_w   = 480;
static u32 g_h   = 272;
static u32 g_bpp = 32;

/* Brio allocates several buffers INSIDE one framebuffer and flips between
 * them with the pan ioctl — observed live:
 *     AllocBuffer: new buf offset 000FF000, length 0007F800
 * 0x7F800 is exactly one 480x272x4 screen and 0xFF000 is two screens in, so
 * a single-screen smem_len makes it allocate off the end. Advertise a tall
 * virtual display and size the backing file to match. */
#define NBUF 8

/* WHERE THE LF1000 PUTS ITS FRAMEBUFFERS IN PHYSICAL MEMORY.
 *
 * The Didj's libDisplay does not mmap a layer node. It asks the layer where it
 * lives, opens /dev/mem, and maps that physical address — which on the host is
 * root-only, so the open failed with EACCES and Brio reported
 *
 *     !ASSERT: [5] DisplayModule::InitModule: /dev/mem driver failed
 *
 * That is the same base the LeapPad line uses and the same one this shim
 * already reports in fill_fix(): the arena IS the guest's idea of video
 * memory. Brio treats it as one address space and allocates every layer inside
 * it — see the note in init() — so physical 0x82000000 is arena offset 0, and
 * no per-layer arithmetic is wanted or correct here. */
#define LF_VMEM_BASE 0x82000000u
/* The LF1000 maps 1 MB at 0x100000 and 4 MB at 0x400000 out of /dev/mem, so
 * the arena has to reach 8 MB for the second of those to be backed. Measured
 * from the guest's own mmap2 calls, not chosen. */
#define LF_VMEM_MIN  (8 * 1024 * 1024)

/* AND THE VIDEO LAYER MAPS AT THE DIDJ'S DRAM BASE, which is 0x20000000.
 *
 * libDisplay maps the 2D layer at the offset get_address gave it, plainly —
 * layer0 comes back at offset 0. The video/GL layer does not: it adds the
 * physical base of DRAM first, so with get_address answering 0 it asks for
 * offset 0x20000000 and gets a mapping 512 MB past the end of an 8 MB file.
 * Nothing fails at that point; the first store into it takes SIGBUS, four
 * seconds into a boot that had already drawn its copyright screen.
 *
 * Proved rather than assumed: making get_address answer 0x600000 for that
 * layer moved the request to 0x20600000, exactly 0x20000000 higher.
 *
 * So the arena reaches past that base, SPARSELY. It is modelling physical
 * memory and this is where the Didj's physical memory is; a hole costs nothing
 * until something writes into it, and only the video layer ever does. The
 * visible planes stay at the bottom, which is what keeps tools/fbshot.py and
 * the viewer reading a couple of megabytes rather than half a gigabyte. */
#define LF_VMEM_TOP  0x20100000u


static int  g_ready;
static int  g_debug;
static int  g_logfd = 2;     /* see TADPOLE_LOG in init() */
static char g_logpfx[256];   /* empty unless TADPOLE_LOG is set */
static char g_dir[256];
static long g_io_delay_us;   /* see io_pace() — artificial NAND latency */
static int  g_abs_panel;     /* TADPOLE_ABS_PANEL — see EVIOCGABS below */

/* qemu-user's -L only redirects paths that ALREADY EXIST in the sysroot.
 * Creating a new file therefore falls through to the host path and fails
 * with ENOENT even when the parent directory plainly exists:
 *     stat64("/LF/Bulk/Data/Uploads/0")   = 0
 *     open(".../profile.log", O_CREAT)    = -1 ENOENT
 * The guest can read the sysroot but never write into it. Since we already
 * intercept open(), we do the translation ourselves for creating opens. */
static char g_sysroot[256];

#define VSYNC_HZ_DEFAULT 60
static long g_vsync_ns;          /* period; 0 = uncapped */
static long g_vs_sec, g_vs_nsec; /* next deadline */

static struct tadpole_state *g_state;

/* fd -> device mapping. Small linear tables; opens are rare. */
#define MAXFD 4096
static signed char g_fb_of_fd[MAXFD];   /* -1 none, else layer index */
static signed char g_ev_of_fd[MAXFD];

/* AN EVDEV NODE IS A BROADCAST. A FIFO IS A QUEUE. THAT IS THE WHOLE BUG.
 *
 * Each evdev node here was a single FIFO, handed to the guest as-is. That is
 * right for one reader and silently wrong for two, because the kernel and a
 * FIFO differ exactly where it matters: open() on /dev/input/eventN gives you
 * your OWN event queue and every event is copied into all of them, while a
 * byte read from a FIFO is gone for everyone else.
 *
 * The touchscreen has three readers inside AppServer alone — Qt's tslib mouse
 * handler, and both of Brio's ButtonPowerUSBTask tslib loads, which watch for
 * activity to feed the idle timeout. Measured, on one tap:
 *
 *     93 fd=24        <- Brio
 *     71 fd=8         <- Qt
 *
 * Neither reader saw a whole gesture. Each got an arbitrary subsequence: a
 * BTN_TOUCH with no coordinates after it, coordinates with no SYN_REPORT to
 * commit them, a release belonging to a press it never saw. tslib's dejitter
 * and Qt's press/move/release state machine both need the run intact, so both
 * produced nothing at all, and nothing anywhere reported an error.
 *
 * This is why the Ultra's notes could say "the events reach the guest and Qt
 * does not act on them" and be entirely correct about both halves. The debug
 * line printed the DEVICE index, not the fd, so three readers splitting one
 * stream looked identical to one reader receiving it.
 *
 * The fix restores the kernel's semantics: the shared FIFO is opened ONCE per
 * device and never handed out; every open() gets a private pipe; and a pump
 * copies each complete event from the FIFO into every open pipe. Readers then
 * see identical, complete streams, as they would on hardware.
 */
#define EV_MAX_READERS  8               /* three today; eight is plenty */
static int g_ev_fifo[NUM_EV];           /* shared source, -1 until first open */
static int g_ev_rd[NUM_EV][EV_MAX_READERS];   /* what the guest reads from */
static int g_ev_wr[NUM_EV][EV_MAX_READERS];   /* what the pump writes into */
static volatile int g_ev_pumping;       /* try-lock; see ev_pump() */

#define ABS_X_BIT        (1u << 0)
#define ABS_Y_BIT        (1u << 1)
#define ABS_PRESSURE_BIT (1u << 24)

/* THE INPUT DEVICES ARE PER DEVICE FAMILY, and the guest matches on the name.
 *
 * Brio finds its keyboard by opening /dev/input/event0.. in turn and asking
 * EVIOCGNAME, then stops at the one it wants. Answer with another machine's
 * names and it never finds it: on the Didj that is
 *
 *     !ASSERT: [6] CEventModule::ButtonPowerUSBTask: reading switch state failed
 *
 * before the first frame. So the table is chosen at runtime from
 * TADPOLE_EVDEV, which the launcher sets from DEV_EVDEV in the device profile.
 * Unset means the LeapPad2/LF2000 set, which is what every device that worked
 * before this existed used. */
struct ev_device {
	const char *name;   /* EVIOCGNAME — what the guest matches on */
	const char *phys;   /* EVIOCGPHYS */
	u32 ev_bits;        /* EVIOCGBIT(0)      capability classes */
	u32 abs_bits;       /* EVIOCGBIT(EV_ABS) axes, word 0 */
	u32 role;           /* TAD_EV_*: published in state.bin for the viewer */
};

/* Exact names, order and phys strings from a live LeapPad2's
 * /proc/bus/input/devices — see reference/device-capture/. Do not "tidy"
 * these: AppManager matches on them, and the real kernel names are
 * "LF2000 USB" / "LF2000 Accelerometer", not the shorter forms that appear
 * in AppManager's log messages.
 *
 * The bitmaps come from the same capture. tslib's input-raw module
 * (usr/lib/ts/input.so) REQUIRES EVIOCGBIT(EV_ABS) to report ABS_X and ABS_Y,
 * or it prints "selected device is not a touchscreen I understand" and fails —
 * after which the caller dereferences the null handle and dies. Word 0 =
 * bits 0..31. */
static const struct ev_device g_ev_lf2000[] = {
	{ "LF2000 USB",            "lf2000/usb",             0x21, 0, TAD_EV_USB },
	{ "gpio-keys",             "gpio-keys/input0",       0x23, 0, TAD_EV_KEYS },
	{ "touchscreen interface", "lf2000/touchscreen",     0x0b,
	  ABS_X_BIT | ABS_Y_BIT | ABS_PRESSURE_BIT,                   TAD_EV_TOUCH },
	{ "touchscreen raw",       "lf2000/touchscreen-raw", 0x09, 0x7ff, TAD_EV_TOUCH_RAW },
	{ "LF2000 Accelerometer",  "lf2000/aclmtr",          0x0b, 0x107, TAD_EV_ACCEL },
	{ "Power Button",          "lf2000/power_button",    0x03, 0, TAD_EV_POWER },
};

/* THE DIDJ HAS THREE, read out of the device's own kernel rather than captured
 * from hardware nobody here has. kernel.bin in DIDJ-0x000E0003-000001.lfp is a
 * container with a gzip'd Linux 2.6.20.1 inside, and each driver's name and
 * phys strings sit adjacent in its rodata:
 *
 *     LF1000 Keyboard\0lf1000/input0\0
 *     Power Button\0lf1000/power_button\0
 *     LF1000 USB\0lf1000/usb\0
 *
 * which is the same shape, and very nearly the same set, as the LF2000 table
 * above — one generation of the same vendor's drivers apart.
 *
 * ONE WAS NOT ENOUGH, and the way it failed is worth keeping. With only the
 * keyboard here Brio found it, stopped asserting, and then span: its
 * ButtonPowerUSBTask polls THREE descriptors, and having filled only the first
 * it polled two uninitialised ones — which happened to hold 1, so it asked
 * about stdout, was told POLLIN every time, read it, got EBADF, and went round
 * again. Three hundred thousand iterations in twelve seconds, no error message
 * anywhere. A missing device does not announce itself; it corrupts the poll
 * set of whatever wanted it.
 *
 * THE ORDER IS OURS, and nothing depends on it: the guest opens event0,
 * event1, ... in turn and matches on the NAME, which is how it found the
 * keyboard here while the real device may well enumerate them differently.
 *
 * The capability bits are the one thing not read out of the image. They are
 * the LF2000 capture's values for the devices of the same name and purpose:
 * SYN|KEY for a keyboard and for a power button, SYN|SW for the USB cable,
 * which reports insertion as a switch. No absolute axes anywhere — the Didj
 * has no touchscreen and runtime/devices/didj.conf records that. If a Didj
 * capture ever turns up, this is the line to check. */
static const struct ev_device g_ev_lf1000[] = {
	{ "LF1000 Keyboard", "lf1000/input0",       0x03, 0, TAD_EV_KEYS },
	{ "Power Button",    "lf1000/power_button", 0x03, 0, TAD_EV_POWER },
	{ "LF1000 USB",      "lf1000/usb",          0x21, 0, TAD_EV_USB },
};

/* Selected by init() from TADPOLE_EVDEV; g_ev_count is how many of the
 * NUM_EV slots are real on this device. */
/* What get_address answers. Zero — an offset into the arena, not a physical
 * address — and settable with TADPOLE_MLC25 only because pinning this down
 * took a sweep and the next device may want a different one. */
static u32 g_mlc_q25;
static u32 g_mlc_dflt = 1;  /* answer for an unrecognised _IO('m',n) query */

static const struct ev_device *g_ev = g_ev_lf2000;
static int g_ev_count = (int)(sizeof(g_ev_lf2000) / sizeof(g_ev_lf2000[0]));

/* real libc entry points */
static int  (*real_pipe)(int *);
static long (*real_write)(int, const void *, size_t);
static int  (*real_fcntl)(int, int, ...);
static int  (*real_poll)(void *, ulong, int);
static int  (*real_pthread_create)(ulong *, const void *,
                                   void *(*)(void *), void *);
static int  (*real_open)(const char *, int, ...);
static int  (*real_open64)(const char *, int, ...);
static int  (*real_openat)(int, const char *, int, ...);
static int  (*real_ioctl)(int, ulong, ...);
static int  (*real_close)(int);
static void *(*real_mmap)(void *, size_t, int, int, int, long);
static int  (*real_mkstemp)(char *);
static int  (*real_mkstemp64)(char *);
static int  (*real_mkstemps)(char *, int);
static int  (*real_rename)(const char *, const char *);
static int  (*real_unlink)(const char *);
static int  (*real_chdir)(const char *);
static int  (*real_mkdir)(const char *, u32);
static void (*(*real_signal)(int, void (*)(int)))(int);
static int  (*real_stat)(const char *, void *);
static int  (*real_stat64)(const char *, void *);
static int  (*real_lstat)(const char *, void *);
static int  (*real_access)(const char *, int);
static long (*real_read)(int, void *, size_t);
static void *(*real_fopen)(const char *, const char *);
static void *(*real_dlopen)(const char *, int);
static char *(*real_dlerror)(void);
static void *(*real_fopen64)(const char *, const char *);
static int  (*real_execve)(const char *, char *const [], char *const []);
static int  (*real_sigaction)(int, const void *, void *);

/* Open <TADPOLE_LOG>.<pid>.log and park it on a high fd.
 *
 * Named for the CURRENT pid, so a process that forks gets its own file from
 * the moment it next logs — which is what you want when the fork is a daemon
 * and the parent exits immediately. */
static void log_open(void)
{
	char lp[320];
	int fd, hi;

	g_logfd = -1;
	if (!g_logpfx[0] || !real_open)
		return;
	snprintf(lp, sizeof(lp), "%s.%d.log", g_logpfx, getpid());
	fd = real_open(lp, O_RDWR | O_CREAT | O_APPEND, 0666);
	if (fd < 0)
		return;
	/* Above 100 so an ordinary dup2 onto 0/1/2 cannot land on it. */
	hi = fcntl(fd, F_DUPFD, 100);
	if (hi >= 0) { real_close(fd); fd = hi; }
	g_logfd = fd;
}

/* SAY IT WHATEVER THE DEBUG LEVEL IS.
 *
 * dbg() is for the running commentary and is off unless somebody asked for it.
 * This is for the handful of failures that make the emulator useless and are
 * INVISIBLE from every other vantage point — the arena not being creatable
 * being the one that prompted it. A user does not turn debugging on before the
 * thing goes wrong, and the whole cost of the bug below was that nothing,
 * anywhere, named the cause.
 *
 * Deliberately not routed through log_open()'s per-pid file: this has to
 * arrive on the guest's stderr, which is what the viewer pumps into
 * tadpole.log and what a user pastes into a report. */
static void note(const char *msg)
{
	size_t n = strlen(msg);
	if (g_logfd >= 0)
		write(g_logfd, msg, n);
	else
		write(2, msg, n);
}

static void dbg(const char *msg)
{
	size_t n;

	if (!g_debug || g_logfd < 0)
		return;
	n = strlen(msg);
	if (write(g_logfd, msg, n) >= 0)
		return;

	/* THE FD IS GONE, AND THAT IS NORMAL FOR A DAEMON.
	 *
	 * VideoDaemon daemonizes the textbook way: fork, setsid, then close every
	 * descriptor up to RLIMIT_NOFILE and reopen 0/1/2 on /dev/null. No fd
	 * survives that, however high we parked it — which is exactly why the
	 * shim's account of the one process we most needed to watch went missing
	 * the moment it started doing real work.
	 *
	 * Reopen and retry once. O_APPEND, so if a parent and child do end up
	 * sharing a file their writes still interleave whole lines rather than
	 * overwriting each other.
	 *
	 * CAVEAT: between the close-all and this reopen, the guest could in
	 * principle have opened enough files to be handed our old number back,
	 * and the failed write above would then have gone to it instead. It takes
	 * >100 open fds in a daemon that has just closed all of them, and this
	 * whole path only exists under TADPOLE_LOG, so the trade is worth it —
	 * but do not promote this to always-on without solving that. */
	log_open();
	if (g_logfd >= 0)
		write(g_logfd, msg, n);
}

/* ---- the real dlopen, found WITHOUT RTLD_NEXT --------------------------
 *
 * RTLD_NEXT IS NOT "THE NEXT DEFINITION". uClibc's do_dlsym walks the symbol
 * table chain from the entry after the caller and searches each later
 * module's scope in turn — so whether a libdl is ever reached depends on the
 * EXECUTABLE's link order, not on ours. The LeapTV's GlasgowUI names us at
 * the first level as libGLESv2.so and names no libdl at all; every module
 * after us in its chain has a scope without one, the lookup returns NULL, and
 * the loader's "Unable to resolve symbol" is left behind for the next
 * dlerror() to report — which made every Brio module load look like a
 * missing symbol. (The probe that proved it, linked libEGL.so then
 * libdl.so.0, worked perfectly: there libdl WAS the next entry.)
 *
 * So ask the loader for its object list instead, find the libdl it mapped —
 * ours (libdl.so.9) or the device's, whichever comes first — and read the
 * symbol out of its own dynamic symbol table. That is what dlsym does
 * underneath, minus the scope rules that were the problem. uClibc's libdl has
 * only a GNU hash table, so the symbol count comes from walking its chains.
 */
struct tad_phdr_info {
	u32         addr;
	const char *name;
	const struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } *phdr;
	u16         phnum;
};
extern int dl_iterate_phdr(int (*cb)(struct tad_phdr_info *, size_t, void *), void *data);

struct tad_dlsym_req { const char *lib; const char *name; void *found; };

static int libdl_cb(struct tad_phdr_info *info, size_t size, void *data)
{
	struct tad_dlsym_req *req = data;
	const char *base, *nm = info->name;
	const char *strtab = 0;
	const u8 *symtab = 0;
	const u32 *gnuhash = 0, *sysvhash = 0;
	u32 i, nsyms = 0;
	(void)size;
	if (!nm) return 0;
	base = nm;
	for (i = 0; nm[i]; i++) if (nm[i] == '/') base = nm + i + 1;
	/* "libdl" matches libdl.so.9 and libdl-0.9.33.1-git.so, not libdlfoo. */
	{
		const char *want = req->lib;
		u32 k = 0;
		while (want[k] && base[k] == want[k]) k++;
		if (want[k] || !(base[k] == '.' || base[k] == '-')) return 0;
	}
	for (i = 0; i < info->phnum; i++) {
		const s32 *dyn;
		if (info->phdr[i].type != 2) continue;               /* PT_DYNAMIC */
		dyn = (const s32 *)(info->addr + info->phdr[i].vaddr);
		for (; dyn[0] != 0; dyn += 2) {
			u32 v = (u32)dyn[1];
			/* Entries stay as the file wrote them here: a small vaddr, which
			 * wants the load base adding. */
			if (dyn[0] == 5 || dyn[0] == 6 || dyn[0] == 4 || dyn[0] == (s32)0x6ffffef5)
				if (v < info->addr) v += info->addr;
			if (dyn[0] == 5) strtab = (const char *)v;
			else if (dyn[0] == 6) symtab = (const u8 *)v;
			else if (dyn[0] == 4) sysvhash = (const u32 *)v;
			else if (dyn[0] == (s32)0x6ffffef5) gnuhash = (const u32 *)v;
		}
	}
	if (!strtab || !symtab) return 0;
	if (sysvhash) {
		nsyms = sysvhash[1];
	} else if (gnuhash) {
		u32 nbuckets = gnuhash[0], symoff = gnuhash[1], bloom = gnuhash[2];
		const u32 *buckets = gnuhash + 4 + bloom;
		const u32 *chain = buckets + nbuckets;
		u32 last = 0;
		for (i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
		if (last >= symoff) {
			while (!(chain[last - symoff] & 1)) last++;
			nsyms = last + 1;
		}
	}
	for (i = 1; i < nsyms; i++) {
		const u8 *sym = symtab + i * 16;
		u32 st_name = *(const u32 *)sym, st_value = *(const u32 *)(sym + 4);
		u16 st_shndx = *(const u16 *)(sym + 14);
		const char *a = strtab + st_name, *b = req->name;
		if (!st_value || !st_shndx) continue;
		while (*a && *a == *b) { a++; b++; }
		if (*a || *b) continue;
		req->found = (void *)(info->addr + st_value);
		return 1;
	}
	return 0;
}

void *tad_module_symbol(const char *lib, const char *name)
{
	struct tad_dlsym_req req;
	req.lib = lib; req.name = name; req.found = 0;
	dl_iterate_phdr(libdl_cb, &req);
	if (g_debug && req.found) {
		dbg("[tadpole] "); dbg(name); dbg(" found in "); dbg(lib); dbg(" by object walk\n");
	}
	return req.found;
}

static void *libdl_symbol(const char *name) { return tad_module_symbol("libdl", name); }

/* The load base of a named module, by the same walk: what a caller needs to
 * read a value out of a library's own data at a known offset. */
static int base_cb(struct tad_phdr_info *info, size_t size, void *data)
{
	struct tad_dlsym_req *req = data;
	const char *base, *nm = info->name, *want = req->lib;
	u32 i, k = 0;
	(void)size;
	if (!nm) return 0;
	base = nm;
	for (i = 0; nm[i]; i++) if (nm[i] == '/') base = nm + i + 1;
	while (want[k] && base[k] == want[k]) k++;
	if (want[k] || !(base[k] == '.' || base[k] == '-')) return 0;
	req->found = (void *)info->addr;
	return 1;
}

void *tad_module_base(const char *lib)
{
	struct tad_dlsym_req req;
	req.lib = lib; req.name = 0; req.found = 0;
	dl_iterate_phdr(base_cb, &req);
	return req.found;
}

static void init(void)
{
	const char *e;
	char path[320];
	int fd, i;

	if (g_ready)
		return;
	g_ready = 1;

	real_chdir  = dlsym(RTLD_NEXT, "chdir");
	real_open   = dlsym(RTLD_NEXT, "open");
	real_open64 = dlsym(RTLD_NEXT, "open64");
	real_openat = dlsym(RTLD_NEXT, "openat");
	real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
	real_close  = dlsym(RTLD_NEXT, "close");
	real_mmap   = dlsym(RTLD_NEXT, "mmap");
	real_read   = dlsym(RTLD_NEXT, "read");
	real_pipe   = dlsym(RTLD_NEXT, "pipe");
	real_write  = dlsym(RTLD_NEXT, "write");
	real_fcntl  = dlsym(RTLD_NEXT, "fcntl");
	real_poll   = dlsym(RTLD_NEXT, "poll");
	real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
	/* RTLD_NEXT finds no libpthread from inside GlasgowUI either (see
	 * tad_module_symbol); left NULL, the pthread_create wrapper answered
	 * EAGAIN and Brio's KernelMPI asserted two seconds into the shell. */
	if (!real_pthread_create)
		real_pthread_create = tad_module_symbol("libpthread", "pthread_create");
	real_rename = dlsym(RTLD_NEXT, "rename");
	real_mkstemp   = dlsym(RTLD_NEXT, "mkstemp");
	real_mkstemp64 = dlsym(RTLD_NEXT, "mkstemp64");
	real_mkstemps = dlsym(RTLD_NEXT, "mkstemps");
	real_unlink = dlsym(RTLD_NEXT, "unlink");
	real_mkdir  = dlsym(RTLD_NEXT, "mkdir");
	real_signal = dlsym(RTLD_NEXT, "signal");
	real_stat   = dlsym(RTLD_NEXT, "stat");
	real_stat64 = dlsym(RTLD_NEXT, "stat64");
	real_lstat  = dlsym(RTLD_NEXT, "lstat");
	real_access = dlsym(RTLD_NEXT, "access");
	real_dlopen = dlsym(RTLD_NEXT, "dlopen");
	real_dlerror= dlsym(RTLD_NEXT, "dlerror");
	real_fopen  = dlsym(RTLD_NEXT, "fopen");
	real_fopen64= dlsym(RTLD_NEXT, "fopen64");
	real_execve = dlsym(RTLD_NEXT, "execve");
	real_sigaction = dlsym(RTLD_NEXT, "sigaction");
	if (!real_sigaction) real_sigaction = tad_module_symbol("libc", "sigaction");
	if (!real_sigaction) real_sigaction = tad_module_symbol("libuClibc", "sigaction");

	/* DID RTLD_NEXT FIND US AGAIN?
	 *
	 * Every interceptor here is "do our bit, then call real_X". If dlsym
	 * hands back OUR OWN definition, that call is a self-call and the first
	 * open() recurses until the stack is gone. The failure is silent and
	 * deeply unhelpful: SIGSEGV at the bottom of the guest stack, no syscalls
	 * repeated in an strace because the recursion never reaches one, and no
	 * crash report either — the handler needs stack it no longer has.
	 *
	 * It happens when TWO copies of this shim end up in one process, each
	 * resolving RTLD_NEXT to the other. That is a live hazard now there are
	 * four impersonation variants and a rootfs can name libEGL by an absolute
	 * path; see the shimlibs-egl notes in the Makefile.
	 *
	 * So check, and say so. Falling back to a null real_open would hide the
	 * problem; this at least names it in one line.
	 */
	{
		/* Our own interceptors are defined further down this file; naming
		 * them here needs the prototypes, not the definitions. */
		extern int open(const char *, int, ...);
		extern int close(int);
		extern int ioctl(int, ulong, ...);
		extern void _exit(int);
		void *global_open = dlsym(RTLD_DEFAULT, "open");
		int twice = 0;

		/* CASE 1: RTLD_NEXT found US. Then real_open IS open, and the first
		 * call recurses forever. */
		if (real_open && real_open == (void *)open)
			twice = 1;
		/* CASE 2 — the one that actually happened. TWO DIFFERENT COPIES of
		 * this shim in one process, each chaining into the other. Neither
		 * sees itself, so case 1 never fires, and the recursion is mutual
		 * instead of direct. Detect it by asking who owns `open` GLOBALLY: if
		 * the winner is neither us nor something outside this file, there is
		 * a second impersonator in the link map.
		 *
		 * HOW IT HAPPENS. Every variant here (libdl.so.0, libz.so.1,
		 * libEGL.so) is the whole shim under a different name, and they are
		 * all on LD_LIBRARY_PATH at once. That is harmless while a guest
		 * links exactly one of those names. Qt links libEGL AND — through
		 * libpng — libz, so it got two.
		 *
		 * The symptom is otherwise unreadable: SIGSEGV at the bottom of the
		 * guest stack, no repeated syscall in an strace because the recursion
		 * never reaches one, and no crash report because the handler needs
		 * stack that is gone. It cost a gdb session to find; it costs four
		 * lines to name. */
		if (!twice && global_open && global_open != (void *)open &&
		    real_open && global_open != real_open)
			twice = 2;

		/* CASE 1 IS FATAL, CASE 2 ONLY WARNS.
		 *
		 * Self-reference cannot be anything but the recursion, so stopping is
		 * the kindest thing. The global-owner test is a heuristic and it does
		 * produce false positives — VideoDaemon tripped it while running
		 * perfectly well — so it prints and continues. What actually PREVENTS
		 * the recursion is one variant per guest on LD_LIBRARY_PATH; this is
		 * only here so the next person does not need a gdb session to
		 * recognise it. */
		if (twice == 1) {
			static const char b[] =
			    "[tadpole] FATAL: dlsym(RTLD_NEXT) returned our own open().\n"
			    "[tadpole] Two copies of the shim are loaded; open() would "
			    "recurse until the stack is gone.\n[tadpole] Put ONE "
			    "impersonation variant on LD_LIBRARY_PATH for this guest.\n";
			write(2, b, sizeof(b) - 1);
			_exit(70);
		}
		if (twice == 2)
			dbg("[tadpole] note: open() is owned by neither us nor our "
			    "RTLD_NEXT; check for a second shim on the path\n");
	}

	for (i = 0; i < MAXFD; i++) {
		g_fb_of_fd[i] = -1;
		g_ev_of_fd[i] = -1;
	}
	{
		int d, s;
		for (d = 0; d < NUM_EV; d++) {
			g_ev_fifo[d] = -1;
			for (s = 0; s < EV_MAX_READERS; s++)
				g_ev_rd[d][s] = g_ev_wr[d][s] = -1;
		}
	}

	{
		/* TESTING THE VALUE, not just presence. tadpole.sh used to pass
		 * TADPOLE_DEBUG=0 unconditionally — `${debug:+...}` expands for "0"
		 * because it is non-empty — so every run had full debug logging on.
		 * One boot produced 2.1 MILLION log lines and never finished. */
		const char *d = getenv("TADPOLE_DEBUG");
		g_debug = (d && d[0] && d[0] != '0');
		d = getenv("TADPOLE_ABS_PANEL");
		g_abs_panel = (d && d[0] && d[0] != '0');
	}
	{
		/* TADPOLE_LOG=<prefix> — DEBUG OUTPUT THAT SURVIVES A DAEMONIZE.
		 *
		 * dbg() wrote to fd 2, which is fine right up until the guest is a
		 * daemon. VideoDaemon forks, setsid()s and reopens 0/1/2 on /dev/null,
		 * so from that moment the shim is writing its entire account of what
		 * the process is doing into the void — and VideoDaemon is exactly the
		 * process whose behaviour we could not see. Hours went into "the video
		 * layer turns on and goes blank again" with no way to ask why.
		 *
		 * One file per PID, because AppManager and VideoDaemon run at once and
		 * interleaved lines from two processes are worse than none. The fd is
		 * moved above 100 so the guest's own dup2 onto 0/1/2 cannot land on
		 * it — a daemonize that silently reassigned our log fd would corrupt
		 * whatever it then wrote there. */
		const char *pfx = getenv("TADPOLE_LOG");
		if (pfx && pfx[0] && g_debug) {
			snprintf(g_logpfx, sizeof(g_logpfx), "%s", pfx);
			log_open();
		}
	}
	{
		/* Microseconds of artificial latency on guest .png opens — see io_pace.
		 * Parsed by hand; the shim has no strtol. */
		const char *d = getenv("TADPOLE_IO_DELAY_US");
		long v = 0;
		if (d) { while (*d >= '0' && *d <= '9') v = v * 10 + (*d++ - '0'); }
		g_io_delay_us = v;
	}
	{
		/* TADPOLE_HZ=0 restores the old uncapped behaviour, for measuring
		 * how fast the guest COULD run. */
		const char *hz = getenv("TADPOLE_HZ");
		int v = hz ? 0 : VSYNC_HZ_DEFAULT;
		if (hz) { while (*hz >= '0' && *hz <= '9') v = v * 10 + (*hz++ - '0'); }
		g_vsync_ns = (v > 0 && v <= 1000) ? (1000000000L / v) : 0;
	}

	/* A NULL here is fatal in a subtle way: the fall-through paths below are
	 * tail calls, so jumping through a null pointer lands at PC=0 with LR
	 * still pointing at OUR caller — the backtrace then blames whichever
	 * library called us, and the real cause is invisible. Always check. */
	if (!real_open)  dbg("[tadpole] WARNING: dlsym(open) failed\n");
	if (!real_ioctl) dbg("[tadpole] WARNING: dlsym(ioctl) failed\n");
	if (!real_close) dbg("[tadpole] WARNING: dlsym(close) failed\n");
	if (!real_mmap)  dbg("[tadpole] WARNING: dlsym(mmap) failed\n");
	if (!real_read)  dbg("[tadpole] WARNING: dlsym(read) failed\n");
	/* A NULL here makes every dlopen() in the guest fail SILENTLY, and the
	 * caller's dlerror() then reports whatever the loader last complained
	 * about — which is this very lookup, "Unable to resolve symbol". That read
	 * as a Brio module with a missing symbol for an afternoon. */
	if (!real_dlopen)  real_dlopen  = libdl_symbol("dlopen");
	if (!real_dlerror) real_dlerror = libdl_symbol("dlerror");
	if (!real_dlopen) dbg("[tadpole] WARNING: dlsym(dlopen) failed — every dlopen will fail\n");

	/* WHICH INPUT DEVICES THIS MACHINE HAS. The launcher passes DEV_EVDEV
	 * from the device profile; anything unrecognised, or nothing at all,
	 * keeps the LF2000 set that every device used before this was per
	 * device. Compared by hand because the shim has no string.h. */
	if ((e = getenv("TADPOLE_MLC25")) != 0) {
		u32 v = 0; int hex = (e[0] == '0' && e[1] == 'x');
		if (hex) e += 2;
		while (*e) {
			u32 d = (*e >= '0' && *e <= '9') ? (u32)(*e - '0')
			      : (*e >= 'a' && *e <= 'f') ? (u32)(*e - 'a' + 10)
			      : (*e >= 'A' && *e <= 'F') ? (u32)(*e - 'A' + 10) : 99;
			if (d > (hex ? 15u : 9u)) break;
			v = v * (hex ? 16u : 10u) + d; e++;
		}
		g_mlc_q25 = v;
	}
	if ((e = getenv("TADPOLE_MLCDFLT")) != 0) {
		u32 v = 0;
		while (*e >= '0' && *e <= '9') v = v * 10 + (u32)(*e++ - '0');
		g_mlc_dflt = v;
	}
	if ((e = getenv("TADPOLE_EVDEV")) != 0 &&
	    e[0] == 'l' && e[1] == 'f' && e[2] == '1' && e[3] == '0' &&
	    e[4] == '0' && e[5] == '0' && e[6] == '\0') {
		g_ev = g_ev_lf1000;
		g_ev_count = (int)(sizeof(g_ev_lf1000) / sizeof(g_ev_lf1000[0]));
	}

	e = getenv("TADPOLE_SYSROOT");
	snprintf(g_sysroot, sizeof(g_sysroot), "%s", e ? e : "");

	e = getenv("TADPOLE_DIR");
	snprintf(g_dir, sizeof(g_dir), "%s", e ? e : "/tmp/tadpole");
	/* real_mkdir, NOT our own wrapper below. TADPOLE_DIR is a HOST path that the
	 * viewer also opens by that exact name; sending it through the sysroot-first
	 * rule would try to create it under the guest tree, and on a sysroot that
	 * happens to have a /tmp it would SUCCEED there — leaving the guest and the
	 * viewer looking at two different runtime directories. */
	real_mkdir(g_dir, 0777);

	/* Installed EARLY and unconditionally, not behind TADPOLE_DEBUG: a crash is
	 * always worth a report, and most of them happen during an app's own
	 * startup — before anything else here has run. */
	tad_crash_install(g_dir, real_open);

	if ((e = getenv("TADPOLE_W")) != 0) { u32 v = 0; while (*e >= '0' && *e <= '9') v = v*10 + (u32)(*e++ - '0'); if (v) g_w = v; }
	if ((e = getenv("TADPOLE_H")) != 0) { u32 v = 0; while (*e >= '0' && *e <= '9') v = v*10 + (u32)(*e++ - '0'); if (v) g_h = v; }
	if ((e = getenv("TADPOLE_BPP")) != 0) { u32 v = 0; while (*e >= '0' && *e <= '9') v = v*10 + (u32)(*e++ - '0'); if (v) g_bpp = v; }

	/* ONE shared arena for all three framebuffers.
	 * Brio treats /dev/fb0..2 as a single address space: it allocates every
	 * layer inside fb0's smem (CreateHandle @ 0x820ff000 and @ 0x8217e800,
	 * i.e. lines 544 and 816 of fb0) but pans a DIFFERENT fb device for
	 * each layer (fb0 PAN 544, fb1 PAN 816). Backing each device with its
	 * own file therefore loses every layer but the first. */

	/* AN ARENA THAT CANNOT BE CREATED HAS TO SAY SO, AND USED NOT TO.
	 *
	 * Both opens below were written as `if (fd >= 0)` with no else, so a
	 * TADPOLE_DIR the guest cannot write left the shim silent and carrying on.
	 * What the user then sees is Brio discovering the consequence several
	 * layers away and describing it in its own vocabulary:
	 *
	 *     [0x5] InitModule: Screen = 0 x 0, pitch = 0
	 *     [0x5] InitModule: Mapped 00000000 to 0x50d30000, size 00000000
	 *     [0x5] CreateHandle: No framebuffer allocation available
	 *     <ASSERT>: Unsupported destination PixelFormat used 0
	 *               (line 175 in LightningBase/Src/BlitBuffer.cpp)
	 *
	 * — four messages about pixel formats and display handles for a bug that
	 * is one failed open of one file, whose NAME appears nowhere. And the
	 * assert does not kill AppManager: it parks it, for ever, so the same
	 * fault reads as "it crashed" to whoever finds the log and as "it is stuck
	 * on a black screen" to whoever is only looking at the window. Both
	 * reports were made, separately, about this one line.
	 *
	 * It was worse than merely unhelpful. That guest message already had a
	 * documented cause on Windows — mmap view exhaustion, since fixed — so the
	 * evidence pointed confidently at an unrelated and already-repaired bug.
	 *
	 * Say the path. Always, not under TADPOLE_DEBUG: the emulator is useless
	 * from here on, nobody turns logging up before the thing fails, and one
	 * line naming the directory is the difference between reading this and
	 * guessing. */
	snprintf(path, sizeof(path), "%s/fb0.bin", g_dir);
	fd = real_open(path, O_RDWR | O_CREAT, 0666);
	if (fd >= 0) {
		/* BIG ENOUGH FOR WHAT THE GUEST ACTUALLY MAPS, not just for the
		 * planes we compose. w*h*bpp*NBUF covers the LF2000 devices, which
		 * mmap their fb nodes at small offsets. The Didj maps /dev/mem —
		 * physical memory — and asks for 4 MB at offset 0x400000, which is
		 * past the end of that: the mapping SUCCEEDS and the first store into
		 * it takes SIGBUS, a bus error with nothing in it naming a file or a
		 * size. A hole costs nothing on any filesystem this runs on. */
		{
			long want = (long)(g_w * g_h * (g_bpp / 8) * NBUF);
			if (want < LF_VMEM_MIN)
				want = LF_VMEM_MIN;
			/* Only for the device that needs it: half a gigabyte of hole is
			 * cheap but it is not free of surprise, and no LF2000 maps
			 * anywhere near there. */
			if (g_ev == g_ev_lf1000 && want < (long)LF_VMEM_TOP)
				want = (long)LF_VMEM_TOP;
			ftruncate(fd, want);
		}
		real_close(fd);
	} else {
		char m[420];
		snprintf(m, sizeof(m),
		         "[tadpole] FATAL: cannot create the framebuffer arena %s — "
		         "the display will have no memory to allocate from, and Brio "
		         "will report that as \"No framebuffer allocation available\". "
		         "Is TADPOLE_DIR reachable and writable by the guest?\n", path);
		note(m);
	}

	/* shared state, mmapped so the viewer sees updates live */
	snprintf(path, sizeof(path), "%s/state.bin", g_dir);
	fd = real_open(path, O_RDWR | O_CREAT, 0666);
	if (fd >= 0) {
		ftruncate(fd, (long)sizeof(struct tadpole_state));
		g_state = real_mmap(0, sizeof(struct tadpole_state), 3 /*RW*/, 1 /*SHARED*/, fd, 0);
		real_close(fd);
		if (g_state == (void *)-1)
			g_state = 0;
	}
	if (!g_state) {
		char m[420];
		snprintf(m, sizeof(m),
		         "[tadpole] FATAL: cannot map the shared state %s — the viewer "
		         "has nothing to read, so the window stays black however well "
		         "the guest runs. Is TADPOLE_DIR reachable and writable by the "
		         "guest?\n", path);
		note(m);
	} else {
		memset(g_state, 0, sizeof(*g_state));
		g_state->magic   = TADPOLE_MAGIC;
		g_state->version = TADPOLE_VERSION;
		g_state->width   = g_w;
		g_state->height  = g_h;
		for (i = 0; i < NUM_FB; i++) {
			g_state->layer[i].xres  = g_w;
			g_state->layer[i].yres  = g_h;
			g_state->layer[i].bpp   = g_bpp;
			g_state->layer[i].alpha = 255;
			g_state->layer[i].win_x = 0;
			g_state->layer[i].win_y = 0;
			g_state->layer[i].win_w = g_w;
			g_state->layer[i].win_h = g_h;
			/* fb0 is the primary; the overlays start disabled */
			g_state->layer[i].enabled = (i == 0);
		}
		/* Tell the viewer which node is which. g_ev was chosen from
		 * TADPOLE_EVDEV above, before the state existed. */
		for (i = 0; i < TAD_EV_SLOTS; i++)
			g_state->ev_role[i] = (i < g_ev_count) ? g_ev[i].role : TAD_EV_NONE;
	}

	/* input FIFOs — viewer writes struct input_event, guest reads */
	for (i = 0; i < NUM_EV; i++) {
		snprintf(path, sizeof(path), "%s/ev%d", g_dir, i);
		mkfifo(path, 0666);
	}

	dbg("[tadpole] shim initialised\n");
}

/* /dev/fbN -> N, else -1 */
/* A LAYER IS A FRAMEBUFFER, and calling it one costs nothing.
 *
 * The LF2000 devices expose their display planes as /dev/fb0..2, ordinary
 * fbdev nodes with a handful of LeapFrog ioctls bolted on. The LF1000 — the
 * Didj — exposes the same three planes of the same multi-layer controller as
 * /dev/layer0..2, with a control node /dev/mlc beside them. The pixels are the
 * pixels either way: one arena, three planes, the guest mmaps a plane and
 * draws into it, and the viewer composites what it finds.
 *
 * So the Didj's layers ARE this shim's framebuffers, under another name. That
 * is not a shortcut taken to save work — it is why fbshot.py, the viewer's
 * compositor and the whole state.bin protocol need no Didj-specific anything. */
static int fb_index(const char *path)
{
	if (!path)
		return -1;
	if (!strncmp(path, "/dev/fb", 7)) {
		if (path[7] >= '0' && path[7] < '0' + NUM_FB && path[8] == 0)
			return path[7] - '0';
		return -1;
	}
	if (!strncmp(path, "/dev/layer", 10)) {
		if (path[10] >= '0' && path[10] < '0' + NUM_FB && path[11] == 0)
			return path[10] - '0';
		return -1;
	}
	return -1;
}

/* /dev/mlc, /dev/dpc, /dev/gpio, /dev/ga3d — the LF1000's display CONTROL
 * nodes, as opposed to the layers, which carry pixels. Nothing is mmapped
 * through them; they exist to be asked questions with ioctl(). */
static int mlc_is(const char *path)
{
	return path && (!strcmp(path, "/dev/mlc") || !strcmp(path, "/dev/dpc") ||
	                !strcmp(path, "/dev/gpio") || !strcmp(path, "/dev/ga3d"));
}

/* /dev/input/eventN -> N, else -1. N >= NUM_EV is a real device we don't have. */
static int ev_index(const char *path)
{
	const char *p;
	int n = 0;

	if (!path)
		return -1;
	/* THE DEVICE'S OWN NAME FOR THE TOUCHSCREEN.
	 *
	 * /etc/profile sets TSLIB_TSDEVICE=/dev/input/touchscreen0, and that is
	 * the path tslib opens — a symlink the kernel's udev rules make on
	 * hardware, and nothing makes here. tslib's module_raw input then failed
	 * to open anything, the chain came up with a null ops->read, and the
	 * first tap jumped through it. That crash is why tadpole.sh disables
	 * tslib by default; this is the cause of it, not the symptom.
	 *
	 * event2 is the node whose EVIOCGNAME is "touchscreen interface", which
	 * is what the device's own list-input-devices | fgrep looks for. */
	if (!strcmp(path, "/dev/input/touchscreen0"))
		return 2;
	if (strncmp(path, "/dev/input/event", 16))
		return -1;
	p = path + 16;
	if (!*p)
		return -1;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + (*p++ - '0');
	if (*p)
		return -1;
	return n;
}

/* ---- evdev fan-out -------------------------------------------------------
 *
 * See the note beside g_ev_fifo. ev_open() hands out a private pipe per open;
 * ev_pump() copies the shared FIFO into every one of them.
 */

/* Copy whatever is waiting on device idx's FIFO into every open reader.
 *
 * Called from read(), by whichever reader happens to ask first — so no thread
 * is needed and no reader is privileged. Each pump fills EVERY pipe, including
 * the caller's, so it does not matter which one runs it.
 *
 * The try-lock is not for correctness of the copy but to stop two guest
 * threads splitting one FIFO read between them, which would reintroduce
 * exactly the bug this exists to fix. A thread that loses the race skips the
 * pump and reads its own pipe; the winner has already filled it.
 */
static void ev_pump(int idx)
{
	/* 64 events. The viewer sends a whole gesture in bursts of ~5. */
	char buf[16 * 64];
	long n;
	int s;

	if (idx < 0 || idx >= NUM_EV || g_ev_fifo[idx] < 0 || !real_read || !real_write)
		return;
	if (!__sync_bool_compare_and_swap(&g_ev_pumping, 0, 1))
		return;

	while ((n = real_read(g_ev_fifo[idx], buf, sizeof(buf))) > 0) {
		/* Whole events only. Every writer emits one event per write() and
		 * 16 bytes is far below PIPE_BUF, so the kernel keeps them atomic
		 * and a short read cannot land mid-event. Truncate rather than
		 * trust it. */
		n -= n % 16;
		if (n <= 0)
			break;
		for (s = 0; s < EV_MAX_READERS; s++)
			if (g_ev_wr[idx][s] >= 0)
				/* Non-blocking: a reader that has stopped draining its
				 * pipe loses events rather than stalling the others. */
				(void)real_write(g_ev_wr[idx][s], buf, (size_t)n);
		if (n < (long)sizeof(buf))
			break;
	}
	__sync_lock_release(&g_ev_pumping);
}

/* THE PUMP NEEDS ITS OWN THREAD, AND HERE IS WHY IT CANNOT BORROW ONE.
 *
 * Pumping from inside read() looks sufficient and deadlocks instantly. The
 * readers do not spin on read(); they sleep in select()/poll() until their fd
 * is readable. While each of them held the shared FIFO directly that worked —
 * a write woke everybody. Once each holds a private pipe, the pipe only fills
 * if somebody pumps, and nobody pumps until they wake: every reader sleeps
 * forever with a full FIFO sitting in front of them. Measured exactly that
 * way — the opens and the pipes were all correct and not one event arrived.
 *
 * So one thread per process blocks on the FIFOs and does nothing else. This is
 * what the kernel's input core does for us on hardware, which is the reason
 * none of this machinery is normally anyone's problem.
 *
 * pthread_create comes from the GUEST's libpthread by dlsym, so the thread is
 * a guest thread and qemu-user schedules it like any other. If it is not there
 * to be found — a guest that does not link libpthread — ev_open() falls back
 * to handing out the shared FIFO, which is what this did before and is still
 * correct for a single reader.
 */
static void *ev_pump_thread(void *arg)
{
	/* struct pollfd is {int fd; short events; short revents;} = 8 bytes. */
	struct { int fd; short events; short revents; } pfds[NUM_EV];
	int d, n;

	(void)arg;
	for (;;) {
		n = 0;
		for (d = 0; d < NUM_EV; d++)
			if (g_ev_fifo[d] >= 0) {
				pfds[n].fd = g_ev_fifo[d];
				pfds[n].events = 1;          /* POLLIN */
				pfds[n].revents = 0;
				n++;
			}
		if (!n || !real_poll) {              /* nothing open yet */
			struct tad_timespec nap;
			nap.tv_sec = 0; nap.tv_nsec = 50L * 1000 * 1000;
			nanosleep(&nap, 0);
			continue;
		}
		if (real_poll(pfds, (ulong)n, 200) <= 0)
			continue;
		for (d = 0; d < NUM_EV; d++)
			if (g_ev_fifo[d] >= 0)
				ev_pump(d);
	}
	return 0;
}

static void ev_start_pump(void)
{
	static int started;
	ulong tid;

	if (started || !real_pthread_create)
		return;
	started = 1;
	if (real_pthread_create(&tid, 0, ev_pump_thread, 0) != 0) {
		started = 0;
		dbg("[tadpole] evdev: no pump thread; readers will share one queue\n");
	}
}

/* -> a read fd for device idx, with its own queue. */
static int ev_open(int idx)
{
	char real[512];
	int pfd[2], s;

	if (!real_open || !real_pipe)
		return -1;

	/* NO THREAD, NO FAN-OUT. Handing out private pipes with nothing to fill
	 * them is strictly worse than the old shared FIFO, so fall back to it
	 * rather than deliver silence. */
	ev_start_pump();
	if (!real_pthread_create) {
		snprintf(real, sizeof(real), "%s/ev%d", g_dir, idx);
		return real_open(real, O_RDWR | O_NONBLOCK, 0666);
	}

	/* One shared reader of the FIFO for the whole process, opened O_RDWR so
	 * it never blocks waiting for a writer and never sees EOF when the
	 * viewer restarts. */
	if (g_ev_fifo[idx] < 0) {
		snprintf(real, sizeof(real), "%s/ev%d", g_dir, idx);
		g_ev_fifo[idx] = real_open(real, O_RDWR | O_NONBLOCK, 0666);
		if (g_ev_fifo[idx] < 0)
			return -1;
	}

	for (s = 0; s < EV_MAX_READERS; s++)
		if (g_ev_rd[idx][s] < 0)
			break;
	if (s == EV_MAX_READERS)
		return -1;

	if (real_pipe(pfd) != 0)
		return -1;
	/* The old code handed out an O_NONBLOCK fd and every reader here copes
	 * with EAGAIN, so keep that contract exactly. */
	if (real_fcntl) {
		(void)real_fcntl(pfd[0], 4 /* F_SETFL */, O_NONBLOCK);
		(void)real_fcntl(pfd[1], 4 /* F_SETFL */, O_NONBLOCK);
	}
	g_ev_rd[idx][s] = pfd[0];
	g_ev_wr[idx][s] = pfd[1];
	return pfd[0];
}

/* Drop a reader when the guest closes its fd. */
static void ev_close(int fd)
{
	int d, s;

	for (d = 0; d < NUM_EV; d++)
		for (s = 0; s < EV_MAX_READERS; s++)
			if (g_ev_rd[d][s] == fd) {
				if (g_ev_wr[d][s] >= 0 && real_close)
					real_close(g_ev_wr[d][s]);
				g_ev_rd[d][s] = g_ev_wr[d][s] = -1;
				return;
			}
}

/* ---- the guest's clock ---------------------------------------------------
 *
 * FBIO_WAITFORVSYNC used to return IMMEDIATELY. On real hardware it blocks
 * until the panel's next refresh, and that is the only thing pacing a Brio
 * title: the render loop runs flat out and waits here. Returning at once gives
 * the guest an uncapped frame rate, which is not a cosmetic problem —
 *
 *   * titles run absurdly fast (Mr. Pencil, the SpongeBob menus),
 *   * and audio is GENERATED faster than it can be played. The viewer then
 *     trims the backlog to hold latency down, so the sound skips forward and
 *     comes out fast and garbled, with phrases cut short ("Rinse your p-").
 *
 * Both symptoms are one bug. Pace the guest here and the audio paces itself.
 *
 * Deliberately NOT tied to the viewer's cadence: probes run headless with no
 * viewer at all, and the guest still needs a clock.
 *
 * A guest that is ALREADY slower than the period never sleeps — the deadline is
 * in the past — so this cannot make heavy 3D any slower. When we fall far
 * behind we resync instead of accumulating debt, which would otherwise make the
 * guest sprint to "catch up" later.
 */
static void vsync_wait(void)
{
	struct tad_timespec now, req;
	long dsec, dnsec;

	if (!g_vsync_ns)
		return;
	if (clock_gettime(CLOCK_MONOTONIC_, &now) != 0)
		return;

	if (!g_vs_sec) {                       /* first call: start the clock */
		g_vs_sec = now.tv_sec; g_vs_nsec = now.tv_nsec;
	}
	g_vs_nsec += g_vsync_ns;
	while (g_vs_nsec >= 1000000000L) { g_vs_nsec -= 1000000000L; g_vs_sec++; }

	dsec  = g_vs_sec  - now.tv_sec;
	dnsec = g_vs_nsec - now.tv_nsec;
	if (dnsec < 0) { dnsec += 1000000000L; dsec--; }

	/* More than a quarter second behind: the guest is not keeping up, so stop
	 * pretending and restart the clock from now. */
	if (dsec < 0 || (dsec == 0 && dnsec == 0)) {
		if (dsec < -1) { g_vs_sec = now.tv_sec; g_vs_nsec = now.tv_nsec; }
		return;
	}
	if (dsec > 1)                          /* clock jumped; do not sleep long */
		return;
	req.tv_sec = dsec; req.tv_nsec = dnsec;
	nanosleep(&req, 0);
}

/* ---- pretending to be NAND -------------------------------------------------
 *
 * TADPOLE_IO_DELAY_US=<n> sleeps n microseconds on every open of a guest .png.
 *
 * WHY THAT IS A REASONABLE THING TO WANT. The home picker loads one image per
 * installed title — meta.inf's Icon="GAMS/BaseImage.png" — asynchronously, and
 * its ActionScript polls for the result, logging "waiting for load of the
 * image" while it waits. On the device that wait is real: the file comes off
 * NAND. Here it comes off the host page cache on an NVMe SSD, so a load can
 * complete within the same script frame that asked for it, which on hardware it
 * never could. Code that assumes "the answer is never ready this soon" then
 * reads a state that does not exist on a real LeapPad, and the crash we see is
 * an undefined value dereferenced inside libflashlite's interpreter.
 *
 * This is a MEASUREMENT, not a fix: if slowing the icon reads changes the crash
 * rate, the race is real and the fix belongs wherever the timing assumption is,
 * not in a sleep. Off unless the variable is set.
 *
 * Only .png, and only guest paths, so a boot does not crawl: the picker's icons
 * are the reads under suspicion, and nothing else needs to be slowed to test
 * them. */
static int ends_with_png(const char *path)
{
	size_t n = path ? strlen(path) : 0;
	if (n < 4) return 0;
	return (path[n-4] == '.' &&
	        (path[n-3] == 'p' || path[n-3] == 'P') &&
	        (path[n-2] == 'n' || path[n-2] == 'N') &&
	        (path[n-1] == 'g' || path[n-1] == 'G'));
}

static void io_pace(const char *path)
{
	struct tad_timespec req;
	if (g_io_delay_us <= 0 || !ends_with_png(path)) return;
	req.tv_sec  = g_io_delay_us / 1000000L;
	req.tv_nsec = (g_io_delay_us % 1000000L) * 1000L;
	nanosleep(&req, 0);
}

/* Is this the emulator's OWN runtime directory rather than a guest path? The
 * viewer opens TADPOLE_DIR by its literal host name, so it must never be
 * rewritten into the sysroot. */
static int under_gdir(const char *path)
{
	unsigned dlen = (unsigned)strlen(g_dir);
	return dlen && path && strncmp(path, g_dir, dlen) == 0 &&
	       (path[dlen] == '\0' || path[dlen] == '/');
}

/* ---- which way up: telling the viewer what is on screen ------------------
 *
 * THE GUEST SAYS SO BY OPENING FILES, and two openings are unambiguous. Both
 * were measured off a live session (boot -> home -> Pet Pad -> Home button),
 * not reasoned about:
 *
 *   /LF/Base/LPAD/<state>.swf            the UI. Opened when a screen is
 *                                        pushed AND again on every pop back
 *                                        out of a title, which is the whole
 *                                        reason leaving a game is visible.
 *   /LF/Bulk/ProgramFiles/<pkg>/<AppSo>  a title's entry point — a .swf that
 *                                        the Flash player opens, or an App.so
 *                                        that CAppManager dlopen()s.
 *
 * THE ENTRY POINT IS MATCHED AGAINST meta.inf, not guessed from the
 * extension. The home picker opens a .swf out of EVERY installed package to
 * draw its tile — icon.swf, base_icon.swf — so "a .swf under ProgramFiles" is
 * true of a screen that is not running anything at all, and reading it that
 * way would spin the window once per icon.
 *
 * WHAT IS NOT DONE HERE. This says which SCREEN is up; it does not say which
 * way to hold the window. That is presentation, it belongs to the viewer's -r
 * and nothing else's, and the viewer needs the package name to make its own
 * exceptions — which is why the name is published rather than a rotation.
 */
static int seg_eq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

/* Match `pfx` against the front of `path`, treating a run of slashes as one
 * separator, and hand back what follows. Guest paths really do arrive with
 * doubled slashes — "/LF/Base//LpadAssets/Art/..." and
 * ".../PAD2-0x001F0005-000000//GameInfo.json" are both from one boot log — so
 * a plain strncmp would miss whichever spelling it was not written for. */
static const char *path_after(const char *path, const char *pfx)
{
	if (!path) return 0;
	for (;;) {
		if (*pfx == '/') {
			if (*path != '/') return 0;
			while (*path == '/') path++;
			while (*pfx  == '/') pfx++;
			continue;
		}
		if (!*pfx) return path;
		if (*path != *pfx) return 0;
		path++; pfx++;
	}
}

static int has_slash(const char *s)
{
	for (; *s; s++)
		if (*s == '/') return 1;
	return 0;
}

/* The AppSo= line of a package's meta.inf: the file the picker would launch.
 * Read with real_open so it cannot recurse back into our own open(). */
static void pkg_entry(const char *pkg, char *out, unsigned outsz)
{
	char path[512], buf[2048];
	int fd;
	long n;
	unsigned i, j;

	out[0] = 0;
	if (!real_open || !real_read || !real_close)
		return;
	snprintf(path, sizeof(path), "%s/LF/Bulk/ProgramFiles/%s/meta.inf",
	         g_sysroot, pkg);
	fd = real_open(path, O_RDONLY, 0);
	if (fd < 0) {
		snprintf(path, sizeof(path), "/LF/Bulk/ProgramFiles/%s/meta.inf", pkg);
		fd = real_open(path, O_RDONLY, 0);
	}
	if (fd < 0)
		return;
	n = real_read(fd, buf, sizeof(buf) - 1);
	real_close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	for (i = 0; (long)i + 7 < n; i++) {
		if (strncmp(buf + i, "AppSo=\"", 7))
			continue;
		i += 7;
		for (j = 0; j + 1 < outsz && buf[i] && buf[i] != '"'; )
			out[j++] = buf[i++];
		out[j] = 0;
		return;
	}
}

/* Is `path` the entry point of an installed package? Fills `pkg_out` if so.
 * The meta.inf answer is cached for one package, because the picker opens
 * dozens of files from a package directory and only one of them is this. */
static int title_entry(const char *path, char *pkg_out, unsigned pkg_sz)
{
	static char last_pkg[PKGID_MAX], last_entry[PKGID_MAX];
	const char *rest, *file;
	unsigned i;

	if (!(rest = path_after(path, "/LF/Bulk/ProgramFiles/")))
		return 0;
	for (i = 0; rest[i] && rest[i] != '/'; i++)
		;
	if (i == 0 || i >= sizeof(last_pkg) || rest[i] != '/')
		return 0;
	file = rest + i;
	while (*file == '/') file++;
	/* An entry point sits directly in the package directory; assets do not. */
	if (!*file || has_slash(file))
		return 0;

	if (strncmp(rest, last_pkg, i) || last_pkg[i]) {
		memcpy(last_pkg, rest, i);
		last_pkg[i] = 0;
		pkg_entry(last_pkg, last_entry, sizeof(last_entry));
	}
	if (!last_entry[0] || !seg_eq(file, last_entry))
		return 0;
	snprintf(pkg_out, pkg_sz, "%s", last_pkg);
	return 1;
}

static void screen_set(u32 kind, const char *pkg)
{
	if (!g_state)
		return;
	if (g_state->screen == kind &&
	    (kind != TAD_SCREEN_TITLE || seg_eq(g_state->screen_pkg, pkg ? pkg : "")))
		return;
	snprintf(g_state->screen_pkg, sizeof(g_state->screen_pkg), "%s",
	         (kind == TAD_SCREEN_TITLE && pkg) ? pkg : "");
	g_state->screen = kind;
	g_state->screen_seq++;
	dbg("[tadpole] screen -> ");
	dbg(kind == TAD_SCREEN_TITLE ? g_state->screen_pkg : "system UI");
	dbg("\n");
}

static void screen_note(const char *path)
{
	char pkg[PKGID_MAX];
	const char *rest;

	if (!g_state || !path)
		return;
	/* main.swf AND NOTHING ELSE UNDER LPAD.
	 *
	 * THE UI KEEPS RUNNING UNDERNEATH A TITLE — AppManager draws the
	 * ViewFrame chrome on fb0 while a Leapster game owns fb1 — and its Flash
	 * side goes on loading states and sound effects the whole time. Both
	 * LaunchApp.swf and HomePicker.swf are opened DURING a launch, after the
	 * title has been handed over. Reading any LPAD file as "the UI is on top"
	 * therefore turned the window straight back for native titles, because
	 * the two kinds hand over at different moments:
	 *
	 *     Flash   LaunchApp.swf  HomePicker.swf  <pkg>/main.swf   <- title last
	 *     native  LaunchApp.swf  dlopen App.so   HomePicker.swf   <- UI last
	 *
	 * which is exactly the reported symptom: Flash titles turned, native ones
	 * turned for a split second and turned back.
	 *
	 * main.swf is the LPAD app ITSELF, and it is loaded on exactly the two
	 * occasions the UI takes the screen: at boot, and on every pop back out of
	 * a title (kPopApp -> PushApp LPAD/main.swf -> LoadNewApp). Both measured.
	 */
	if ((rest = path_after(path, "/LF/Base/LPAD/")) != 0) {
		if (seg_eq(rest, "main.swf"))
			screen_set(TAD_SCREEN_SYSTEM, 0);
	} else if (title_entry(path, pkg, sizeof(pkg))) {
		screen_set(TAD_SCREEN_TITLE, pkg);
	}
}


/* ---- /dev/dsp — OSS playback -------------------------------------------
 *
 * WHY OSS AT ALL, when there is a whole fake libasound next door.
 *
 * Every other device Tadpole runs reaches audio through ALSA, and
 * shim/tadpole_asound.c replaces libasound.so.2 outright for them. The Didj is
 * six years older than any of them: its Brio links portaudio, and portaudio's
 * unix build talks OSS straight to /dev/dsp. There is no libasound anywhere in
 * that process to replace, so the interception has to be at the device node —
 * which is where the framebuffer and the evdev nodes are already handled.
 *
 * WITHOUT IT APPMANAGER DOES NOT START. Audio init is not best-effort here:
 *
 *     !ASSERT: [1] Failed to initalize audio output
 *
 * and with a bare file in place of the node it gets one step further and names
 * the call it wanted, which is what this list was built from:
 *
 *     Expression 'ioctl( *odev, SNDCTL_DSP_SETTRIGGER, &enableBits )' failed
 *     in 'pa_unix_oss.c', line: 830
 *
 * WHERE THE SAMPLES GO: $TADPOLE_DIR/audio.<pid>.dsp, a FIFO, exactly like the
 * ALSA path's audio.<pid>.<slot>. The viewer picks up anything matching
 * "audio.*" in that directory, so this needed no viewer change at all.
 *
 * THE FD THE GUEST GETS IS A REAL ONE, onto the placeholder file the installer
 * touches at dev/dsp — the same trick /dev/fb0..2 use. That means poll(),
 * select(), fcntl() and close() all work on it without being intercepted; only
 * ioctl() and write() are answered here.
 */

/* From <sys/soundcard.h>, spelled out because there is no ARM sysroot at build
 * time — the same reason the LF1000FB_* numbers above are literals. All are
 * _SIO/_SIOR/_SIOW/_SIOWR with magic 'P' (0x50). */
#define SNDCTL_DSP_RESET       0x00005000ul
#define SNDCTL_DSP_SYNC        0x00005001ul
#define SNDCTL_DSP_SPEED       0xC0045002ul
#define SNDCTL_DSP_STEREO      0xC0045003ul
#define SNDCTL_DSP_GETBLKSIZE  0xC0045004ul
#define SNDCTL_DSP_SETFMT      0xC0045005ul
#define SNDCTL_DSP_CHANNELS    0xC0045006ul
#define SNDCTL_DSP_POST        0x00005008ul
#define SNDCTL_DSP_SUBDIVIDE   0xC0045009ul
#define SNDCTL_DSP_SETFRAGMENT 0xC004500Aul
#define SNDCTL_DSP_GETFMTS     0x8004500Bul
#define SNDCTL_DSP_GETOSPACE   0x8010500Cul
#define SNDCTL_DSP_GETISPACE   0x8010500Dul
#define SNDCTL_DSP_NONBLOCK    0x0000500Eul
#define SNDCTL_DSP_GETCAPS     0x8004500Ful
#define SNDCTL_DSP_GETTRIGGER  0x80045010ul
#define SNDCTL_DSP_SETTRIGGER  0x40045010ul
#define SNDCTL_DSP_GETIPTR     0x800C5011ul
#define SNDCTL_DSP_GETOPTR     0x800C5012ul
#define SNDCTL_DSP_SETDUPLEX   0x00005016ul
#define SNDCTL_DSP_GETODELAY   0x80045017ul

#define AFMT_U8       0x00000008
#define AFMT_S16_LE   0x00000010

#define DSP_CAP_REALTIME 0x00000200
#define DSP_CAP_TRIGGER  0x00010000
#define PCM_ENABLE_OUTPUT 0x00000002

/* How much buffer we claim to have. Only GETOSPACE and GETBLKSIZE see it; the
 * real pacing is done by the FIFO's backpressure, or by dsp_pace() when there
 * is nothing on the other end. */
/* The modelled device buffer, in fragments: what GETOSPACE reports and how
 * far dsp_pace() lets the guest run ahead. Four 4096-byte fragments is 128 ms
 * at 32000 Hz stereo — the same lead the ALSA path allows (a 4096-frame
 * buffer), and half the viewer's 260 ms trim threshold. Eight put the guest
 * 256 ms ahead, a jitter away from being trimmed. */
#define DSP_FRAGS 4

static signed char g_dsp_of_fd[MAXFD];  /* 1 when this fd is /dev/dsp */
static signed char g_mlc_of_fd[MAXFD];  /* 1 when this fd is an LF1000 control node */
static signed char g_mem_of_fd[MAXFD];  /* 1 when this fd is /dev/mem */

static int  g_dsp_fifo = -1;            /* our end of the viewer's FIFO */
static u32  g_dsp_rate = 32000;
static u32  g_dsp_ch   = 2;
static u32  g_dsp_fmt  = AFMT_S16_LE;
static u32  g_dsp_frag = 4096;
static unsigned long long g_dsp_played; /* bytes accepted since t0 */
static unsigned long long g_dsp_total;  /* bytes accepted ever, for GETOPTR */
static long long g_dsp_t0_us;           /* 0 = not started */

static u32 dsp_bits(void)     { return g_dsp_fmt == AFMT_U8 ? 8u : 16u; }
static u32 dsp_byterate(void) { return g_dsp_rate * g_dsp_ch * (dsp_bits() / 8u); }

static int dsp_is(const char *p)
{
	return p && strcmp(p, "/dev/dsp") == 0;
}

/* The negotiated format, in the one line the viewer reads:
 * "rate channels bits period". Written on every change, because portaudio sets
 * them one ioctl at a time and the viewer must not open its device on a half
 * negotiated answer. */
static void dsp_publish(void)
{
	char path[512], line[64];
	int fd;

	if (!real_open || !g_dir[0])
		return;
	snprintf(path, sizeof(path), "%s/audio.fmt", g_dir);
	fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return;
	snprintf(line, sizeof(line), "%u %u %u %u\n",
	         g_dsp_rate, g_dsp_ch, dsp_bits(), g_dsp_frag / (g_dsp_ch * (dsp_bits() / 8u)));
	if (real_write)
		real_write(fd, line, strlen(line));
	if (real_close)
		real_close(fd);
}

static unsigned g_dsp_open_tries;
static void dsp_open_fifo(void)
{
	char path[512];

	if (g_dsp_fifo >= 0 || !real_open || !g_dir[0])
		return;
	/* BACK OFF WHEN NOBODY IS LISTENING, for the reason spelled out beside
	 * open_fifo() in tadpole_asound.c: a headless boot has no viewer and
	 * never will, and retrying on every write costs millions of syscalls.
	 * Eager for the first 32 tries so a viewer that is merely slow to scan
	 * does not cost the opening sound, then one in 64 forever. */
	{
		unsigned t = g_dsp_open_tries++;
		if (t >= 32 && (t & 0x3F))
			return;
	}
	snprintf(path, sizeof(path), "%s/audio.%d.dsp", g_dir, getpid());
	mkfifo(path, 0666);                     /* harmless if it exists */
	g_dsp_fifo = real_open(path, O_WRONLY | O_NONBLOCK, 0);
	if (g_dsp_fifo >= 0)
		g_dsp_open_tries = 0;               /* eager again after a reopen */
}

/* PACE TO REAL TIME ON EVERY WRITE, NOT ONLY WHEN NOBODY IS LISTENING.
 *
 * This ran only with no viewer, on the theory that with one the FIFO supplies
 * the backpressure: it fills, writes come back short, the retry loop in
 * dsp_write() waits. Measured on the Didj with a reader draining the FIFO:
 *
 *     bytes=480387072 over 10.00s -> 48038693 B/s   (128000 expected)
 *
 * 375 times real time. The viewer drains the pipe faster than real time BY
 * DESIGN — into a ring it trims back to its latency cap — so the pipe never
 * stays full, and the retry loop's 64 ms bound then drops whatever does not
 * fit. Brio's mixer thread therefore rendered the whole soundtrack as fast as
 * qemu could go, most of it was thrown away, and what reached the speaker
 * was a sampling of the song at fast-forward speed: "much too fast", which
 * sounds like a sample-rate mistake and is not one — audio.fmt and the
 * viewer both said 32000 Hz stereo throughout.
 *
 * tadpole_asound.c met exactly this on the ALSA path ("running EXTREMELY
 * FAST and sounds terrible") and its answer is taken here unchanged: model
 * the device. Bytes drain at the byte rate; hold the writer until what is in
 * flight fits one device buffer — DSP_FRAGS fragments, 128 ms at 32000 Hz
 * stereo with the 4096-byte fragments this guest never changes — and resync
 * rather than accumulate credit when the buffer drains, or a quiet passage
 * would earn the right to dump a burst afterwards. The same in-flight count
 * answers GETODELAY, so a guest that asks is told the truth. Sleeps are
 * bounded so a stopped clock cannot wedge the thread.
 *
 * TADPOLE_AUDIO_PACE=0 turns it off, as it does for ALSA (Options -> Audio ->
 * "Hold guest to realtime"), and TADPOLE_AUDIO_DEBUG=1 prints once a second
 * what the pacer saw, in the same shape as the ALSA line. */
static int g_dsp_pace_on = -1;
static int g_dsp_pace_dbg = -1;
static unsigned long long g_dsp_dbg_bytes, g_dsp_dbg_slept;
static long long g_dsp_dbg_t0;

static long long dsp_now_us(void)
{
	struct tad_timespec now;
	if (clock_gettime(CLOCK_MONOTONIC_, &now) != 0)
		return 0;
	return (long long)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* Bytes still queued in the modelled device at `now_us`; restarts the clock
 * when it has drained. */
static u32 dsp_inflight(long long now_us)
{
	unsigned long long played;
	u32 br = dsp_byterate();

	if (!br || !g_dsp_t0_us || now_us < g_dsp_t0_us)
		return 0;
	played = (unsigned long long)(now_us - g_dsp_t0_us) * br / 1000000ull;
	if (played >= g_dsp_played) {
		g_dsp_t0_us = now_us;
		g_dsp_played = 0;
		return 0;
	}
	return (u32)(g_dsp_played - played);
}

static void dsp_pace(u32 bytes)
{
	long long now;
	u32 br = dsp_byterate(), cap, inflight;
	int guard = 0;

	if (g_dsp_pace_on < 0) {
		const char *e = getenv("TADPOLE_AUDIO_PACE");
		g_dsp_pace_on = (e && e[0] == '0') ? 0 : 1;
	}
	if (!g_dsp_pace_on || !br)
		return;
	cap = g_dsp_frag * DSP_FRAGS;
	if (cap < 4096)
		cap = 4096;
	now = dsp_now_us();
	if (!now)
		return;                              /* no clock: do not throttle */
	if (!g_dsp_t0_us) {
		g_dsp_t0_us = now;
		g_dsp_played = 0;
	}
	for (;;) {
		inflight = dsp_inflight(now);
		if (inflight + bytes <= cap)
			break;
		{
			unsigned long long over = inflight + bytes - cap;
			long long us = (long long)(over * 1000000ull / br);
			struct tad_timespec nap;
			if (us < 1000) us = 1000;
			if (us > 100000) us = 100000;    /* never wedge the thread */
			nap.tv_sec  = (long)(us / 1000000);
			nap.tv_nsec = (long)((us % 1000000) * 1000);
			nanosleep(&nap, 0);
			g_dsp_dbg_slept += (unsigned long long)us;
		}
		now = dsp_now_us();
		if (!now || ++guard > 40)
			break;
	}
	g_dsp_played += bytes;
	g_dsp_total  += bytes;

	if (g_dsp_pace_dbg < 0) {
		const char *e = getenv("TADPOLE_AUDIO_DEBUG");
		g_dsp_pace_dbg = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	if (g_dsp_pace_dbg) {
		g_dsp_dbg_bytes += bytes;
		if (!g_dsp_dbg_t0)
			g_dsp_dbg_t0 = now;
		if (now && now - g_dsp_dbg_t0 >= 1000000) {
			char b[192];
			snprintf(b, sizeof(b),
			         "[tadpole] dsp pace[pid %d]: %llu B/s in (rate=%u ch=%u -> %u B/s"
			         " expected) cap=%u slept=%llu ms/s\n",
			         (int)getpid(),
			         g_dsp_dbg_bytes * 1000000ull / (unsigned long long)(now - g_dsp_dbg_t0),
			         g_dsp_rate, g_dsp_ch, br, cap, g_dsp_dbg_slept / 1000ull);
			note(b);
			g_dsp_dbg_bytes = 0; g_dsp_dbg_slept = 0; g_dsp_dbg_t0 = now;
		}
	}
}

static long dsp_write(const void *buf, size_t n)
{
	size_t done = 0;
	int tries = 0;

	dsp_open_fifo();
	while (g_dsp_fifo >= 0 && done < n && real_write) {
		long r = real_write(g_dsp_fifo, (const char *)buf + done, n - done);
		if (r > 0) {
			done += (size_t)r;
			tries = 0;
			continue;
		}
		/* Full pipe, or a reader that has gone away. Wait a little, but
		 * bounded: dropping audio is always better than stalling the guest,
		 * which is the same trade tadpole_asound.c makes. */
		if (++tries > 64)
			break;
		{
			struct tad_timespec s;
			s.tv_sec = 0;
			s.tv_nsec = 1000000;         /* 1 ms */
			nanosleep(&s, 0);
		}
	}
	if (done < n && g_dsp_fifo >= 0) {
		/* 64 ms without a byte accepted: either the viewer has stalled and
		 * the pipe is full, or it has gone and every write is EPIPE. No
		 * errno here to tell them apart, and no need: drop the fd. The
		 * next write reopens — at once if a reader is there, ENXIO and the
		 * no-viewer path if not — and whatever was in the pipe stays there
		 * for a reader that has merely paused. */
		if (real_close)
			real_close(g_dsp_fifo);
		g_dsp_fifo = -1;
	}
	if (g_debug) {
		static unsigned long nw, nb;
		nw++; nb += (unsigned long)n;
		if ((nw & 0xF) == 1) {
			char b[96];
			snprintf(b, sizeof(b), "[tadpole] dsp write #%lu, %lu bytes total%s\n",
			         nw, nb, g_dsp_fifo < 0 ? " (no viewer)" : "");
			dbg(b);
		}
	}
	dsp_pace((u32)n);                    /* with or without a reader: see above */
	/* ALWAYS CLAIM THE WHOLE BUFFER. A short write from an OSS device means
	 * something specific to portaudio and none of it is true here. */
	return (long)n;
}

/* -> 0 handled, -1 not ours. Unknown 'P' ioctls are ACCEPTED rather than
 * refused: this device does not exist, so there is nothing an honest ENOTTY
 * would be honest about, and portaudio turns any failure into a fatal
 * "Unanticipated host error". Logged under TADPOLE_DEBUG so a call that needs
 * a real answer shows up as one rather than as silence. */
static int dsp_ioctl(ulong req, void *arg)
{
	int *ip = (int *)arg;

	if (g_debug) {
		const char *n = "?";
		switch (req) {
		case SNDCTL_DSP_RESET: n = "RESET"; break;
		case SNDCTL_DSP_SYNC: n = "SYNC"; break;
		case SNDCTL_DSP_SPEED: n = "SPEED"; break;
		case SNDCTL_DSP_STEREO: n = "STEREO"; break;
		case SNDCTL_DSP_GETBLKSIZE: n = "GETBLKSIZE"; break;
		case SNDCTL_DSP_SETFMT: n = "SETFMT"; break;
		case SNDCTL_DSP_CHANNELS: n = "CHANNELS"; break;
		case SNDCTL_DSP_POST: n = "POST"; break;
		case SNDCTL_DSP_SETFRAGMENT: n = "SETFRAGMENT"; break;
		case SNDCTL_DSP_GETFMTS: n = "GETFMTS"; break;
		case SNDCTL_DSP_GETOSPACE: n = "GETOSPACE"; break;
		case SNDCTL_DSP_GETISPACE: n = "GETISPACE"; break;
		case SNDCTL_DSP_NONBLOCK: n = "NONBLOCK"; break;
		case SNDCTL_DSP_GETCAPS: n = "GETCAPS"; break;
		case SNDCTL_DSP_GETTRIGGER: n = "GETTRIGGER"; break;
		case SNDCTL_DSP_SETTRIGGER: n = "SETTRIGGER"; break;
		case SNDCTL_DSP_GETIPTR: n = "GETIPTR"; break;
		case SNDCTL_DSP_GETOPTR: n = "GETOPTR"; break;
		case SNDCTL_DSP_SETDUPLEX: n = "SETDUPLEX"; break;
		case SNDCTL_DSP_GETODELAY: n = "GETODELAY"; break;
		}
		{
			char b[96];
			snprintf(b, sizeof(b), "[tadpole] dsp %s(%08lx) arg=%d\n",
			         n, req, ip ? *ip : -1);
			dbg(b);
		}
	}

	switch (req) {
	case SNDCTL_DSP_RESET:
	case SNDCTL_DSP_SYNC:
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_NONBLOCK:
	case SNDCTL_DSP_SETDUPLEX:
	case SNDCTL_DSP_SUBDIVIDE:
		return 0;

	/* The three that matter, and we accept whatever we are told rather than
	 * negotiating: there is no hardware to disagree with, and the viewer
	 * opens its own device to match via audio.fmt. */
	case SNDCTL_DSP_SPEED:
		if (ip && *ip > 0) { g_dsp_rate = (u32)*ip; dsp_publish(); }
		return 0;
	case SNDCTL_DSP_CHANNELS:
		if (ip && *ip > 0) { g_dsp_ch = (u32)*ip; dsp_publish(); }
		return 0;
	case SNDCTL_DSP_STEREO:
		if (ip) { g_dsp_ch = *ip ? 2u : 1u; dsp_publish(); }
		return 0;

	case SNDCTL_DSP_SETFMT:
		if (ip) {
			if (*ip == AFMT_U8 || *ip == AFMT_S16_LE)
				g_dsp_fmt = (u32)*ip;
			else
				*ip = (int)(g_dsp_fmt = AFMT_S16_LE);
			dsp_publish();
		}
		return 0;
	case SNDCTL_DSP_GETFMTS:
		if (ip) *ip = AFMT_U8 | AFMT_S16_LE;
		return 0;

	/* arg is (max_fragments << 16) | log2(fragment bytes). */
	case SNDCTL_DSP_SETFRAGMENT:
		if (ip) {
			u32 sz = 1u << ((u32)*ip & 0xFFFFu);
			if (sz >= 64 && sz <= (1u << 20)) {
				g_dsp_frag = sz;
				dsp_publish();
			}
		}
		return 0;
	case SNDCTL_DSP_GETBLKSIZE:
		if (ip) *ip = (int)g_dsp_frag;
		return 0;

	case SNDCTL_DSP_GETCAPS:
		/* No MMAP and no DUPLEX on purpose: both would have portaudio take
		 * paths that want a real device underneath. */
		if (ip) *ip = DSP_CAP_TRIGGER | DSP_CAP_REALTIME;
		return 0;

	case SNDCTL_DSP_SETTRIGGER:
		return 0;
	case SNDCTL_DSP_GETTRIGGER:
		if (ip) *ip = PCM_ENABLE_OUTPUT;
		return 0;

	/* audio_buf_info { fragments, fragstotal, fragsize, bytes } — always
	 * empty ON PURPOSE, though the pacer knows better. portaudio's OSS host
	 * polls this before each write and its poll() on our fd — a regular
	 * placeholder file — returns at once, so a truthful "one fragment free"
	 * would have it spin a whole fragment's worth of time asking again. An
	 * empty buffer lets it write, and write() then holds it to real time,
	 * which is how a real driver with room feels from the outside. */
	case SNDCTL_DSP_GETOSPACE:
		if (arg) {
			int *b = (int *)arg;
			b[0] = DSP_FRAGS;
			b[1] = DSP_FRAGS;
			b[2] = (int)g_dsp_frag;
			b[3] = (int)(g_dsp_frag * DSP_FRAGS);
		}
		return 0;
	case SNDCTL_DSP_GETISPACE:
		if (arg) {
			int *b = (int *)arg;
			b[0] = b[1] = b[2] = b[3] = 0;
		}
		return 0;

	case SNDCTL_DSP_GETODELAY:
		if (ip) *ip = (int)dsp_inflight(dsp_now_us());   /* what the pacer holds */
		return 0;
	/* count_info { bytes, blocks, ptr } */
	case SNDCTL_DSP_GETOPTR:
	case SNDCTL_DSP_GETIPTR:
		if (arg) {
			int *c = (int *)arg;
			c[0] = (int)(u32)g_dsp_total;
			c[1] = 0;
			c[2] = 0;
		}
		return 0;

	default:
		if (g_debug) {
			char b[80];
			snprintf(b, sizeof(b),
			         "[tadpole] dsp: unhandled ioctl %08lx, accepted\n", req);
			dbg(b);
		}
		return 0;
	}
}

static int open_common(const char *path, int flags, int mode)
{
	char real[320];
	int idx, fd;

	init();

	/* A DAEMON'S /dev/null IS WHERE ITS OWN ACCOUNT OF ITSELF GOES.
	 *
	 * VideoDaemon narrates every decision it makes — "Creating Display
	 * Surface", "Starting Video", "UI is ready!! Stopping video early!",
	 * "Got a power down!!" — and then daemonizes onto /dev/null, so the one
	 * process whose reasoning we needed was the one process that could not be
	 * heard. Under TADPOLE_LOG, hand it our log file instead of the bit
	 * bucket and the narration comes back.
	 *
	 * Writing opens only: something that reads /dev/null wants zero bytes,
	 * and giving it a log file would hand it our own output. */
	if (g_logpfx[0] && g_debug && path && (flags & 3) != O_RDONLY &&
	    strncmp(path, "/dev/null", 10) == 0) {
		char lp[320];
		snprintf(lp, sizeof(lp), "%s.%d.log", g_logpfx, getpid());
		fd = real_open(lp, O_RDWR | O_CREAT | O_APPEND, 0666);
		if (fd >= 0) {
			dbg("[tadpole] /dev/null -> this log (guest daemonizing)\n");
			return fd;
		}
	}

	if (path && !strcmp(path, "/dev/mem")) {
		snprintf(real, sizeof(real), "%s/fb0.bin", g_dir);
		fd = real_open(real, O_RDWR, 0666);
		if (fd >= 0 && fd < MAXFD)
			g_mem_of_fd[fd] = 1;
		if (g_debug) dbg("[tadpole] open /dev/mem -> the framebuffer arena\n");
		return fd;
	}

	if ((idx = fb_index(path)) >= 0) {
		/* all layers share one arena — see the note in init() */
		snprintf(real, sizeof(real), "%s/fb0.bin", g_dir);
		fd = real_open(real, O_RDWR, 0666);
		if (fd >= 0 && fd < MAXFD)
			g_fb_of_fd[fd] = (signed char)idx;
		if (g_debug) { dbg("[tadpole] open "); dbg(path); dbg("\n"); }
		return fd;
	}

	if ((idx = ev_index(path)) >= 0) {
		if (idx >= g_ev_count) {
			if (g_debug) {
				char b[80];
				snprintf(b, sizeof(b),
				         "[tadpole] REFUSED %s (this device has %d)\n",
				         path, g_ev_count);
				dbg(b);
			}
			return -1;                       /* no such device HERE */
		}
		/* NOT the FIFO itself — a private pipe fed from it. Handing the
		 * shared FIFO to each caller made concurrent readers steal each
		 * other's events; see the note beside g_ev_fifo. */
		fd = ev_open(idx);
		if (fd >= 0 && fd < MAXFD)
			g_ev_of_fd[fd] = (signed char)idx;
		if (g_debug) {
			char b[120];
			snprintf(b, sizeof(b), "[tadpole] open %s -> %s = fd %d\n",
			         path, g_ev[idx].name, fd);
			dbg(b);
		}
		return fd;
	}

	/* /dev/videoN — the fake USB camera, when the device has one. */
	if (tad_cam_match(path))
		return tad_cam_open(path, g_dir, real_open,
		                    (void *(*)(void *, size_t, int, int, int, long))real_mmap);

	/* SYSROOT FIRST FOR EVERY ABSOLUTE PATH, not only for creating opens.
	 *
	 * This was gated on O_CREAT because the symptom that prompted it was "qemu
	 * will not create files inside -L". The gate is too narrow, and the reason
	 * is the same qemu behaviour seen from the other end: `-L` is a SNAPSHOT.
	 * init_paths() walks the sysroot once at startup and every later lookup is
	 * answered from that tree, so a file the guest creates during the run is
	 * invisible to translation for the rest of the run — and reading it back
	 * resolves against the HOST root and fails with ENOENT.
	 *
	 * Creating a file and then opening it again is not an exotic sequence. It
	 * is what Cooking! Recipes on the Road does on its very first launch, and
	 * it cost the whole title:
	 *
	 *     clearInit()          creates SAVE.DAT (O_CREAT -> translated, fine)
	 *     DSLIBI_OpenFile()    open(SAVE.DAT, O_RDONLY) -> NOT translated
	 *                          -> ENOENT -> returns 0
	 *     Backup::read()       -> errorCallback -> PanicScreen -> spin forever
	 *
	 * So the title could only ever have worked on its SECOND run, once the card
	 * was old enough to be in the snapshot. Answering every absolute path from
	 * the sysroot first, and falling back to the literal path, removes the whole
	 * class — stat/lstat/access carry the identical rule for the identical
	 * reason.
	 *
	 * TADPOLE_DIR is excluded: it is a host path shared with the viewer, and the
	 * framebuffer and event nodes above already resolve into it by name. */
	if (path && path[0] == '/' && g_sysroot[0] && !under_gdir(path)) {
		char full[512];
		int fd;
		snprintf(full, sizeof(full), "%s%s", g_sysroot, path);
		fd = real_open(full, flags, mode);
		if (fd >= 0) {
			if (g_debug) { dbg("[tadpole] open(sysroot) "); dbg(path); dbg("\n"); }
			screen_note(path);
			io_pace(path);
			return fd;
		}
	}
	{
		int fd = real_open(path, flags, mode);
		if (fd >= 0) { screen_note(path); io_pace(path); }
		return fd;
	}
}

/* Tag a freshly opened /dev/dsp. Called from open()/open64() rather than from
 * open_common(), because open_common has four different returns and the fd is
 * a real one on the placeholder file in every case — there is nothing to
 * decide, only something to remember. */

/* ---- /dev/mlc, /dev/dpc, /dev/gpio — the LF1000 display controller -------
 *
 * These carry no pixels. libDisplay.so opens them, asks them about the panel,
 * and asserts on the first question it cannot get an answer to:
 *
 *     !ASSERT: [5] DisplayModule::InitModule: failed to open GPIO device
 *     !ASSERT: [5] DisplayModule::GetScreenSize: ioctl failed
 *
 * THE MAGIC IS 'm', THE SAME AS THE FRAMEBUFFER'S. The LF1000FB_* numbers at
 * the top of this file come from include/linux/lf1000/lf1000fb.h and run
 * _IO*('m', 1..6); the MLC control node continues the same series. So these
 * are not a separate ABI to discover, they are the rest of one we already had
 * half of.
 *
 * WHAT IS NOT KNOWN is the exact meaning of each number beyond 6, because the
 * header is not in the firmware and nobody here has the LF1000 kernel source.
 * What IS known is the shape — _IOR('m', n, int) asks a question with a 4-byte
 * answer — and what the answers have to be, because the panel is 320x240 and
 * that is not in doubt. So an unrecognised read is answered with the panel's
 * geometry rather than refused, and every write is accepted: a display that
 * reports the right size and shrugs at everything else is much closer to the
 * truth than one that cannot be opened.
 */
#define MLC_IOC_MAGIC 0x6du             /* 'm' */
#define MLC_GETSCREENSIZE 0x80046d07ul  /* _IOR('m', 7, int) — measured */
/* NAMED, NOT NUMBERED, because usr/bin/imager told us what they are: it prints
 * "get_address ioctl failed" for the first and "get_fbsize ioctl failed" for
 * the second, and both are _IO — the answer comes back as the RETURN VALUE,
 * not in a buffer. */
#define MLC_GET_ADDRESS 25              /* _IO('m', 25) -> physical base */
#define MLC_GET_FBSIZE  29              /* _IO('m', 29) -> bytes         */
#define MLC_GET_RECT    14              /* _IOR('m', 14) -> the four-word rect */

/* `handled` RATHER THAN A SENTINEL RETURN, because one of the answers is an
 * ADDRESS. 0x82000000 is negative as a signed int, and a caller testing
 * "r >= 0" throws it away — which is exactly what happened, and it looked like
 * the ioctl was unimplemented. Linux itself only treats [-4095, -1] as errors
 * for this reason; anything else, however negative, is a value. */
static int mlc_ioctl_idx(ulong req, void *arg, int idx, int *handled)
{
	u32 *p = (u32 *)arg;
	u32 dir = (u32)(req >> 30);
	u32 type = (u32)((req >> 8) & 0xFF);

	*handled = 0;
	if (type != MLC_IOC_MAGIC)
		return -1;                       /* not ours; let it fall through */
	*handled = 1;

	/* The panel, packed the way GetScreenSize wants it: width in the high
	 * half, height in the low. 320x240 for every Didj — all eight of its boot
	 * screens in var/screens are that size. */
	if (req == MLC_GETSCREENSIZE) {
		if (p) *p = (g_h << 16) | (g_w & 0xFFFF);
		return 0;
	}
	/* THE TWO QUERIES InitModule MAKES OF A LAYER, and they are _IO rather
	 * than _IOR: the argument is 0 and the answer comes back as the ioctl's
	 * RETURN VALUE. Returning 0 to both is what produced
	 *
	 *     !ASSERT: [5] DisplayModule::InitModule: MLC layer ioctl failed
	 *
	 * on two calls that had each returned "success" — libDisplay was reading
	 * the answer, not the status.
	 *
	 * Which is which was settled by the Leapster GS, whose libDisplay is the
	 * same code against fbdev and prints what it found:
	 *
	 *     InitModule: Mapped 82000000 to 0x82000000, size 00258000
	 *
	 * at 320x240 — and 0x258000 is exactly w*h*4*NBUF, this shim's arena for
	 * one plane. So one query is the plane's physical base and the other its
	 * length, and both answers are ones we already compute for /dev/fb0..2.
	 * The base is never dereferenced: the guest mmaps the fd. */
	/* NOT GATED ON arg BEING NULL. These are _IO: there is no third argument
	 * at all, so what va_arg hands back is whatever was in the register — 0
	 * from one caller and rubbish from the next. Keying on it made the same
	 * ioctl answer get_fbsize for AppManager and fall through to the generic
	 * "accepted" for imager, which then mmapped zero bytes and failed with
	 * EINVAL. The request number is the whole of the question. */
	if (dir == 0) {
		int v = -12345;
		if ((req & 0xFF) == MLC_GET_ADDRESS)
			/* ZERO, and that is the whole trick. The answer is an offset
			 * the caller then mmaps out of this same layer fd, and this
			 * shim's arena starts at 0. Handing back the hardware's real
			 * 0x82000000 makes every caller map past the end of the file
			 * and take SIGBUS on the first store. */
			v = (int)g_mlc_q25;
		else if ((req & 0xFF) == MLC_GET_FBSIZE)
			v = (int)(g_w * g_h * (g_bpp / 8) * NBUF);
		if (v != -12345) {
			if (g_debug) {
				char b[96];
				snprintf(b, sizeof(b),
				         "[tadpole] mlc: layer%d nr=%lu -> %d (0x%08x)\n",
				         idx, req & 0xFF, v, (u32)v);
				dbg(b);
			}
			return v;
		}
	}
	/* GEOMETRY IS A RECTANGLE, NOT A SIZE — {left, top, bottom, right}.
	 *
	 * _IOR('m', 14) fills four words, and the layout is not guessed: it was
	 * read out of usr/bin/imager, which the device ships and which does
	 *
	 *     width  = buf[3] - buf[0] + 1
	 *     height = buf[2] - buf[1] + 1
	 *
	 * before comparing them with the PNG it was handed and refusing with
	 * "Image dimensions don't match screen". Four packings of a width and a
	 * height had already failed against that test; the disassembly said why,
	 * which is that it was never a width and a height. This is the kernel's
	 * mlc_GetLayerInvisibleArea shape — the strings are in the Didj's own
	 * kernel image. Inclusive bounds, hence the -1. */
	if (dir & 2) {                       /* _IOR: it wants an answer */
		if (p) {
			p[0] = 0;            /* left   */
			p[1] = 0;            /* top    */
			p[2] = g_w - 1;      /* right  */
			p[3] = g_h - 1;      /* bottom */
		}
		if (g_debug) {
			char b[80];
			snprintf(b, sizeof(b),
			         "[tadpole] mlc: read ioctl %08lx answered with %ux%u\n",
			         req, g_w, g_h);
			dbg(b);
		}
		return 0;
	}
	if (g_debug) {
		char b[96];
		snprintf(b, sizeof(b),
		         "[tadpole] mlc: layer%d nr=%lu arg=%lu -> %d\n",
		         idx, req & 0xFF, (ulong)arg, (int)g_mlc_dflt);
		dbg(b);
	}
	/* NOT 0. Every _IO('m', n) with a null argument seen so far has been a
	 * QUERY whose answer is the return value, and zero reads as "nothing" to
	 * all of them — libDisplay took a 0 for a buffer address and dereferenced
	 * it. A small positive number is the answer that got past nr=25, and it
	 * is a better default than a value the caller treats as failure. */
	return (int)g_mlc_dflt;
}

static int dsp_note_open(const char *path, int fd)
{
	if (fd >= 0 && fd < MAXFD && dsp_is(path)) {
		g_dsp_of_fd[fd] = 1;
		/* THE VIEWER GOING AWAY MUST NOT TAKE THE GUEST WITH IT. The audio
		 * FIFO's write end is ours and its read end is the viewer's; a
		 * write with no reader left raises SIGPIPE, whose default is a
		 * silent death — the guest simply stopped, with no signal line in
		 * the log, first seen when a measurement script closed the FIFO.
		 * A process that has opened /dev/dsp ignores it from here on and
		 * gets EPIPE back instead, which dsp_write() survives. Scoped to
		 * such processes: nothing else in the guest is exposed. */
		if (real_signal)
			real_signal(13 /* SIGPIPE */, (void (*)(int))1 /* SIG_IGN */);
		dsp_publish();
		dsp_open_fifo();
		if (g_debug) dbg("[tadpole] open /dev/dsp\n");
	}
	if (fd >= 0 && fd < MAXFD && mlc_is(path)) {
		g_mlc_of_fd[fd] = 1;
		if (g_debug) { dbg("[tadpole] open "); dbg(path); dbg("\n"); }
	}
	return fd;
}

/* uClibc's stdio calls its own open through a hidden alias that never goes
 * through the PLT, so interposing open() cannot see fopen(). Intercept fopen
 * as well, or nothing that uses C stdio can create files in the sysroot. */
static void *fopen_common(const char *path, const char *mode,
                          void *(*real)(const char *, const char *))
{
	init();
	if (real && path && path[0] == '/' && g_sysroot[0] && mode &&
	    !under_gdir(path)) {
		char full[512];
		void *f;
		snprintf(full, sizeof(full), "%s%s", g_sysroot, path);
		f = real(full, mode);
		if (f) {
			if (g_debug) { dbg("[tadpole] fcreate "); dbg(full); dbg("\n"); }
			return f;
		}
	}
	return real ? real(path, mode) : 0;
}

void *fopen(const char *path, const char *mode)
{
	return fopen_common(path, mode, real_fopen);
}

void *fopen64(const char *path, const char *mode)
{
	return fopen_common(path, mode, real_fopen64 ? real_fopen64 : real_fopen);
}

/* SAY WHEN A dlopen FAILS. Debug builds only.
 *
 * Brio loads its modules by hand — libModuleMPI scans /LF/Base/Brio/Module/
 * and dlopen()s what it finds — so a module that cannot be loaded does not
 * produce a link error at startup. It produces a working program with one
 * capability silently absent, which is a much worse thing to debug. libVideo.so
 * alone pulls in libtheora, libogg and four libav libraries; any one of them
 * missing takes the whole video path out with nothing said.
 *
 * NOTE: calling dlerror() CONSUMES the error, so a caller that checks it after
 * us sees none. That is why this is inside `if (g_debug)` and stays there. */
void *dlopen(const char *path, int flags)
{
	void *h;

	init();
	if (!real_dlopen) {
		if (g_debug) dbg("[tadpole] dlopen: no real dlopen to chain to\n");
		return 0;
	}
	h = real_dlopen(path, flags);
	/* A NATIVE TITLE ARRIVES HERE AND NOWHERE ELSE. CAppManager::LoadNewApp
	 * dlopen()s the package's App.so, and the guest's own loader then opens
	 * the file with raw syscalls that never reach our open() — so this is the
	 * only place the start of a native title is visible. */
	if (h) screen_note(path);
	if (g_debug && !h) {
		const char *e = real_dlerror ? real_dlerror() : 0;
		dbg("[tadpole] dlopen FAILED ");
		dbg(path ? path : "(self)");
		dbg(": ");
		dbg(e ? e : "(no dlerror)");
		dbg("\n");
	} else if (g_debug && path) {
		dbg("[tadpole] dlopen ");
		dbg(path);
		dbg("\n");
	}
	return h;
}

/* tslib's input.so is dlopen'd and its PLT entry for read() resolves to NULL
 * in that scope — calling it jumps to PC=0 the moment a touch event arrives,
 * which is what made every click crash. (Confirmed by mapping the faulting
 * call target back to .rel.plt entry 4 = "read".) Defining read() here means
 * the symbol resolves against us, early in the global scope, instead. */
/* Same qemu-user trap as open(O_CREAT): -L only redirects paths that already
 * exist, so a rename whose DESTINATION does not exist yet falls through to the
 * host and fails. Brio's CAtomicFile writes "<name>.atomic" then renames it
 * into place, so without this every atomic write is left stranded as a
 * .atomic file — which is exactly how /tmp/ui_ready.atomic got stuck. */
static void sysrootify(char *dst, unsigned dstsz, const char *path)
{
	if (path && path[0] == '/' && g_sysroot[0])
		snprintf(dst, dstsz, "%s%s", g_sysroot, path);
	else
		snprintf(dst, dstsz, "%s", path ? path : "");
}

/* chdir() — qemu-user does NOT path-translate this one.
 *
 * `-L` rewrites paths for open/stat/etc, but qemu's TARGET_NR_chdir passes the
 * guest string straight to the host chdir() with no translation at all. So a
 * guest doing chdir("/LF/Bulk/ProgramFiles/<pkg>") lands on a host path that
 * does not exist, the call fails, and the working directory silently stays
 * wherever tadpole.sh left it.
 *
 * That matters because Leapster games chdir into their own package directory
 * and then open assets RELATIVELY:
 *
 *     open ./res/Sound/Ben10.soundproject failed: No such file or directory
 *     -> SoundProject::loadFromFile() gets no document
 *     -> TiXmlNode::FirstChildElement() dereferences NULL -> SIGSEGV
 *
 * The file is present and correct; only the working directory was wrong. Try
 * the sysroot-relative path first and fall back to the literal one, so guest
 * absolute paths work while anything already host-valid keeps working.
 */
int chdir(const char *path)
{
	char buf[512];

	init();
	if (!real_chdir)
		return -1;
	if (path && path[0] == '/' && g_sysroot[0]) {
		sysrootify(buf, sizeof(buf), path);
		if (real_chdir(buf) == 0) {
			dbg("[tadpole] chdir -> sysroot\n");
			return 0;
		}
	}
	return real_chdir(path);
}

/* execve() — THE ONE PATH qemu-user's -L DOES NOT TRANSLATE.
 *
 * `-L` is not a chroot. It rewrites paths for the process qemu launched, so
 * stat("/LF/Base/...") succeeds, but an exec is handed to the HOST kernel with
 * the guest's path and the host has no /LF:
 *
 *     stat64(".../runtime/sysroot/LF/.../MainPicker")            = 0
 *     execve("/LF/Base/Qt/Modules/MainPicker/MainPicker")        = -1 ENOENT
 *
 * The LeapPad2 port never hit this because AppManager is one process.
 * AppServer is not: it launches every screen as a child, so the Ultra's home
 * screen died instantly and AppServer relaunched it forever —
 *
 *     Application: '.../MainPicker' crashed, exit=0, error=0
 *
 * — which reads like the application failing rather than the exec never
 * happening. docs/device-deps.md predicted this and proposed binfmt_misc,
 * which needs root. Doing it here needs nothing: we already intercept, we
 * know the sysroot, and re-entering qemu explicitly is what binfmt_misc would
 * have arranged anyway.
 *
 * Rewrites  execve("/LF/x", argv, envp)
 * into      execve(qemu, {qemu, "-L", sysroot, "<sysroot>/LF/x", argv...}, envp)
 *
 * TADPOLE_QEMU says which qemu; tadpole.sh exports it. Without it, or for a
 * path that already resolves on the host, nothing is changed.
 */
int execve(const char *path, char *const argv[], char *const envp[])
{
	char real[512];
	const char *qemu;
	char *newargv[64];
	int i, n = 0;

	init();
	if (!real_execve)
		return -1;

	dbg("[tadpole] execve hook entered\n");
	qemu = getenv("TADPOLE_QEMU");
	if (!path || path[0] != '/' || !g_sysroot[0] || !qemu || !qemu[0]) {
		dbg("[tadpole] execve: pass through (no qemu/sysroot, or relative)\n");
		return real_execve(path, argv, envp);
	}
	/* DO NOT ASK access() WHETHER THE HOST CAN RUN IT. access IS one of the
	 * calls qemu-user translates, so access("/LF/...") succeeds through the
	 * sysroot for exactly the paths execve cannot run — the test says "this
	 * already works on the host" about every guest binary, and passes them
	 * all straight through. That mistake cost a rebuild to spot.
	 *
	 * The only host binary we ever exec is qemu itself, on the re-entry
	 * below, so name it explicitly and translate everything else. */
	{
		const char *a = path, *b = qemu;
		while (*a && *a == *b) { a++; b++; }
		if (!*a && !*b) {
			dbg("[tadpole] execve: pass through (this is qemu)\n");
			return real_execve(path, argv, envp);
		}
	}

	sysrootify(real, sizeof(real), path);
	if (real_access && real_access(real, 0 /*F_OK*/) != 0) {
		dbg("[tadpole] execve: no guest binary at that path\n");
		return real_execve(path, argv, envp);
	}

	newargv[n++] = (char *)qemu;
	/* -strace DOES NOT SURVIVE THE RE-ENTRY UNLESS WE CARRY IT.
	 *
	 * TADPOLE_STRACE=1 puts -strace on the qemu that tadpole.sh launches, and
	 * that is the only one it reaches: everything AppServer starts as a child
	 * comes back through here and gets a fresh, untraced qemu. So a trace of
	 * "the guest" silently covers the shell and none of the titles.
	 *
	 * That is not a small gap. Chasing a null dereference inside BrioWrapper,
	 * the trace showed no syscalls from it at all — which reads like a thread
	 * that computes and never calls, and was really a process nobody was
	 * watching.
	 *
	 * tadpole.sh passes the variable through as -E when tracing, so the guest
	 * environment carries it and every generation re-adds the flag. */
	if (getenv("TADPOLE_STRACE"))
		newargv[n++] = (char *)"-strace";
	/* TADPOLE_GDB_MATCH=<substring> TADPOLE_GDB_PORT=<port> — WAIT FOR gdb,
	 * BUT ONLY FOR THE ONE BINARY BEING DEBUGGED.
	 *
	 * qemu's -g makes the guest stop before its first instruction and wait for
	 * a debugger. Applying that to every re-entry would hang the first child
	 * AppServer launches — MainPicker — and the home screen would never
	 * appear, so the title under investigation could never be started. Match
	 * on the path instead: only BrioWrapper waits, and everything around it
	 * runs normally.
	 *
	 *     TADPOLE_GDB_MATCH=BrioWrapper TADPOLE_GDB_PORT=1234 ./tadpole.sh --boot
	 *     gdb -ex 'set architecture arm' -ex 'target remote :1234'
	 */
	{
		const char *m = getenv("TADPOLE_GDB_MATCH");
		const char *p = getenv("TADPOLE_GDB_PORT");
		if (m && *m && p && *p) {
			const char *h = path;
			size_t ml = strlen(m);
			int hit = 0;
			for (; *h; h++)
				if (strncmp(h, m, ml) == 0) { hit = 1; break; }
			if (hit) {
				newargv[n++] = (char *)"-g";
				newargv[n++] = (char *)p;
				dbg("[tadpole] execve: waiting for gdb on this one\n");
			}
		}
	}
	/* TADPOLE_NO_TSLIB=<substring> — TURN TSLIB OFF FOR BRIO, PROCESS BY
	 * PROCESS, BECAUSE THE SHELL AND THE TITLES WANT OPPOSITE ANSWERS.
	 *
	 * On a Qt device the shell has exactly one mouse driver, the tslib plugin,
	 * so tslib MUST initialise or nothing on the home screen can be touched.
	 * A Brio title wants the reverse: when tslib initialises, Brio takes the
	 * tslib path ("LoadTSLib: use_tslib=1") and the taps go nowhere; when it
	 * fails, Brio falls back to reading the touchscreen itself ("Falling back
	 * on touchscreen interface") and they land. The LeapPad2 gets that for
	 * free because tadpole.sh sabotages TSLIB_CONFFILE globally there — it has
	 * no Qt to keep happy.
	 *
	 * One environment cannot be both, and they are separate processes, so the
	 * choice has to be made at the exec: the shell keeps /etc/ts.conf and
	 * whatever it launches whose path matches gets a file that is not there.
	 * Which binary that is belongs to the device, not here — leappad3.conf
	 * names BrioWrapper in DEV_ENV.
	 */
	{
		const char *m = getenv("TADPOLE_NO_TSLIB");
		if (m && *m) {
			const char *h = path;
			size_t ml = strlen(m);
			for (; *h; h++)
				if (strncmp(h, m, ml) == 0) {
					newargv[n++] = (char *)"-E";
					newargv[n++] = (char *)"TSLIB_CONFFILE=/nonexistent-ts.conf";
					dbg("[tadpole] execve: tslib off for this one\n");
					break;
				}
		}
	}
	newargv[n++] = (char *)"-L";
	newargv[n++] = g_sysroot;
	newargv[n++] = real;
	/* argv[0] is the guest's own name for itself and is dropped: qemu takes
	 * the binary path as its first non-option argument and passes the rest
	 * through. */
	for (i = 1; argv && argv[i] && n < (int)(sizeof(newargv)/sizeof(newargv[0])) - 1; i++)
		newargv[n++] = argv[i];
	newargv[n] = 0;

	dbg("[tadpole] execve -> re-entering qemu for the guest path\n");
	return real_execve(qemu, newargv, envp);
}

/* execv() AND execvp() TOO — AND THIS IS THE PART THAT MATTERS.
 *
 * Overriding execve alone changed nothing, and the strace said why: the hook
 * was never entered, yet an execve syscall still went out. uClibc's execv and
 * execvp reach execve through an INTERNAL binding, not through the PLT, so
 * interposing the public execve does not catch a caller that used one of the
 * wrappers. Qt's QProcess uses the wrappers.
 *
 * These are the public entry points, so they are interposable, and routing
 * them through our execve above gets the path translation for free.
 *
 * environ is the guest's own, which is what execv/execvp pass on.
 */
extern char **environ;

int execv(const char *path, char *const argv[])
{
	return execve(path, argv, environ);
}

/* PATH CONTAINS GUEST DIRECTORIES, SO THE SEARCH IS OURS TO DO.
 *
 * This used to hand a bare name straight to uClibc's execvp, on the reasoning
 * that "anything to be found on PATH is a host command". True for a LeapPad2;
 * false for a Qt device, where PATH is the guest's own —
 * /LF/Base/Brio/bin:/LF/Base/bin:/LF/Base/Qt/bin:... — and /LF/Base/Qt/bin
 * holds a symlink for every launchable module.
 *
 * Delegating loses it in a way that leaves no trace in our own log: uClibc's
 * execvp walks PATH and calls its INTERNAL execve, which never goes through
 * the PLT, so our hook is never entered and each attempt reaches the host
 * kernel as a raw guest path. Only qemu's -strace shows it:
 *
 *     execve("/LF/Base/Qt/bin/BrioWrapper", {"BrioWrapper",NULL})
 *     execve("/usr/bin/BrioWrapper", ...)  = -1 errno=2
 *
 * — the right directory IS searched, and the exec fails anyway. AppServer
 * reports "Application: 'BrioWrapper' crashed, exit=0, error=0", where error=0
 * is QProcess::FailedToStart, and that one line is the whole visible symptom
 * of no app launching, no sign-in screen and no parent settings.
 *
 * So walk PATH here and hand each candidate to our own execve(), which
 * re-roots it under the sysroot and re-enters qemu. Same order and same
 * semantics as the C library's: the first candidate that execs wins, and we
 * return only if none did.
 */
int execvp(const char *file, char *const argv[])
{
	const char *path, *p;
	char cand[512];
	size_t flen, n, i;

	if (!file || !file[0])
		return -1;
	/* A name containing a slash is used as-is — no PATH search, as per the
	 * C library. */
	for (i = 0; file[i]; i++)
		if (file[i] == '/')
			return execve(file, argv, environ);
	init();

	path = getenv("PATH");
	if (!path || !path[0])
		path = "/bin:/usr/bin";
	flen = strlen(file);

	for (p = path; ; ) {
		const char *q = p;
		while (*q && *q != ':')
			q++;
		n = (size_t)(q - p);
		if (n == 0) {                  /* empty element means "." */
			cand[0] = '.';
			n = 1;
		} else if (n < sizeof(cand) - 2) {
			for (i = 0; i < n; i++)
				cand[i] = p[i];
		} else {
			n = 0;                     /* too long to try */
		}
		if (n && n + 1 + flen < sizeof(cand)) {
			if (cand[n - 1] != '/')
				cand[n++] = '/';
			for (i = 0; i <= flen; i++)
				cand[n + i] = file[i];
			execve(cand, argv, environ);   /* returns only on failure */
		}
		if (!*q)
			break;
		p = q + 1;
	}
	return -1;
}

/* mkstemp() creates a brand-new file, so it hits the same qemu-user trap as
 * open(O_CREAT): -L only redirects paths that already exist. Brio's
 * fopenAtomic() uses it for every atomic write, so without this you get
 *   fopenAtomic(/LF/Bulk/settings.cfg): mkstemp failed us!
 * and AppManager shuts down as soon as it needs to persist anything.
 *
 * The template is modified in place and the caller later renames it, so we
 * must hand back the GUEST path with the resolved XXXXXX suffix, not the
 * host path. Our rename() then translates it again. */
static int mkstemp_common(char *tmpl, int suffixlen, int have_suffix)
{
	char full[512];
	unsigned rootlen;
	int fd;

	init();
	if (!tmpl)
		return -1;

	if (tmpl[0] == '/' && g_sysroot[0]) {
		snprintf(full, sizeof(full), "%s%s", g_sysroot, tmpl);
		if (have_suffix)
			fd = real_mkstemps ? real_mkstemps(full, suffixlen) : -1;
		else if (real_mkstemp)
			fd = real_mkstemp(full);
		else
			fd = real_mkstemp64 ? real_mkstemp64(full) : -1;
		if (fd >= 0) {
			rootlen = strlen(g_sysroot);
			memcpy(tmpl, full + rootlen, strlen(full) - rootlen + 1);
			if (g_debug) { dbg("[tadpole] mkstemp "); dbg(tmpl); dbg("\n"); }
			return fd;
		}
	}
	if (have_suffix)
		return real_mkstemps ? real_mkstemps(tmpl, suffixlen) : -1;
	if (real_mkstemp)
		return real_mkstemp(tmpl);
	return real_mkstemp64 ? real_mkstemp64(tmpl) : -1;
}

/* libUtility.so imports mkstemp64, NOT mkstemp — exporting only the plain
 * name means the interception is never reached. Provide both. */
int mkstemp(char *tmpl)                { return mkstemp_common(tmpl, 0, 0); }
int mkstemp64(char *tmpl)              { return mkstemp_common(tmpl, 0, 0); }
int mkstemps(char *tmpl, int suffixlen){ return mkstemp_common(tmpl, suffixlen, 1); }

int rename(const char *from, const char *to)
{
	char f[512], t[512];
	init();
	if (!real_rename)
		return -1;
	if (from && to && from[0] == '/' && to[0] == '/' && g_sysroot[0]) {
		sysrootify(f, sizeof(f), from);
		sysrootify(t, sizeof(t), to);
		if (real_rename(f, t) == 0) {
			if (g_debug) { dbg("[tadpole] rename "); dbg(to); dbg("\n"); }
			return 0;
		}
	}
	return real_rename(from, to);
}

int unlink(const char *path)
{
	char f[512];
	init();
	if (!real_unlink)
		return -1;
	if (path && path[0] == '/' && g_sysroot[0]) {
		sysrootify(f, sizeof(f), path);
		if (real_unlink(f) == 0)
			return 0;
	}
	return real_unlink(path);
}

/* stat() AND FRIENDS — "does this file exist?" asked about a file WE created.
 *
 * qemu-user's -L is not a live view of the sysroot: init_paths() walks the tree
 * ONCE at startup and every later lookup is answered from that snapshot. So a
 * path the guest creates during the run stays invisible to translation for the
 * rest of the run, and a plain stat() of it resolves against the HOST root and
 * reports ENOENT for a file that is plainly there.
 *
 * THIS IS WHAT KEPT COOKING! FROM STARTING, and it is a far better disguise
 * than the missing save directory was. dslib creates the save card on first
 * launch and then immediately asks about it again:
 *
 *     clearInit():          stat(SAVE.DAT) -> missing, so create it:
 *                           fopenAtomic + fwrite(1024 zero bytes) + fcloseAtomic
 *     DSLIBI_InitFileInfo:  stat(SAVE.DAT) -> STILL "missing", because the
 *                           snapshot predates the file, so it returns 0
 *     Backup::read:         -> errorCallback(1, 0)
 *                           -> PanicScreen::showDirect(msg, true)
 *                           -> terminate(): spin forever, on a white screen
 *
 * So the title fails on its FIRST run specifically, and could only ever have
 * worked on a later one once the card was old enough to be in the snapshot.
 * All of that reads as "the save system is broken" and none of it is.
 *
 * open() and fopen() have carried this workaround for a long time; stat, lstat
 * and access were never given it, and they are how code ASKS about a file
 * rather than uses one. Sysroot first, literal second, as everywhere else.
 */
#define STAT_WRAPPER(name, realfn, argtype)                                   \
	int name(const char *path, argtype buf)                                   \
	{                                                                         \
		char f[512];                                                          \
		unsigned dlen;                                                        \
		init();                                                               \
		if (!realfn) return -1;                                               \
		dlen = (unsigned)strlen(g_dir);                                       \
		if (path && path[0] == '/' && g_sysroot[0] &&                         \
		    !(dlen && strncmp(path, g_dir, dlen) == 0)) {                     \
			sysrootify(f, sizeof(f), path);                                   \
			if (realfn(f, buf) == 0)                                          \
				return 0;                                                     \
		}                                                                     \
		return realfn(path, buf);                                             \
	}

STAT_WRAPPER(stat,   real_stat,   void *)
STAT_WRAPPER(stat64, real_stat64, void *)
STAT_WRAPPER(lstat,  real_lstat,  void *)
STAT_WRAPPER(access, real_access, int)

/* signal() — DO NOT LET THE GUEST UNINSTALL THE CRASH REPORTER.
 *
 * AppManager installs its own handlers through this (libLightningBase.so
 * imports `signal`; the boot log's "AppManager signal handler installed" is the
 * call), and signal() REPLACES rather than chains. From that line onward the
 * shim's SIGSEGV handler was gone, which is why a real crash produced no report
 * while everything about the reporter itself worked — see tadpole_crash.c.
 *
 * The guest's request is honoured, just not by unregistering us: the handler is
 * recorded and called from ours, after the report. Signals the reporter does
 * not manage are none of our business and fall through untouched.
 */
void (*signal(int sig, void (*h)(int)))(int)
{
	void (*prev)(int);
	init();
	prev = tad_crash_take_signal(sig, h);
	if (prev != (void (*)(int))-1) {
		if (g_debug) { dbg("[tadpole] signal() kept the crash reporter\n"); }
		return prev;
	}
	return real_signal ? real_signal(sig, h) : (void (*)(int))-1;
}

/* sigaction() — THE SAME GUARD, FOR THE OTHER DOOR.
 *
 * signal() above kept the reporter through AppManager's handlers; the
 * LeapTV's titles install theirs through sigaction() instead, and that door
 * was open. Pet Play World's engine did exactly that on its way in, so its
 * crash six seconds later printed only qemu's "uncaught target signal 11"
 * and no report at all. Same treatment: for the crash signals, record the
 * guest's handler and its flags (the reporter calls an SA_SIGINFO handler
 * back with three arguments) and answer as if it had been installed.
 *
 * THE STRUCT IS THE KERNEL'S, NOT GLIBC'S. uClibc here passes the caller's
 * struct straight to rt_sigaction, so it is { handler; flags; restorer;
 * mask[2] } — twenty bytes. A first cut assumed glibc's 140-byte layout, read
 * the flags from beyond the end and zeroed 140 bytes into the caller's
 * twenty-byte `old`. That smashed Qt's stack on its first SIGQUIT install and
 * GlasgowUI hung on a condition for ever, black, with the replay idle.
 *
 * NO init() HERE: a sigaction() can come from a constructor before the shim
 * has any business starting threads and mapping files. The real function is
 * found the way dlopen's is, and by init() as well once it runs. */
struct tad_libc_sigaction { void *handler; unsigned long flags; void *restorer; unsigned long mask[2]; };
int sigaction(int sig, const struct tad_libc_sigaction *act, struct tad_libc_sigaction *old)
{
	void (*prev)(int);
	if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
	if (!real_sigaction && g_ready) real_sigaction = tad_module_symbol("libc", "sigaction");
	if (!real_sigaction && g_ready) real_sigaction = tad_module_symbol("libuClibc", "sigaction");
	if (act) {
		prev = tad_crash_take_sigaction(sig, (void (*)(int))act->handler, act->flags);
		if (prev != (void (*)(int))-1) {
			if (g_debug) {
				char b[96];
				snprintf(b, sizeof b, "[tadpole] sigaction(%d) kept the crash reporter: handler=%p flags=0x%lx\n",
				         sig, act->handler, act->flags);
				dbg(b);
			}
			if (old) {
				old->handler = (void *)prev;
				old->flags = 0; old->restorer = 0;
				old->mask[0] = old->mask[1] = 0;
			}
			return 0;
		}
	}
	return real_sigaction ? real_sigaction(sig, act, old) : -1;
}

/* pthread_create() — EVERY THREAD GETS THE REPORTER'S ALTERNATE STACK.
 *
 * sigaltstack is per thread, and a title's crash is rarely on the main one.
 * The guest's start routine runs unchanged after a one-line prologue. The
 * shim's own pump thread goes through real_pthread_create and is not
 * wrapped, which is fine: it has nothing to overflow. */
extern void tad_crash_altstack(void);
extern void *malloc(size_t n);
extern void  free(void *p);
struct tad_thread_start { void *(*fn)(void *); void *arg; };
static void *tad_thread_tramp(void *p)
{
	struct tad_thread_start s = *(struct tad_thread_start *)p;
	free(p);
	tad_crash_altstack();
	return s.fn(s.arg);
}
int pthread_create(ulong *tid, const void *attr, void *(*fn)(void *), void *arg)
{
	struct tad_thread_start *s;
	init();
	if (!real_pthread_create) return 11;              /* EAGAIN */
	s = malloc(sizeof *s);
	if (!s) return real_pthread_create(tid, attr, fn, arg);
	s->fn = fn; s->arg = arg;
	return real_pthread_create(tid, attr, tad_thread_tramp, s);
}

/* mkdir() — THE GUEST COULD NOT CREATE A DIRECTORY AT ALL.
 *
 * `real_mkdir` has been resolved since the shim was written but nothing ever
 * called it, so guest mkdir()s went straight to qemu — and this is the same
 * trap open(O_CREAT) and mkstemp() are already worked around for above: `-L`
 * only rewrites a path that ALREADY EXISTS in the sysroot, and the whole point
 * of mkdir is that its target does not. The guest string was therefore
 * attempted against the HOST root, where /LF does not exist and could not be
 * written to anyway, and every directory a title tried to make silently failed.
 *
 * WHAT THAT COSTS, because "no new directories" sounds cosmetic and is not.
 * A title's save area is /LF/Bulk/Data/Local/<profile>/<PackageID>/, and it is
 * created on first launch. Without it:
 *
 *     fopenAtomic(/LF/Bulk/Data/Local/0/MULT-0x0018004C-000000/SAVE.DAT):
 *         mkstemp failed us!
 *
 * and Cooking! Recipes on the Road answers that by calling
 * dslib::PanicScreen::showDirect(msg, true) — whose terminate(bool) is an
 * unconditional `while (flag) ;` spin. The title hangs forever at 100% CPU on
 * a white screen, having drawn nothing, with no crash and no message: the
 * panic screen's own addDirect() is compiled out to `bx lr` in this build, so
 * the text is never rendered. That is a softlock which looks exactly like a
 * renderer bug, and three sessions read it as one.
 *
 * The three titles that DO have save directories have them because HANDOVER's
 * "transplanting real /LF/Bulk" brought them in from hardware, not because
 * anything here ever created one.
 *
 * Sysroot first, literal second, as chdir/rename/unlink already do — so the
 * shim's own runtime directory under /tmp keeps working.
 */
extern int *__errno_location(void);

int mkdir(const char *path, u32 mode)
{
	char f[512];
	unsigned dlen;
	init();
	if (!real_mkdir)
		return -1;
	/* TADPOLE_DIR IS A HOST PATH, AND IT IS THE ONE ABSOLUTE PATH HERE THAT IS
	 * NOT THE GUEST'S. Everything else the guest names — /LF, /var, /tmp — it
	 * means in its own namespace, but the runtime directory is a rendezvous with
	 * the VIEWER, which opens it by that literal name from the host side. Sending
	 * it through the sysroot rule quietly created <sysroot>/tmp/tadpole-<run>
	 * instead: measured, on the first boot after this interception was added.
	 * Nothing failed loudly — the guest and the viewer simply had two different
	 * runtime directories, which is the framebuffer never appearing. */
	dlen = (unsigned)strlen(g_dir);
	if (path && dlen && strncmp(path, g_dir, dlen) == 0 &&
	    (path[dlen] == '\0' || path[dlen] == '/'))
		return real_mkdir(path, mode);
	if (path && path[0] == '/' && g_sysroot[0]) {
		int r;
		sysrootify(f, sizeof(f), path);
		r = real_mkdir(f, mode);
		if (r == 0) {
			if (g_debug) { dbg("[tadpole] mkdir "); dbg(path); dbg("\n"); }
			return 0;
		}
		/* THE SYSROOT'S ANSWER IS THE ANSWER, unless the parent is simply
		 * not there. Falling through to the raw path after EEXIST turned
		 * "/LF already exists" into EACCES from the host's root directory,
		 * and a LeapTV title's make-the-parents loop, which accepts EEXIST
		 * and retries anything else, then retried for ever: 200k rounds of
		 * stat/mkdir/open in twelve seconds under strace, and without it a
		 * stack overflow in vfprintf eight seconds after launch. Measured
		 * on Pet Play World creating /LF/Bulk/Data/Uploads/<profile>. */
		if (*__errno_location() != 2 /* ENOENT */)
			return r;
	}
	return real_mkdir(path, mode);
}

long read(int fd, void *buf, size_t n)
{
	init();
	long r;

	if (!real_read)
		return -1;
	/* Refill every reader's queue from the shared FIFO before serving this
	 * one. Cheap when there is nothing there (one non-blocking read), and it
	 * is what removes the need for a pump thread: the readers keep each
	 * other fed. */
	if (fd >= 0 && fd < MAXFD && g_ev_of_fd[fd] >= 0)
		ev_pump(g_ev_of_fd[fd]);
	r = real_read(fd, buf, n);

	/* Log what the GUEST actually receives, so a single click can be traced
	 * end to end: viewer window coords -> fb coords -> these events. */
	if (g_debug && r >= 16 && fd >= 0 && fd < MAXFD && g_ev_of_fd[fd] >= 0) {
		const u8 *e = buf;
		long off;
		for (off = 0; off + 16 <= r; off += 16) {
			u16 type = (u16)(e[off+8]  | (e[off+9]  << 8));
			u16 code = (u16)(e[off+10] | (e[off+11] << 8));
			s32 val  = (s32)((u32)e[off+12] | ((u32)e[off+13] << 8) |
			                 ((u32)e[off+14] << 16) | ((u32)e[off+15] << 24));
			char b[96];
			const char *tn = type == 0 ? "SYN" : type == 1 ? "KEY" :
			                 type == 3 ? "ABS" : "?";
			/* THE fd IS PART OF THE EVIDENCE, NOT DECORATION. One evdev
			 * node gets opened by SEVERAL readers in a single process —
			 * on a Qt device the touchscreen carries Qt's tslib mouse
			 * handler and both of Brio's ButtonPowerUSBTask tslib loads
			 * at once. Printing only the device index makes three
			 * readers competing for one FIFO look exactly like one
			 * reader working correctly, which is how this hid. */
			snprintf(b, sizeof(b), "[tadpole] ev%d fd=%d GUEST-GOT %s code=%u val=%d\n",
			         g_ev_of_fd[fd], fd, tn, code, val);
			dbg(b);
		}
	}
	return r;
}

/* THE ONLY REASON THIS EXISTS IS /dev/dsp, and it is deliberately the whole
 * function: every other write in the guest has to go straight through. It is
 * on the hottest path in the process, so it does one bounds check and one
 * array read before getting out of the way.
 *
 * No init() call. A write to a tagged fd can only happen after the open() that
 * tagged it, which ran init already — and this is also on the crash handler's
 * path (tadpole_crash.c writes its report through whatever `write` the link map
 * hands it), where re-entering initialisation would be a poor idea. */
long write(int fd, const void *buf, size_t n)
{
	if (fd >= 0 && fd < MAXFD && g_dsp_of_fd[fd] && buf && n)
		return dsp_write(buf, n);
	/* NOT init() UNCONDITIONALLY, but not never either. A write can be the
	 * very first thing a process does — before it has opened anything, which
	 * is what normally brings the shim up — and answering -1 there would lose
	 * the guest's own output. Resolving lazily costs one predictable branch on
	 * every later call. init() is re-entrant safe (it sets g_ready first), so
	 * the dbg() inside it landing back here is a bounded, harmless recursion. */
	if (!real_write) {
		init();
		if (!real_write)
			return -1;
	}
	return real_write(fd, buf, n);
}

int open(const char *path, int flags, ...)
{
	va_list ap; int mode = 0;
	va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
	return dsp_note_open(path, open_common(path, flags, mode));
}

int open64(const char *path, int flags, ...)
{
	va_list ap; int mode = 0;
	va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
	return dsp_note_open(path, open_common(path, flags, mode));
}

int openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap; int mode = 0;
	va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
	init();
	if (path && path[0] == '/' && (fb_index(path) >= 0 || ev_index(path) >= 0))
		return open_common(path, flags, mode);
	if (!real_openat) return -1;
	return real_openat(dirfd, path, flags, mode);
}

int close(int fd)
{
	init();
	if (fd >= 0 && fd < MAXFD) {
		if (g_ev_of_fd[fd] >= 0)
			ev_close(fd);            /* also closes our write end */
		g_fb_of_fd[fd] = -1;
		g_ev_of_fd[fd] = -1;
		/* The FIFO stays open across a close of /dev/dsp. portaudio opens
		 * and closes the device around stream setup, and letting the FIFO
		 * come and going would have the viewer see the stream appear and
		 * vanish repeatedly. One process, one audio stream. */
		g_dsp_of_fd[fd] = 0;
		g_mlc_of_fd[fd] = 0;
		g_mem_of_fd[fd] = 0;
		tad_cam_close(fd);
	}
	if (!real_close) return -1;
	return real_close(fd);
}

static void fill_var(struct fb_var_screeninfo *v, int idx)
{
	memset(v, 0, sizeof(*v));
	v->xres = v->xres_virtual = g_w;
	v->yres = g_h;
	v->yres_virtual = g_h * NBUF;
	v->bits_per_pixel = g_bpp;
	v->height = 61;   /* ~5" diagonal at 480x272, in mm */
	v->width  = 108;
	v->activate = 0;  /* FB_ACTIVATE_NOW */
	v->vmode  = 0;    /* FB_VMODE_NONINTERLACED */

	if (g_bpp == 16) {                       /* RGB565 */
		v->red.offset   = 11; v->red.length   = 5;
		v->green.offset =  5; v->green.length = 6;
		v->blue.offset  =  0; v->blue.length  = 5;
	} else {                                 /* BGRA8888 (SDL ARGB8888 LE) */
		v->blue.offset  =  0; v->blue.length  = 8;
		v->green.offset =  8; v->green.length = 8;
		v->red.offset   = 16; v->red.length   = 8;
		v->transp.offset= 24; v->transp.length= 8;
	}
	if (g_state)
		v->nonstd = g_state->layer[idx].nonstd;
}

static void fill_fix(struct fb_fix_screeninfo *f, int idx)
{
	memset(f, 0, sizeof(*f));
	f->id[0] = 'l'; f->id[1] = 'f'; f->id[2] = '2'; f->id[3] = '0';
	f->id[4] = '0'; f->id[5] = '0'; f->id[6] = 'f'; f->id[7] = 'b';
	f->id[8] = (char)('0' + idx);
	/* A non-zero smem_start keeps callers that sanity-check it happy; the
	 * guest never dereferences it, it mmaps the fd instead. */
	f->smem_start  = 0x82000000u + (ulong)idx * 0x400000u;
	f->smem_len    = g_w * g_h * (g_bpp / 8) * NBUF;
	f->type        = 0;   /* FB_TYPE_PACKED_PIXELS */
	f->visual      = 2;   /* FB_VISUAL_TRUECOLOR */
	f->accel       = 0;
	f->ypanstep    = 1;

	/* ALWAYS THE PANEL WIDTH. Hardware really does report 1280 for Clam Prix's
	 * 320x240 3D surface where we report 1920, but deriving the pitch from
	 * layer[].win_w — as an earlier attempt did — is WRONG and regressed
	 * scaling across many titles.
	 *
	 * win_w is the layer's ON-PANEL WINDOW RECTANGLE (the ViewFrame box), not
	 * the width of its source buffer. The two coincide for the 3D surface and
	 * differ for every 2D title that scales its viewport, so keying the pitch
	 * off win_w told those games their rows were 1280 bytes apart while they
	 * carried on writing them 1920 apart. Symptom: content rendered far too
	 * large. Fixing this properly needs a source width tracked separately from
	 * the window rect — see HANDOVER. */
	f->line_length = g_w * (g_bpp / 8);
}

int ioctl(int fd, ulong req, ...)
{
	va_list ap;
	void *arg;
	int idx;
	u32 id;

	init();

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	/* ---------------- /dev/video0 ---------------- */
	if (tad_cam_is(fd))
		return tad_cam_ioctl(fd, req, arg);

	/* ---------------- /dev/dsp ---------------- */
	if (fd >= 0 && fd < MAXFD && g_dsp_of_fd[fd])
		return dsp_ioctl(req, arg);

	/* ---------------- LF1000 display control ---------------- */
	if (fd >= 0 && fd < MAXFD && g_mlc_of_fd[fd]) {
		int handled = 0;
		int r = mlc_ioctl_idx(req, arg, 0, &handled);
		if (handled)
			return r;
		/* Not an 'm' ioctl: a terminal query on a node that is a plain file
		 * here. Fall through to the real one, which says ENOTTY — which is
		 * what a real device would say too. */
	}

	/* ---------------- framebuffer ---------------- */
	if (fd >= 0 && fd < MAXFD && (idx = g_fb_of_fd[fd]) >= 0) {
		if (g_debug) {
			char b[96]; const char *n = "fb-ioctl";
			if (req == FBIOPAN_DISPLAY)        n = "PAN";
			else if (req == FBIOPUT_VSCREENINFO) n = "PUT_VSCREEN";
			else if (req == FBIOGET_VSCREENINFO) n = "GET_VSCREEN";
			else if (req == FBIOGET_FSCREENINFO) n = "GET_FSCREEN";
			else if (req == FBIO_WAITFORVSYNC)   n = "VSYNC";
			else if (req == FBIOBLANK)           n = "BLANK";
			if (req == FBIOBLANK) {
				snprintf(b, sizeof(b), "[tadpole] fb%d BLANK arg=%u\n",
				         idx, (u32)(ulong)arg);
				dbg(b);
			} else
			if (n[0] == 'f') {          /* unnamed: show the number */
				snprintf(b, sizeof(b), "[tadpole] fb%d ioctl %08lx arg=%lu\n",
				         idx, req, (ulong)arg);
				dbg(b);
			} else
			if (req == FBIOPAN_DISPLAY || req == FBIOPUT_VSCREENINFO) {
				struct fb_var_screeninfo *v = arg;
				snprintf(b, sizeof(b),
				         "[tadpole] fb%d %s yoff=%u nonstd=%08x prio=%u\n",
				         idx, n, v ? v->yoffset : 0, v ? v->nonstd : 0,
				         v ? ((v->nonstd >> 24) & 0x3) : 0);
			} else {
				snprintf(b, sizeof(b), "[tadpole] fb%d %s\n", idx, n);
			}
			dbg(b);
		}
		switch (req) {
		case FBIOGET_VSCREENINFO:
			fill_var(arg, idx);
			return 0;
		case FBIOPUT_VSCREENINFO: {
			struct fb_var_screeninfo *v = arg;
			if (g_debug && v) {
				char b2[160];
				snprintf(b2, sizeof(b2),
				         "[tadpole] fb%d PUTVAR req %ux%u virt %ux%u off %u,%u\n",
				         idx, v->xres, v->yres, v->xres_virtual,
				         v->yres_virtual, v->xoffset, v->yoffset);
				dbg(b2);
			}
			/* The requested xres/yres IS the layer's window size — read it
			 * before fill_var() below replaces it with the panel size. */
			if (g_state && v && v->xres && v->yres &&
			    v->xres <= g_w && v->yres <= g_h) {
				g_state->layer[idx].win_w = v->xres;
				g_state->layer[idx].win_h = v->yres;
			}
			if (g_state && v) {
				g_state->layer[idx].xoffset = v->xoffset;
				g_state->layer[idx].yoffset = v->yoffset;
				g_state->layer[idx].nonstd  = v->nonstd;
				g_state->layer[idx].enabled = 1;
			}
			/* Report back what we actually support rather than failing;
			 * refusing a mode here makes Brio give up on the layer. */
			fill_var(v, idx);
			return 0;
		}
		case FBIOGET_FSCREENINFO:
			fill_fix(arg, idx);
			return 0;
		case FBIOPAN_DISPLAY: {
			struct fb_var_screeninfo *v = arg;
			if (g_state && v) {
				g_state->layer[idx].xoffset = v->xoffset;
				g_state->layer[idx].yoffset = v->yoffset;
				/* Per-layer offset into the shared arena. (An earlier
				 * version broadcast the pan to every layer, which collapsed
				 * all layers onto one offset and hid the Flash content.) */
				g_state->layer[idx].enabled = 1;
				g_state->layer[idx].blank   = 0;
				g_state->vsync_count++;
			}
			return 0;
		}
		case FBIOBLANK:
			/* FB_BLANK_UNBLANK == 0 means "show me". */
			if (g_state) {
				g_state->layer[idx].blank = (u32)(ulong)arg;
				if ((u32)(ulong)arg == 0)
					g_state->layer[idx].enabled = 1;
			}
			return 0;
		case FBIO_WAITFORVSYNC:
			vsync_wait();
			if (g_state)
				g_state->vsync_count++;
			return 0;
		case LF1000FB_IOCSALPHA:
			if (g_state && arg)
				g_state->layer[idx].alpha = *(u32 *)arg;
			return 0;
		case LF1000FB_IOCGALPHA:
			if (arg)
				*(u32 *)arg = g_state ? g_state->layer[idx].alpha : 255;
			return 0;
		case LF1000FB_IOCSPOSTION:
		case LF1000FB_IOCGPOSTION:
		case LF1000FB_IOCSVIDSCALE:
		case LF1000FB_IOCGVIDSCALE:
			/* Payload words 0 and 1 are left and top — verified against
			 * every ViewFrame.json we have. Word 2 holds a pointer, so the
			 * struct is not the flat {l,t,r,b} the name suggests; words 3
			 * and 4 do read as right/bottom but we take the size from
			 * PUT_VSCREENINFO instead, whose meaning is unambiguous. */
			if (req == LF1000FB_IOCSPOSTION && g_state && arg) {
				const u32 *w = (const u32 *)arg;
				if (w[0] < g_w && w[1] < g_h) {
					g_state->layer[idx].win_x = w[0];
					g_state->layer[idx].win_y = w[1];
				}
			}
			/* struct lf1000fb_vidscale_cmd is { sizex, sizey, apply:1 } —
			 * the size of the DECODED picture, which the MLC then stretches
			 * to the layer window. Publish it so the viewer can do the same
			 * stretch; accepting and discarding it left every scaled video
			 * cropped to the top-left corner of its window. */
			if (req == LF1000FB_IOCSVIDSCALE && g_state && arg) {
				const u32 *v = (const u32 *)arg;
				if (v[0] && v[1] && v[0] <= 4096 && v[1] <= 4096) {
					g_state->layer[idx].vid_w = v[0];
					g_state->layer[idx].vid_h = v[1];
					if (g_debug) {
						char b[96];
						snprintf(b, sizeof(b),
						         "[tadpole] fb%d vidscale src %ux%u\n",
						         idx, v[0], v[1]);
						dbg(b);
					}
				}
			}
			if (g_debug && arg && req == LF1000FB_IOCSPOSTION) {
				const u32 *w = (const u32 *)arg;
				char b[96];
				snprintf(b, sizeof(b), "[tadpole] fb%d window %u,%u\n",
				         idx, w[0], w[1]);
				dbg(b);
			}
			return 0;
		default:
			/* AN UNKNOWN 'm' IOCTL IS AN LF1000 ONE, and on a layer node
			 * those are how the Didj asks about the plane. Succeeding
			 * quietly without filling the caller's buffer is what produced
			 *
			 *     !ASSERT: [5] DisplayModule::InitModule: MLC layer ioctl failed
			 *
			 * on an ioctl that had returned 0: libDisplay checked the ANSWER,
			 * not the status. mlc_ioctl answers reads with the panel
			 * geometry, which is the only thing any of them can sensibly want
			 * from a layer this shim owns. */
			{
				int handled = 0;
				int r = mlc_ioctl_idx(req, arg, idx, &handled);
				if (handled)
					return r;
			}
			if (g_debug) {
				char b[80];
				snprintf(b, sizeof(b),
				         "[tadpole] fb%d unknown ioctl %08lx, accepted\n",
				         idx, req);
				dbg(b);
			}
			return 0;   /* unknown fb ioctl: succeed quietly */
		}
	}

	/* ---------------- evdev ---------------- */
	if (fd >= 0 && fd < MAXFD && (idx = g_ev_of_fd[fd]) >= 0) {
		id = (u32)req & EV_MASK;

		if (id == EVIOCGNAME_ID) {
			u32 len = ((u32)req >> 16) & 0x3FFF;
			const char *n = g_ev[idx].name;
			u32 l = strlen(n) + 1;
			if (l > len) l = len;
			if (arg) memcpy(arg, n, l);
			return (int)l;
		}
		if (id == EVIOCGPHYS_ID) {
			u32 len = ((u32)req >> 16) & 0x3FFF;
			const char *n = g_ev[idx].phys;
			u32 l = strlen(n) + 1;
			if (l > len) l = len;
			if (arg) memcpy(arg, n, l);
			return (int)l;
		}
		if (id == EVIOCGVERSION_ID) {
			if (arg) *(u32 *)arg = 0x010001;   /* EV_VERSION */
			return 0;
		}
		if (id == EVIOCGID_ID) {
			if (arg) memset(arg, 0, 8);
			return 0;
		}
		if ((id & 0xFFFFFFE0u) == EVIOCGBIT_BASE) {
			u32 len = ((u32)req >> 16) & 0x3FFF;
			u32 ev  = (u32)req & 0x1Fu;      /* which EV_* is being asked about */
			if (arg) {
				memset(arg, 0, len);
				if (ev == 0 && len >= 4) {
					/* EVIOCGBIT(0, ..) = which event types exist */
					*(u32 *)arg = g_ev[idx].ev_bits;
				} else if (ev == 3 && len >= 4) {
					/* EV_ABS — what tslib actually gates on */
					*(u32 *)arg = g_ev[idx].abs_bits;
				} else if (ev == 1 && len >= 44) {
					/* EV_KEY: BTN_TOUCH is 330 = word 10, bit 10 */
					if (idx == 2)
						((u32 *)arg)[10] = (1u << 10);
				}
			}
			return (int)len;
		}

		/* EVIOCGABS(axis) = _IOR('E', 0x40+axis, struct input_absinfo).
		 * With EV_MASK applied the id is 0x80004540..0x8000457F — note the
		 * 0x45 ('E') in bits 8-15, which an earlier version of this check
		 * got wrong, so the ioctl fell through and tslib read uninitialised
		 * stack as the axis range and then jumped through a null pointer.
		 * struct input_absinfo is 6 x s32 on this kernel:
		 *   value, minimum, maximum, fuzz, flat, resolution
		 *
		 * REPORT WHAT THE HARDWARE REPORTS, which is not what it emits.
		 * `evtest /dev/input/event2` on a real LeapPad2:
		 *
		 *   ABS_X         min 1  max 1023  fuzz 2   but emits   2..482
		 *   ABS_Y         min 1  max 1023  fuzz 2   but emits   0..271
		 *   ABS_PRESSURE  min 1  max 1023  fuzz 5   but emits  10..70
		 *
		 * The driver advertises a 10-bit range and then hands out panel
		 * pixels. Nothing rescales in between: there is no /etc/pointercal,
		 * so tslib's linear module is identity. We were reporting the panel
		 * size as the maximum, which no real device ever does — so anything
		 * that trusted this got a different answer here than on hardware.
		 * Advertise the hardware numbers and keep sending pixels, exactly as
		 * the device does. */
		{
			u32 nr = id & 0xFFu;
			if ((id & 0xFFFFFF00u) == 0x80004500u && nr >= 0x40 && nr <= 0x7F) {
				u32 axis = nr - 0x40;
				s32 *ai = arg;
				if (ai) {
					memset(ai, 0, 24);
					if (axis == 0 || axis == 1) {     /* ABS_X, ABS_Y */
						/* ...EXCEPT WHEN Qt READS EVDEV ITSELF.
						 *
						 * The paragraph above is right for the device, where
						 * tslib sits between driver and application and an
						 * identity pointercal turns the 10-bit claim back into
						 * pixels. The Ultra's Qt has no tslib in the way: its
						 * LinuxInput mouse handler scales for itself,
						 *
						 *   x = (value - min) * screenWidth / (max - min)
						 *
						 * so advertising 1..1023 while sending pixels makes every
						 * touch land short: on a 1024x600 panel y=599 arrives as
						 * about 351, and the bottom third cannot be reached.
						 *
						 * TADPOLE_ABS_PANEL advertises the panel instead, making
						 * that scaling identity. tadpole.sh sets it for a device
						 * whose shell reads evdev directly. */
						if (g_abs_panel) {
							ai[1] = 0;
							ai[2] = (s32)((axis == 0 ? g_w : g_h) - 1);
							ai[3] = 0;
						} else {
							ai[1] = 1; ai[2] = 1023; ai[3] = 2;
						}
					} else if (axis == 24) {          /* ABS_PRESSURE */
						ai[1] = 1; ai[2] = 1023; ai[3] = 5;
					} else {
						ai[2] = 4095;
					}
				}
				return 0;
			}
		}
		return 0;
	}

	if (!real_ioctl) {
		dbg("[tadpole] real_ioctl is NULL — refusing to jump to 0\n");
		return -1;
	}
	return real_ioctl(fd, req, arg);
}

/* ---- locales the guest's libstdc++ refuses ------------------------------
 *
 * WHAT BREAKS WITHOUT THIS. Nineteen titles die in their first second with
 *
 *     terminate called after throwing an instance of 'std::runtime_error'
 *       what():  locale::facet::_S_create_c_locale name not valid
 *
 * They read Locale="en-us" out of their own meta.inf and construct
 * std::locale("en-us") without catching. Five shared engines account for all
 * nineteen — BookApp2.so, cartLauncher.so, UEB2013.so, trans.so, and the
 * camera/photo/video widgets — so it is one fault reached nineteen ways.
 *
 * WHERE IT ACTUALLY COMES FROM, disassembled rather than guessed. libstdc++
 * 6.0.14 here is built on the GENERIC locale model, and its
 * _S_create_c_locale is the whole of the check:
 *
 *     *__cloc = 0;
 *     if (strcmp(__s, "C") == 0) return;
 *     __throw_runtime_error("locale::facet::_S_create_c_locale name not valid");
 *
 * It never calls setlocale — interposing that, which was the obvious first
 * move, changes nothing. Any name but "C" throws, full stop.
 *
 * NONE OF THIS IS THE EMULATOR'S DOING. It is a literal strcmp inside the
 * guest's own libstdc++, reached without touching a file or a syscall. The
 * same binaries on this firmware fail the same way on real hardware. What it
 * really says is that these titles are NEWER THAN THE FIRMWARE they are being
 * run against — the packages are dated December 2013, the system August — and
 * expect a build of libstdc++ with locale support.
 *
 * So this is a DELIBERATE DEVIATION FROM THE DEVICE, not an accuracy fix, and
 * it should be read as one. It provides the success path of the function above
 * for every name: clear the out-parameter and return, leaving the C locale in
 * force underneath.
 *
 * The lie is small. The generic model has no locale but C to offer, so the
 * alternative on the table is not "correct en-US formatting" — there is no
 * such thing in this libstdc++ — it is SIGABRT before the first frame.
 * Collation and number grouping stay C's, and these are ASCII titles.
 *
 * TADPOLE_STRICT_LOCALE=1 restores the stock behaviour, for measuring what the
 * unmodified system does.
 */
static void (*real_create_c_locale)(short **, const char *, short *);

void _ZNSt6locale5facet18_S_create_c_localeERPsPKcS1_(short **cloc,
                                                      const char *name,
                                                      short *old);
void _ZNSt6locale5facet18_S_create_c_localeERPsPKcS1_(short **cloc,
                                                      const char *name,
                                                      short *old)
{
	init();
	if (getenv("TADPOLE_STRICT_LOCALE")) {
		if (!real_create_c_locale)
			real_create_c_locale = dlsym(RTLD_NEXT,
			        "_ZNSt6locale5facet18_S_create_c_localeERPsPKcS1_");
		if (real_create_c_locale) {
			real_create_c_locale(cloc, name, old);
			return;
		}
	}
	/* The success path, verbatim: the generic model stores nothing but a null
	 * handle, because it has no locale object to build. */
	if (cloc)
		*cloc = 0;
	if (g_debug && name && name[0] && !(name[0] == 'C' && !name[1])) {
		dbg("[tadpole] std::locale(\"");
		dbg(name);
		dbg("\") accepted; C semantics underneath\n");
	}
}
