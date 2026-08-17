/* Tadpole — a fake net.connman for the Qt shell.
 *
 * WHAT THIS IS FOR, AND WHAT IT IS NOT FOR
 * ----------------------------------------
 * THE GUEST ALREADY HAS INTERNET. qemu-user hands socket calls to the host, so
 * the guest shares the host's network stack: DNS resolves, TCP connects, TLS
 * verifies against the host's own CA bundle (see setup-sysroot.sh). A curl of a
 * real LeapFrog CDN object from inside the guest returns HTTP 200 in about a
 * second. Connectivity has never been the problem.
 *
 * The problem is that the SHELL does not know any of that. It does not probe
 * the network; it ASKS ConnMan over the system bus, through
 * libQtRioConnman.so.1, and believes the answer. With no wireless hardware the
 * honest answer is "there is no wifi technology", so the tablet decides it is
 * offline and behaves accordingly: no wifi icon, App Center unreachable,
 * firmware update unreachable, and — before /flags/skip_oobe — an out-of-box
 * WiFi step that can be neither satisfied nor skipped.
 *
 * So this daemon does not provide connectivity. It DESCRIBES the connectivity
 * that is already there, in the only vocabulary the shell speaks.
 *
 * WHY NOT RUN THE REAL connmand — IT IS IN THE ROOTFS AND IT DOES START
 * --------------------------------------------------------------------
 * It did, and tadpole.sh started it, and that is now switched off by default.
 * /usr/sbin/connmand is guest ARM and runs happily, but under qemu-user IT IS
 * NOT SANDBOXED: qemu forwards its netlink socket to the host kernel, so the
 * "device" it manages is the DEVELOPER'S MACHINE. From a boot of this very
 * branch, with nothing asked of it:
 *
 *     connmand[701041]: Could not clear IPv4 address index 2
 *     connmand[701041]: enp5s0 {newlink} index 2 address 00:E0:23:B3:70:09
 *     connmand[701041]: Adding interface enp5s0 [ ethernet ]
 *     connmand[701041]: enp5s0 {add} address 192.168.1.3/24 label enp5s0
 *     connmand[701041]: virbr1 {add} route 10.0.2.0 gw 0.0.0.0 scope 253
 *
 * index 2 is enp5s0, the host's real NIC, holding the host's real address. The
 * clear failed only because we are not root. Everything else it wants to do —
 * DHCP, resolv.conf, routes — is aimed at the same place. And having done all
 * that it still reports no wifi, because there is no wireless device to find,
 * so the shell gets nothing out of the risk. See tadpole.sh for the switch and
 * TADPOLE_REAL_CONNMAN for the way back.
 *
 * THIS PROCESS TOUCHES NO NETWORK AT ALL. It opens exactly one socket — the
 * D-Bus address handed to it on the command line — and answers questions. It
 * opens no netlink socket, enumerates no interface, and has no code that could.
 *
 * WHY GUEST-SIDE ARM RATHER THAN A HOST HELPER
 * --------------------------------------------
 * A twenty-line Python script on the host could bind the same bus and would
 * have been quicker to write. It would also be Linux-only: the address is an
 * abstract unix socket, which is a Linux concept, and Tadpole runs on Windows
 * through MSYS and on glasspole. A guest binary is portable BY CONSTRUCTION —
 * it runs wherever the emulator runs, because the emulator is what runs it, and
 * it reaches the bus by exactly the same route the shell does.
 *
 * It links the guest's own /usr/lib/libdbus-1.so.3 (1.6.x), which is in the
 * image because dbus-daemon and dbus-send are.
 *
 * NO HEADERS, DECLARATIONS BY HAND — same rule as every other file here, and
 * with an extra reason. The obvious shortcut is to include the HOST's
 * /usr/include/dbus-1.0 headers, but dbus-arch-deps.h is generated per
 * architecture: on an x86-64 host it declares dbus_int64_t as `long`, which is
 * 32 bits on our 32-bit ARM target. The rest of the public ABI (DBusError,
 * DBusMessageIter, DBusObjectPathVTable) is frozen by design and has been
 * byte-identical since 1.0, so writing it out is safe and costs one screen.
 *
 * WHAT THE SHELL ACTUALLY CALLS
 * -----------------------------
 * Read off the real daemon rather than guessed — connmand was introspected
 * over this very bus and its XML is what the interfaces below are shaped from —
 * and cross-checked against the strings in libQtRioConnman.so.1, which is a
 * build of Nemo's libconnman-qt:
 *
 *   Manager (at "/"):    GetProperties -> a{sv}
 *                        GetTechnologies -> a(oa{sv})
 *                        GetServices -> a(oa{sv})
 *                        RegisterAgent/UnregisterAgent(o)
 *                        signals PropertyChanged, TechnologyAdded,
 *                                TechnologyRemoved, ServicesChanged
 *   Technology:          GetProperties, SetProperty, Scan, PropertyChanged
 *   Service:             GetProperties, SetProperty, Connect, Disconnect,
 *                        Remove, PropertyChanged
 *
 * net.connman.Clock is NOT implemented, and that is a checked omission rather
 * than an oversight: the string "net.connman.Clock" appears in exactly one file
 * in the whole rootfs, connmand itself. Nothing in the shell asks for it.
 *
 * WHAT THE SHELL DOES WITH THE ANSWERS
 * ------------------------------------
 * Three separate consumers, and they read three different things, which is why
 * all three have to be right at once:
 *
 *   WifiStatusIcon (libQtSystemStatus) — the status bar. Its m_status is the
 *       MANAGER's State property. "idle"/"offline" -> "wifi state:disabled",
 *       empty -> "wifi state: error", and only "online" gets it as far as
 *       "wifi state: online,strength: N", where N comes from the default
 *       route service's Strength.
 *   NetworkingModel (libQtRioConnman) — the WiFi settings page. It keys
 *       technologies by their TYPE property and looks for "wifi"; that is what
 *       prints "NetworkingModel::updateTechnologies, added wifi".
 *   NetworkManager::updateDefaultRoute — takes the FIRST service in the list
 *       and adopts it as the default route only if its state is "ready" or
 *       "online". Everything that watches signal strength watches that object,
 *       so a service in any other state leaves the icon with nothing to read.
 *
 * An OPEN network, deliberately. Security is reported as ["none"] rather than
 * ["psk"] because a secured network is a network some code path may want a
 * passphrase for, and passphrases come from the net.connman.Agent the shell
 * registers with us — which we accept and never call. With no security there
 * is no path that can ask.
 *
 * The addresses below describe nothing. No packet is ever sent to 192.168.4.1;
 * the guest's traffic goes out of the host's real stack, wherever that is. They
 * exist because the settings screen has fields to print.
 */

/* ---- the bits of libdbus we use, declared rather than included ---------- */

typedef unsigned int   dbus_bool_t;
typedef unsigned int   dbus_uint32_t;
typedef unsigned char  dbus_uint8_t;

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
#define DBUS_TYPE_BOOLEAN      'b'
#define DBUS_TYPE_UINT32       'u'
#define DBUS_TYPE_STRING       's'
#define DBUS_TYPE_OBJECT_PATH  'o'
#define DBUS_TYPE_ARRAY        'a'
#define DBUS_TYPE_VARIANT      'v'
#define DBUS_TYPE_STRUCT       'r'
#define DBUS_TYPE_DICT_ENTRY   'e'

#define DBUS_MESSAGE_TYPE_METHOD_CALL 1

#define DBUS_NAME_FLAG_DO_NOT_QUEUE          4
#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1

extern void dbus_error_init(DBusError *);
extern void dbus_error_free(DBusError *);
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
extern dbus_bool_t dbus_message_iter_init(DBusMessage *, DBusMessageIter *);
extern int  dbus_message_iter_get_arg_type(DBusMessageIter *);
extern void dbus_message_iter_get_basic(DBusMessageIter *, void *value);
extern void dbus_message_iter_recurse(DBusMessageIter *, DBusMessageIter *sub);
extern dbus_bool_t dbus_message_iter_next(DBusMessageIter *);

/* ---- the bits of uClibc we use ----------------------------------------- */

extern int   printf(const char *fmt, ...);
extern int   fflush(void *stream);
extern void *stdout;
extern int   strcmp(const char *a, const char *b);
extern void  exit(int code) __attribute__((noreturn));
extern long  time(long *t);

#define NULL ((void *)0)

/* ---- who we are on the bus --------------------------------------------- */

#define CM_NAME     "net.connman"
#define MGR_PATH    "/"
#define MGR_IFACE   "net.connman.Manager"
#define TECH_IFACE  "net.connman.Technology"
#define SVC_IFACE   "net.connman.Service"
#define INTROSPECT  "org.freedesktop.DBus.Introspectable"

#define TECH_PATH   "/net/connman/technology/wifi"

/* connman names a service wifi_<mac>_<ssid-in-hex>_<mode>_<security>, and the
 * shell treats the whole thing as an opaque handle. Keep the two halves in step
 * by hand if SSID_NAME changes: 546164706f6c65 is "Tadpole" in ASCII hex, and
 * 02:00:54:41:44:70 is a locally-administered MAC that belongs to nothing. */
#define SSID_NAME   "Tadpole"
#define SVC_PATH    "/net/connman/service/wifi_020054414470_546164706f6c65_managed_none"

#define SIGNAL_STRENGTH  88   /* > 50, which is RioConnmanApp's "strengthHigh" */

/* ---- state ------------------------------------------------------------- */

/* The two things the UI can actually change. Everything else is constant.
 * wifi_powered is the Technology's Powered/Connected; svc_connected is the
 * Service's state. The Manager's State is derived from both, because that is
 * what connman does and what WifiStatusIcon reads. */
static int wifi_powered   = 1;
static int svc_connected  = 1;
static int chatty         = 1;

static const char *mgr_state(void)
{
	return (wifi_powered && svc_connected) ? "online" : "idle";
}

static const char *svc_state(void)
{
	return svc_connected ? "online" : "idle";
}

static void say(const char *fmt, const char *a, const char *b)
{
	if (!chatty)
		return;
	printf(fmt, a, b);
	fflush(stdout);
}

/* ---- marshalling helpers ----------------------------------------------- */
/*
 * Every one of these appends one dict entry to an open a{sv} array. libdbus's
 * append_basic takes the ADDRESS of the value for every type including strings
 * (where it wants a char **), which is the single easiest thing to get wrong
 * here, so the wrapping is worth having.
 */

static void de_open(DBusMessageIter *arr, const char *key, const char *vsig,
                    DBusMessageIter *ent, DBusMessageIter *var)
{
	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, ent);
	dbus_message_iter_append_basic(ent, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(ent, DBUS_TYPE_VARIANT, vsig, var);
}

static void de_close(DBusMessageIter *arr, DBusMessageIter *ent, DBusMessageIter *var)
{
	dbus_message_iter_close_container(ent, var);
	dbus_message_iter_close_container(arr, ent);
}

static void ap_str(DBusMessageIter *arr, const char *key, const char *val)
{
	DBusMessageIter e, v;
	de_open(arr, key, "s", &e, &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
	de_close(arr, &e, &v);
}

static void ap_bool(DBusMessageIter *arr, const char *key, int val)
{
	DBusMessageIter e, v;
	dbus_bool_t b = val ? 1u : 0u;
	de_open(arr, key, "b", &e, &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
	de_close(arr, &e, &v);
}

static void ap_byte(DBusMessageIter *arr, const char *key, int val)
{
	DBusMessageIter e, v;
	dbus_uint8_t y = (dbus_uint8_t)val;
	de_open(arr, key, "y", &e, &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE, &y);
	de_close(arr, &e, &v);
}

/* An "as". n may be 0, which is how connman reports "nothing configured" — an
 * empty array, not a missing property. */
static void ap_strlist(DBusMessageIter *arr, const char *key,
                       const char *const *vals, int n)
{
	DBusMessageIter e, v, a;
	int i;
	de_open(arr, key, "as", &e, &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
	for (i = 0; i < n; i++)
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &vals[i]);
	dbus_message_iter_close_container(&v, &a);
	de_close(arr, &e, &v);
}

/* A nested a{sv} — IPv4, Proxy, Ethernet and their .Configuration twins. The
 * caller fills sub and then calls ap_dict_end. Up to four string pairs, which
 * is all any of ours needs. */
static void ap_dict(DBusMessageIter *arr, const char *key,
                    const char *k1, const char *v1, const char *k2, const char *v2,
                    const char *k3, const char *v3, const char *k4, const char *v4)
{
	DBusMessageIter e, v, sub;
	de_open(arr, key, "a{sv}", &e, &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{sv}", &sub);
	if (k1) ap_str(&sub, k1, v1);
	if (k2) ap_str(&sub, k2, v2);
	if (k3) ap_str(&sub, k3, v3);
	if (k4) ap_str(&sub, k4, v4);
	dbus_message_iter_close_container(&v, &sub);
	de_close(arr, &e, &v);
}

/* ---- the property sets ------------------------------------------------- */

static void manager_props(DBusMessageIter *d)
{
	ap_str(d, "State", mgr_state());
	ap_bool(d, "OfflineMode", 0);
	ap_bool(d, "SessionMode", 0);
}

static void tech_props(DBusMessageIter *d)
{
	/* Type is the load-bearing one: NetworkingModel keys its technology map
	 * by Type, not by object path, and looks up "wifi". */
	ap_str(d, "Name", "WiFi");
	ap_str(d, "Type", "wifi");
	ap_bool(d, "Powered", wifi_powered);
	ap_bool(d, "Connected", wifi_powered && svc_connected);
	ap_bool(d, "Tethering", 0);
}

static void service_props(DBusMessageIter *d)
{
	static const char *const security[] = { "none" };
	static const char *const nameservers[] = { "192.168.4.1" };
	static const char *const nothing[] = { NULL };

	ap_str(d, "State", svc_state());
	ap_str(d, "Name", SSID_NAME);
	ap_str(d, "Type", "wifi");
	ap_strlist(d, "Security", security, 1);
	ap_byte(d, "Strength", SIGNAL_STRENGTH);
	/* Favorite AND AutoConnect: a favourite network is one the device has
	 * already joined and stored, which is what stops anything deciding it
	 * needs to ask the Agent for credentials. */
	ap_bool(d, "Favorite", 1);
	ap_bool(d, "Immutable", 0);
	ap_bool(d, "AutoConnect", 1);
	ap_bool(d, "Roaming", 0);
	ap_dict(d, "Ethernet", "Method", "auto", "Interface", "wlan0",
	        "Address", "02:00:54:41:44:70", NULL, NULL);
	ap_dict(d, "IPv4", "Method", "dhcp", "Address", "192.168.4.2",
	        "Netmask", "255.255.255.0", "Gateway", "192.168.4.1");
	ap_dict(d, "IPv4.Configuration", "Method", "dhcp", NULL, NULL, NULL, NULL, NULL, NULL);
	ap_dict(d, "IPv6", "Method", "off", NULL, NULL, NULL, NULL, NULL, NULL);
	ap_dict(d, "IPv6.Configuration", "Method", "off", NULL, NULL, NULL, NULL, NULL, NULL);
	ap_strlist(d, "Nameservers", nameservers, 1);
	ap_strlist(d, "Nameservers.Configuration", nothing, 0);
	ap_strlist(d, "Domains", nothing, 0);
	ap_strlist(d, "Domains.Configuration", nothing, 0);
	ap_strlist(d, "Timeservers", nothing, 0);
	ap_strlist(d, "Timeservers.Configuration", nothing, 0);
	ap_dict(d, "Proxy", "Method", "direct", NULL, NULL, NULL, NULL, NULL, NULL);
	ap_dict(d, "Proxy.Configuration", "Method", "direct", NULL, NULL, NULL, NULL, NULL, NULL);
}

/* One (oa{sv}) — connman's "ConnmanObject", which is what GetServices and
 * GetTechnologies return arrays of and what ServicesChanged carries. */
static void append_object(DBusMessageIter *arr, const char *path,
                          void (*props)(DBusMessageIter *))
{
	DBusMessageIter st, d;
	dbus_message_iter_open_container(arr, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "{sv}", &d);
	props(&d);
	dbus_message_iter_close_container(&st, &d);
	dbus_message_iter_close_container(arr, &st);
}

/* ---- replies ----------------------------------------------------------- */

static DBusHandlerResult ship(DBusConnection *c, DBusMessage *r)
{
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_connection_send(c, r, NULL);
	dbus_message_unref(r);
	dbus_connection_flush(c);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult reply_empty(DBusConnection *c, DBusMessage *m)
{
	return ship(c, dbus_message_new_method_return(m));
}

static DBusHandlerResult reply_props(DBusConnection *c, DBusMessage *m,
                                     void (*props)(DBusMessageIter *))
{
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter it, d;
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
	props(&d);
	dbus_message_iter_close_container(&it, &d);
	return ship(c, r);
}

/* a(oa{sv}) with either nothing in it or our single object. */
static DBusHandlerResult reply_objlist(DBusConnection *c, DBusMessage *m,
                                       const char *path, void (*props)(DBusMessageIter *),
                                       int present)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter it, a;
	if (!r)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(oa{sv})", &a);
	if (present)
		append_object(&a, path, props);
	dbus_message_iter_close_container(&it, &a);
	return ship(c, r);
}

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

/* ---- signals ----------------------------------------------------------- */

static DBusConnection *bus;

/* PropertyChanged(s, v) — the same shape on all three interfaces. Only the
 * string and boolean forms are needed, so `sval` picks which. */
static void emit_prop_str(const char *path, const char *iface,
                          const char *key, const char *val)
{
	DBusMessage *s = dbus_message_new_signal(path, iface, "PropertyChanged");
	DBusMessageIter it, v;
	if (!s)
		return;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
	dbus_message_iter_close_container(&it, &v);
	dbus_connection_send(bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(bus);
}

static void emit_prop_bool(const char *path, const char *iface,
                           const char *key, int val)
{
	DBusMessage *s = dbus_message_new_signal(path, iface, "PropertyChanged");
	DBusMessageIter it, v;
	dbus_bool_t b = val ? 1u : 0u;
	if (!s)
		return;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
	dbus_message_iter_close_container(&it, &v);
	dbus_connection_send(bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(bus);
}

/* ServicesChanged(a(oa{sv}) changed, ao removed). Our service is either in the
 * first list or named in the second; there is only ever the one. */
static void emit_services_changed(int present)
{
	DBusMessage *s = dbus_message_new_signal(MGR_PATH, MGR_IFACE, "ServicesChanged");
	DBusMessageIter it, a, rm;
	const char *path = SVC_PATH;
	if (!s)
		return;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(oa{sv})", &a);
	if (present)
		append_object(&a, SVC_PATH, service_props);
	dbus_message_iter_close_container(&it, &a);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &rm);
	if (!present)
		dbus_message_iter_append_basic(&rm, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_close_container(&it, &rm);
	dbus_connection_send(bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(bus);
}

static void emit_technology_added(void)
{
	DBusMessage *s = dbus_message_new_signal(MGR_PATH, MGR_IFACE, "TechnologyAdded");
	DBusMessageIter it, d;
	const char *path = TECH_PATH;
	if (!s)
		return;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
	tech_props(&d);
	dbus_message_iter_close_container(&it, &d);
	dbus_connection_send(bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(bus);
}

/* Everything the shell would have learned from a fresh GetProperties /
 * GetTechnologies / GetServices, pushed instead. Sent once shortly after
 * startup so that anything which built its NetworkManager before we owned the
 * name is corrected without having to reconnect. Idempotent by construction —
 * these are the same values a poll would return. */
static void announce(void)
{
	emit_technology_added();
	emit_prop_bool(TECH_PATH, TECH_IFACE, "Powered", wifi_powered);
	emit_prop_bool(TECH_PATH, TECH_IFACE, "Connected", wifi_powered && svc_connected);
	emit_services_changed(wifi_powered);
	emit_prop_str(SVC_PATH, SVC_IFACE, "State", svc_state());
	emit_prop_str(MGR_PATH, MGR_IFACE, "State", mgr_state());
	say("[connman-fake] announced: manager state %s, wifi technology present\n",
	    mgr_state(), NULL);
}

/* The whole visible world changes together: turning the radio off has to take
 * the service and the manager state with it, or the status bar and the settings
 * page disagree with each other. */
static void set_powered(int on)
{
	if (wifi_powered == !!on)
		return;
	wifi_powered = !!on;
	svc_connected = wifi_powered;
	emit_prop_bool(TECH_PATH, TECH_IFACE, "Powered", wifi_powered);
	emit_prop_bool(TECH_PATH, TECH_IFACE, "Connected", wifi_powered);
	emit_services_changed(wifi_powered);
	if (wifi_powered)
		emit_prop_str(SVC_PATH, SVC_IFACE, "State", svc_state());
	emit_prop_str(MGR_PATH, MGR_IFACE, "State", mgr_state());
	say("[connman-fake] wifi %s\n", wifi_powered ? "on" : "off", NULL);
}

static void set_connected(int on)
{
	if (svc_connected == !!on)
		return;
	svc_connected = !!on;
	emit_prop_str(SVC_PATH, SVC_IFACE, "State", svc_state());
	emit_prop_bool(TECH_PATH, TECH_IFACE, "Connected", wifi_powered && svc_connected);
	emit_prop_str(MGR_PATH, MGR_IFACE, "State", mgr_state());
	say("[connman-fake] service %s\n", svc_connected ? "connected" : "disconnected", NULL);
}

/* ---- introspection ----------------------------------------------------- */
/*
 * Not needed by the shell — libQtRioConnman's proxies are generated from XML at
 * build time and never ask. It is here because `dbus-send --print-reply ...
 * Introspect` is how anyone debugging this will look at it, and a service that
 * cannot be introspected is a service nobody can check by hand.
 */

#define XML_HEAD \
  "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n" \
  "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>" \
  "<interface name=\"org.freedesktop.DBus.Introspectable\">" \
  "<method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>" \
  "</interface>"

static const char xml_manager[] =
	XML_HEAD
	"<interface name=\"net.connman.Manager\">"
	"<method name=\"GetProperties\"><arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"SetProperty\"><arg name=\"name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"in\"/></method>"
	"<method name=\"GetTechnologies\"><arg name=\"technologies\" type=\"a(oa{sv})\" direction=\"out\"/></method>"
	"<method name=\"GetServices\"><arg name=\"services\" type=\"a(oa{sv})\" direction=\"out\"/></method>"
	"<method name=\"GetPeers\"><arg name=\"peers\" type=\"a(oa{sv})\" direction=\"out\"/></method>"
	"<method name=\"RegisterAgent\"><arg name=\"path\" type=\"o\" direction=\"in\"/></method>"
	"<method name=\"UnregisterAgent\"><arg name=\"path\" type=\"o\" direction=\"in\"/></method>"
	"<method name=\"RegisterCounter\"><arg name=\"path\" type=\"o\" direction=\"in\"/><arg name=\"accuracy\" type=\"u\" direction=\"in\"/><arg name=\"period\" type=\"u\" direction=\"in\"/></method>"
	"<method name=\"UnregisterCounter\"><arg name=\"path\" type=\"o\" direction=\"in\"/></method>"
	"<signal name=\"PropertyChanged\"><arg name=\"name\" type=\"s\"/><arg name=\"value\" type=\"v\"/></signal>"
	"<signal name=\"TechnologyAdded\"><arg name=\"path\" type=\"o\"/><arg name=\"properties\" type=\"a{sv}\"/></signal>"
	"<signal name=\"TechnologyRemoved\"><arg name=\"path\" type=\"o\"/></signal>"
	"<signal name=\"ServicesChanged\"><arg name=\"changed\" type=\"a(oa{sv})\"/><arg name=\"removed\" type=\"ao\"/></signal>"
	"</interface>"
	"<node name=\"net\"/></node>";

static const char xml_technology[] =
	XML_HEAD
	"<interface name=\"net.connman.Technology\">"
	"<method name=\"GetProperties\"><arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"SetProperty\"><arg name=\"name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"in\"/></method>"
	"<method name=\"Scan\"/>"
	"<signal name=\"PropertyChanged\"><arg name=\"name\" type=\"s\"/><arg name=\"value\" type=\"v\"/></signal>"
	"</interface></node>";

static const char xml_service[] =
	XML_HEAD
	"<interface name=\"net.connman.Service\">"
	"<method name=\"GetProperties\"><arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"SetProperty\"><arg name=\"name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"in\"/></method>"
	"<method name=\"ClearProperty\"><arg name=\"name\" type=\"s\" direction=\"in\"/></method>"
	"<method name=\"Connect\"/><method name=\"Disconnect\"/><method name=\"Remove\"/>"
	"<method name=\"MoveBefore\"><arg name=\"service\" type=\"o\" direction=\"in\"/></method>"
	"<method name=\"MoveAfter\"><arg name=\"service\" type=\"o\" direction=\"in\"/></method>"
	"<signal name=\"PropertyChanged\"><arg name=\"name\" type=\"s\"/><arg name=\"value\" type=\"v\"/></signal>"
	"</interface></node>";

/* ---- the handler ------------------------------------------------------- */

/* SetProperty(s, v) with a boolean in it. Returns -1 if the argument is not
 * shaped like that, which is the only form anything sends us. */
static int arg_bool(DBusMessage *m, const char *want_key)
{
	DBusMessageIter it, v;
	const char *key = NULL;
	dbus_bool_t b = 0;
	if (!dbus_message_iter_init(m, &it))
		return -1;
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
		return -1;
	dbus_message_iter_get_basic(&it, &key);
	if (!key || strcmp(key, want_key) != 0)
		return -1;
	if (!dbus_message_iter_next(&it))
		return -1;
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT)
		return -1;
	dbus_message_iter_recurse(&it, &v);
	if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_BOOLEAN)
		return -1;
	dbus_message_iter_get_basic(&v, &b);
	return b ? 1 : 0;
}

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

	say("[connman-fake] call %s %s\n", path, memb);

	/* Introspection first, because it is asked of every path. */
	if (iface && strcmp(iface, INTROSPECT) == 0 && strcmp(memb, "Introspect") == 0) {
		if (strcmp(path, TECH_PATH) == 0)
			return reply_xml(c, m, xml_technology);
		if (strcmp(path, SVC_PATH) == 0)
			return reply_xml(c, m, xml_service);
		return reply_xml(c, m, xml_manager);
	}

	if (strcmp(path, MGR_PATH) == 0) {
		if (strcmp(memb, "GetProperties") == 0)
			return reply_props(c, m, manager_props);
		if (strcmp(memb, "GetTechnologies") == 0)
			return reply_objlist(c, m, TECH_PATH, tech_props, 1);
		if (strcmp(memb, "GetServices") == 0)
			return reply_objlist(c, m, SVC_PATH, service_props, wifi_powered);
		/* Wi-Fi Direct. Nothing in this shell uses it, but answering with an
		 * empty list is cheaper than letting a caller time out. */
		if (strcmp(memb, "GetPeers") == 0)
			return reply_objlist(c, m, NULL, NULL, 0);
		/* WE ACCEPT THE AGENT AND NEVER CALL IT. The shell registers
		 * /WifiSettings so connman can ask it for a passphrase; our network is
		 * open and already joined, so there is nothing to ask. Refusing the
		 * registration would be worse than accepting it — libQtRioConnman
		 * treats an error here as connman being broken. */
		if (strcmp(memb, "RegisterAgent") == 0 ||
		    strcmp(memb, "UnregisterAgent") == 0 ||
		    strcmp(memb, "RegisterCounter") == 0 ||
		    strcmp(memb, "UnregisterCounter") == 0 ||
		    strcmp(memb, "SetProperty") == 0)
			return reply_empty(c, m);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strcmp(path, TECH_PATH) == 0) {
		if (strcmp(memb, "GetProperties") == 0)
			return reply_props(c, m, tech_props);
		if (strcmp(memb, "SetProperty") == 0) {
			int on = arg_bool(m, "Powered");
			if (on >= 0)
				set_powered(on);
			return reply_empty(c, m);
		}
		/* A scan finds what is already here. Reply first, then re-announce the
		 * one network, which is what makes the settings page redraw its list. */
		if (strcmp(memb, "Scan") == 0) {
			DBusHandlerResult r = reply_empty(c, m);
			emit_services_changed(wifi_powered);
			return r;
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strcmp(path, SVC_PATH) == 0) {
		if (strcmp(memb, "GetProperties") == 0)
			return reply_props(c, m, service_props);
		if (strcmp(memb, "Connect") == 0) {
			DBusHandlerResult r = reply_empty(c, m);
			set_connected(1);
			return r;
		}
		if (strcmp(memb, "Disconnect") == 0 || strcmp(memb, "Remove") == 0) {
			DBusHandlerResult r = reply_empty(c, m);
			set_connected(0);
			return r;
		}
		if (strcmp(memb, "SetProperty") == 0 || strcmp(memb, "ClearProperty") == 0 ||
		    strcmp(memb, "MoveBefore") == 0 || strcmp(memb, "MoveAfter") == 0)
			return reply_empty(c, m);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable vtable = { NULL, on_message, NULL, NULL, NULL, NULL };

/* ---- entry point ------------------------------------------------------- */

static void usage(void)
{
	printf("tadpole-connman --address <dbus system bus address> [--quiet]\n");
	printf("  A fake net.connman. The bus address is REQUIRED and there is no\n");
	printf("  default, on purpose: libdbus's built-in fallback is\n");
	printf("  /var/run/dbus/system_bus_socket, and qemu-user does not translate\n");
	printf("  that path, so it names the HOST's own system bus.\n");
	fflush(stdout);
}

static int cm_main(int argc, char **argv)
{
	DBusError err;
	const char *address = NULL;
	int i, rc, announced = 0;
	long t0;

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
	/* open_private + bus_register, NOT dbus_bus_get(DBUS_BUS_SYSTEM).
	 *
	 * dbus_bus_get would read DBUS_SYSTEM_BUS_ADDRESS from the environment and
	 * fall back to the compiled-in socket path if it were unset or unreadable.
	 * Two reasons not to give it the chance. The fallback path is the host's
	 * bus, as above. And this binary is linked -nostdlib with its own _start,
	 * so uClibc's __environ is never populated and getenv() inside libdbus
	 * would come back NULL every time — the fallback would not be an edge case
	 * here, it would be the only case. */
	bus = dbus_connection_open_private(address, &err);
	if (!bus) {
		printf("[connman-fake] cannot reach the system bus: %s\n",
		       dbus_error_is_set(&err) ? err.message : "unknown error");
		fflush(stdout);
		return 1;
	}
	if (!dbus_bus_register(bus, &err)) {
		printf("[connman-fake] Hello refused: %s\n",
		       dbus_error_is_set(&err) ? err.message : "unknown error");
		fflush(stdout);
		return 1;
	}
	/* If the bus goes away the shell is gone too; leave rather than spin. */
	dbus_connection_set_exit_on_disconnect(bus, 1);

	rc = dbus_bus_request_name(bus, CM_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		/* Almost always means the real connmand got here first. Say so plainly
		 * rather than sitting in a queue nobody will drain — two owners of
		 * net.connman is not a state to paper over. */
		printf("[connman-fake] net.connman is already owned (rc=%d%s%s) — "
		       "not starting. Is connmand running?\n", rc,
		       dbus_error_is_set(&err) ? ": " : "",
		       dbus_error_is_set(&err) ? err.message : "");
		fflush(stdout);
		return 1;
	}

	/* One fallback at "/" catches the manager, the technology and the service
	 * in a single handler; there are only three paths and they are compared by
	 * name anyway. */
	if (!dbus_connection_register_fallback(bus, "/", &vtable, NULL)) {
		printf("[connman-fake] could not register the object tree\n");
		fflush(stdout);
		return 1;
	}

	printf("[connman-fake] owning net.connman: 1 wifi technology, "
	       "1 service \"%s\" (%s), manager state %s\n",
	       SSID_NAME, svc_state(), mgr_state());
	fflush(stdout);

	t0 = time(NULL);
	/* A bounded timeout rather than -1 so the announce below happens even on a
	 * completely silent bus. */
	while (dbus_connection_read_write_dispatch(bus, 250)) {
		if (!announced && time(NULL) - t0 >= 2) {
			announced = 1;
			announce();
		}
	}
	return 0;
}

/* Freestanding, exactly as tone_test.c is: the guest lib set has no ARM crt1.o,
 * so there is no C runtime to hand us argc/argv. At _start the ABI leaves them
 * on the stack — argc, then argv[], then a NULL, then the environment — so take
 * sp before the compiler has a chance to move it. `naked` is what guarantees no
 * prologue runs first; without it clang pushes registers and sp no longer
 * points at argc.
 *
 * We do not go on to set uClibc's __environ from that stack. Nothing here needs
 * the environment, and leaving it empty removes the one way libdbus could pick
 * up a bus address we did not hand it. */
void tad_connman_start(unsigned long *sp) __attribute__((noreturn, used));

__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile("mov r0, sp\n\t"
	                 "b   tad_connman_start\n");
}

void tad_connman_start(unsigned long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)&sp[1];
	exit(cm_main(argc, argv));
}
