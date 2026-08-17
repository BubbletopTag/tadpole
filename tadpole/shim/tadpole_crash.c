/* tadpole_crash.c — turn a guest crash into a readable, attributable report.
 *
 * WHY THIS EXISTS
 * ---------------
 * Roughly 79 of the 112 installed titles are native Brio apps sharing one
 * engine, so a single fault reproduces across dozens of games and the useful
 * question is never "did it crash" but "did it crash in the SAME PLACE".
 * Answering that needs a library name and an offset, and neither was available:
 *
 *   * qemu writes a core, but every r-x segment in it has p_filesz == 0 — it
 *     does not dump executable file-backed pages. The code the PC points into
 *     is simply not in the file, so post-mortem symbolisation cannot work.
 *   * qemu writes no NT_FILE note either, so there is not even a mapping table
 *     to look the address up in.
 *   * gdb resolves the guest's recorded paths against the HOST filesystem, so
 *     guest ARM libraries appear as x86-64 objects.
 *
 * Inside the process, while it is still alive, all of that is trivially
 * available from /proc/self/maps. So we catch the signal and resolve addresses
 * ourselves, then re-raise so qemu still writes its core.
 *
 * WHICH GAME CRASHED comes from getcwd(): AppManager chdir()s into the package
 * directory before handing control to App.so, which is also why the stray core
 * files land inside the game's own folder.
 *
 * ASYNC-SIGNAL SAFETY. Everything here is write(), open(), read(), close() and
 * hand-rolled integer formatting. No printf, no malloc, no locks. Reading
 * /proc/self/maps in a handler is not strictly sanctioned, but the process is
 * already dying and the alternative is a report with no symbols in it.
 *
 * HANGS: ASK FOR THE REPORT INSTEAD OF WAITING FOR A SIGNAL.
 *
 * A white-screen softlock raises nothing, so there is no crash to catch — and
 * the host-side tools cannot fill the gap either. qemu-user translates guest
 * code into its own TCG buffer, so a host PC says nothing about where the GUEST
 * is, and yama's ptrace_scope keeps a separate gdb from attaching to a running
 * emulator at all. qemu's own gdbstub works but must be armed with -g before
 * boot, and its default sysroot pulls every guest .so back through the remote
 * protocol one packet at a time — measured: the boot had not reached the sign-in
 * screen after four minutes.
 *
 * The process that is spinning already knows the answer, so ask it:
 *
 *     kill -QUIT <host tid of the spinning thread>
 *
 * SIGQUIT prints exactly the same pc/lr/stack report and RETURNS, so the guest
 * carries on and can be sampled repeatedly — two dumps a few seconds apart is
 * the difference between "stuck at one instruction" and "looping over a range".
 * Signalling the THREAD (kill -QUIT on the tid, not the pid) matters: qemu maps
 * guest threads to host threads one-to-one, and a process-directed signal lands
 * on whichever thread has it unblocked, which is rarely the one spinning.
 */

typedef unsigned int   u32;
typedef unsigned long  ulong;
/* The compiler's own size_t, matching tadpole_shim.c. Spelling it
 * `unsigned int` instead makes clang warn that every libc prototype here is an
 * incompatible redeclaration. */
typedef __SIZE_TYPE__  size_t;

extern long  write(int fd, const void *buf, size_t n);
extern int   open(const char *path, int flags, ...);
extern long  read(int fd, void *buf, size_t n);
extern int   close(int fd);
extern char *getcwd(char *buf, size_t size);
extern int   getpid(void);
extern int   kill(int pid, int sig);
extern size_t strlen(const char *s);
extern int   snprintf(char *s, size_t n, const char *fmt, ...);
extern char *getenv(const char *name);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000

/* ARM sigcontext / ucontext. Declared by hand: the shim builds -nostdlib and
 * clang's default include path is the HOST's glibc, whose layout is not
 * guaranteed to match the guest's uClibc. These offsets are the kernel ABI and
 * are stable. */
struct tad_sigcontext {
	ulong trap_no, error_code, oldmask;
	ulong arm_r0, arm_r1, arm_r2, arm_r3, arm_r4, arm_r5;
	ulong arm_r6, arm_r7, arm_r8, arm_r9, arm_r10;
	ulong arm_fp, arm_ip, arm_sp, arm_lr, arm_pc, arm_cpsr;
	ulong fault_address;
};

struct tad_ucontext {
	ulong uc_flags;
	void *uc_link;
	void *ss_sp;
	int   ss_flags;
	size_t ss_size;
	struct tad_sigcontext uc_mcontext;
};

/* THE KERNEL'S sigaction, invoked through rt_sigaction rather than libc's
 * wrapper.
 *
 * libc's struct sigaction puts a sigset_t between the handler and the flags,
 * and its size is a build-time choice of the C library: glibc uses 1024 bits,
 * the kernel uses 64. Guess wrong and sa_flags lands at the wrong offset, so
 * SA_SIGINFO never reaches the kernel — the handler is then called as a plain
 * void(int), the third argument is garbage, and the first thing that touches
 * the ucontext faults. That is precisely what happened here, and since
 * SA_RESETHAND was lost the same way, the second fault went straight to
 * SIG_DFL and the process died with no report at all.
 *
 * The kernel ABI has no such ambiguity: handler, flags, restorer, then a
 * 64-bit mask, with sigsetsize passed explicitly. */
struct k_sigaction {
	void (*handler)(int, void *, void *);
	ulong flags;
	void (*restorer)(void);
	ulong mask[2];
};

extern long syscall(long number, ...);
#define __NR_rt_sigaction 174

#define SA_SIGINFO   0x00000004
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

static char g_logpath[320];

/* CLOCK_MONOTONIC at install, so the report can say how long the guest had
 * been alive. "Crashes instantly" and "crashes in the second race" are
 * completely different bugs and the reports were indistinguishable. */
struct tad_crash_ts { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_crash_ts *tp);
static struct tad_crash_ts g_t0;
static int  g_installed;

/* THE REAL open(), handed to us by the shim.
 *
 * Calling the public open() here is wrong twice over. It is the shim's own
 * hook, which rewrites absolute paths to live inside the sysroot — so the
 * report went to <sysroot>/tmp/tadpole/crash.log and failed to create — and
 * reading /proc/self/maps through it fails for the same reason, which loses
 * every symbol in the report. */
static int (*g_open)(const char *, int, ...);

/* ---- output ------------------------------------------------------------ */
/* Every line goes to stderr AND to a log file, because stderr is where a
 * developer is looking and the file is what survives a session of play. */
static int g_logfd = -1;

static void emit(const char *s, size_t n)
{
	write(2, s, n);
	if (g_logfd >= 0)
		write(g_logfd, s, n);
}

static void say(const char *s) { emit(s, strlen(s)); }

static void say_hex(ulong v)
{
	static const char d[] = "0123456789abcdef";
	char b[11];
	int i;
	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 8; i++)
		b[2 + i] = d[(v >> ((7 - i) * 4)) & 0xf];
	emit(b, 10);
}

/* NO DIVISION. ARM has no integer divide instruction here and the shim links
 * -nostdlib, so `v / 10` becomes a call to __aeabi_uidiv, which nothing
 * provides — the library then fails to load with "can't resolve symbol
 * '__aeabi_uidiv'" and takes the whole guest with it. Repeated subtraction of
 * powers of ten avoids the helper entirely. */
static void say_dec(ulong v)
{
	static const ulong pow10[10] = {
		1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
		10000UL, 1000UL, 100UL, 10UL, 1UL
	};
	char b[10];
	int n = 0, i, started = 0;

	for (i = 0; i < 10; i++) {
		int d = 0;
		while (v >= pow10[i]) { v -= pow10[i]; d++; }
		if (d || started || i == 9) {
			b[n++] = (char)('0' + d);
			started = 1;
		}
	}
	emit(b, (size_t)n);
}

/* ---- /proc/self/maps --------------------------------------------------- */
/* Snapshotted once per crash rather than per lookup: the stack scan below asks
 * about hundreds of addresses and re-reading for each would be absurd. */
static char g_maps[192 * 1024];
static size_t g_maps_len;

static void maps_load(void)
{
	int fd = g_open ? g_open("/proc/self/maps", O_RDONLY) : -1;
	long n;
	g_maps_len = 0;
	if (fd < 0)
		return;
	while (g_maps_len < sizeof(g_maps) - 1) {
		n = read(fd, g_maps + g_maps_len, sizeof(g_maps) - 1 - g_maps_len);
		if (n <= 0)
			break;
		g_maps_len += (size_t)n;
	}
	g_maps[g_maps_len] = 0;
	close(fd);
}

static ulong parse_hex(const char *p, const char **end)
{
	ulong v = 0;
	while (*p) {
		int c = *p;
		if (c >= '0' && c <= '9')      v = v * 16 + (ulong)(c - '0');
		else if (c >= 'a' && c <= 'f') v = v * 16 + (ulong)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v = v * 16 + (ulong)(c - 'A' + 10);
		else break;
		p++;
	}
	*end = p;
	return v;
}

/* Find the mapping containing addr. Returns the line start, or 0.
 * exec_only skips non-executable mappings, which is what makes the stack scan
 * discriminating: a return address must point into r-x memory. */
static const char *maps_find(ulong addr, int exec_only,
                             ulong *base, const char **name, size_t *namelen)
{
	const char *p = g_maps;

	while (p < g_maps + g_maps_len && *p) {
		const char *line = p, *q;
		ulong lo, hi;
		const char *perms;

		lo = parse_hex(p, &q);
		if (*q != '-') goto next;
		hi = parse_hex(q + 1, &q);
		if (*q != ' ') goto next;
		perms = q + 1;
		if (perms[0] == 0 || perms[1] == 0 || perms[2] == 0) goto next;
		if (exec_only && perms[2] != 'x') goto next;

		if (addr >= lo && addr < hi) {
			/* The path is the last field. Walk to end of line, then back
			 * to the last space; a leading '/' or '[' marks a real name. */
			const char *e = line, *nm;
			while (e < g_maps + g_maps_len && *e && *e != '\n') e++;
			nm = e;
			while (nm > line && nm[-1] != ' ') nm--;
			*base = lo;
			if (nm < e && (*nm == '/' || *nm == '[')) {
				const char *slash = nm, *s;
				for (s = nm; s < e; s++)
					if (*s == '/') slash = s + 1;
				*name = slash;
				*namelen = (size_t)(e - slash);
			} else {
				*name = "anon";
				*namelen = 4;
			}
			return line;
		}
next:
		while (p < g_maps + g_maps_len && *p && *p != '\n') p++;
		if (p < g_maps + g_maps_len && *p == '\n') p++;
	}
	return 0;
}

/* "libFoo.so+0x1234", the form that makes two crashes comparable. */
static void say_addr(ulong addr, int exec_only)
{
	ulong base = 0;
	const char *name = 0;
	size_t namelen = 0;

	say_hex(addr);
	if (maps_find(addr, exec_only, &base, &name, &namelen)) {
		say("  ");
		emit(name, namelen);
		say("+");
		say_hex(addr - base);
	} else if (!exec_only) {
		say("  <unmapped>");
	}
}

static const char *signame(int sig)
{
	switch (sig) {
	case 3:  return "SIGQUIT";
	case 4:  return "SIGILL";
	case 6:  return "SIGABRT";
	case 7:  return "SIGBUS";
	case 8:  return "SIGFPE";
	case 11: return "SIGSEGV";
	default: return "signal";
	}
}

/* ---- the report -------------------------------------------------------- */
/* ONE BODY FOR BOTH CALLERS. A crash report and a hang dump differ in the
 * heading and in what happens afterwards, not in their content — and the
 * content is the part worth keeping identical, so that a hang can be compared
 * against a crash from the same title without allowing for two formats. */
static void report(int sig, void *ucv, const char *what)
{
	struct tad_ucontext *uc = (struct tad_ucontext *)ucv;
	char cwd[256];
	ulong pc = 0, lr = 0, sp = 0, fault = 0;
	int found = 0;

	/* The log is opened before the first line is written, so the file captures
	 * the whole report rather than everything after the header — the signal
	 * name is the one field worth having if nothing else survives. */
	if (g_logpath[0] && g_logfd < 0 && g_open)
		g_logfd = g_open(g_logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);

	say("\n=== tadpole: guest ");
	say(what);
	say(" ===\n");
	say("  signal   ");
	say(signame(sig));
	say(" (");
	say_dec((ulong)sig);
	say(")\n");

	maps_load();

	if (uc) {
		pc    = uc->uc_mcontext.arm_pc;
		lr    = uc->uc_mcontext.arm_lr;
		sp    = uc->uc_mcontext.arm_sp;
		fault = uc->uc_mcontext.fault_address;
	}

	/* The package directory identifies the title — AppManager chdir()s into it
	 * before calling CreateApp. */
	if (getcwd(cwd, sizeof(cwd))) {
		say("  cwd      ");
		say(cwd);
		say("\n");
	}

	if (sig == 11 || sig == 7) {
		say("  fault    ");
		say_addr(fault, 0);
		say("\n");
	}

	{
		/* Whole seconds only: hand-rolled decimal, no float, no snprintf —
		 * this runs in a signal handler. */
		struct tad_crash_ts now;
		if (g_t0.tv_sec && clock_gettime(1, &now) == 0) {
			say("  alive    ");
			say_dec((ulong)(now.tv_sec - g_t0.tv_sec));
			say("s\n");
		}
	}

	say("  pc       ");  say_addr(pc, 0); say("\n");
	say("  lr       ");  say_addr(lr, 0); say("\n");
	say("  sp       ");  say_hex(sp);     say("\n");

	if (uc) {
		say("  r0 "); say_hex(uc->uc_mcontext.arm_r0);
		say("  r1 "); say_hex(uc->uc_mcontext.arm_r1);
		say("  r2 "); say_hex(uc->uc_mcontext.arm_r2);
		say("  r3 "); say_hex(uc->uc_mcontext.arm_r3);
		say("\n");
	}

	/* A STACK SCAN, not a real unwind. The guest libraries are built without
	 * frame pointers and carry no usable .ARM.exidx for us to walk, so instead
	 * we read up the stack and report every word that points into executable
	 * memory. It over-reports — dead values from earlier calls survive on the
	 * stack — but it reliably contains the real call chain, which is all that
	 * is needed to tell whether two crashes share a path. */
	if (sp) {
		const ulong *s = (const ulong *)sp;
		int i;
		say("  stack (executable words, most recent first):\n");
		for (i = 0; i < 512 && found < 24; i++) {
			ulong v = s[i];
			ulong base = 0;
			const char *nm = 0;
			size_t nl = 0;
			if (v < 0x1000)
				continue;
			if (!maps_find(v, 1, &base, &nm, &nl))
				continue;
			say("    [sp+");
			say_dec(i * 4);
			say("] ");
			say_addr(v, 1);
			say("\n");
			found++;
		}
		if (!found)
			say("    (none found)\n");
	}

	say("=== end ===\n\n");

	if (g_logfd >= 0) {
		close(g_logfd);
		g_logfd = -1;
	}
}

/* ---- the guest's own handlers ------------------------------------------
 *
 * WHY THE REPORT NEVER APPEARED FOR A REAL CRASH.
 *
 * `libLightningBase.so` imports `signal`, and AppManager uses it — the boot log
 * says so in as many words, "AppManager signal handler installed". signal()
 * REPLACES a handler outright, so from that line onward the shim's SIGSEGV
 * handler is simply no longer registered, and every crash after it is somebody
 * else's to report. That is the whole of the long-standing "a segfault where
 * tadpole_crash.c did NOT produce a report" mystery: the handler was not
 * failing, it was not installed any more. The `qemu: uncaught target signal 11`
 * that accompanies those crashes is the matching evidence — qemu prints it when
 * the GUEST has no handler, which is exactly what AppManager's own handler
 * leaves behind once it restores SIG_DFL to get its core.
 *
 * So keep ours registered and remember theirs. We report first — the report is
 * the reason this file exists — and then hand the signal on unchanged, because
 * whatever AppManager does with a crash (its relaunch logic lives on the other
 * side of it) is not ours to cancel.
 */
#define MAXSIG 32
static void (*g_guest[MAXSIG])(int);

static int ours(int sig)
{
	return sig == 3 || sig == 4 || sig == 6 || sig == 7 || sig == 8 || sig == 11;
}

/* -> the handler the guest had previously installed, signal()'s return value. */
void (*tad_crash_take_signal(int sig, void (*h)(int)))(int)
{
	void (*prev)(int);
	if (sig <= 0 || sig >= MAXSIG || !ours(sig))
		return (void (*)(int))-1;      /* not ours: caller falls through */
	prev = g_guest[sig];
	g_guest[sig] = h;
	return prev;
}

/* ---- the handlers ------------------------------------------------------ */
static void on_crash(int sig, void *info, void *ucv)
{
	void (*guest)(int) = (sig > 0 && sig < MAXSIG) ? g_guest[sig] : 0;
	(void)info;
	report(sig, ucv, "crashed");
	/* SIG_DFL(0) and SIG_IGN(1) are not addresses to call. */
	if (guest && guest != (void (*)(int))1) {
		guest(sig);
		/* It returned, so it did not want the process gone. Neither do we. */
		return;
	}
	/* Re-raise so the default action still runs and qemu still writes its core.
	 * SA_RESETHAND already restored SIG_DFL, and SA_NODEFER means this is
	 * delivered rather than held pending. */
	kill(getpid(), sig);
}

/* ON DEMAND, AND SURVIVABLE. Installed without SA_RESETHAND and returning
 * normally, because the whole point is to sample a process that is still
 * running: the guest resumes its spin and can be asked again. */
static void on_dump(int sig, void *info, void *ucv)
{
	(void)info;
	report(sig, ucv, "stack dump");
}

/* ---- installation ------------------------------------------------------ */
void tad_crash_install(const char *dir, int (*real_open)(const char *, int, ...))
{
	static const int sigs[] = { 4, 6, 7, 8, 11 };   /* ILL ABRT BUS FPE SEGV */
	struct k_sigaction sa;
	unsigned i;

	if (g_installed)
		return;
	g_installed = 1;

	g_open = real_open;
	clock_gettime(1, &g_t0);

	/* WHERE THE REPORT GOES, AND WHY NOT $TADPOLE_DIR.
	 *
	 * It used to be $TADPOLE_DIR/crash.log — which is under /tmp, is wiped by
	 * every probe script before it starts, and is shared by every run. So a
	 * crash was observable only if you happened to look before the next boot,
	 * and two crashes could never be compared because the first was gone.
	 *
	 * tadpole.sh now makes a dated directory per run under
	 * ~/.local/state/tadpole/crashes/ and passes it here. Falling back to
	 * $TADPOLE_DIR keeps a guest launched by hand working exactly as before.
	 */
	{
		const char *cd = getenv("TADPOLE_CRASHDIR");
		if (cd && cd[0])
			snprintf(g_logpath, sizeof(g_logpath), "%s/crash.log", cd);
		else if (dir && dir[0])
			snprintf(g_logpath, sizeof(g_logpath), "%s/crash.log", dir);
	}

	for (i = 0; i < sizeof(sa); i++)
		((char *)&sa)[i] = 0;
	sa.handler = on_crash;
	/* RESETHAND so a fault inside the handler cannot loop forever; NODEFER so
	 * the deliberate re-raise at the end is delivered immediately. */
	sa.flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;

	/* The trailing 8 is sigsetsize, which rt_sigaction validates against its
	 * own sigset_t; anything else fails with EINVAL. */
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		syscall(__NR_rt_sigaction, (long)sigs[i], (long)&sa, 0L, 8L);

	/* SIGQUIT: the same report, on demand, for a guest that is not dying.
	 * NEITHER RESETHAND NOR NODEFER here. RESETHAND would make the first dump
	 * the only one, which defeats sampling a spin twice to see whether the PC
	 * moves; NODEFER only exists above to let the deliberate re-raise through,
	 * and this handler does not re-raise. SIGQUIT's default action would dump
	 * core and kill the guest, so an unhandled one is not a harmless thing to
	 * send — installing this is what makes the tool safe to use. */
	sa.handler = on_dump;
	sa.flags = SA_SIGINFO;
	syscall(__NR_rt_sigaction, 3L, (long)&sa, 0L, 8L);
}
