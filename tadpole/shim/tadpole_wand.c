/* Tadpole — a stand-in LeapTV controller: the viewer's mouse as the wand.
 *
 * WHAT THE POINTER IS ON A LeapTV. The controller's tip glows, the camera
 * watches it, and libVisionMPI's wand tracker turns the blob into a location
 * that libControllerMPI hangs off the HWController object. GlasgowUI polls
 * that object every frame — HWController::GetLocation(), GetButtonData(),
 * GetCurrentMode() — and navigates by it. Neither the Bluetooth stack nor the
 * camera exists here.
 *
 * WHERE THIS STANDS IN. GlasgowUI reaches the controller through exactly a
 * dozen NON-VIRTUAL methods, all imported by name, and this library sits ahead
 * of libControllerMPI in its symbol scope (it is libGLESv2.so at the first
 * level; see the Makefile's note on SHIM_OUT_EGL). So defining those same
 * mangled names here makes the shell's calls land on us: one controller,
 * always connected, whose location and buttons are whatever the viewer last
 * wrote to $TADPOLE_DIR/pointer.bin from its mouse. The real library keeps
 * running underneath — its Bluetooth scan fails harmlessly — and every call
 * made while TADPOLE_WAND is unset is forwarded to it untouched, found by the
 * same object walk the shim uses for libdl.
 *
 * THE ABI IS AAPCS, NOT GUESSWORK. A method is a function with `this` first;
 * a struct wider than four bytes comes back through a hidden pointer, which
 * is what a C function returning the same-sized struct does. The sizes below
 * are the SDK's: VNPoint is cv::Point (two ints), tButtonData2 is two masks
 * and a timeval, tHWAnalogStickData two floats and a timeval.
 *
 * A PROOF OF CONCEPT, and honest about what it does not know: which mode
 * value means "wand", and which button bits are A and B, are guesses that
 * TADPOLE_WAND_MODE and TADPOLE_WAND_BUTTONS override — see wand_init().
 */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;
typedef unsigned long  ulong;
typedef __SIZE_TYPE__  size_t;
#define NULL ((void *)0)

extern char *getenv(const char *);
extern int   open(const char *, int, ...);
extern void *mmap(void *, size_t, int, int, int, long);
extern int   close(int);
extern int   ftruncate(int, long);
extern int   snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern long  write(int, const void *, size_t);
struct tad_ts { long tv_sec; long tv_nsec; };
extern int   clock_gettime(int, struct tad_ts *);

/* tadpole_shim.c: a symbol out of a named module, found by the loader's own
 * object list rather than by RTLD_NEXT (whose scope rules are the reason that
 * file has the helper at all). */
extern void *tad_module_symbol(const char *lib, const char *name);
extern void *tad_module_base(const char *lib);

#define O_RDWR_   02
#define O_CREAT_  0100
#define PROT_RW   3
#define MAP_SHARED_ 1

/* What the viewer writes: panel-pixel position, an SDL-style button mask
 * (bit 0 left, bit 1 middle, bit 2 right), and a counter so a reader can tell
 * a stale file from a fresh one. Kept at 32 bytes; the file is a page. */
struct tad_wand_state { u32 magic, x, y, buttons, seq, pad[3]; };
#define WAND_MAGIC 0x444E4157u             /* 'WAND' */

static int   g_on = -1;                    /* -1 unknown, 0 off, 1 on */
static struct tad_wand_state *g_ws;
static u32   g_mode = 1;                   /* TADPOLE_WAND_MODE */
static u32   g_btn[3] = { 0x10, 0x20, 0x100 };  /* left, middle, right -> A, B, Menu */
static u32   g_last_state;
static unsigned char g_dummy[512];         /* the one controller; only we look inside */
static int   g_dbg;
/* THE SHELL DOES NOT POLL — it registers the controller and then waits for
 * Brio events. So the stand-in posts them, from the frame tick eglSwapBuffers
 * gives it, with the same message class and the same CEventMPI the real
 * library uses. The event TYPE values are computed by libControllerMPI's own
 * static initialisers into its .bss; for this build (7.0.1.2634) the twelve
 * candidates sit at the offsets below, and which one means "button state
 * changed" is chosen by TADPOLE_WAND_EVT while it is being established. */
static u32   g_evt_off[4];                 /* offsets of the types to post on a button change */
static u32   g_connect_off[4];             /* ... and once, at the first tick, if set */
static u32   g_last_seq, g_last_btn_mask;
static int   g_connected_sent;
static int   g_seen_getall;                 /* the shell has asked for controllers */
static u32   g_ticks_since_getall;
static const u32 g_evt_table[] = { 0x1a0a8, 0x1a0ac, 0x1a0b0, 0x1a0b4, 0x1a0b8, 0x1a0bc,
                                   0x1a0c0, 0x1a0c4, 0x1a0c8, 0x1a0cc,
                                   0x1a3e8, 0x1a3ec, 0x1a3f0 };

static void dbg(const char *s)
{ u32 n = 0; while (s[n]) n++; write(2, s, n); }

/* Say which methods the shell actually calls, three times each, under
 * TADPOLE_DEBUG. Whether it polls the location or waits for events is the
 * whole question this stand-in has to answer. */
static void trace(const char *what, u32 *count)
{
	if (!g_dbg || *count >= 3) return;
	(*count)++;
	dbg("[wand] "); dbg(what); dbg("\n");
}

static u32 env_u32(const char *name, u32 dflt)
{
	const char *e = getenv(name);
	u32 v = 0; int hex;
	if (!e || !*e) return dflt;
	hex = (e[0] == '0' && (e[1] == 'x' || e[1] == 'X'));
	if (hex) e += 2;
	while (*e) {
		u32 d = (*e >= '0' && *e <= '9') ? (u32)(*e - '0')
		      : (*e >= 'a' && *e <= 'f') ? (u32)(*e - 'a' + 10)
		      : (*e >= 'A' && *e <= 'F') ? (u32)(*e - 'A' + 10) : 99;
		if (d > (hex ? 15u : 9u)) break;
		v = v * (hex ? 16u : 10u) + d; e++;
	}
	return v;
}

/* "0x1a0a8 0x1a0ac": up to four offsets, from the variable or the default. */
static void env_offsets(const char *name, u32 *out, const char *dflt)
{
	const char *e = getenv(name);
	int k;
	if (!e) e = dflt;
	for (k = 0; k < 4; k++) out[k] = 0;
	for (k = 0; e && *e && k < 4; k++) {
		u32 v = 0; int hex;
		while (*e == ' ') e++;
		if (!*e) break;
		hex = (e[0] == '0' && (e[1] == 'x' || e[1] == 'X'));
		if (hex) e += 2;
		while (*e && *e != ' ') {
			u32 dd = (*e >= '0' && *e <= '9') ? (u32)(*e - '0')
			       : (*e >= 'a' && *e <= 'f') ? (u32)(*e - 'a' + 10)
			       : (*e >= 'A' && *e <= 'F') ? (u32)(*e - 'A' + 10) : 0;
			v = v * (hex ? 16u : 10u) + dd; e++;
		}
		out[k] = v;
	}
}

static void wand_init(void)
{
	const char *e, *d;
	char path[600];
	int fd;
	void *m;
	if (g_on >= 0) return;
	g_on = 0;
	e = getenv("TADPOLE_WAND");
	if (!e || !*e || *e == '0') return;
	g_dbg = getenv("TADPOLE_DEBUG") != NULL;
	d = getenv("TADPOLE_DIR"); if (!d) d = "/tmp/tadpole";
	snprintf(path, sizeof path, "%s/pointer.bin", d);
	fd = open(path, O_RDWR_ | O_CREAT_, 0666);
	if (fd < 0) { dbg("[wand] cannot open pointer.bin\n"); return; }
	ftruncate(fd, 4096);
	m = mmap(NULL, 4096, PROT_RW, MAP_SHARED_, fd, 0);
	close(fd);
	if (m == (void *)-1) { dbg("[wand] cannot map pointer.bin\n"); return; }
	g_ws = m;
	g_mode = env_u32("TADPOLE_WAND_MODE", 1);
	/* "0x10 0x20 0x100": the button bits for left, middle and right. */
	e = getenv("TADPOLE_WAND_BUTTONS");
	if (e && *e) {
		int k;
		for (k = 0; k < 3 && *e; k++) {
			u32 v = 0; int hex;
			while (*e == ' ') e++;
			hex = (e[0] == '0' && (e[1] == 'x' || e[1] == 'X'));
			if (hex) e += 2;
			while (*e && *e != ' ') {
				u32 dd = (*e >= '0' && *e <= '9') ? (u32)(*e - '0')
				       : (*e >= 'a' && *e <= 'f') ? (u32)(*e - 'a' + 10)
				       : (*e >= 'A' && *e <= 'F') ? (u32)(*e - 'A' + 10) : 0;
				v = v * (hex ? 16u : 10u) + dd; e++;
			}
			g_btn[k] = v;
		}
	}
	/* WHICH EVENTS, BY OFFSET INTO libControllerMPI's .bss — see g_evt_table.
	 * Measured on 7.0.1.2634: the first of the three types the controller
	 * constructor pre-builds (+0x1a3e8) makes GlasgowUI read the analog
	 * stick, and posting the other two (+0x1a3ec, +0x1a3f0) makes it read the
	 * buttons and act on them, so those two are the button default. The
	 * table is checked before anything is posted: its values run
	 * 0x10007002.. in order, and a build where they do not gets no events
	 * rather than the wrong ones. TADPOLE_WAND_EVT and
	 * TADPOLE_WAND_CONNECT_EVT are the overrides, as space-separated hex. */
	env_offsets("TADPOLE_WAND_EVT", g_evt_off, "0x1a3ec 0x1a3f0");
	/* Connected, then mode-changed, once the shell is listening: with these
	 * it asks the controller its mode and re-reads the controller list. */
	env_offsets("TADPOLE_WAND_CONNECT_EVT", g_connect_off, "0x1a0c4 0x1a0bc");
	{
		const u8 *b = tad_module_base("libControllerMPI");
		if (!b || *(const u32 *)(b + 0x1a0a8) != 0x10007002u ||
		    *(const u32 *)(b + 0x1a0ac) != 0x10007003u) {
			int k;
			for (k = 0; k < 4; k++) g_evt_off[k] = g_connect_off[k] = 0;
			dbg("[wand] libControllerMPI's event table is not the one this was"
			    " measured on; buttons will not be posted\n");
		}
	}
	g_on = 1;
	dbg("[wand] mouse is the controller (TADPOLE_WAND)\n");
	/* TRIAL ONLY: the five event types GlasgowUI's controller handler
	 * recognises, out of its own data (a non-PIE executable, so the addresses
	 * are its link addresses — 7.0.1.2634's GlasgowUI and nothing else). */
	if (getenv("TADPOLE_WAND_PEEK")) {
		static const u32 at[5] = { 0x1c4720, 0x1c4724, 0x1c4728, 0x1c472c, 0x1c4734 };
		u32 i; char line[80];
		for (i = 0; i < 5; i++) {
			snprintf(line, sizeof line, "[wand] shell handles type 0x%08x\n", *(const u32 *)at[i]);
			dbg(line);
		}
	}
	if (g_dbg) {
		const u8 *b = tad_module_base("libControllerMPI");
		u32 i;
		char line[96];
		if (b) for (i = 0; i < sizeof g_evt_table / sizeof g_evt_table[0]; i++) {
			snprintf(line, sizeof line, "[wand] event table +0x%05x = 0x%08x\n",
			         g_evt_table[i], *(const u32 *)(b + g_evt_table[i]));
			dbg(line);
		}
	}
}

/* Post one HWControllerEventMessage of `type` for our controller through a
 * CEventMPI of our own — exactly the two calls libControllerMPI makes. */
static void post_event(u32 type)
{
	static unsigned char evmpi[64];
	static int have_evmpi;
	static unsigned char msg[64];
	void (*ev_ctor)(void *) = tad_module_symbol("libEventMPI", "_ZN8LeapFrog4Brio9CEventMPIC1Ev");
	u32  (*ev_post)(const void *, const void *, u8, const void *) =
		tad_module_symbol("libEventMPI",
		    "_ZNK8LeapFrog4Brio9CEventMPI9PostEventERKNS0_13IEventMessageEhPKNS0_14IEventListenerE");
	void (*msg_ctor)(void *, ulong, const void *) = tad_module_symbol("libControllerMPI",
		    "_ZN2LF8Hardware24HWControllerEventMessageC1EmPKNS0_12HWControllerE");
	char line[64];
	if (!ev_ctor || !ev_post || !msg_ctor) { dbg("[wand] cannot post: symbols missing\n"); return; }
	if (!have_evmpi) { ev_ctor(evmpi); have_evmpi = 1; }
	msg_ctor(msg, type, g_dummy);
	{
		u32 r = ev_post(evmpi, msg, 0, NULL);
		if (g_dbg) { snprintf(line, sizeof line, "[wand] posted 0x%08x -> %u\n", type, r); dbg(line); }
	}
}

/* An entry is either an offset into libControllerMPI's table (a few hundred
 * KB at most) or, from 0x01000000 up, a literal event type — Brio's are
 * 0x1000xxxx. The vision group's events are not in that table, and the
 * shell's location handler answers to those, which is what the literal form
 * is for. */
static u32 evt_at(u32 off)
{
	const u8 *b = tad_module_base("libControllerMPI");
	if (off >= 0x01000000u) return off;
	return (b && off) ? *(const u32 *)(b + off) : 0;
}

/* Called once per frame from eglSwapBuffers. */
void tad_wand_tick(void)
{
	wand_init();
	if (!g_on || !g_ws || g_ws->magic != WAND_MAGIC) return;
	/* NOT ON THE FIRST FRAME. The shell registers its controller listener
	 * after it has asked for the controller list, and an event posted before
	 * that is dropped on the floor. Half a second after GetAllControllers is
	 * comfortably after. */
	if (!g_connected_sent && g_seen_getall && ++g_ticks_since_getall > 30) {
		int k;
		g_connected_sent = 1;
		for (k = 0; k < 4; k++) if (g_connect_off[k]) post_event(evt_at(g_connect_off[k]));
	}
	if (g_ws->seq == g_last_seq) return;
	g_last_seq = g_ws->seq;
	if (g_ws->buttons != g_last_btn_mask) {
		g_last_btn_mask = g_ws->buttons;
		{ int k; for (k = 0; k < 4; k++) if (g_evt_off[k]) post_event(evt_at(g_evt_off[k])); }
	}
}

static u32 button_state(void)
{
	u32 b = g_ws ? g_ws->buttons : 0, s = 0;
	if (b & 1) s |= g_btn[0];
	if (b & 2) s |= g_btn[1];
	if (b & 4) s |= g_btn[2];
	return s;
}

static void now(u32 *sec, u32 *usec)
{
	struct tad_ts t;
	t.tv_sec = 0; t.tv_nsec = 0;
	clock_gettime(1 /* CLOCK_MONOTONIC */, &t);
	*sec = (u32)t.tv_sec; *usec = (u32)(t.tv_nsec / 1000);
}

/* ---- the controller object ---------------------------------------------- */

struct vnpoint { i32 x, y; };
struct button_data { u32 state, transition, tv_sec, tv_usec; };
struct stick_data  { float x, y; u32 tv_sec, tv_usec; };

#define S_GETLOC   "_ZNK2LF8Hardware12HWController11GetLocationEv"
#define S_ISCONN   "_ZNK2LF8Hardware12HWController11IsConnectedEv"
#define S_BUTTONS  "_ZNK2LF8Hardware12HWController13GetButtonDataEv"
#define S_MODE     "_ZNK2LF8Hardware12HWController14GetCurrentModeEv"
#define S_STICK    "_ZNK2LF8Hardware12HWController18GetAnalogStickDataEv"
#define S_GETID    "_ZNK2LF8Hardware12HWController5GetIDEv"
#define S_ALL      "_ZN2LF8Hardware15HWControllerMPI17GetAllControllersERSt6vectorIPNS0_12HWControllerESaIS4_EE"
#define S_BYID     "_ZN2LF8Hardware15HWControllerMPI17GetControllerByIDEm"

#define REAL(sym) tad_module_symbol("libControllerMPI", sym)

struct vnpoint wand_GetLocation(const void *self) __asm__(S_GETLOC);
struct vnpoint wand_GetLocation(const void *self)
{
	struct vnpoint p;
	wand_init();
	{ static u32 n; trace("GetLocation", &n); }
	if (!g_on) {
		struct vnpoint (*real)(const void *) = REAL(S_GETLOC);
		if (real) return real(self);
		p.x = p.y = 0; return p;
	}
	p.x = g_ws ? (i32)g_ws->x : 0;
	p.y = g_ws ? (i32)g_ws->y : 0;
	return p;
}

int wand_IsConnected(const void *self) __asm__(S_ISCONN);
int wand_IsConnected(const void *self)
{
	wand_init();
	{ static u32 n; trace("IsConnected", &n); }
	if (!g_on) { int (*real)(const void *) = REAL(S_ISCONN); return real ? real(self) : 0; }
	return 1;
}

u32 wand_GetID(const void *self) __asm__(S_GETID);
u32 wand_GetID(const void *self)
{
	wand_init();
	{ static u32 n; trace("GetID", &n); }
	if (!g_on) { u32 (*real)(const void *) = REAL(S_GETID); return real ? real(self) : 0; }
	return 0;
}

u32 wand_GetCurrentMode(const void *self) __asm__(S_MODE);
u32 wand_GetCurrentMode(const void *self)
{
	wand_init();
	{ static u32 n; trace("GetCurrentMode", &n); }
	if (!g_on) { u32 (*real)(const void *) = REAL(S_MODE); return real ? real(self) : 0; }
	return g_mode;
}

struct button_data wand_GetButtonData(const void *self) __asm__(S_BUTTONS);
struct button_data wand_GetButtonData(const void *self)
{
	struct button_data b;
	wand_init();
	{ static u32 n; trace("GetButtonData", &n); }
	if (!g_on) {
		struct button_data (*real)(const void *) = REAL(S_BUTTONS);
		if (real) return real(self);
		b.state = b.transition = b.tv_sec = b.tv_usec = 0; return b;
	}
	b.state = button_state();
	b.transition = b.state ^ g_last_state;
	g_last_state = b.state;
	now(&b.tv_sec, &b.tv_usec);
	return b;
}

struct stick_data wand_GetAnalogStickData(const void *self) __asm__(S_STICK);
struct stick_data wand_GetAnalogStickData(const void *self)
{
	struct stick_data s;
	wand_init();
	{ static u32 n; trace("GetAnalogStickData", &n); }
	if (!g_on) {
		struct stick_data (*real)(const void *) = REAL(S_STICK);
		if (real) return real(self);
	}
	s.x = 0.0f; s.y = 0.0f;
	now(&s.tv_sec, &s.tv_usec);
	return s;
}

/* std::vector<HWController*>&: three pointers in libstdc++. Empty on the way
 * in (the real library has no controllers), one entry on the way out. The
 * storage is malloc'd because that is what libstdc++'s operator delete will
 * eventually free it with. */
struct vec3 { void **start, **finish, **eos; };

void wand_GetAllControllers(void *self, struct vec3 *v) __asm__(S_ALL);
void wand_GetAllControllers(void *self, struct vec3 *v)
{
	wand_init();
	{ static u32 n; trace("GetAllControllers", &n); }
	if (!g_on) {
		void (*real)(void *, struct vec3 *) = REAL(S_ALL);
		if (real) real(self, v);
		return;
	}
	if (!v) return;
	g_seen_getall = 1;
	if (v->start == v->finish) {
		if (v->eos == v->start) {
			void **p = malloc(sizeof(void *) * 4);
			if (!p) return;
			v->start = v->finish = p; v->eos = p + 4;
		}
		*v->finish++ = g_dummy;
	}
}

void *wand_GetControllerByID(void *self, ulong id) __asm__(S_BYID);
void *wand_GetControllerByID(void *self, ulong id)
{
	wand_init();
	{ static u32 n; trace("GetControllerByID", &n); }
	if (!g_on) { void *(*real)(void *, ulong) = REAL(S_BYID); return real ? real(self, id) : NULL; }
	(void)id;
	return g_dummy;
}
