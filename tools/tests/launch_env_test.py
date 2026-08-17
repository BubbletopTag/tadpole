#!/usr/bin/env python3
"""Do the front end's saved settings actually reach the guest?

    ./tools/tests/launch_env_test.py

One script, no framework, no emulator — the same shape as link_resolve_test.py.

THE BUG THIS EXISTS TO PREVENT has now been reported twice, against two
different settings, with the same shape both times: the viewer writes a setting
to ui.cfg, `./tadpole.sh` never reads it, and the guest quietly uses its own
built-in default instead. The setting appears to do nothing, and restarting
does not help, because the launch PATH is what is wrong rather than the state.

  * TADPOLE_GL was the first (fixed at tadpole.sh:90) — "the checkbox does
    nothing, but TADPOLE_GL=1 ./tadpole.sh works".
  * TADPOLE_HZ was the second: the frame cap was stuck at exactly 60, which is
    the hardcoded fallback in BOTH pacers (tadpole_shim.c:483 via
    VSYNC_HZ_DEFAULT, and tadpole_gles_core.c:519), reported against a Didj
    title after an unrelated resolution change drew attention to it.

So this pins the whole class down rather than the one key, and `--print-env`
exists so it can: it resolves the environment the guest would be handed and
prints it without booting anything.

What it checks:

  * a saved frame cap reaches the guest as TADPOLE_HZ
  * an explicit 0 SURVIVES — "uncapped" is a real setting, and the obvious
    `[ -z "$x" ]` spelling of this fix silently turns it back into 60
  * the environment still wins over the file, so `TADPOLE_HZ=15 ./tadpole.sh`
    keeps working for anyone measuring something
  * a setting absent from the file exports nothing, leaving the guest's own
    default alone rather than pinning it to ours
  * TADPOLE_GL, the one that was already fixed, has not regressed
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(PROJ, "tadpole.sh")

failures = []


def run_print_env(cfg_text, extra_env=None):
    """Run `tadpole.sh --print-env` against a throwaway ui.cfg."""
    tmp = tempfile.mkdtemp(prefix="tadpole-env-test-")
    cfgdir = os.path.join(tmp, "tadpole")
    os.makedirs(cfgdir)
    if cfg_text is not None:
        with open(os.path.join(cfgdir, "ui.cfg"), "w") as f:
            f.write(cfg_text)

    env = dict(os.environ)
    # Both, so neither the real config nor a real home can leak in.
    env["XDG_CONFIG_HOME"] = tmp
    env["HOME"] = tmp
    # Inherited from whatever shell runs the test suite otherwise, which would
    # make the "environment wins" case pass for the wrong reason.
    for k in ("TADPOLE_HZ", "TADPOLE_GL"):
        env.pop(k, None)
    if extra_env:
        env.update(extra_env)

    p = subprocess.run([SCRIPT, "--print-env"], env=env,
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(
            "tadpole.sh --print-env exited %d\nstdout:\n%s\nstderr:\n%s"
            % (p.returncode, p.stdout, p.stderr))
    out = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


def check(what, got, want):
    if got == want:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s: got %r, want %r" % (what, got, want))
        failures.append(what)


print("saved frame cap reaches the guest")
env = run_print_env("gl 1\nframe_cap 30\n")
check("frame_cap 30 -> TADPOLE_HZ", env.get("TADPOLE_HZ"), "30")

print("uncapped is a real setting, not an absent one")
env = run_print_env("gl 1\nframe_cap 0\n")
check("frame_cap 0 -> TADPOLE_HZ", env.get("TADPOLE_HZ"), "0")

print("the environment still wins over the file")
env = run_print_env("gl 1\nframe_cap 30\n", {"TADPOLE_HZ": "15"})
check("TADPOLE_HZ=15 beats frame_cap 30", env.get("TADPOLE_HZ"), "15")

print("a setting the file does not mention is left to the guest")
env = run_print_env("gl 1\n")
check("no frame_cap -> no TADPOLE_HZ", env.get("TADPOLE_HZ"), None)

print("the setting that was already fixed has not regressed")
env = run_print_env("gl 0\nframe_cap 30\n")
check("gl 0 -> TADPOLE_GL", env.get("TADPOLE_GL"), "0")

print("")
if failures:
    print("FAILED (%d)" % len(failures))
    for f in failures:
        print("  - %s" % f)
    sys.exit(1)
print("all good")
