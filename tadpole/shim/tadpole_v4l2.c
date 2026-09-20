/* Tadpole — a USB camera that is not there: /dev/video0 for the LeapTV.
 *
 * WHY A CAMERA. The LeapTV's pointer is the controller's glowing tip as the
 * camera sees it. GlasgowUI starts a video capture into a surface, runs
 * libVisionMPI's wand tracker over it, and the tracker posts the events the
 * shell's location handler answers to (see tadpole_wand.c for how far the
 * controller side got without one). No camera, no tracker, no pointer — the
 * shell logs kCameraRemovedEvent and never starts VNVisionMPI at all.
 *
 * WHAT THIS IS. Enough of Video4Linux2 for Brio's USB camera module: format
 * and size enumeration, S_FMT/S_PARM, the mmap streaming I/O loop
 * (REQBUFS/QUERYBUF/QBUF/DQBUF/STREAMON), and no controls. The descriptor is
 * a real file, $TADPOLE_DIR/cam.bin, exactly the trick /dev/fb0 uses: the
 * guest's own mmap() of the buffer offsets is a plain file mapping, so no
 * mmap wrapper is needed, and poll()/select() on it report readable, which is
 * what a device with a frame ready would say. DQBUF paces itself to the
 * frame rate.
 *
 * WHAT THE FRAMES SHOW. A flat grey scene with one saturated disc at the
 * mouse — the position the viewer publishes to pointer.bin — in the colour
 * the wand tracker's default calibration calls "green" (its YUV window,
 * read out of libVisionMPI). The tracker then does what it does on the
 * device: finds the blob, drives the hotspots, posts the events.
 *
 * TADPOLE_CAMERA=1 turns it on (the LeapTV profile sets it); without it
 * /dev/video0 does not exist. TADPOLE_CAM_MIRROR=1 flips the blob's x, for
 * when the cursor runs the wrong way; TADPOLE_CAM_YUV="Y U V" changes the
 * colour. The V4L2 structs are laid out for 32-bit ARM by hand, offsets
 * checked against the ioctl sizes the module uses.
 */
typedef unsigned char  u8;
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  ulong;
typedef __SIZE_TYPE__  size_t;
#define NULL ((void *)0)

extern char *getenv(const char *);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int   snprintf(char *, size_t, const char *, ...);
extern int   ftruncate(int, long);
extern int   usleep(u32);
extern long  write(int, const void *, size_t);
extern int  *__errno_location(void);
struct tad_ts { long tv_sec; long tv_nsec; };
extern int   clock_gettime(int, struct tad_ts *);

#define CAM_MAXFD  4096
#define CAM_NBUF   4
#define CAM_W      640
#define CAM_H      480
#define CAM_BUFSZ  (CAM_W * CAM_H * 2)          /* room for YUYV at VGA */
#define FOURCC(a,b,c,d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))
#define PIX_YU12 FOURCC('Y','U','1','2')
#define PIX_YUYV FOURCC('Y','U','Y','V')

/* The requests the module was seen to use, plus QUERYCAP for good measure. */
#define VIDIOC_QUERYCAP        0x80685600u
#define VIDIOC_ENUM_FMT        0xc0405602u
#define VIDIOC_G_FMT           0xc0cc5604u
#define VIDIOC_S_FMT           0xc0cc5605u
#define VIDIOC_REQBUFS         0xc0145608u
#define VIDIOC_QUERYBUF        0xc0445609u
#define VIDIOC_QBUF            0xc044560fu
#define VIDIOC_DQBUF           0xc0445611u
#define VIDIOC_STREAMON        0x40045612u
#define VIDIOC_STREAMOFF       0x40045613u
#define VIDIOC_G_PARM          0xc0cc5615u
#define VIDIOC_S_PARM          0xc0cc5616u
#define VIDIOC_G_CTRL          0xc008561bu
#define VIDIOC_S_CTRL          0xc008561cu
#define VIDIOC_QUERYCTRL       0xc0445624u
#define VIDIOC_TRY_FMT         0xc0d05640u
#define VIDIOC_ENUM_FRAMESIZES 0xc02c564au
#define VIDIOC_ENUM_FRAMEINTERVALS 0xc034564bu
#define EINVAL_ 22
#define EAGAIN_ 11
#define ENOENT_ 2

struct tad_wand_state { u32 magic, x, y, buttons, seq, pad[3]; };
#define WAND_MAGIC 0x444E4157u

static int   g_on = -1;
static u8    g_is[CAM_MAXFD];
static u8   *g_map;                 /* our view of cam.bin */
static struct tad_wand_state *g_ptr;
static u32   g_w = CAM_W, g_h = CAM_H, g_fourcc = PIX_YU12;
static u32   g_nbufs;
static u8    g_queue[CAM_NBUF * 2];
static u32   g_qh, g_qt;
static int   g_streaming;
static u32   g_seq;
static long  g_last_ns;
static int   g_mirror;
static u32   g_yuv[3] = { 220, 219, 179 };
static int   g_dbg;
static u32   g_unknown_logged;

static void dbg(const char *s) { u32 n = 0; while (s[n]) n++; write(2, s, n); }

static int fail(int err) { *__errno_location() = err; return -1; }

static u32 frame_bytes(void)
{
	return g_fourcc == PIX_YUYV ? g_w * g_h * 2 : g_w * g_h * 3 / 2;
}

static void env_init(void)
{
	const char *e;
	g_dbg = getenv("TADPOLE_DEBUG") != NULL;
	g_mirror = (e = getenv("TADPOLE_CAM_MIRROR")) != NULL && *e && *e != '0';
	if ((e = getenv("TADPOLE_CAM_YUV")) != NULL) {
		u32 k;
		for (k = 0; k < 3 && *e; k++) {
			u32 v = 0;
			while (*e == ' ') e++;
			while (*e >= '0' && *e <= '9') v = v * 10 + (u32)(*e++ - '0');
			g_yuv[k] = v > 255 ? 255 : v;
		}
	}
}

int tad_cam_match(const char *path)
{
	const char *e;
	if (!path || !(path[0]=='/'&&path[1]=='d'&&path[2]=='e'&&path[3]=='v'&&path[4]=='/'&&
	               path[5]=='v'&&path[6]=='i'&&path[7]=='d'&&path[8]=='e'&&path[9]=='o'))
		return 0;
	if (g_on < 0) {
		e = getenv("TADPOLE_CAMERA");
		g_on = (e && *e && *e != '0') ? 1 : 0;
		if (g_on) env_init();
	}
	return 1;
}

/* -> a real descriptor onto cam.bin, or -1 (ENOENT) for video1.. and for a
 * camera that is switched off. */
int tad_cam_open(const char *path, const char *dir,
                 int (*real_open)(const char *, int, ...),
                 void *(*real_mmap)(void *, size_t, int, int, int, long))
{
	char p[600];
	int fd;
	if (!g_on || path[10] != '0' || path[11] != 0) return fail(ENOENT_);
	snprintf(p, sizeof p, "%s/cam.bin", dir);
	fd = real_open(p, 02 | 0100 /* O_RDWR|O_CREAT */, 0666);
	if (fd < 0) return -1;
	ftruncate(fd, (long)(CAM_NBUF * CAM_BUFSZ));
	if (!g_map) {
		void *m = real_mmap(NULL, CAM_NBUF * CAM_BUFSZ, 3, 1 /* MAP_SHARED */, fd, 0);
		if (m != (void *)-1) g_map = m;
	}
	if (!g_ptr) {
		int pfd;
		snprintf(p, sizeof p, "%s/pointer.bin", dir);
		pfd = real_open(p, 02 | 0100, 0666);
		if (pfd >= 0) {
			void *m;
			ftruncate(pfd, 4096);
			m = real_mmap(NULL, 4096, 3, 1, pfd, 0);
			if (m != (void *)-1) g_ptr = m;
			/* pfd is closed by the caller's close() path? No — it is ours. */
			extern int close(int);
			close(pfd);
		}
	}
	if (fd < CAM_MAXFD) g_is[fd] = 1;
	if (g_dbg) dbg("[cam] /dev/video0 opened (fake USB camera)\n");
	return fd;
}

int tad_cam_is(int fd) { return fd >= 0 && fd < CAM_MAXFD && g_is[fd]; }

void tad_cam_close(int fd) { if (fd >= 0 && fd < CAM_MAXFD) g_is[fd] = 0; }

/* ---- the picture ---------------------------------------------------------- */

static void fill_disc(u8 *plane, u32 pw, u32 ph, i32 cx, i32 cy, i32 r, u8 v)
{
	i32 y, x;
	for (y = cy - r; y <= cy + r; y++) {
		if (y < 0 || y >= (i32)ph) continue;
		for (x = cx - r; x <= cx + r; x++) {
			if (x < 0 || x >= (i32)pw) continue;
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
				plane[(u32)y * pw + (u32)x] = v;
		}
	}
}

static void render(u8 *buf)
{
	i32 cx = -1000, cy = -1000, r = (i32)(g_w / 24);
	if (g_ptr && g_ptr->magic == WAND_MAGIC) {
		u32 px = g_ptr->x, py = g_ptr->y;
		if (px < 1280 && py < 720) {
			cx = (i32)((g_mirror ? 1279u - px : px) * g_w / 1280u);
			cy = (i32)(py * g_h / 720u);
		}
	}
	if (g_fourcc == PIX_YUYV) {
		u32 i, n = g_w * g_h;
		u8 *p = buf;
		for (i = 0; i < n; i += 2) { *p++ = 48; *p++ = 128; *p++ = 48; *p++ = 128; }
		/* the disc, as Y then chroma pairs */
		{
			i32 y, x;
			for (y = cy - r; y <= cy + r; y++) {
				if (y < 0 || y >= (i32)g_h) continue;
				for (x = cx - r; x <= cx + r; x++) {
					if (x < 0 || x >= (i32)g_w) continue;
					if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
					buf[((u32)y * g_w + (u32)x) * 2] = (u8)g_yuv[0];
					buf[((u32)y * g_w + ((u32)x & ~1u)) * 2 + 1] = (u8)g_yuv[1];
					buf[((u32)y * g_w + ((u32)x & ~1u)) * 2 + 3] = (u8)g_yuv[2];
				}
			}
		}
		return;
	}
	/* YU12: Y plane, then U and V at quarter size */
	memset(buf, 48, g_w * g_h);
	memset(buf + g_w * g_h, 128, (g_w / 2) * (g_h / 2) * 2);
	fill_disc(buf, g_w, g_h, cx, cy, r, (u8)g_yuv[0]);
	fill_disc(buf + g_w * g_h, g_w / 2, g_h / 2, cx / 2, cy / 2, r / 2, (u8)g_yuv[1]);
	fill_disc(buf + g_w * g_h + (g_w / 2) * (g_h / 2), g_w / 2, g_h / 2, cx / 2, cy / 2, r / 2, (u8)g_yuv[2]);
}

/* ---- the ioctls ------------------------------------------------------------ */

static u32 rd32(const void *p, u32 off) { u32 v; memcpy(&v, (const u8 *)p + off, 4); return v; }
static void wr32(void *p, u32 off, u32 v) { memcpy((u8 *)p + off, &v, 4); }
static void wrstr(void *p, u32 off, const char *s, u32 max)
{ u32 i; for (i = 0; i < max; i++) { ((u8 *)p)[off + i] = (u8)s[i]; if (!s[i]) break; } }

static void fill_pix(void *f)
{
	/* struct v4l2_format: type; then v4l2_pix_format at +4 */
	wr32(f, 0, 1);                               /* V4L2_BUF_TYPE_VIDEO_CAPTURE */
	wr32(f, 4, g_w); wr32(f, 8, g_h); wr32(f, 12, g_fourcc);
	wr32(f, 16, 1);                              /* V4L2_FIELD_NONE */
	wr32(f, 20, g_fourcc == PIX_YUYV ? g_w * 2 : g_w);
	wr32(f, 24, frame_bytes());
	wr32(f, 28, 8);                              /* V4L2_COLORSPACE_JPEG */
	wr32(f, 32, 0);
}

static void now_ts(u32 *sec, u32 *usec)
{
	struct tad_ts t; t.tv_sec = 0; t.tv_nsec = 0;
	clock_gettime(1, &t);
	*sec = (u32)t.tv_sec; *usec = (u32)(t.tv_nsec / 1000);
}

int tad_cam_ioctl(int fd, ulong req, void *arg)
{
	u32 r = (u32)req;
	(void)fd;
	if (!arg) return fail(EINVAL_);
	switch (r) {
	case VIDIOC_QUERYCAP:
		memset(arg, 0, 104);
		wrstr(arg, 0, "tadpole", 16);
		wrstr(arg, 16, "LeapTV Camera", 32);
		wrstr(arg, 48, "usb-tadpole-1", 32);
		wr32(arg, 80, 0x00030418);                /* kernel 3.4.24 */
		wr32(arg, 84, 0x04000001);                /* VIDEO_CAPTURE | STREAMING */
		wr32(arg, 88, 0x04000001);
		return 0;
	case VIDIOC_ENUM_FMT: {
		u32 idx = rd32(arg, 0);
		if (idx > 1) return fail(EINVAL_);
		memset((u8 *)arg + 8, 0, 56);
		wr32(arg, 4, 1);
		wrstr(arg, 12, idx == 0 ? "Planar YUV 4:2:0" : "YUYV 4:2:2", 32);
		wr32(arg, 44, idx == 0 ? PIX_YU12 : PIX_YUYV);
		return 0;
	}
	case VIDIOC_ENUM_FRAMESIZES: {
		u32 idx = rd32(arg, 0), pf = rd32(arg, 4);
		if ((pf != PIX_YU12 && pf != PIX_YUYV) || idx > 1) return fail(EINVAL_);
		wr32(arg, 8, 1);                          /* DISCRETE */
		wr32(arg, 12, idx == 0 ? 640u : 320u);
		wr32(arg, 16, idx == 0 ? 480u : 240u);
		wr32(arg, 36, 0); wr32(arg, 40, 0);
		return 0;
	}
	case VIDIOC_ENUM_FRAMEINTERVALS: {
		u32 idx = rd32(arg, 0);
		if (idx > 0) return fail(EINVAL_);
		wr32(arg, 16, 1);                         /* DISCRETE */
		wr32(arg, 20, 1); wr32(arg, 24, 30);      /* 1/30 s */
		wr32(arg, 44, 0); wr32(arg, 48, 0);
		return 0;
	}
	case VIDIOC_G_FMT:
		fill_pix(arg);
		return 0;
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT: {
		u32 w = rd32(arg, 4), h = rd32(arg, 8), pf = rd32(arg, 12);
		u32 nw = (w <= 320) ? 320 : 640, nh = (nw == 320) ? 240 : 480;
		u32 npf = (pf == PIX_YUYV) ? PIX_YUYV : PIX_YU12;
		if (r == VIDIOC_S_FMT) { g_w = nw; g_h = nh; g_fourcc = npf; }
		{
			u32 sw = g_w, sh = g_h, sf = g_fourcc;
			g_w = nw; g_h = nh; g_fourcc = npf;
			fill_pix(arg);
			if (r != VIDIOC_S_FMT) { g_w = sw; g_h = sh; g_fourcc = sf; }
		}
		if (g_dbg) {
			char b[96];
			snprintf(b, sizeof b, "[cam] %s %ux%u %c%c%c%c\n",
			         r == VIDIOC_S_FMT ? "S_FMT" : "TRY_FMT", nw, nh,
			         (int)(npf & 255), (int)((npf >> 8) & 255), (int)((npf >> 16) & 255), (int)(npf >> 24));
			dbg(b);
		}
		return 0;
	}
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM:
		wr32(arg, 0, 1);
		wr32(arg, 4, 0x1000);                     /* V4L2_CAP_TIMEPERFRAME */
		wr32(arg, 8, 0);
		wr32(arg, 12, 1); wr32(arg, 16, 30);
		wr32(arg, 20, 0); wr32(arg, 24, CAM_NBUF);
		return 0;
	case VIDIOC_REQBUFS: {
		u32 n = rd32(arg, 0), mem = rd32(arg, 8);
		if (mem != 1 /* MMAP */) return fail(EINVAL_);
		if (n > CAM_NBUF) n = CAM_NBUF;
		g_nbufs = n; g_qh = g_qt = 0;
		wr32(arg, 0, n);
		if (g_dbg) { char b[48]; snprintf(b, sizeof b, "[cam] REQBUFS %u\n", n); dbg(b); }
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		u32 idx = rd32(arg, 0);
		if (idx >= g_nbufs) return fail(EINVAL_);
		memset(arg, 0, 68);
		wr32(arg, 0, idx); wr32(arg, 4, 1);
		wr32(arg, 12, 0x2);                       /* V4L2_BUF_FLAG_MAPPED */
		wr32(arg, 48, 1);                         /* MMAP */
		wr32(arg, 52, idx * CAM_BUFSZ);           /* m.offset */
		wr32(arg, 56, CAM_BUFSZ);                 /* length */
		return 0;
	}
	case VIDIOC_QBUF: {
		u32 idx = rd32(arg, 0);
		if (idx >= g_nbufs) return fail(EINVAL_);
		g_queue[g_qh % (CAM_NBUF * 2)] = (u8)idx; g_qh++;
		wr32(arg, 12, rd32(arg, 12) | 0x1 | 0x2); /* QUEUED | MAPPED */
		return 0;
	}
	case VIDIOC_DQBUF: {
		u32 idx, sec, usec;
		struct tad_ts t;
		long ns;
		if (g_qt == g_qh) return fail(EAGAIN_);
		/* Pace to the frame rate: the tracker runs at the camera's speed. */
		clock_gettime(1, &t);
		ns = t.tv_sec * 1000000000L + t.tv_nsec;
		if (g_last_ns && ns - g_last_ns < 33000000L)
			usleep((u32)((33000000L - (ns - g_last_ns)) / 1000));
		clock_gettime(1, &t);
		g_last_ns = t.tv_sec * 1000000000L + t.tv_nsec;
		idx = g_queue[g_qt % (CAM_NBUF * 2)]; g_qt++;
		if (g_map) render(g_map + idx * CAM_BUFSZ);
		now_ts(&sec, &usec);
		memset(arg, 0, 68);
		wr32(arg, 0, idx); wr32(arg, 4, 1);
		wr32(arg, 8, frame_bytes());
		wr32(arg, 12, 0x2 | 0x4);                 /* MAPPED | DONE */
		wr32(arg, 16, 1);
		wr32(arg, 20, sec); wr32(arg, 24, usec);
		wr32(arg, 44, g_seq++);
		wr32(arg, 48, 1);
		wr32(arg, 52, idx * CAM_BUFSZ);
		wr32(arg, 56, CAM_BUFSZ);
		return 0;
	}
	case VIDIOC_STREAMON:
		g_streaming = 1; g_last_ns = 0;
		if (g_dbg) dbg("[cam] STREAMON\n");
		return 0;
	case VIDIOC_STREAMOFF:
		g_streaming = 0; g_qh = g_qt = 0;
		if (g_dbg) dbg("[cam] STREAMOFF\n");
		return 0;
	case VIDIOC_QUERYCTRL:
	case VIDIOC_G_CTRL:
	case VIDIOC_S_CTRL:
		return fail(EINVAL_);                     /* no controls, honestly */
	default:
		if (g_dbg && g_unknown_logged < 12) {
			char b[64];
			g_unknown_logged++;
			snprintf(b, sizeof b, "[cam] unhandled ioctl 0x%08x\n", r);
			dbg(b);
		}
		return fail(EINVAL_);
	}
}
