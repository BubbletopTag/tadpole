/* Tadpole — the viewer's few portability shims.
 *
 * SMALL ON PURPOSE. Everything in here is a call with no portable spelling;
 * anything that has one (SDL, C stdio, dirent — which mingw-w64 provides) is
 * used directly and does not appear here. The genuinely Unix-shaped code —
 * fork/exec guest supervision, FIFOs, shared mappings — is NOT shimmed here
 * either: it is stubbed at its definition site, where the stub can say
 * honestly what is missing, rather than faked call by call.
 */
#ifndef TADPOLE_PORT_H
#define TADPOLE_PORT_H

#ifdef _WIN32

#include <direct.h>    /* _mkdir */
#include <io.h>        /* access, chmod oldnames */
#include <process.h>   /* execv oldname, for the AppImage-only restart path */
#include <stdlib.h>    /* _fullpath */

/* Windows mkdir takes no mode; the mode was advisory everywhere we call it. */
static inline int tp_mkdir(const char *path) { return _mkdir(path); }

/* realpath without the realpath: _fullpath does not require the file to
 * exist, which is fine for our one caller (resolving argv[0], which does). */
static inline char *tp_realpath(const char *path, char *out, size_t cap)
{
    return _fullpath(out, path, cap);
}

/* O_NONBLOCK has no Win32 meaning. The only opens that pass it are for the
 * guest's FIFO event nodes, which do not exist in this build — the open
 * fails on the missing path first, so a zero flag changes nothing. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

#else  /* POSIX */

#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>

static inline int tp_mkdir(const char *path) { return mkdir(path, 0755); }

static inline char *tp_realpath(const char *path, char *out, size_t cap)
{
    (void)cap;                 /* PATH_MAX-sized by contract of the caller */
    return realpath(path, out);
}

#endif
#endif /* TADPOLE_PORT_H */
