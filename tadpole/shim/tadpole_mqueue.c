/* Tadpole — LeapPad2 (NXP3200 / VALENCIA) emulator
 *
 * tadpole_mqueue.c — POSIX message queues, in userspace.
 *
 * Brio's event dispatcher is built on mq_open/mq_send/mq_receive, and there
 * are exactly two reasons the host kernel cannot serve them. Both were
 * measured rather than assumed.
 *
 * 1. THE OPTION IS OFF ON ANDROID. A Phh-Treble GSI on an armeabi-v7a tablet,
 *    kernel 3.18.79:
 *
 *        # CONFIG_POSIX_MQUEUE is not set
 *        # CONFIG_SYSVIPC is not set
 *
 *    so the syscall does not exist. Under strace the guest's very first queue
 *    comes back
 *
 *        mq_open("eventDispatchQueue", O_WRONLY|O_CREAT|O_TRUNC, 0700,
 *                {mq_maxmsg=8, mq_msgsize=16}) = -1 ENOSYS
 *
 *    and AppManager asserts and kills itself in System/Kernel/Kernel.cpp:473
 *    before it has drawn anything. That is the whole distance between "the
 *    firmware runs natively on this tablet" and "the system menu boots".
 *
 * 2. AND TURNING THE OPTION ON WOULD NOT HAVE HELPED, which is the part worth
 *    writing down because it makes a kernel hunt a waste of a weekend. Look at
 *    that name: `eventDispatchQueue`, with no leading slash. Linux requires
 *    one — do_mq_open rejects anything else — and it is not a formality.
 *    Measured on the desktop, where the option IS on:
 *
 *        mq_open("eventDispatchQueue",  ...) -> EINVAL
 *        mq_open("/eventDispatchQueue", ...) -> ok
 *
 *    So no stock Linux was ever going to serve Brio's queues. Whatever the
 *    LeapPad's own 2.6 kernel did with that name, mainline does not do it.
 *
 * WHY THIS IS A SHIM AND NOT A PATCH TO THE ENGINE. glasspole already
 * implements all of this for the desktop path — see SYS_mq_open and friends in
 * glasspole/src/syscall.cpp — and its comment there records the fact that makes
 * the whole thing cheap: "no host primitive is involved, because every one of
 * these is between threads of a single guest process". Same model, one level
 * down. There the emulator owned the descriptor table and could hand out an
 * index; here the guest's own uClibc owns it, so the descriptors have to be
 * ours and carry a tag that cannot collide with a real fd.
 *
 * WHY IT LANDS IN THE SHIM'S SYMBOL TABLE AT ALL. mq_* live in the guest's
 * librt.so.0, which is not one of AppManager's own dependencies — it arrives
 * only when libKernel.so is dlopen'd, long after startup. libdl.so.0, which is
 * what this library is pretending to be, is AppManager's NEEDED entry 22. The
 * global scope is searched in load order, so these definitions are found first,
 * exactly as the shim's open() and ioctl() already beat libc.so.0 at 33.
 *
 * libKernel.so is the only importer in the whole firmware, and it wants eight
 * functions: open, close, unlink, send, receive, timedsend, timedreceive,
 * getattr. mq_notify is not imported and is not provided.
 */

#define _GNU_SOURCE
#include <stdarg.h>

typedef __SIZE_TYPE__ size_t;
typedef long           ssize_t;
typedef unsigned int   u32;

/* ---- libc, declared by hand -----------------------------------------------
 * Same rule as tadpole_shim.c: this is cross-compiled with the host's clang
 * and no ARM sysroot, so nothing may be #included that describes the target.
 * Everything below is exported by the guest's own libc.so.0, which is what the
 * Makefile's check-undefined rule allows and what it checks. */
extern void  *malloc(size_t n);
extern void   free(void *p);
extern void  *memcpy(void *d, const void *s, size_t n);
extern void  *memmove(void *d, const void *s, size_t n);
extern void  *memset(void *s, int c, size_t n);
extern size_t strlen(const char *s);
extern int    strcmp(const char *a, const char *b);
extern char  *getenv(const char *name);
extern long   write(int fd, const void *buf, size_t n);
extern int   *__errno_location(void);
extern long   syscall(long number, ...);

/* struct timespec on this 32-bit target is two longs, and the shim spells it
 * out for the same reason it spells out struct input_event. */
struct tad_timespec { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk, struct tad_timespec *tp);
#define CLOCK_REALTIME_ 0

/* The caller's struct mq_attr is four longs — flags, maxmsg, msgsize, curmsgs.
 * glibc pads it out with four more; uClibc does not, and it does not matter
 * either way because nothing here reads or writes past the fourth. */
struct tad_mq_attr { long mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs; };

typedef int mqd_t;

#define O_CREAT_    0100
#define O_EXCL_     0200
#define O_NONBLOCK_ 04000

#define E_PERM        1
#define E_NOENT       2
#define E_BADF        9
#define E_AGAIN      11
#define E_NOMEM      12
#define E_EXIST      17
#define E_INVAL      22
#define E_MFILE      24
#define E_NAMETOOLONG 36
#define E_MSGSIZE    90
#define E_TIMEDOUT  110

static int fail(int e) { *__errno_location() = e; return -1; }

/* ---- logging --------------------------------------------------------------
 * dbg() in tadpole_shim.c is static and its log fd is private to that file, so
 * this keeps its own. Same prefix and same switch, so the lines interleave with
 * the rest of the shim's tracing and read as one stream. */
static int g_mq_debug = -1;

static void mqdbg(const char *msg)
{
	if (g_mq_debug < 0) {
		const char *d = getenv("TADPOLE_DEBUG");
		g_mq_debug = (d && d[0] && d[0] != '0');
	}
	if (g_mq_debug)
		write(2, msg, strlen(msg));
}

/* ---- futex ----------------------------------------------------------------
 *
 * NOT pthreads, even though the guest's libc.so.0 exports pthread_mutex_lock
 * and pthread_cond_wait and linking against them would need no build changes.
 * Using them means declaring pthread_mutex_t and pthread_cond_t by hand, and
 * their layout differs between uClibc's linuxthreads and its NPTL — a wrong
 * guess there is silent memory corruption inside somebody else's library. The
 * futex ABI is the kernel's, it is the same on every build of it, and the one
 * number this needs is already written down in glasspole's syscall table:
 * SYS_futex = 240 on ARM.
 *
 * PRIVATE, because these queues never leave the process — the same fact that
 * lets glasspole implement them with a plain std::map and a condition variable.
 */
#define SYS_futex_          240
#define FUTEX_WAIT_PRIVATE_ (0 | 128)
#define FUTEX_WAKE_PRIVATE_ (1 | 128)

static void futex_wait(volatile int *addr, int val, long ms)
{
	struct tad_timespec t;
	t.tv_sec  = ms / 1000;
	t.tv_nsec = (ms % 1000) * 1000000L;
	/* Return value deliberately ignored: EAGAIN (the word already moved),
	 * EINTR and ETIMEDOUT all mean the same thing to the caller, which is
	 * "go round and look at the queue again under the lock". */
	syscall(SYS_futex_, addr, FUTEX_WAIT_PRIVATE_, val, &t, (void *)0, 0);
}

static void futex_wake(volatile int *addr)
{
	syscall(SYS_futex_, addr, FUTEX_WAKE_PRIVATE_, 0x7fffffff,
	        (void *)0, (void *)0, 0);
}

/* One lock for every queue and for the descriptor table. The critical sections
 * are a memmove of at most a few hundred bytes and there are four queues in the
 * entire firmware, so a lock per queue would be more code and more ways to be
 * wrong for contention that does not exist.
 *
 * The three-state mutex: 0 free, 1 held, 2 held and somebody is waiting. The
 * third state is what keeps the uncontended unlock free of a syscall. */
static volatile int g_lock;

static void mq_lock(void)
{
	int c = __sync_val_compare_and_swap(&g_lock, 0, 1);
	if (c == 0)
		return;
	do {
		if (c == 2 || __sync_val_compare_and_swap(&g_lock, 1, 2) != 0)
			futex_wait(&g_lock, 2, 1000);
		c = __sync_val_compare_and_swap(&g_lock, 0, 2);
	} while (c != 0);
}

static void mq_unlock(void)
{
	if (__sync_fetch_and_sub(&g_lock, 1) != 1) {
		g_lock = 0;
		futex_wake(&g_lock);
	}
}

/* ---- the queues -----------------------------------------------------------
 *
 * A message is [u32 prio][u32 len][payload], padded to msgsize, and the slots
 * are kept sorted by priority descending with insertion order preserved inside
 * a priority. That is what POSIX says mq_receive returns and what the LeapPad's
 * own kernel would have done.
 *
 * (glasspole keeps them in a std::multimap and takes begin(), which is the
 * LOWEST priority first. It has never mattered because Brio sends everything at
 * priority 0, where the two orders are the same — but the real hardware is the
 * thing being reproduced here, so this one follows the standard.)
 */
#define MQ_MAX_QUEUES  16
#define MQ_MAX_DESCS   32
#define MQ_NAME_MAX    64
#define MQ_HDR         8            /* [u32 prio][u32 len] */

/* Linux's own defaults, used when mq_open is given no attr. Matching them
 * rather than inventing smaller ones keeps a queue that is opened without an
 * attr behaving the way it did on the device. */
#define MQ_DFL_MAXMSG  10
#define MQ_DFL_MSGSIZE 8192
/* A ceiling only so that a nonsense attr cannot ask for a gigabyte. Well above
 * anything in the firmware, which asks for 8 x 16 bytes. */
#define MQ_CAP_MAXMSG  1024
#define MQ_CAP_MSGSIZE 65536

struct mq_queue {
	int            used;
	char           name[MQ_NAME_MAX];
	int            named;          /* the name is still live (not unlinked) */
	int            refs;           /* open descriptors onto it */
	u32            maxmsg, msgsize;
	u32            nmsg;
	unsigned char *slots;          /* maxmsg * (MQ_HDR + msgsize) */
	/* Bumped on every change to the queue. Waiters read it under the lock,
	 * drop the lock, then futex_wait on this exact value — so a change that
	 * lands in the gap makes the wait return immediately instead of being
	 * lost. */
	volatile int   seq;
};

struct mq_desc {
	int used;
	int q;                         /* index into g_q */
	int oflags;
};

static struct mq_queue g_q[MQ_MAX_QUEUES];
static struct mq_desc  g_d[MQ_MAX_DESCS];

/* The tag that makes a descriptor of ours impossible to confuse with a real
 * file descriptor. It has to be positive, because Brio tests for failure with
 * == -1, and it has to be nowhere near the small integers the guest's libc
 * hands out. */
#define MQ_TAG      0x6d710000      /* 'm','q' */
#define MQ_TAG_MASK 0xffff0000

/* For the shim's close(): a guest that closes a queue with close() instead of
 * mq_close() would otherwise get EBADF out of the real one. */
int tad_mq_is_mqd(int fd)
{
	return (fd & MQ_TAG_MASK) == MQ_TAG;
}

static unsigned char *slot_at(struct mq_queue *q, u32 i)
{
	return q->slots + (size_t)i * (MQ_HDR + q->msgsize);
}

static u32 slot_prio(struct mq_queue *q, u32 i)
{
	u32 p;
	memcpy(&p, slot_at(q, i), 4);
	return p;
}

static struct mq_desc *desc_of(mqd_t d)
{
	int i = d & ~MQ_TAG_MASK;
	if (!tad_mq_is_mqd(d) || i < 0 || i >= MQ_MAX_DESCS || !g_d[i].used)
		return (struct mq_desc *)0;
	return &g_d[i];
}

/* Caller holds the lock. Drops the name and, if nothing has it open any more,
 * the memory with it — which is exactly the lifetime POSIX asks for and the
 * one glasspole gets from a shared_ptr. */
static void queue_release(struct mq_queue *q)
{
	if (q->refs > 0 || q->named)
		return;
	free(q->slots);
	memset(q, 0, sizeof(*q));
}

/* ---- open / close / unlink ----------------------------------------------- */

mqd_t mq_open(const char *name, int oflag, ...)
{
	struct tad_mq_attr *attr = (struct tad_mq_attr *)0;
	struct mq_queue *q = (struct mq_queue *)0;
	int i, qi = -1, di = -1;
	u32 maxmsg = MQ_DFL_MAXMSG, msgsize = MQ_DFL_MSGSIZE;

	if (!name || !name[0])
		return fail(E_INVAL);
	if (strlen(name) >= MQ_NAME_MAX)
		return fail(E_NAMETOOLONG);

	/* NO LEADING SLASH IS REQUIRED, and that is the entire point — see the
	 * note at the top of this file. The name is used as an opaque key, exactly
	 * as glasspole uses it as a std::map key, so "q" and "/q" are two queues
	 * and both sides agree about that. */

	if (oflag & O_CREAT_) {
		va_list ap;
		va_start(ap, oflag);
		(void)va_arg(ap, unsigned int);          /* mode_t, promoted to int */
		attr = va_arg(ap, struct tad_mq_attr *);
		va_end(ap);
		if (attr) {
			if (attr->mq_maxmsg  > 0) maxmsg  = (u32)attr->mq_maxmsg;
			if (attr->mq_msgsize > 0) msgsize = (u32)attr->mq_msgsize;
			if (maxmsg > MQ_CAP_MAXMSG || msgsize > MQ_CAP_MSGSIZE)
				return fail(E_INVAL);
		}
	}

	mq_lock();

	for (i = 0; i < MQ_MAX_QUEUES; i++)
		if (g_q[i].used && g_q[i].named && !strcmp(g_q[i].name, name)) {
			qi = i;
			break;
		}

	if (qi < 0) {
		if (!(oflag & O_CREAT_)) { mq_unlock(); return fail(E_NOENT); }
		for (i = 0; i < MQ_MAX_QUEUES; i++)
			if (!g_q[i].used) { qi = i; break; }
		if (qi < 0) { mq_unlock(); return fail(E_NOMEM); }

		q = &g_q[qi];
		memset(q, 0, sizeof(*q));
		q->slots = (unsigned char *)malloc((size_t)maxmsg * (MQ_HDR + msgsize));
		if (!q->slots) { memset(q, 0, sizeof(*q)); mq_unlock(); return fail(E_NOMEM); }
		for (i = 0; name[i]; i++) q->name[i] = name[i];
		q->name[i]  = 0;
		q->used     = 1;
		q->named    = 1;
		q->maxmsg   = maxmsg;
		q->msgsize  = msgsize;
		mqdbg("[tadpole] mq_open created ");
		mqdbg(name);
		mqdbg("\n");
	} else if ((oflag & O_CREAT_) && (oflag & O_EXCL_)) {
		mq_unlock();
		return fail(E_EXIST);
	} else {
		q = &g_q[qi];
	}

	for (i = 0; i < MQ_MAX_DESCS; i++)
		if (!g_d[i].used) { di = i; break; }
	if (di < 0) { queue_release(q); mq_unlock(); return fail(E_MFILE); }

	g_d[di].used   = 1;
	g_d[di].q      = qi;
	g_d[di].oflags = oflag;
	q->refs++;

	mq_unlock();
	return (mqd_t)(MQ_TAG | di);
}

int mq_close(mqd_t mqdes)
{
	struct mq_desc *d;

	mq_lock();
	if (!(d = desc_of(mqdes))) { mq_unlock(); return fail(E_BADF); }
	g_q[d->q].refs--;
	queue_release(&g_q[d->q]);
	memset(d, 0, sizeof(*d));
	mq_unlock();
	return 0;
}

/* For the shim's close(). Named like the rest of the cross-file interface
 * rather than reaching straight for mq_close, so the one place that calls
 * across files is obvious from both ends. */
int tad_mq_close(int mqdes)
{
	return mq_close((mqd_t)mqdes);
}

int mq_unlink(const char *name)
{
	int i;

	if (!name || !name[0])
		return fail(E_INVAL);

	mq_lock();
	for (i = 0; i < MQ_MAX_QUEUES; i++)
		if (g_q[i].used && g_q[i].named && !strcmp(g_q[i].name, name)) {
			/* The NAME goes now; descriptors already open keep working and the
			 * memory goes with the last of them. That is what POSIX promises
			 * and what the desktop path already does. */
			g_q[i].named = 0;
			queue_release(&g_q[i]);
			mq_unlock();
			return 0;
		}
	mq_unlock();
	return fail(E_NOENT);
}

/* ---- send / receive -------------------------------------------------------
 *
 * One body for the timed and untimed forms, because they differ only in whether
 * there is a deadline. `abs_timeout` is an ABSOLUTE CLOCK_REALTIME time, unlike
 * the relative one futex itself takes, so it is converted at every pass rather
 * than once — the loop can go round several times and a stale interval would
 * quietly extend the wait each time.
 *
 * The wait is capped at 100ms whatever the deadline says. glasspole caps its
 * own for the same reason: a queue nothing ever feeds must not be able to pin a
 * thread past the point where the process is trying to leave.
 */
static long deadline_ms(const struct tad_timespec *abs)
{
	struct tad_timespec now;
	long ms;

	if (clock_gettime(CLOCK_REALTIME_, &now) != 0)
		return 0;
	ms = (abs->tv_sec - now.tv_sec) * 1000L
	   + (abs->tv_nsec - now.tv_nsec) / 1000000L;
	return ms;
}

static int mq_send_common(mqd_t mqdes, const char *msg, size_t len,
                          unsigned prio, const struct tad_timespec *abs)
{
	struct mq_desc *d;
	struct mq_queue *q;
	int nonblock, observed;
	u32 i, rec;

	if (!msg && len)
		return fail(E_INVAL);

	for (;;) {
		mq_lock();
		if (!(d = desc_of(mqdes))) { mq_unlock(); return fail(E_BADF); }
		q = &g_q[d->q];
		nonblock = (d->oflags & O_NONBLOCK_) != 0;

		if (len > q->msgsize) { mq_unlock(); return fail(E_MSGSIZE); }

		if (q->nmsg < q->maxmsg) {
			/* Insert AFTER everything of the same priority or higher, so equal
			 * priorities come out in the order they went in. */
			for (i = 0; i < q->nmsg; i++)
				if (slot_prio(q, i) < prio)
					break;
			rec = MQ_HDR + q->msgsize;
			if (i < q->nmsg)
				memmove(slot_at(q, i + 1), slot_at(q, i),
				        (size_t)(q->nmsg - i) * rec);
			{
				unsigned char *s = slot_at(q, i);
				u32 l = (u32)len;
				memcpy(s, &prio, 4);
				memcpy(s + 4, &l, 4);
				if (len) memcpy(s + MQ_HDR, msg, len);
			}
			q->nmsg++;
			q->seq++;
			futex_wake(&q->seq);
			mq_unlock();
			return 0;
		}

		if (nonblock) { mq_unlock(); return fail(E_AGAIN); }

		observed = q->seq;
		mq_unlock();

		if (abs) {
			long left = deadline_ms(abs);
			if (left <= 0) return fail(E_TIMEDOUT);
			futex_wait(&q->seq, observed, left > 100 ? 100 : left);
		} else {
			futex_wait(&q->seq, observed, 100);
		}
	}
}

static ssize_t mq_receive_common(mqd_t mqdes, char *msg, size_t len,
                                 unsigned *prio, const struct tad_timespec *abs)
{
	struct mq_desc *d;
	struct mq_queue *q;
	int nonblock, observed;

	for (;;) {
		mq_lock();
		if (!(d = desc_of(mqdes))) { mq_unlock(); return fail(E_BADF); }
		q = &g_q[d->q];
		nonblock = (d->oflags & O_NONBLOCK_) != 0;

		/* POSIX: the buffer must be able to hold ANY message the queue can
		 * carry, not merely the one at the head. Checked before the queue is
		 * looked at so that an undersized buffer fails the same way whether or
		 * not anything happens to be waiting. */
		if (len < q->msgsize) { mq_unlock(); return fail(E_MSGSIZE); }

		if (q->nmsg > 0) {
			unsigned char *s = slot_at(q, 0);
			u32 p, l, rec = MQ_HDR + q->msgsize;
			memcpy(&p, s, 4);
			memcpy(&l, s + 4, 4);
			if (l) memcpy(msg, s + MQ_HDR, l);
			if (prio) *prio = p;
			q->nmsg--;
			if (q->nmsg)
				memmove(slot_at(q, 0), slot_at(q, 1), (size_t)q->nmsg * rec);
			q->seq++;
			futex_wake(&q->seq);
			mq_unlock();
			return (ssize_t)l;
		}

		if (nonblock) { mq_unlock(); return fail(E_AGAIN); }

		observed = q->seq;
		mq_unlock();

		if (abs) {
			long left = deadline_ms(abs);
			if (left <= 0) return fail(E_TIMEDOUT);
			futex_wait(&q->seq, observed, left > 100 ? 100 : left);
		} else {
			futex_wait(&q->seq, observed, 100);
		}
	}
}

int mq_send(mqd_t mqdes, const char *msg, size_t len, unsigned prio)
{
	return mq_send_common(mqdes, msg, len, prio, (struct tad_timespec *)0);
}

ssize_t mq_receive(mqd_t mqdes, char *msg, size_t len, unsigned *prio)
{
	return mq_receive_common(mqdes, msg, len, prio, (struct tad_timespec *)0);
}

int mq_timedsend(mqd_t mqdes, const char *msg, size_t len, unsigned prio,
                 const struct tad_timespec *abs_timeout)
{
	return mq_send_common(mqdes, msg, len, prio, abs_timeout);
}

ssize_t mq_timedreceive(mqd_t mqdes, char *msg, size_t len, unsigned *prio,
                        const struct tad_timespec *abs_timeout)
{
	return mq_receive_common(mqdes, msg, len, prio, abs_timeout);
}

/* ---- attributes ---------------------------------------------------------- */

int mq_getattr(mqd_t mqdes, struct tad_mq_attr *attr)
{
	struct mq_desc *d;

	if (!attr)
		return fail(E_INVAL);
	mq_lock();
	if (!(d = desc_of(mqdes))) { mq_unlock(); return fail(E_BADF); }
	attr->mq_flags   = (d->oflags & O_NONBLOCK_);
	attr->mq_maxmsg  = g_q[d->q].maxmsg;
	attr->mq_msgsize = g_q[d->q].msgsize;
	attr->mq_curmsgs = g_q[d->q].nmsg;
	mq_unlock();
	return 0;
}

/* Not imported by anything in the firmware, but it is the other half of
 * mq_getattr and leaving it out would send a caller through to librt's real
 * one, which is the ENOSYS this file exists to avoid. O_NONBLOCK is the only
 * flag POSIX lets it change. */
int mq_setattr(mqd_t mqdes, const struct tad_mq_attr *attr,
               struct tad_mq_attr *old)
{
	struct mq_desc *d;

	mq_lock();
	if (!(d = desc_of(mqdes))) { mq_unlock(); return fail(E_BADF); }
	if (old) {
		old->mq_flags   = (d->oflags & O_NONBLOCK_);
		old->mq_maxmsg  = g_q[d->q].maxmsg;
		old->mq_msgsize = g_q[d->q].msgsize;
		old->mq_curmsgs = g_q[d->q].nmsg;
	}
	if (attr) {
		if (attr->mq_flags & O_NONBLOCK_) d->oflags |=  O_NONBLOCK_;
		else                              d->oflags &= ~O_NONBLOCK_;
	}
	mq_unlock();
	return 0;
}
