#!/usr/bin/env python3
"""fetch-firmware.py — pull LeapPad2 packages straight from LeapFrog's CDN.

    ./tools/fetch-firmware.py --probe            what is downloadable, no writes
    ./tools/fetch-firmware.py --get update,repair   fetch by package TYPE
    ./tools/fetch-firmware.py --get all -o /tmp/fw

WHY THIS EXISTS. Installing has meant feeding the emulator a firmware dump taken
off somebody's hardware over a flaky USB cable. The packages are served
publicly, and LFConnect finds them the same way this does: the device's package
list names the IDs, and the CDN lays them out by ID.

    https://digitalcontent.leapfrog.com/packages/<middle>/<PackageID>.lf2

`<middle>` is the middle field of the ID, so PADS-0x001E000F-000001 lives at
packages/0x001E000F/PADS-0x001E000F-000001.lf2. The ID list is not discoverable
from the CDN — it comes from tools/packagelists/EnglishLeapPad2.xml, which is
the list LFConnect itself uses, with the <Package type="..."> attribute saying
what each one is for (update, repair, optional, content, ...).

THE BUCKET LISTING IS A DEAD END, so do not spend another session on it. The
root URL answers with an S3 ListBucket XML, but the host in front of it does not
forward query strings: `marker`, `prefix` and `max-keys` are all ignored, so
every request returns the same first 1000 keys. Paging appears to work — you get
a fresh 1000 keys each time — and they are the SAME 1000 keys. The only reliable
question is "does this exact object exist", asked with HEAD.
"""
import argparse, os, sys, urllib.request, urllib.error
import xml.etree.ElementTree as ET

BASE = "https://digitalcontent.leapfrog.com/packages"
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
DEVICES = os.path.join(PROJ, "runtime", "devices")
XML = os.path.join(HERE, "packagelists", "EnglishLeapPad2.xml")
BUNDLED = os.path.join(HERE, "packagelists", "lp2-bundled.txt")
EXTS = ("lf2", "lf3", "lfp")


def profile(dev_id):
    """-> the DEV_* map from runtime/devices/<dev_id>.conf.

    The same file runtime/device.sh reads. Which packages exist and which CDN
    directory holds them are per-device facts, so they live beside the rest of
    the device's description; adding a tablet is then one file rather than an
    edit here and an edit there.
    """
    out, path = {}, os.path.join(DEVICES, dev_id + ".conf")
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            if k.startswith("DEV_"):
                out[k] = v.strip().strip('"')
    return out


def bundled(path=BUNDLED):
    """The titles that come with the device, plus each one's DeviceAsset.

    They are in no manifest the machine ships with — see the note in
    lp2-bundled.txt — so the IDs are listed there and the icon package is
    derived: always PADS-<middle>-DA0000.lf2, whatever the game's own prefix.
    """
    out = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                pid, _, desc = line.partition(" ")
                mid = pid.split("-")[1] if "-" in pid else ""
                out.append((pid, desc.strip(), {"bundled"}))
                if mid:
                    out.append(("PADS-%s-DA0000" % mid,
                                (desc.strip() + " DA").strip(), {"bundled"}))
    except OSError:
        pass
    return out


def packages(xml_path):
    """-> [(id, description, types)], in document order, de-duplicated.

    Parsed as XML rather than by regex: the file is hand-maintained and its
    attribute spacing is not uniform — one entry reads `description= "My Books"`
    with a space after the equals, which a pattern keyed on the exact spelling
    silently drops."""
    root = ET.parse(xml_path).getroot()
    out, seen = [], set()
    for el in root.iter():
        if el.tag.lower() != "package":
            continue
        pid = el.get("id")
        if not pid or pid in seen:
            continue
        seen.add(pid)
        types = set(t.strip() for t in (el.get("type") or "").split(",") if t.strip())
        out.append((pid, (el.get("description") or "").strip(), types))
    return out


# THE DIRECTORY IS NOT ALWAYS THE ID'S MIDDLE FIELD.
#
# Content packages live at packages/<middle>/<id>.lf2, which is the shape the
# example URL everyone passes around has. The FIRMWARE does not: OpenLFConnect's
# leappad2.cfg gives the whole answer —
#
#     [names]    LF_URL:PADFW
#     [packages] FIRMWARE:PAD2-0x00220004-000000.lfp
#
# so the directory is a per-DEVICE name and the extension is .lfp. Probing the
# firmware ID under its own middle field 404s in every extension, which is why
# "the firmware is not on the CDN" is an easy and wrong conclusion to reach.
#
# Set from the chosen profile's DEV_FW_DIR at startup. LeapPad2 is PADFW,
# LeapPad Ultra is PAD3FW, Leapster GS is GAMFW — all confirmed against the
# CDN, and all matching OpenLFConnect's LF_URL for the same device.
DEVICE_DIR = "PADFW"          # LeapPad2; other devices have their own LF_URL


def candidates(pid):
    """-> URLs to try, most likely first.

    THREE LAYOUTS, and missing the third cost thirty-six packages — which is
    to say the entire home screen. Content lives under the middle field of its
    ID, the firmware under the device directory as .lfp, and the device's own
    APPS AND WIDGETS under the device directory as .lf2:

        packages/0x0028000C/PADS-0x0028000C-000000.lf2   a content package
        packages/PADFW/PAD2-0x00220004-000000.lfp        the firmware
        packages/PADFW/PAD2-0x001E0003-000002.lf2        the camera widget

    Only .lfp was ever tried under the device directory, so every PAD2-*
    package — Pet Pad, the camera, gallery, keyboard, microphone and paint
    widgets, the photo editor, sneak peeks — reported "not on the CDN" and the
    finished install had two tiles on its home screen.

    Some early packages use the four-letter system name as the directory
    instead (packages/LPAD/LPAD-0x001F002D-000000.lf3), so that is tried too.

    A DEVICE MAY HAVE NO DIRECTORY AT ALL, and an empty DEV_FW_DIR means
    exactly that rather than "not filled in yet". The LeapPad3 is the case: its
    firmware is served from the ordinary content layout, and every device name
    that could plausibly have held it answers 404. Asking anyway would put a
    `packages//PAD3-...` in front of the CDN once per package in the list.
    """
    mid = pid.split("-")[1]
    sysname = pid.split("-")[0]
    for ext in EXTS:
        yield f"{BASE}/{mid}/{pid}.{ext}"
    for ext in ("lfp", "lf2", "lf3") if DEVICE_DIR else ():
        yield f"{BASE}/{DEVICE_DIR}/{pid}.{ext}"
    for ext in EXTS:
        yield f"{BASE}/{sysname}/{pid}.{ext}"


def head(url, timeout=20):
    """-> size in bytes, or None if it is not there."""
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return int(r.headers.get("content-length", 0))
    except urllib.error.HTTPError:
        return None
    except OSError as e:
        print(f"  ! {url}: {e}", file=sys.stderr)
        return None


def locate(pid):
    """-> (url, size, ext) for whichever candidate exists, else None."""
    for u in candidates(pid):
        n = head(u)
        if n is not None:
            return u, n, u.rsplit(".", 1)[1]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true", help="report availability, download nothing")
    ap.add_argument("--get", metavar="TYPES", help="comma-separated types, or 'all'")
    ap.add_argument("-o", "--out", default="firmware", help="output directory")
    ap.add_argument("--xml", default=None)
    ap.add_argument("--device", default="leappad2",
                    help="a DEV_ID from runtime/devices (default: leappad2)")
    a = ap.parse_args()
    if not a.probe and not a.get:
        ap.error("one of --probe or --get is required")

    global DEVICE_DIR
    prof = profile(a.device)
    DEVICE_DIR = prof.get("DEV_FW_DIR", DEVICE_DIR)
    xml = a.xml or os.path.join(HERE, "packagelists",
                                prof.get("DEV_PKGLIST", "EnglishLeapPad2.xml"))
    print(f"device: {prof.get('DEV_NAME', a.device)}  "
          f"cdn dir: {DEVICE_DIR}  list: {os.path.basename(xml)}")

    # The bundled-titles list is LeapPad2's, and only LeapPad2's: those IDs are
    # in no manifest the device ships with and had to be recovered by hand.
    # Applying them to another tablet would probe ~40 URLs that cannot exist.
    pkgs = packages(xml) + (bundled() if a.device == "leappad2" else [])
    sel = (a.get or "").strip()
    want = None if sel in ("", "all") else set(
        t.strip() for t in sel.split(",") if t.strip())

    # LOOK BEFORE DOWNLOADING. Every package's size is one HEAD away, so the
    # total is knowable before a single byte is fetched — which is what makes
    # an honest percentage possible here, as opposed to the extraction step
    # further on, where the durations are genuinely not knowable and a bar
    # would be a lie.
    plan, missing = [], 0
    for pid, desc, types in pkgs:
        if want is not None and not (types & want):
            continue
        hit = locate(pid)
        if hit is None:
            missing += 1
            if a.probe:
                print(f"  --   {pid:28s} {desc}")
            continue
        url, size, ext = hit
        plan.append((pid, desc, url, size, ext))
        if a.probe:
            print(f"  OK   {pid:28s} {size:>12,}  {desc}")

    total = sum(p[3] for p in plan)
    print(f"\n{len(plan)} available, {missing} not on the CDN, {total:,} bytes total")
    if a.probe:
        return

    os.makedirs(a.out, exist_ok=True)
    done = 0
    # A machine-readable line beside the human one. The viewer's progress panel
    # reads these and draws a real bar; anything that does not understand them
    # sees one more line of log.
    print(f"@@PROGRESS 0 {total}", flush=True)
    for n, (pid, desc, url, size, ext) in enumerate(plan, 1):
        dest = os.path.join(a.out, f"{pid}.{ext}")
        if os.path.exists(dest) and os.path.getsize(dest) == size:
            done += size
            print(f"  [{n}/{len(plan)}] {desc or pid} (already here)")
            print(f"@@PROGRESS {done} {total}", flush=True)
            continue
        print(f"  [{n}/{len(plan)}] {desc or pid}  {size/1048576:.1f} MB", flush=True)
        try:
            with urllib.request.urlopen(url, timeout=120) as r, \
                 open(dest + ".part", "wb") as f:
                while True:
                    chunk = r.read(1 << 16)
                    if not chunk:
                        break
                    f.write(chunk)
                    done += len(chunk)
                    print(f"@@PROGRESS {done} {total}", flush=True)
            os.replace(dest + ".part", dest)
        except OSError as e:
            # One package failing is not the run failing: the rest still make
            # a working system, and saying which one went missing is more use
            # than stopping.
            print(f"      failed: {e}")
            try:
                os.unlink(dest + ".part")
            except OSError:
                pass
    print(f"downloaded into {a.out}")


if __name__ == "__main__":
    main()
