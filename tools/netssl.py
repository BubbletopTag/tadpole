# Tadpole — one HTTPS setup, used by every tool that downloads something.
#
#   from netssl import urlopen
#   with urlopen(url, timeout=30) as r: ...
#
# WHY THIS EXISTS: WINDOWS 7 CANNOT VERIFY LEAPFROG'S CERTIFICATE.
#
# digitalcontent.leapfrog.com presents a chain rooted at DigiCert Global Root
# G2, issued in 2013. Windows ships its trusted roots through Windows Update
# rather than in the installer, so a Windows 7 that has not been updated —
# which is every fresh install, and Windows Update on 7 barely runs any more —
# does not have that root. Python asks the Windows certificate store, the store
# says no, and the download fails verification.
#
# It presented as "cannot reach digitalcontent.leapfrog.com" while the same
# host browsed fine in Internet Explorer, because IE was on http:// and this is
# https://. The machine had a working network the entire time.
#
# THE FIX IS A CA BUNDLE IN THE BOX, not disabled verification. Turning
# verification off would "work" and would mean anyone between the user and
# LeapFrog can hand them a firmware image, which is a bad trade for a program
# whose whole job is to unpack that image and run it. tools/build-windows.sh
# copies the host's Mozilla CA bundle in beside the interpreter; this finds it
# and hands it to OpenSSL explicitly.
#
# On Linux there is no bundle beside the interpreter and none is needed — the
# system store is current — so this falls through to the default context and
# behaves exactly as before.

import os
import ssl
import sys
import urllib.request

_CTX = None


def _find_bundle():
    """Where the bundled roots might be, nearest first.

    Beside the interpreter is where the Windows build puts them, because that
    directory is already the thing the installer ships and the viewer probes.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    for p in (os.path.join(os.path.dirname(sys.executable), "cacert.pem"),
              os.path.join(here, "cacert.pem"),
              os.environ.get("TADPOLE_CA_BUNDLE") or ""):
        if p and os.path.isfile(p):
            return p
    return None


def context():
    global _CTX
    if _CTX is None:
        bundle = _find_bundle()
        try:
            _CTX = ssl.create_default_context(cafile=bundle) if bundle \
                else ssl.create_default_context()
        except Exception:
            _CTX = ssl.create_default_context()
    return _CTX


def urlopen(url, timeout=30, **kw):
    return urllib.request.urlopen(url, timeout=timeout, context=context(), **kw)


def explain(e):
    """A one-line reason a request failed, aimed at the person reading it.

    The generic "cannot reach the server" this replaces was worse than no
    message: it named the network, which was fine, and hid the certificate,
    which was not.
    """
    import socket
    import urllib.error
    if isinstance(e, urllib.error.HTTPError):
        # The server answered, with a refusal. That is not a network problem
        # and saying "check your network" about it sends people the wrong way.
        return "the server answered HTTP %s (%s)." % (e.code, e.reason)
    if isinstance(e, ssl.SSLCertVerificationError) or \
            isinstance(getattr(e, "reason", None), ssl.SSLCertVerificationError):
        extra = "" if _find_bundle() else \
            " No CA bundle was found beside the interpreter, which is how" \
            " Windows 7 is meant to get one."
        return ("the server's certificate could not be verified.%s" % extra)
    reason = getattr(e, "reason", e)
    if isinstance(reason, socket.timeout):
        return "the server did not answer in time."
    if isinstance(reason, socket.gaierror):
        return "the name could not be looked up — check DNS."
    return "%s: %s" % (type(reason).__name__, reason)
