/* Tadpole — a fake org.freedesktop.Avahi, and the reason every Brio title runs.
 *
 * WHY THIS EXISTS, WHICH IS NOT THE REASON YOU WOULD GUESS
 * -------------------------------------------------------
 * Nothing here is about mDNS working. It is about a constructor not throwing.
 *
 * Every Brio title is launched through BrioWrapper, which links libWirelessMPI.
 * That MPI loads /LF/Base/Brio/Module/libWireless.so, whose CWirelessModule
 * constructor opens an Avahi ServiceBrowser to look for other LeapPads. rcS
 * says what for, on line 246:
 *
 *     #Start avahi (needed for P2P gaming)
 *
 * The device starts avahi-daemon from /etc/init.d/avahi-daemon. We did not, so
 * org.freedesktop.Avahi was unowned, so the bus tried to ACTIVATE it — and the
 * firmware's own service file exists precisely to make that fail:
 *
 *     # This service should not be bus activated if systemd isn't running,
 *     # so that activation won't conflict with the init script startup.
 *     Exec=/bin/false
 *
 * /bin/false exits 1, the daemon replies
 * org.freedesktop.DBus.Error.Spawn.ChildExited, dbus-c++ turns any non-reply
 * into a thrown DBus::Error, and the constructor has no handler. The unwinder
 * then runs a cleanup that stores through a pointer that was never assigned, so
 * the process dies on `str r3, [r4]` with r4 = 0.
 *
 * THAT NULL WRITE IS A SYMPTOM OF AN UNCAUGHT EXCEPTION, NOT OF MISSING
 * WIRELESS HARDWARE — and the distinction cost about a week. The faulting
 * instruction sits in wireless code, so every round looked for something
 * wireless that was absent, and each candidate fix (the fake ConnMan,
 * wpa_supplicant, disabling P2P in Parent Settings, /LF/System/Wireless) was
 * reasonable, applied cleanly, and changed nothing at all.
 *
 * What ended it was reading the thrown object instead of theorising about it:
 * at __cxa_throw, r0 points at the DBus::Error, and two pointer hops in are the
 * strings "org.freedesktop.DBus.Error.Spawn.ChildExited" and "Process
 * /bin/false exited with status 1". One run, and the service names itself.
 * tools/gdb-dbus-error.py is that reader.
 *
 * dbus-monitor could not have found this, which is worth knowing for next time:
 * it does not report the bus daemon's own replies to calls addressed to the
 * daemon, and an activation failure is exactly such a reply. Hours went into
 * "there is no error on the bus", which was evidence of nothing whatsoever.
 *
 * WHY NOT RUN THE REAL avahi-daemon — IT IS IN THE ROOTFS AND IT DOES START
 * ------------------------------------------------------------------------
 * It was tried first, and it is a trap, for the same reason connmand is: under
 * qemu-user it is NOT SANDBOXED, and the paths it cares about are the
 * DEVELOPER'S.
 *
 * qemu's -L only redirects a path that ALREADY EXISTS in the sysroot. The
 * daemon's first act is to check /var/run/avahi-daemon/pid, which we had not
 * created, so it read the HOST's — and found the developer's own avahi-daemon:
 *
 *     [mdns] Daemon already running on PID 4806
 *
 * That one is fixable by pre-creating the file. The next one is not. The shim
 * does not intercept bind(), so the daemon's unix socket resolves to the host's
 * /var/run/avahi-daemon/socket, and avahi unlink()s that path before binding
 * it. On a machine where the emulator had the rights, starting a LeapPad
 * emulator would quietly break the desktop's own mDNS. A rootfs binary that
 * reaches outside the rootfs is not worth the fidelity.
 *
 * So this owns the name instead. It opens exactly one socket — the bus address
 * handed to it on the command line — and answers questions.
 *
 * WHAT IT ANSWERS, AND WHY "NOTHING FOUND" IS THE HONEST ANSWER
 * ------------------------------------------------------------
 * libWireless's whole Avahi vocabulary is four methods and five signals; the
 * generated dbus-c++ proxy carries the lot as strings:
 *
 *     Server        ServiceBrowserNew, ResolveService
 *     ServiceBrowser Free, Start
 *     signals       ItemNew, ItemRemove, AllForNow, CacheExhausted, Failure
 *
 * A browser here finds no peers, and says so in the proper order —
 * CacheExhausted, then AllForNow. That is not a stub cutting a corner: one
 * emulated tablet on the network genuinely has no other LeapPads to play with,
 * and AllForNow is exactly how Avahi reports the end of a search that found
 * nothing. Pet Chat and the other P2P features will show an empty list, which
 * is the truth.
 *
 * NO HEADERS, DECLARATIONS BY HAND — same rule and same reason as
 * tadpole_connman.c: this is built -nostdlib against the GUEST's libdbus, and
 * the host's dbus-arch-deps.h is generated for the host's word size.
 */

/* ---- the bits of libdbus we use, declared rather than included ---------- */

typedef unsigned int   dbus_bool_t;
typedef unsigned int   dbus_uint32_t;
typedef int            dbus_int32_t;
typedef unsigned short dbus_uint16_t;

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage    DBusMessage;

typedef struct {
	const char *name;
	const char *message;
	unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
	void *padding1;
} DBusError;

/* Opaque by contract; the size and shape are the frozen public ABI. */
typedef struct {
	void *dummy1;
	void *dummy2;
	dbus_uint32_t dummy3;
	int dummy4, dummy5, dummy6, dummy7, dummy8, dummy9, dummy10, dummy11;
	int pad1, pad2;
	void *pad3;
} DBusMessageIter;

typedef int DBusHandlerResult;
#define DBUS_HANDLER_RESULT_HANDLED         0
#define DBUS_HANDLER_RESULT_NOT_YET_HANDLED 1

typedef struct {
	void (*unregister_function)(DBusConnection *, void *);
	DBusHandlerResult (*message_function)(DBusConnection *, DBusMessage *, void *);
	void (*pad1)(void *);
	void (*pad2)(void *);
	void (*pad3)(void *);
	void (*pad4)(void *);
} DBusObjectPathVTable;

#define DBUS_TYPE_INVALID      0
#define DBUS_TYPE_BYTE         'y'
#define DBUS_TYPE_INT32        'i'
#define DBUS_TYPE_UINT16       'q'
#define DBUS_TYPE_UINT32       'u'
#define DBUS_TYPE_STRING       's'
#define DBUS_TYPE_OBJECT_PATH  'o'
#define DBUS_TYPE_ARRAY        'a'

#define DBUS_MESSAGE_TYPE_METHOD_CALL 1

#define DBUS_NAME_FLAG_DO_NOT_QUEUE          4
#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1

extern void dbus_error_init(DBusError *);
extern dbus_bool_t dbus_error_is_set(const DBusError *);

extern DBusConnection *dbus_connection_open_private(const char *address, DBusError *);
extern dbus_bool_t dbus_bus_register(DBusConnection *, DBusError *);
extern int  dbus_bus_request_name(DBusConnection *, const char *name,
                                  unsigned int flags, DBusError *);
extern void dbus_connection_set_exit_on_disconnect(DBusConnection *, dbus_bool_t);
extern dbus_bool_t dbus_connection_register_fallback(DBusConnection *, const char *path,
                                                     const DBusObjectPathVTable *,
                                                     void *user_data);
extern dbus_bool_t dbus_connection_read_write_dispatch(DBusConnection *, int timeout_ms);
extern dbus_bool_t dbus_connection_send(DBusConnection *, DBusMessage *, dbus_uint32_t *serial);
extern void dbus_connection_flush(DBusConnection *);

extern DBusMessage *dbus_message_new_method_return(DBusMessage *);
extern DBusMessage *dbus_message_new_signal(const char *path, const char *iface, const char *name);
extern DBusMessage *dbus_message_new_error(DBusMessage *, const char *name, const char *msg);
extern void dbus_message_unref(DBusMessage *);
extern int  dbus_message_get_type(DBusMessage *);
extern const char *dbus_message_get_path(DBusMessage *);
extern const char *dbus_message_get_interface(DBusMessage *);
extern const char *dbus_message_get_member(DBusMessage *);

extern void dbus_message_iter_init_append(DBusMessage *, DBusMessageIter *);
extern dbus_bool_t dbus_message_iter_open_container(DBusMessageIter *, int type,
                                                    const char *sig, DBusMessageIter *sub);
extern dbus_bool_t dbus_message_iter_close_container(DBusMessageIter *, DBusMessageIter *sub);
extern dbus_bool_t dbus_message_iter_append_basic(DBusMessageIter *, int type, const void *value);

/* ---- the bits of uClibc we use ----------------------------------------- */

extern int   printf(const char *fmt, ...);
extern int   fflush(void *stream);
extern void *stdout;
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, unsigned long n);
extern void  exit(int code) __attribute__((noreturn));

#define NULL ((void *)0)

/* ---- who we are on the bus --------------------------------------------- */

#define AV_NAME        "org.freedesktop.Avahi"
#define SERVER_PATH    "/"
#define SERVER_IFACE   "org.freedesktop.Avahi.Server"
#define BROWSER_IFACE  "org.freedesktop.Avahi.ServiceBrowser"
#define INTROSPECT     "org.freedesktop.DBus.Introspectable"

/* Avahi hands out /Client<n>/ServiceBrowser<m>. The real daemon counts clients
 * separately; one counter is enough here because the paths only have to be
 * unique and to look like what the proxy expects to receive. */
#define BROWSER_PREFIX "/Client0/ServiceBrowser"

/* ---- state ------------------------------------------------------------- */

static DBusConnection *bus;
static int chatty = 1;

/* Browser paths, handed out in order. The cap is not a resource limit — it is a
 * loop guard. A title that somehow asked for thousands would be misbehaving,
 * and answering forever would hide that. */
#define MAX_BROWSERS 64
static int browser_count;

/* Browsers created but not yet told "that is all there is". Kept as a small
 * queue rather than answered inline because the reply to ServiceBrowserNew has
 * to reach the client BEFORE the signals about the object it just made;
 * otherwise the proxy is still being constructed when they arrive and drops
 * them on the floor. Drained by the main loop on the next turn. */
static int pending[MAX_BROWSERS];
static int pending_n;

static char browser_path[MAX_BROWSERS][48];

/* No snprintf: -nostdlib, and uClibc's would drag in more than this needs. */
static void make_browser_path(char *out, int n)
{
	const char *p = BROWSER_PREFIX;
	int i = 0;
	while (*p)
		out[i++] = *p++;
	if (n >= 10)
		out[i++] = (char)('0' + (n / 10) % 10);
	out[i++] = (char)('0' + n % 10);
	out[i] = 0;
}

static void say(const char *fmt, const char *a)
{
	if (!chatty)
		return;
	printf(fmt, a);
	fflush(stdout);
}

/* ---- replies ----------------------------------------------------------- */

static DBusHandlerResult ship(DBusConnection *c, DBusMessage *r)
{
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_connection_send(c, r, NULL);
	dbus_connection_flush(c);
	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult reply_empty(DBusConnection *c, DBusMessage *m)
{
	return ship(c, dbus_message_new_method_return(m));
}

static DBusHandlerResult reply_err(DBusConnection *c, DBusMessage *m,
                                   const char *name, const char *msg)
{
	return ship(c, dbus_message_new_error(m, name, msg));
}

/* ServiceBrowserNew(i, i, s, s, u) -> o
 *
 * The arguments are not read. Every one of them narrows a search that is going
 * to come back empty regardless: interface, protocol, service type, domain and
 * flags all describe WHERE to look for peers, and there are none anywhere. */
static DBusHandlerResult server_browser_new(DBusConnection *c, DBusMessage *m)
{
	DBusMessage *r;
	DBusMessageIter it;
	const char *path;
	int n = browser_count;

	if (n >= MAX_BROWSERS)
		return reply_err(c, m, "org.freedesktop.Avahi.Error.TooManyObjects",
		                 "browser limit reached");

	make_browser_path(browser_path[n], n);
	path = browser_path[n];
	browser_count++;

	r = dbus_message_new_method_return(m);
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);

	pending[pending_n++] = n;
	say("[avahi-fake] ServiceBrowserNew -> %s\n", path);
	return ship(c, r);
}

/* CacheExhausted then AllForNow, in that order, which is the order the real
 * daemon uses and the order the proxy's state machine expects: the first says
 * "nothing left in the cache to replay", the second "the search is done". A
 * browser that never gets AllForNow leaves the caller waiting for peers
 * forever, which is a hang rather than an empty list. */
static void finish_browser(int n)
{
	const char *path = browser_path[n];
	DBusMessage *s;

	s = dbus_message_new_signal(path, BROWSER_IFACE, "CacheExhausted");
	if (s) {
		dbus_connection_send(bus, s, NULL);
		dbus_message_unref(s);
	}
	s = dbus_message_new_signal(path, BROWSER_IFACE, "AllForNow");
	if (s) {
		dbus_connection_send(bus, s, NULL);
		dbus_message_unref(s);
	}
	dbus_connection_flush(bus);
	say("[avahi-fake] %s: no peers (CacheExhausted, AllForNow)\n", path);
}

/* ResolveService(...) -> a great many fields.
 *
 * Unreachable in practice: it resolves a service learned from ItemNew, and we
 * never emit one. Answer with the error the real daemon uses for a name it
 * cannot find, rather than a malformed success that would desynchronise the
 * proxy's iterator. */
static DBusHandlerResult server_resolve(DBusConnection *c, DBusMessage *m)
{
	return reply_err(c, m, "org.freedesktop.Avahi.Error.NotFound",
	                 "no such service");
}

static const char *server_xml =
"<node>"
 "<interface name=\"org.freedesktop.Avahi.Server\">"
  "<method name=\"ServiceBrowserNew\">"
   "<arg name=\"interface\" type=\"i\" direction=\"in\"/>"
   "<arg name=\"protocol\" type=\"i\" direction=\"in\"/>"
   "<arg name=\"type\" type=\"s\" direction=\"in\"/>"
   "<arg name=\"domain\" type=\"s\" direction=\"in\"/>"
   "<arg name=\"flags\" type=\"u\" direction=\"in\"/>"
   "<arg name=\"path\" type=\"o\" direction=\"out\"/>"
  "</method>"
 "</interface>"
"</node>";

static const char *browser_xml =
"<node>"
 "<interface name=\"org.freedesktop.Avahi.ServiceBrowser\">"
  "<method name=\"Free\"/>"
  "<method name=\"Start\"/>"
  "<signal name=\"ItemNew\"/>"
  "<signal name=\"ItemRemove\"/>"
  "<signal name=\"AllForNow\"/>"
  "<signal name=\"CacheExhausted\"/>"
  "<signal name=\"Failure\"/>"
 "</interface>"
"</node>";

static DBusHandlerResult reply_xml(DBusConnection *c, DBusMessage *m, const char *xml)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter it;
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &xml);
	return ship(c, r);
}

/* ---- dispatch ---------------------------------------------------------- */

static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *m, void *ud)
{
	const char *path, *iface, *memb;
	(void)ud;

	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	path  = dbus_message_get_path(m);
	iface = dbus_message_get_interface(m);
	memb  = dbus_message_get_member(m);
	if (!path || !memb)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (iface && strcmp(iface, INTROSPECT) == 0 && strcmp(memb, "Introspect") == 0)
		return reply_xml(c, m, strcmp(path, SERVER_PATH) == 0
		                 ? server_xml : browser_xml);

	if (strcmp(path, SERVER_PATH) == 0) {
		if (strcmp(memb, "ServiceBrowserNew") == 0)
			return server_browser_new(c, m);
		if (strcmp(memb, "ResolveService") == 0)
			return server_resolve(c, m);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* Anything under /Client0/ is a browser we handed out. Free and Start are
	 * both bare acknowledgements: there is no search to stop or start. */
	if (strncmp(path, "/Client", 7) == 0) {
		if (strcmp(memb, "Free") == 0 || strcmp(memb, "Start") == 0)
			return reply_empty(c, m);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable vtable = { NULL, on_message, NULL, NULL, NULL, NULL };

static void usage(void)
{
	printf("usage: tadpole-avahi --address <dbus-address> [--quiet]\n");
	fflush(stdout);
}

static int av_main(int argc, char **argv)
{
	DBusError err;
	const char *address = NULL;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--address") == 0 && i + 1 < argc)
			address = argv[++i];
		else if (strcmp(argv[i], "--quiet") == 0)
			chatty = 0;
		else {
			usage();
			return 2;
		}
	}
	if (!address) {
		usage();
		return 2;
	}

	dbus_error_init(&err);
	/* open_private + bus_register rather than dbus_bus_get, for the reason
	 * spelled out in tadpole_connman.c: this binary is -nostdlib with its own
	 * _start, uClibc's __environ is never populated, so getenv() inside libdbus
	 * returns NULL and the "fallback" to the compiled-in socket path would be
	 * the only case rather than an edge case — and that path is the HOST's. */
	bus = dbus_connection_open_private(address, &err);
	if (!bus) {
		printf("[avahi-fake] cannot reach the system bus: %s\n",
		       dbus_error_is_set(&err) ? err.message : "unknown error");
		fflush(stdout);
		return 1;
	}
	if (!dbus_bus_register(bus, &err)) {
		printf("[avahi-fake] Hello refused: %s\n",
		       dbus_error_is_set(&err) ? err.message : "unknown error");
		fflush(stdout);
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(bus, 1);

	rc = dbus_bus_request_name(bus, AV_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		printf("[avahi-fake] org.freedesktop.Avahi is already owned (rc=%d%s%s) "
		       "— not starting. Is avahi-daemon running in the guest?\n", rc,
		       dbus_error_is_set(&err) ? ": " : "",
		       dbus_error_is_set(&err) ? err.message : "");
		fflush(stdout);
		return 1;
	}

	/* One fallback at "/" catches the server and every browser path below it. */
	if (!dbus_connection_register_fallback(bus, "/", &vtable, NULL)) {
		printf("[avahi-fake] could not register the object tree\n");
		fflush(stdout);
		return 1;
	}

	printf("[avahi-fake] owning org.freedesktop.Avahi — browsers will report "
	       "no peers, which is what one emulated tablet can honestly say\n");
	fflush(stdout);

	while (dbus_connection_read_write_dispatch(bus, 250)) {
		/* Drain on the turn AFTER the reply went out, so the client has its
		 * browser object before the signals about it arrive. */
		while (pending_n > 0)
			finish_browser(pending[--pending_n]);
	}
	return 0;
}

/* Freestanding, exactly as tadpole_connman.c is: the guest lib set has no ARM
 * crt1.o, so there is no C runtime to hand us argc/argv. At _start the ABI
 * leaves them on the stack — argc, then argv[], then a NULL, then the
 * environment — so take sp before the compiler can move it. `naked` guarantees
 * no prologue runs first. */
void tad_avahi_start(unsigned long *sp) __attribute__((noreturn, used));

__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile("mov r0, sp\n\t"
	                 "b   tad_avahi_start\n");
}

void tad_avahi_start(unsigned long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)&sp[1];
	exit(av_main(argc, argv));
}
