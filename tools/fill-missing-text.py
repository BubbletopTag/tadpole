#!/usr/bin/env python3
"""fill-missing-text.py — supply UI strings this firmware asks for and the
CDN's string tables do not have.

    ./tools/fill-missing-text.py                 report what is missing
    ./tools/fill-missing-text.py --write         fill the gaps in place

WHY THIS IS NEEDED, AND WHY IT IS NOT A PACKAGING MISTAKE ON OUR SIDE.

The Qt shell gets its on-screen text through AssetManager, which reads the
JSON tables installed by the `src json` package (PAD3-0x00270008-200010) into
/LF/Bulk/Downloads/src_json/. That package installs correctly — self-named,
every file present.

It is simply a DIFFERENT REVISION from the firmware image. The image is the
2016-03-22 build of 6.2.0.654; the CDN serves whatever src_json is current,
and the two disagree about key names. RioConnmanApp's QML asks for 24 keys and
the tables define 5 of them. What the tables do have is an older vocabulary —
btn_Join, btn_Retry, btn_OK — where the QML wants btn_Connect, btn_YesSkip,
SkipConfirmation_Line1. Checked in all three locale variants (base, _gb, _ca);
all carry the same 13 keys.

The effect is not cosmetic. A button whose label resolves to nothing still
occupies its place in the layout but shows no text, so the out-of-box WiFi
page's "Yes, Skip" confirmation is a blank box — unreadable, and unhittable
unless you already know where it is. That is what makes setup loop: the page
cannot be satisfied without a network, and the control that skips it cannot be
found.

THESE STRINGS ARE TADPOLE'S, NOT LEAPFROG'S. They are written here to make the
interface usable, and they are marked in the file they are written to. Where a
sensible English phrase is obvious it is used; otherwise the key's own last
component is spelled out, which is ugly and honest and still better than a
blank control. If a matching src_json revision ever turns up, delete
Tadpole_added keys and use the real thing.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
TABLES = os.path.join(PROJ, "runtime", "sysroot", "LF", "Bulk",
                      "Downloads", "src_json")
MODULES = os.path.join(PROJ, "rootfs")

# Hand-written where the meaning is clear from the QML around it. Everything
# else is derived; see spell().
KNOWN = {
    "WiFiWidget__btn_YesSkip":            "Yes, Skip",
    "WiFiWidget__btn_Skip":               "Skip",
    "WiFiWidget__btn_Next":               "Next",
    "WiFiWidget__btn_Connect":            "Connect",
    "WiFiWidget__btn_Disconnect":         "Disconnect",
    "WiFiWidget__btn_Dismiss":            "Dismiss",
    "WiFiWidget__btn_ConnectToWireless":  "Connect to Wireless",
    "WiFiWidget__ForgetNetwork":          "Forget this Network",
    "WiFiWidget__ConnectToNetwork":       "Connect to Network",
    "WiFiWidget__ConnectedToNetwork":     "Connected",
    "WiFiWidget__ConnectPopup_Title":     "Wireless Network Connection",
    "WiFiWidget__PassphrasePopup_Title":  "Network Password",
    "WiFiWidget__IdentitiyPopup_Title":   "Network Sign-In",
    "WiFiWidget__EnterPassword":          "Enter the network password.",
    "WiFiWidget__EnterUsername":          "Enter the network user name.",
    "WiFiWidget__SkipConfirmation_Line1": "Skip setting up wireless?",
    "WiFiWidget__SkipConfirmation_Line2": "You can connect later from Settings.",
}


def spell(key):
    """-> a readable label from a key, for anything not in KNOWN.

    'WiFiWidget__btn_ForgetThisNetwork' -> 'Forget This Network'. Deliberately
    plain: this is a placeholder that admits to being one, not an attempt to
    guess LeapFrog's copy.
    """
    tail = key.split("__")[-1]
    tail = re.sub(r"^(btn|lbl|txt)_", "", tail)
    tail = tail.replace("_", " ")
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", tail).strip() or key


def asked_for(module_glob):
    """-> {key: set(of qml files)} for every assets.txt("KEY") in the modules."""
    out = {}
    pat = re.compile(r"""assets\.txt\(\s*['"]([^'"]+)['"]""")
    for q in glob.glob(module_glob, recursive=True):
        try:
            src = open(q, encoding="utf8", errors="replace").read()
        except OSError:
            continue
        for k in pat.findall(src):
            d = os.path.dirname(q)
            if os.path.basename(d) == "qml":
                d = os.path.dirname(d)
            out.setdefault(k, set()).add(os.path.basename(d))
    return out


def tables():
    """-> {path: dict} for every readable JSON table."""
    out = {}
    for f in sorted(glob.glob(os.path.join(TABLES, "*.json"))):
        try:
            d = json.load(open(f, encoding="utf8"))
        except Exception:
            continue
        if isinstance(d, dict):
            out[f] = d
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="fill the gaps; without it, only report")
    a = ap.parse_args()

    if not os.path.isdir(TABLES):
        sys.exit(f"no string tables at {TABLES} — install content first")

    tbl = tables()
    have = set()
    for d in tbl.values():
        have |= set(d)

    want = asked_for(os.path.join(
        MODULES, "**", "LF", "Base", "Qt", "Modules", "**", "*.qml"))
    missing = {k: v for k, v in want.items() if k not in have}

    print(f"tables: {len(tbl)} files, {len(have)} keys")
    print(f"asked for: {len(want)} keys across the Qt modules")
    print(f"missing:   {len(missing)}")
    if not missing:
        return 0

    # Group by the module that asks, so the report says who is affected.
    by_mod = {}
    for k, mods in missing.items():
        for m in mods:
            by_mod.setdefault(m, []).append(k)
    for m in sorted(by_mod, key=lambda m: -len(by_mod[m]))[:12]:
        print(f"  {len(by_mod[m]):4d}  {m}")

    if not a.write:
        print("\n(--write fills these in)")
        return 0

    # WHICH FILE TO WRITE INTO MATTERS. AssetManager loads a table by name, so
    # a new file of our own might never be read. Add to the table that already
    # holds this widget's keys — matched on the key prefix, which is how the
    # existing files are organised — and fall back to a Tadpole file only for
    # prefixes that have no table at all.
    added = 0
    for path, d in tbl.items():
        prefixes = {k.split("__")[0] for k in d}
        add = {k: KNOWN.get(k, spell(k))
               for k in missing if k.split("__")[0] in prefixes}
        if not add:
            continue
        d.update(add)
        d["_Tadpole_added"] = (
            "Keys below this point were supplied by Tadpole because this "
            "firmware asks for them and the installed src_json revision does "
            "not define them. See tools/fill-missing-text.py.")
        with open(path, "w", encoding="utf8") as f:
            json.dump(d, f, indent=1, ensure_ascii=False, sort_keys=True)
        print(f"  +{len(add):4d} -> {os.path.basename(path)}")
        added += len(add)

    print(f"filled {added} of {len(missing)}")
    if added < len(missing):
        print("the rest have no table with a matching prefix and are left "
              "alone — adding a file AssetManager never reads would only "
              "look like a fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
