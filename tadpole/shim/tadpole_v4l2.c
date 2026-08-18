/* Tadpole — LeapPad2 (NXP3200 / VALENCIA) emulator
 *
 * tadpole_v4l2.c — /dev/video0 and /dev/video1, guest side.
 *
 * WHAT THE GUEST ACTUALLY DOES, measured rather than assumed. Every ioctl
 * request code below was lifted out of libCameraUSB.so and libCameraVIP.so by
 * reading the constant that is loaded into r1 before each `bl ioctl@plt`; the
 * struct offsets were read off the loads and stores around those calls. The
 * two modules issue an identical set, so this file serves whichever one Brio
 * picks.
 *
 *     QUERYCAP  ENUM_FMT  ENUM_FRAMESIZES  ENUM_FRAMEINTERVALS
 *     S_FMT  S_PARM  QUERYCTRL  G_CTRL  S_CTRL
 *     REQBUFS  QUERYBUF  QBUF  DQBUF  STREAMON  STREAMOFF
 *     (+ OVERLAY and S_FBUF, VIP only)
 *
 * There is no G_FMT, no TRY_FMT and no S_INPUT: the second camera is a second
 * device node, not an input index.
 *
 * WHICH FORMAT — AND WHY ENUM_FMT ANSWERS NOTHING AT ALL
 * ------------------------------------------------------
 * NOTES-camera.md left this as the one open question: "answer ENUM_FMT with
 * only YUYV and see whether it proceeds". The answer is that ENUM_FMT is the
 * wrong lever, and offering ANY format through it is actively harmful.
 *
 * CVIPCameraModule::EnumFormats — the virtual the application actually calls —
 * does not use the V4L2 enumeration at all, unless CCameraModule has cached
 * one. Its first act is to look at that cache, and only if it is EMPTY does it
 * build the list the LeapPad2 really offers, out of eight statics in the
 * module itself:
 *
 *     QSVGA 400x300   SVGA 800x600   QVGA 320x240   VGA 640x480   (always)
 *     WXGA 1280x800   SXGA 1280x960  HD16 1600x900  UXGA 1600x1200
 *                                    (if within /flags/high-res, "%dx%d")
 *
 * and every one of them is fmt 3 — planar YUV420. That number is what makes
 * video recording possible: CameraTaskMain turns tCaptureMode.fmt into a
 * fourcc for AVI_set_video, which sets an ffmpeg pix_fmt from it —
 * '422P' -> 16, 'YUYV' -> 1, 'YU12' -> 15 — and sets NOTHING for anything
 * else. The MJPEG encoder then refuses to open:
 *
 *     [tadpole] cam0: S_FMT 320x240 fourcc 47504a4d
 *     [mjpeg @ 0x560020]colorspace not supported in jpeg
 *     === tadpole: guest crashed ===  signal SIGFPE (8)
 *
 * That was us advertising MJPEG. CCameraModule::InitCameraInt builds its cache
 * with `mode->fmt = (pixel_format == 'MJPG') ? 1 : 0`, so the ONLY two values
 * a V4L2 enumeration can ever produce are 1 (MJPG — no pix_fmt, encoder
 * refuses) and 0 (no fourcc at all — same outcome, plus a garbage mode where
 * VideoWidget::setInfo found no size to match and left its copy
 * uninitialised: "S_FMT 56308x45910 fourcc 00000000").
 *
 * So the correct emulation of the lf2000_vip driver is one that answers no
 * formats — which is presumably why the real driver's list is empty too, since
 * this is the only way the firmware's own video recorder can work. Everything
 * downstream then comes from the module's statics, in YUV420, at the sizes
 * CameraWidget::setSize() and VideoWidget::setInfo() already know about.
 *
 * S_FMT still matters, and it is where the real negotiation happens: the guest
 * asks for one of those sizes and this file produces it.
 *
 * WHERE THE PIXELS LIVE
 * ---------------------
 * $TADPOLE_DIR/camN.bin, laid out by tadpole_cam.h. open() hands the guest a
 * descriptor onto that file, so the mmap it does after QUERYBUF is a real
 * shared mapping and no interception is needed on the data path — the same
 * arrangement the framebuffer has used since the beginning, pointing the other
 * way. Only the control path is emulated.
 *
 * WITHOUT A HOST BACKEND there is still something to see: if
 * $TADPOLE_DIR/camN.raw exists it is served as a still frame, verbatim, in
 * whatever format the guest negotiated. That is what makes this testable on
 * its own — write one frame's worth of bytes and the viewfinder shows it — and
 * it doubles as the placeholder when Android has denied the camera permission.
 */

#define _GNU_SOURCE
#include "tadpole_cam.h"

typedef __SIZE_TYPE__      size_t;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed int         s32;
typedef unsigned long      ulong;

extern int    snprintf(char *s, size_t n, const char *fmt, ...);
extern void  *memcpy(void *d, const void *s, size_t n);
extern void  *memset(void *s, int c, size_t n);
extern size_t strlen(const char *s);
extern int    strncmp(const char *a, const char *b, size_t n);
extern long   read(int fd, void *buf, size_t n);
extern long   lseek(int fd, long off, int whence);
extern int    ftruncate(int fd, long length);
extern int   *__errno_location(void);
extern char  *getenv(const char *name);

struct tad_ts { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_ts *tp);
extern int nanosleep(const struct tad_ts *req, struct tad_ts *rem);
#define CLOCK_MONOTONIC_ 1

#define EINVAL_  22
#define EAGAIN_  11
#define EIO_      5
#define O_RDWR_   2
#define O_CREAT_  0100
#define O_RDONLY_ 0

/* ---- V4L2, declared by hand ---------------------------------------------
 * Same rule as the rest of the shim: no kernel headers, because this is
 * cross-compiled with the host's clang and no ARM sysroot. Sizes are the ones
 * the guest's own ioctl numbers encode — struct v4l2_format is 204 bytes here,
 * not the 208 a modern header gives, which is why nothing may be #included. */

/* Matched on type+nr only. The size field baked into the request differs
 * between kernel versions and we must accept the guest's, not ours. */
#define V4L2_NR(req)  ((u32)(req) & 0xFFFFu)
#define VIDIOC_QUERYCAP_            0x5600u
#define VIDIOC_ENUM_FMT_            0x5602u
#define VIDIOC_G_FMT_               0x5604u
#define VIDIOC_S_FMT_               0x5605u
#define VIDIOC_REQBUFS_             0x5608u
#define VIDIOC_QUERYBUF_            0x5609u
#define VIDIOC_S_FBUF_              0x560Bu
#define VIDIOC_OVERLAY_             0x560Eu
#define VIDIOC_QBUF_                0x560Fu
#define VIDIOC_DQBUF_               0x5611u
#define VIDIOC_STREAMON_            0x5612u
#define VIDIOC_STREAMOFF_           0x5613u
#define VIDIOC_G_PARM_              0x5615u
#define VIDIOC_S_PARM_              0x5616u
#define VIDIOC_G_CTRL_              0x561Bu
#define VIDIOC_S_CTRL_              0x561Cu
#define VIDIOC_QUERYCTRL_           0x5624u
#define VIDIOC_TRY_FMT_             0x5640u
#define VIDIOC_ENUM_FRAMESIZES_     0x564Au
#define VIDIOC_ENUM_FRAMEINTERVALS_ 0x564Bu

#define V4L2_CAP_VIDEO_CAPTURE_ 0x00000001u
#define V4L2_CAP_STREAMING_     0x04000000u
#define V4L2_BUF_TYPE_CAPTURE_  1
#define V4L2_MEMORY_MMAP_       1
#define V4L2_FIELD_NONE_        1
#define V4L2_PIX_FMT_MJPEG_     TAD_FOURCC_MJPG
#define V4L2_FRMSIZE_DISCRETE_  1
#define V4L2_FRMIVAL_DISCRETE_  1
#define V4L2_BUF_FLAG_MAPPED_   0x0001u
#define V4L2_BUF_FLAG_QUEUED_   0x0002u
#define V4L2_BUF_FLAG_DONE_     0x0004u
#define V4L2_FMT_FLAG_COMPRESSED_ 0x0001u

struct v4l2_capability {          /* 104 */
	u8  driver[16], card[32], bus_info[32];
	u32 version, capabilities, reserved[4];
};

struct v4l2_fmtdesc {             /* 64 */
	u32 index, type, flags;
	u8  description[32];
	u32 pixelformat, reserved[4];
};

struct v4l2_pix_format {          /* 32 */
	u32 width, height, pixelformat, field;
	u32 bytesperline, sizeimage, colorspace, priv;
};

struct v4l2_format {              /* 204 */
	u32 type;
	union { struct v4l2_pix_format pix; u8 raw[200]; } fmt;
};

struct v4l2_fract { u32 numerator, denominator; };

struct v4l2_frmsizeenum {         /* 44 */
	u32 index, pixel_format, type;
	union { struct { u32 width, height; } discrete; u32 stepwise[6]; } u;
	u32 reserved[2];
};

struct v4l2_frmivalenum {         /* 52 */
	u32 index, pixel_format, width, height, type;
	union { struct v4l2_fract discrete; u32 stepwise[6]; } u;
	u32 reserved[2];
};

struct v4l2_captureparm {
	u32 capability, capturemode;
	struct v4l2_fract timeperframe;
	u32 extendedmode, readbuffers, reserved[4];
};

struct v4l2_streamparm {          /* 204 */
	u32 type;
	union { struct v4l2_captureparm capture; u8 raw[200]; } parm;
};

struct v4l2_requestbuffers { u32 count, type, memory, reserved[2]; };  /* 20 */

struct v4l2_timecode { u32 type, flags; u8 frames, seconds, minutes, hours, userbits[4]; };

struct v4l2_buffer {              /* 68 */
	u32 index, type, bytesused, flags, field;
	struct { long tv_sec, tv_usec; } timestamp;
	struct v4l2_timecode timecode;
	u32 sequence, memory;
	union { u32 offset; ulong userptr; } m;
	u32 length, input, reserved;
};

struct v4l2_control { u32 id; s32 value; };

/* ---- our state ---------------------------------------------------------- */

#define QMAX TAD_CAM_BUFMAX

struct camdev {
	int  fd;             /* the host file descriptor we handed the guest  */
	int  idx;            /* 0 or 1                                        */
	u32  width, height;
	u32  pixfmt;
	u32  sizeimage;      /* exact bytes in one frame of that fourcc       */
	u32  slot;           /* sizeimage rounded up to a page                */
	u32  nbuf;
	u32  streaming;
	u32  sequence;       /* frames delivered, for v4l2_buffer.sequence    */
	u32  last_seq;       /* the host seq we last consumed                 */
	u8   qflag[QMAX];    /* 1 = queued by the guest and still ours        */
	u32  queue[QMAX];    /* FIFO of queued buffer indices                 */
	u32  qhead, qtail, qcount;

	/* THE PREVIEW PATH, WHICH DOES NOT GO THROUGH DQBUF AT ALL.
	 * CVIPCameraModule::StartVideoCapture points S_FBUF at the display
	 * surface and turns on VIDIOC_OVERLAY; on the real device the VIP block
	 * then DMAs straight into the MLC's video plane and the application never
	 * touches a frame. Measured: n_qbuf=1, n_dqbuf=0 with the viewfinder up.
	 * These four fields are that destination. */
	u32  ov_on;
	u32  ov_off;         /* byte offset of the surface inside fb0.bin      */
	u32  ov_w, ov_h;     /* surface size, from S_FBUF's v4l2_pix_format    */
	u32  ov_pitch;       /* its bytesperline: the panel pitch, not the width */
	u32  ov_fourcc;
	u32  polled;         /* the recorder's PollFrame loop has been seen   */

	/* ONE SCRATCH PER DEVICE, sized when the format is. A fixed array would
	 * have to be TAD_CAM_MAXFRAME to cover 1600x900, and this library is
	 * loaded into every guest process — 8 MB of BSS in AppManager,
	 * VideoDaemon and every tool, for a buffer only the camera touches. */
	u8  *scratch;
	u32  scratch_sz;
};

static struct camdev g_cam[TAD_CAM_N];
static struct tad_cam_state *g_cs;
/* The framebuffer arena, mapped by the shim and by the viewer. The overlay
 * writes the preview into it directly, which is exactly what the hardware
 * does. */
static u8   *g_arena;
static u32   g_arena_bytes;
static const u32 *g_smem;      /* per-layer smem_start, from fill_fix() */
static u32   g_nlayer;
static char  g_dir[256];
static int (*g_open)(const char *, int, ...);
static int (*g_close)(int);
static void (*g_dbg)(const char *);
/* WHAT SIZES TO OFFER, AND WHY MORE THAN ONE.
 *
 * CameraWidget takes the largest — its calcMaxSize() walks the enumeration and
 * keeps the maximum width and height — so one entry would do for photographs.
 * VideoWidget::setInfo() does the opposite: it walks the same list looking for
 * an EXACT match on a size it has already decided (320x240 by default, or one
 * from a table for the two other quality settings), copies the matching
 * tCaptureMode, and if it finds none leaves its copy UNINITIALISED. That is
 * not a hypothetical:
 *
 *     [tadpole] cam0: S_FMT 56308x45910 fourcc 00000000
 *     [mjpeg @ 0x12b8e10] colorspace not supported in jpeg
 *     === tadpole: guest crashed ===  signal SIGFPE (8)
 *
 * — a garbage mode, an encoder opened with no pixel format, and a division by
 * zero. Offering the sizes the widget actually looks for is the whole fix.
 * 400x300 is the second entry in that table; the third is 640x480. */
#define NSIZES 3
static const u32 g_sizes[NSIZES][2] = { {320,240}, {400,300}, {640,480} };

static u32  g_defw = 640, g_defh = 480;
static u32  g_fps  = 15;
static int  g_ready;

static void note(const char *s) { if (g_dbg) g_dbg(s); }

static u32 env_u32(const char *name, u32 dflt)
{
	const char *e = getenv(name);
	u32 v = 0;
	if (!e || !*e) return dflt;
	while (*e >= '0' && *e <= '9') v = v * 10 + (u32)(*e++ - '0');
	return v ? v : dflt;
}

/* Exact bytes in one frame. Planar YUV420 is the one that matters — it is what
 * the guest actually asks for — and its chroma planes are half-resolution in
 * both directions, so odd sizes round UP per plane rather than dividing the
 * total. Getting this wrong by a row is a green stripe along the bottom of the
 * viewfinder, not a crash, which is exactly the sort of bug that survives. */
static u32 frame_bytes(u32 fourcc, u32 w, u32 h)
{
	u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
	switch (fourcc) {
	case TAD_FOURCC_YU12: return w * h + 2 * (cw * ch);
	case TAD_FOURCC_YUYV: return w * h * 2;
	case TAD_FOURCC_422P: return w * h + 2 * (cw * h);
	case TAD_FOURCC_MJPG: /* compressed: no fixed size, so bound it */
	default:              return w * h;      /* generous for any JPEG */
	}
}

static u32 bytes_per_line(u32 fourcc, u32 w)
{
	switch (fourcc) {
	case TAD_FOURCC_YU12: return w;
	case TAD_FOURCC_422P: return w;
	case TAD_FOURCC_YUYV: return w * 2;
	default:              return 0;          /* compressed */
	}
}

static u32 page_up(u32 n) { return (n + 4095u) & ~4095u; }

/* Recompute everything that depends on width/height/pixfmt and publish it. */
static void geom_set(struct camdev *c)
{
	c->sizeimage = frame_bytes(c->pixfmt, c->width, c->height);
	if (c->sizeimage < 4096) c->sizeimage = 4096;
	if (c->sizeimage > TAD_CAM_MAXFRAME) c->sizeimage = TAD_CAM_MAXFRAME;
	c->slot = page_up(c->sizeimage);
	if (g_cs) {
		g_cs[c->idx].width     = c->width;
		g_cs[c->idx].height    = c->height;
		g_cs[c->idx].pixfmt    = c->pixfmt;
		g_cs[c->idx].sizeimage = c->sizeimage;
		g_cs[c->idx].slot_size = c->slot;
		g_cs[c->idx].fps       = g_fps;
	}
}

/* camN.bin holds ONLY the two staging slots now. The V4L2 buffers live in the
 * arena, because that is where the guest looks for them — see the long note
 * above capture_blit_mlc(). */
static u32 stage_bytes(struct camdev *c) { return 2u * c->slot; }

void tad_v4l2_init(const char *dir, struct tad_cam_state *cams,
                   void *arena, u32 arena_bytes,
                   const u32 *smem_start, u32 nlayer,
                   int (*ropen)(const char *, int, ...),
                   int (*rclose)(int),
                   void (*dbgfn)(const char *))
{
	int i;
	if (g_ready) return;
	g_ready = 1;
	g_arena = arena;
	g_arena_bytes = arena_bytes;
	g_smem = smem_start;
	g_nlayer = nlayer;
	snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "/tmp/tadpole");
	g_cs    = cams;
	g_open  = ropen;
	g_close = rclose;
	g_dbg   = dbgfn;
	g_defw  = env_u32("TADPOLE_CAM_W", 640);
	g_defh  = env_u32("TADPOLE_CAM_H", 480);
	g_fps   = env_u32("TADPOLE_CAM_FPS", 15);
	for (i = 0; i < TAD_CAM_N; i++) {
		g_cam[i].fd = -1;
		g_cam[i].idx = i;
		g_cam[i].width  = g_defw;
		g_cam[i].height = g_defh;
		g_cam[i].pixfmt = TAD_FOURCC_YU12;
		geom_set(&g_cam[i]);
	}
}

/* /dev/videoN -> N, else -1. Only the nodes we serve; anything else is a real
 * device we do not have and must keep failing, exactly like ev_index(). */
int tad_v4l2_index(const char *path)
{
	if (!path || strncmp(path, "/dev/video", 10))
		return -1;
	if (path[10] >= '0' && path[10] < '0' + TAD_CAM_N && path[11] == 0)
		return path[10] - '0';
	return -1;
}

static struct camdev *dev_of_fd(int fd)
{
	int i;
	if (fd < 0) return 0;
	for (i = 0; i < TAD_CAM_N; i++)
		if (g_cam[i].fd == fd)
			return &g_cam[i];
	return 0;
}

int tad_v4l2_is_fd(int fd) { return dev_of_fd(fd) != 0; }

static void queue_reset(struct camdev *c)
{
	int i;
	c->qhead = c->qtail = c->qcount = 0;
	for (i = 0; i < QMAX; i++) c->qflag[i] = 0;
}

int tad_v4l2_open(int idx, int flags)
{
	char path[320];
	struct camdev *c;
	int fd;

	(void)flags;
	if (idx < 0 || idx >= TAD_CAM_N || !g_open)
		return -1;
	c = &g_cam[idx];

	snprintf(path, sizeof(path), "%s/cam%d.bin", g_dir, idx);
	fd = g_open(path, O_RDWR_ | O_CREAT_, 0666);
	if (fd < 0) {
		char m[400];
		snprintf(m, sizeof(m),
		         "[tadpole] cam%d: cannot create %s — the camera will report "
		         "no hardware\n", idx, path);
		note(m);
		return -1;
	}
	/* Big enough for the staging slots from the start; the buffer area grows
	 * when REQBUFS says how many the guest wants. */
	ftruncate(fd, (long)stage_bytes(c));

	/* NEVER CLOSE A DESCRIPTOR THAT IS THE ONE WE JUST OPENED. The guest
	 * closes and reopens the node on every DeinitCameraInt/InitCameraInt
	 * pair, so the kernel hands the same number straight back — and closing
	 * "the old one" would then close the new one, leaving every later read
	 * and write on it failing with EBADF for no visible reason. */
	if (c->fd >= 0 && c->fd != fd && g_close)
		g_close(c->fd);
	c->fd = fd;
	c->streaming = 0;
	c->nbuf = 0;
	c->sequence = 0;
	c->last_seq = 0;
	queue_reset(c);
	if (g_cs) {
		g_cs[idx].open      = 1;
		g_cs[idx].streaming = 0;
	}
	geom_set(c);
	{
		char m[96];
		snprintf(m, sizeof(m), "[tadpole] cam%d: open -> fd %d\n", idx, fd);
		note(m);
	}
	return fd;
}

int tad_v4l2_close(int fd)
{
	struct camdev *c = dev_of_fd(fd);
	if (!c) return 0;
	if (g_cs) {
		g_cs[c->idx].open      = 0;
		g_cs[c->idx].streaming = 0;
		g_cs[c->idx].ov_on     = 0;
	}
	c->ov_on = 0;
	c->fd = -1;
	c->streaming = 0;
	queue_reset(c);
	return 1;
}

/* ---- the frame source ---------------------------------------------------
 *
 * Two sources, in order of preference:
 *   1. whatever the host staged in camN.bin (Android's Camera2, or the
 *      viewer's V4L2 capture on a desktop),
 *   2. $TADPOLE_DIR/camN.jpg, a still, which is how this file was brought up
 *      before either backend existed and what a denied camera permission
 *      should leave the user looking at.
 *
 * Returns the number of bytes placed at the destination, or 0 for "nothing to
 * show yet". */
static u32 stage_read(struct camdev *c, u8 *dst, u32 cap)
{
	struct tad_cam_state *s = g_cs ? &g_cs[c->idx] : 0;
	u32 seq, slot, n;
	long off;

	if (!s || c->fd < 0)
		return 0;
	seq = s->seq;
	if (!seq)
		return 0;
	slot = seq & 1u;
	n = s->bytes[slot];
	if (!n || n > c->slot || n > cap)
		return 0;
	off = (long)(slot * c->slot);
	if (lseek(c->fd, off, 0) != off)
		return 0;
	{
		u32 got = 0;
		while (got < n) {
			long r = read(c->fd, dst + got, n - got);
			if (r <= 0) break;
			got += (u32)r;
		}
		if (got != n)
			return 0;
	}
	c->last_seq = seq;
	return n;
}

static u32 still_read(struct camdev *c, u8 *dst, u32 cap)
{
	char path[320];
	int fd;
	u32 got = 0;

	if (!g_open) return 0;
	snprintf(path, sizeof(path), "%s/cam%d.raw", g_dir, c->idx);
	fd = g_open(path, O_RDONLY_, 0);
	if (fd < 0) return 0;
	for (;;) {
		long r = read(fd, dst + got, cap - got);
		if (r <= 0) break;
		got += (u32)r;
		if (got >= cap) break;
	}
	if (g_close) g_close(fd);
	return got;
}

/* ONE SCRATCH BUFFER, SIZED WHEN THE FORMAT IS. A fixed array would have to be
 * TAD_CAM_MAXFRAME to cover 1600x900, and this library is loaded into every
 * guest process — 8 MB of BSS in AppManager, VideoDaemon and every tool, for a
 * buffer only the camera task ever touches. Allocate it at S_FMT instead and
 * drop it when the geometry changes. */
extern void *malloc(size_t n);
extern void  free(void *p);

static void scratch_drop(struct camdev *c)
{
	if (c->scratch) free(c->scratch);
	c->scratch = 0;
	c->scratch_sz = 0;
}

static int scratch_ready(struct camdev *c)
{
	if (c->scratch && c->scratch_sz >= c->slot)
		return 1;
	scratch_drop(c);
	c->scratch = malloc(c->slot);
	if (!c->scratch)
		return 0;
	c->scratch_sz = c->slot;
	return 1;
}

/* THE CAPTURE BUFFER IS IN VIDEO MEMORY, IN THE MLC's OWN LAYOUT, AND THE
 * GUEST WORKS OUT ITS PITCH FROM bytesused.
 *
 * Three facts, all read out of the guest rather than assumed.
 *
 * 1. It never mmaps our node. InitCameraBufferInt computes
 *
 *        buffer[i] = fb2_mapping + something * i
 *
 *    from the mapping it already has of /dev/fb2, so buffer 0 is the base of
 *    video memory. Measured — the two addresses in the log are the same:
 *
 *        CCameraModule: mmap 82800000: a9fe8000, len 003fc000
 *        InitCameraBufferInt: i=0, flags=00000000, mapping=0xa9fe8000
 *
 *    That is right for hardware whose capture buffers live in the same RAM the
 *    display scans out of, and it means QUERYBUF's m.offset is never read.
 *
 * 2. The layout is the video plane's, not packed planar.
 *    CVIPCameraModule::GetFrame walks the frame as
 *
 *        Y  row y : src + y*P
 *        Cb row y : src + P/2       + (y/2)*P
 *        Cr row y : src + P/2 + h*P/2 + (y/2)*P
 *
 *    which is exactly lf2000fb.c's soc_dpc_set_vid_address arithmetic, and the
 *    same shape the viewer's blit_layer_yuv420() already reads. Writing packed
 *    I420 here produced a photo that plainly contained our test pattern and was
 *    scrambled — the structure was there, the strides were not.
 *
 * 3. AND P COMES FROM US. The same function computes it as
 *
 *        P = frameinfo.size / frameinfo.height
 *
 *    with size being the bytesused we return from DQBUF. So the pitch is ours
 *    to choose, and that is worth choosing carefully, because of:
 *
 * THE ONE PLACE THIS EMULATION IS NOT LIKE THE HARDWARE. On the device /dev/fb2
 * is a separate video heap; here all three framebuffers share one arena, because
 * Brio allocates every surface from a single offset allocator and pans whichever
 * fb matches the pixel format (see the note in tadpole_shim.c's init). Video
 * memory offset 0 is therefore the same page as arena offset 0 — and Brio's
 * allocator hands out its first surface at 0xFF000. So the capture buffer has
 * 0xFF000 bytes of room before it starts overwriting a page that is on screen.
 *
 * The smallest legal pitch is 2*width (luma fills the first half of each row,
 * chroma the second), which for the 640x480 this widget settles on is 1280 and
 * a whole frame of 614400 bytes — comfortably under 0xFF000. The hardware's own
 * 4096 would need 1966080 and would scribble over the viewfinder it is trying
 * to photograph.
 */
/* THE RECORDER'S PITCH IS NOT NEGOTIABLE, AND THE SNAPSHOT'S IS.
 *
 * AVI_set_video hardcodes the AVFrame linesizes for a planar mode:
 *
 *     frame->linesize[0] = linesize[1] = linesize[2] = 4096
 *
 * and AVI_write_frame lays the planes out from the raw capture buffer as
 * data[0] = buf, data[1] = buf + 2048, data[2] = buf + 2048 + height*2048 —
 * the video plane's own arrangement, at a fixed 4096. A frame written with any
 * other pitch encodes to stripes, which is exactly what the first recording
 * produced: nine seconds of correct audio over a video track of coloured
 * bands.
 *
 * The still path has no such constraint: CVIPCameraModule::GetFrame recovers
 * the pitch as frameinfo.size / frameinfo.height, so it accepts whatever we
 * report. It gets the SMALLEST legal pitch instead — 2*width, luma in the
 * first half of each row and chroma in the second — because of the arena
 * collision described above capture_blit_mlc(): at 4096 a 640x480 photograph
 * covers 1966080 bytes and Brio's first surface is at 0xFF000, so the display
 * and the photograph would be writing over each other. At 2*width it is
 * 614400 and they do not meet.
 *
 * Which one is wanted is not guessed. CCameraModule::PollFrame — and nothing
 * else — issues QUERYBUF on a buffer that is already QUEUED, so that is the
 * recorder's frame loop identifying itself. */
static u32 cap_pitch(struct camdev *c)
{
	u32 p;
	if (c->polled)
		return 4096;
	p = c->width * 2;
	return (p + 63u) & ~63u;
}

static u32 cap_size(struct camdev *c)
{
	if (c->pixfmt != TAD_FOURCC_YU12)
		return c->slot;
	return cap_pitch(c) * c->height;
}

/* Where buffer `bi` lives inside the arena. */
static u32 cap_off(struct camdev *c, u32 bi)
{
	return bi * ((cap_size(c) + 4095u) & ~4095u);
}

static void capture_blit_mlc(struct camdev *c, const u8 *src, u32 bi)
{
	u32 w = c->width, h = c->height, P = cap_pitch(c);
	u32 cw = (w + 1) / 2, chh = (h + 1) / 2;
	const u8 *sy = src, *su = src + w * h, *sv = su + cw * chh;
	u8 *dst = g_arena + cap_off(c, bi);
	u32 y;

	for (y = 0; y < h; y++)
		memcpy(dst + y * P, sy + y * w, w);
	for (y = 0; y < h / 2; y++) {
		memcpy(dst + y * P + P / 2, su + y * cw, cw);
		memcpy(dst + (h / 2 + y) * P + P / 2, sv + y * cw, cw);
	}
}

/* Put one frame where the guest will look for buffer `bi`, and return the
 * bytesused it should be told — which for planar YUV420 is pitch*height, not
 * the packed frame size, because that is the division the guest does to
 * recover the pitch. */
static u32 fill_buffer(struct camdev *c, u32 bi, u8 *tmp, u32 tmpcap,
                       int need_new)
{
	struct tad_cam_state *s = g_cs ? &g_cs[c->idx] : 0;
	u32 n;

	/* NEED_NEW IS ABOUT WAITING, NOT ABOUT WHETHER THERE IS A FRAME.
	 *
	 * The first version returned nothing when the staged sequence had not
	 * moved, and then fell through to the still — so a camera running slower
	 * than the guest asks (a dark room drops the front camera to a few frames
	 * a second) produced photographs of the test pattern. What "no new frame"
	 * should mean is "wait a moment", and if it still has not moved, hand back
	 * the most recent one: that is what a real driver's buffer holds. */
	if (s && s->seq) {
		if (need_new && s->seq == c->last_seq)
			return 0;
		n = stage_read(c, tmp, tmpcap);
	} else {
		n = still_read(c, tmp, tmpcap);
	}
	if (!n || !g_arena)
		return 0;

	if (c->pixfmt == TAD_FOURCC_YU12) {
		if (n < c->sizeimage)
			return 0;
		if (cap_off(c, bi) + cap_size(c) > g_arena_bytes)
			return 0;
		capture_blit_mlc(c, tmp, bi);
		return cap_pitch(c) * c->height;
	}
	/* Compressed or packed: straight in, and bytesused is the real length. */
	if (cap_off(c, bi) + n > g_arena_bytes)
		return 0;
	memcpy(g_arena + cap_off(c, bi), tmp, n);
	return n;
}

/* IS THERE A FRAME WAITING, AND WHY QUERYBUF HAS TO SAY SO.
 *
 * This is how the recorder finds out, and it is not the DQBUF-in-a-loop shape
 * a V4L2 client usually has. CameraTaskMain polls:
 *
 *     TryLockMutex -> PollFrame(handle) -> ... -> TaskSleep
 *
 * and CCameraModule::PollFrame walks the buffers issuing QUERYBUF, looking for
 * one whose flags carry V4L2_BUF_FLAG_DONE. Only then does it call GetFrame,
 * which is what dequeues. Our QUERYBUF reported MAPPED and QUEUED and never
 * DONE, so PollFrame answered "nothing yet" for ever: the viewfinder was live,
 * the microphone was recording, the AVI header was written — and the file that
 * came out was thirty-two seconds of sound with not one video frame in it, and
 * then a SIGFPE in AVI_close dividing by a frame count of zero.
 *
 * DONE means "a frame the guest has not seen yet", which is also exactly what
 * paces the recording: no new frame from the host, no DONE, no encode. */
static int frame_ready(struct camdev *c)
{
	struct tad_cam_state *s = g_cs ? &g_cs[c->idx] : 0;
	return s && s->seq && s->seq != c->last_seq;
}

/* ---- ioctl -------------------------------------------------------------- */

static int fail(int err) { *__errno_location() = err; return -1; }

static void fill_capability(struct v4l2_capability *cap, int idx)
{
	memset(cap, 0, sizeof(*cap));
	/* Names the guest never parses but a human reading a log does. */
	memcpy(cap->driver, "lf2000-vip", 11);
	if (idx == 0) memcpy(cap->card, "LF2000 Camera (rear)", 21);
	else          memcpy(cap->card, "LF2000 Camera (front)", 22);
	memcpy(cap->bus_info, "platform:vip.0", 15);
	cap->version = (2 << 16) | (6 << 8) | 32;    /* 2.6.32, like the device */
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE_ | V4L2_CAP_STREAMING_;
}

static void fill_pix(struct camdev *c, struct v4l2_format *f)
{
	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_CAPTURE_;
	f->fmt.pix.width        = c->width;
	f->fmt.pix.height       = c->height;
	f->fmt.pix.pixelformat  = c->pixfmt;
	f->fmt.pix.field        = V4L2_FIELD_NONE_;
	f->fmt.pix.bytesperline = bytes_per_line(c->pixfmt, c->width);
	f->fmt.pix.sizeimage    = c->sizeimage;
	f->fmt.pix.colorspace   = 8;                 /* V4L2_COLORSPACE_JPEG */
}

int tad_v4l2_ioctl(int fd, ulong req, void *arg)
{
	struct camdev *c = dev_of_fd(fd);
	u32 nr;
	char m[192];

	if (!c)
		return -2;                    /* not one of ours */
	nr = V4L2_NR(req);

	switch (nr) {

	case VIDIOC_QUERYCAP_:
		if (!arg) return fail(EINVAL_);
		fill_capability(arg, c->idx);
		return 0;

	case VIDIOC_ENUM_FMT_:
		/* NOTHING, ON PURPOSE — see the long note at the top of this file.
		 * An empty list is what makes CVIPCameraModule::EnumFormats fall
		 * through to the module's own static YUV420 modes, which are the only
		 * ones the firmware's video recorder can encode. */
		return fail(EINVAL_);

	case VIDIOC_ENUM_FRAMESIZES_: {
		struct v4l2_frmsizeenum *e = arg;
		if (!e) return fail(EINVAL_);
		if (e->index >= NSIZES)
			return fail(EINVAL_);
		/* DISCRETE OR NOTHING. InitCameraInt drops any entry whose type is
		 * not V4L2_FRMSIZE_TYPE_DISCRETE and then reports the camera as
		 * having no modes at all, so a stepwise answer is the same as no
		 * answer. */
		e->type = V4L2_FRMSIZE_DISCRETE_;
		e->u.discrete.width  = g_sizes[e->index][0];
		e->u.discrete.height = g_sizes[e->index][1];
		return 0;
	}

	case VIDIOC_ENUM_FRAMEINTERVALS_: {
		struct v4l2_frmivalenum *e = arg;
		if (!e) return fail(EINVAL_);
		if (e->index != 0)
			return fail(EINVAL_);
		e->type = V4L2_FRMIVAL_DISCRETE_;
		e->u.discrete.numerator   = 1;
		e->u.discrete.denominator = g_fps;
		return 0;
	}

	case VIDIOC_G_FMT_:
		if (!arg) return fail(EINVAL_);
		fill_pix(c, arg);
		return 0;

	case VIDIOC_TRY_FMT_:
	case VIDIOC_S_FMT_: {
		struct v4l2_format *f = arg;
		u32 w, h, req_w, req_h, req_f;
		if (!f) return fail(EINVAL_);
		req_w = w = f->fmt.pix.width;
		req_h = h = f->fmt.pix.height;
		req_f = f->fmt.pix.pixelformat;
		/* ACCEPT AND ADJUST, never refuse. CameraWidget::setSize() overwrites
		 * the enumerated mode with a fixed resolution of its own (320x240,
		 * 640x480, 800x600, 1280x960, 1600x900 or 1280x800 depending on a
		 * setting), so refusing anything we did not enumerate would fail a
		 * request the widget considers perfectly reasonable. A V4L2 driver is
		 * allowed to hand back what it can do, and this one does. */
		if (w < 32 || w > 4096) w = g_defw;
		if (h < 32 || h > 4096) h = g_defh;
		{
			u32 fourcc = req_f;
			u32 keep_w = c->width, keep_h = c->height, keep_f = c->pixfmt;

			/* SERVE WHAT IT ASKED FOR. Anything we do not know how to produce
			 * — including the 0 that Brio's own enumeration round trip leaves
			 * behind — becomes planar YUV420, which is what the module's
			 * default mode wanted in the first place. */
			switch (fourcc) {
			case TAD_FOURCC_YU12: case TAD_FOURCC_YUYV:
			case TAD_FOURCC_422P: case TAD_FOURCC_MJPG:
				break;
			default:
				fourcc = TAD_FOURCC_YU12;
				break;
			}
			c->width = w; c->height = h; c->pixfmt = fourcc;
			geom_set(c);
			fill_pix(c, f);
			if (nr == VIDIOC_S_FMT_) {
				snprintf(m, sizeof(m),
				         "[tadpole] cam%d: S_FMT %ux%u fourcc %08x -> %ux%u"
				         " %c%c%c%c %u bytes\n", c->idx, req_w, req_h,
				         req_f, w, h,
				         (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
				         (char)((fourcc >> 16) & 0xff),
				         (char)((fourcc >> 24) & 0xff), c->sizeimage);
				note(m);
				scratch_drop(c);
				if (c->fd >= 0)
					ftruncate(c->fd, (long)stage_bytes(c));
			} else {
				c->width = keep_w; c->height = keep_h; c->pixfmt = keep_f;
				geom_set(c);
			}
		}
		return 0;
	}

	case VIDIOC_G_PARM_:
	case VIDIOC_S_PARM_: {
		struct v4l2_streamparm *p = arg;
		if (!p) return fail(EINVAL_);
		if (nr == VIDIOC_S_PARM_ && p->parm.capture.timeperframe.numerator &&
		    p->parm.capture.timeperframe.denominator) {
			u32 n = p->parm.capture.timeperframe.numerator;
			u32 d = p->parm.capture.timeperframe.denominator;
			if (n && d / n >= 1 && d / n <= 120)
				g_fps = d / n;
		}
		memset(&p->parm, 0, sizeof(p->parm));
		p->type = V4L2_BUF_TYPE_CAPTURE_;
		p->parm.capture.capability   = 0x1000;   /* V4L2_CAP_TIMEPERFRAME */
		p->parm.capture.timeperframe.numerator   = 1;
		p->parm.capture.timeperframe.denominator = g_fps;
		p->parm.capture.readbuffers  = 2;
		return 0;
	}

	case VIDIOC_QUERYCTRL_:
		/* NO CONTROLS AT ALL. InitCameraInt walks V4L2_CID_BASE..+0x26 and
		 * simply skips every id this refuses, so an empty control set is a
		 * supported answer and not a failure — which is what we want, because
		 * nothing here has a brightness knob to turn. */
		return fail(EINVAL_);

	case VIDIOC_G_CTRL_: {
		struct v4l2_control *ct = arg;
		if (ct) ct->value = 0;
		return 0;
	}
	case VIDIOC_S_CTRL_:
		return 0;

	case VIDIOC_S_FBUF_: {
		/* WHERE THE VIEWFINDER ACTUALLY COMES FROM.
		 *
		 * struct v4l2_framebuffer: capability(0) flags(4) base(8) and then a
		 * v4l2_pix_format at 12. CVIPCameraModule::StartVideoCapture fills it
		 * from the tVideoSurf the widget handed it — the surface Brio got
		 * from CDisplayMPI::GetCurrentDisplayHandle(kPixelFormatYUV420) — so
		 * base is a PHYSICAL address inside one of the framebuffers, of the
		 * form fill_fix()'s smem_start plus an offset. Translate it back into
		 * an offset in the arena and the overlay becomes a memcpy. */
		const u32 *fb = arg;
		u32 base, i;
		if (!fb) return fail(EINVAL_);
		base = fb[2];
		c->ov_off   = 0;
		c->ov_w     = fb[3];
		c->ov_h     = fb[4];
		c->ov_fourcc= fb[5];
		c->ov_pitch = fb[7];
		{
			int found = -1;
			for (i = 0; i < g_nlayer && g_smem; i++) {
				if (base >= g_smem[i] && base - g_smem[i] < g_arena_bytes) {
					c->ov_off = base - g_smem[i];
					found = (int)i;
					break;
				}
			}
			snprintf(m, sizeof(m),
			         "[tadpole] cam%d: S_FBUF base %08x -> fb%d +%u, %ux%u"
			         " pitch %u %c%c%c%c\n", c->idx, base, found,
			         c->ov_off, c->ov_w, c->ov_h, c->ov_pitch,
			         (char)(c->ov_fourcc & 0xff),
			         (char)((c->ov_fourcc >> 8) & 0xff),
			         (char)((c->ov_fourcc >> 16) & 0xff),
			         (char)((c->ov_fourcc >> 24) & 0xff));
			note(m);
			if (found < 0)
				c->ov_w = c->ov_h = 0;      /* not ours: refuse to draw */
			if (g_cs) {
				g_cs[c->idx].ov_off   = c->ov_off;
				g_cs[c->idx].ov_w     = c->ov_w;
				g_cs[c->idx].ov_h     = c->ov_h;
				g_cs[c->idx].ov_pitch = c->ov_pitch;
			}
		}
		return 0;
	}

	case VIDIOC_OVERLAY_: {
		const u32 *on = arg;
		c->ov_on = (on && *on) ? 1 : 0;
		if (g_cs) g_cs[c->idx].ov_on = c->ov_on;
		snprintf(m, sizeof(m), "[tadpole] cam%d: OVERLAY %u\n",
		         c->idx, c->ov_on);
		note(m);
		return 0;
	}

	case VIDIOC_REQBUFS_: {
		struct v4l2_requestbuffers *r = arg;
		u32 n;
		if (!r) return fail(EINVAL_);
		n = r->count;
		/* ONE BUFFER, WHATEVER IS ASKED FOR — and a V4L2 driver is allowed to
		 * say so, because count is an out-parameter.
		 *
		 * The reason is in the guest, and it is visible in its own log.
		 * InitCameraBufferInt does not mmap our node; it computes each
		 * buffer's address as fb2_mapping + WIDTH * i, which for a 320-wide
		 * frame means:
		 *
		 *     InitCameraBufferInt: i=0, mapping=0xae0e7000
		 *     InitCameraBufferInt: i=1, mapping=0xae0e7140
		 *     InitCameraBufferInt: i=2, mapping=0xae0e7280
		 *
		 * — 320 bytes apart, which is a fifth of one row. Whatever that
		 * arithmetic is for, it is not three separate frames, so honouring a
		 * request for three would have the guest reading three views of the
		 * same overlapping memory. One buffer is the only self-consistent
		 * answer, and it is enough: the recorder queues and dequeues it. */
		if (n > 1) n = 1;
		if (n > QMAX) n = QMAX;
		if (n == 0) {                 /* the documented "free them" spelling */
			c->nbuf = 0;
			queue_reset(c);
			r->count = 0;
			return 0;
		}
		/* The buffers are in the arena; make sure they fit there. */
		{
			u32 stride = (cap_size(c) + 4095u) & ~4095u;
			if (stride && n > g_arena_bytes / stride)
				n = g_arena_bytes / stride;
			if (n == 0) return fail(EIO_);
		}
		c->nbuf = n;
		queue_reset(c);
		r->count = n;
		snprintf(m, sizeof(m), "[tadpole] cam%d: REQBUFS %u\n", c->idx, n);
		note(m);
		return 0;
	}

	case VIDIOC_QUERYBUF_: {
		struct v4l2_buffer *b = arg;
		if (!b || b->index >= c->nbuf) return fail(EINVAL_);
		{
			u32 i = b->index;
			u8  q = c->qflag[i];
			memset(b, 0, sizeof(*b));
			b->index  = i;
			b->type   = V4L2_BUF_TYPE_CAPTURE_;
			b->memory = V4L2_MEMORY_MMAP_;
			/* The offset the real driver would report: a byte offset
			 * into video memory, which is where the guest computes the
			 * address from anyway. */
			b->m.offset = cap_off(c, i);
			b->length   = (cap_size(c) + 4095u) & ~4095u;
			if (q)
				c->polled = 1;      /* only PollFrame asks about a queued
				                     * buffer — see cap_pitch() */
			b->flags    = V4L2_BUF_FLAG_MAPPED_
			            | (q ? V4L2_BUF_FLAG_QUEUED_ : 0)
			            | (q && frame_ready(c) ? V4L2_BUF_FLAG_DONE_ : 0);
		}
		return 0;
	}

	case VIDIOC_QBUF_: {
		struct v4l2_buffer *b = arg;
		if (!b || b->index >= c->nbuf) return fail(EINVAL_);
		if (!c->qflag[b->index]) {
			c->qflag[b->index] = 1;
			c->queue[c->qtail] = b->index;
			c->qtail = (c->qtail + 1) % QMAX;
			c->qcount++;
		}
		if (g_cs) g_cs[c->idx].n_qbuf++;
		b->flags = V4L2_BUF_FLAG_MAPPED_ | V4L2_BUF_FLAG_QUEUED_;
		return 0;
	}

	case VIDIOC_DQBUF_: {
		struct v4l2_buffer *b = arg;
		u32 bi, n = 0, tries;

		if (!b) return fail(EINVAL_);
		if (g_cs) g_cs[c->idx].n_dqbuf++;
		if (!c->streaming || !c->qcount)
			return fail(EAGAIN_);
		bi = c->queue[c->qhead];

		if (g_cs) g_cs[c->idx].want++;
		/* Wait a little for a fresh frame rather than spinning: the guest
		 * opened the node O_NONBLOCK and its GetFrame retries, but a bare
		 * EAGAIN every time turns the viewfinder into a busy loop. A couple of
		 * hundred milliseconds is longer than any camera's frame interval and
		 * shorter than the guest's own timeout. */
		if (!scratch_ready(c))
			return fail(EIO_);
		for (tries = 0; tries < 40; tries++) {
			n = fill_buffer(c, bi, c->scratch, c->slot, 1);
			if (n) break;
			{
				struct tad_ts t; t.tv_sec = 0; t.tv_nsec = 5 * 1000 * 1000;
				nanosleep(&t, 0);
			}
		}
		if (!n)                        /* nothing new in 200 ms: repeat one */
			n = fill_buffer(c, bi, c->scratch, c->slot, 0);
		if (!n)
			return fail(EAGAIN_);

		c->qhead = (c->qhead + 1) % QMAX;
		c->qcount--;
		c->qflag[bi] = 0;
		if (g_cs) g_cs[c->idx].n_frames++;

		memset(b, 0, sizeof(*b));
		b->index     = bi;
		b->type      = V4L2_BUF_TYPE_CAPTURE_;
		b->memory    = V4L2_MEMORY_MMAP_;
		b->m.offset  = cap_off(c, bi);
		b->length    = (cap_size(c) + 4095u) & ~4095u;
		b->bytesused = n;
		b->flags     = V4L2_BUF_FLAG_MAPPED_ | V4L2_BUF_FLAG_DONE_;
		b->field     = V4L2_FIELD_NONE_;
		b->sequence  = c->sequence++;
		{
			struct tad_ts t;
			if (clock_gettime(CLOCK_MONOTONIC_, &t) == 0) {
				b->timestamp.tv_sec  = t.tv_sec;
				b->timestamp.tv_usec = t.tv_nsec / 1000;
			}
		}
		return 0;
	}

	case VIDIOC_STREAMON_:
		c->streaming = 1;
		c->sequence  = 0;
		c->polled    = 0;
		if (g_cs) g_cs[c->idx].streaming = 1;
		snprintf(m, sizeof(m), "[tadpole] cam%d: STREAMON %ux%u\n",
		         c->idx, c->width, c->height);
		note(m);
		return 0;

	case VIDIOC_STREAMOFF_:
		c->streaming = 0;
		queue_reset(c);
		if (g_cs) g_cs[c->idx].streaming = 0;
		note("[tadpole] cam: STREAMOFF\n");
		return 0;

	default:
		snprintf(m, sizeof(m), "[tadpole] cam%d: unhandled ioctl %08lx\n",
		         c->idx, (ulong)req);
		note(m);
		return fail(EINVAL_);
	}
}
