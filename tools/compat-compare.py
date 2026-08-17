#!/usr/bin/env python3
"""Two compatibility sweeps, one page: how close is Glasspole to Tadpole?

    ./tools/compat-compare.py --a build/compat/<qemu-run> \\
                              --b build/compat/<glasspole-run>

Writes index.html into the B run, self-contained — every screenshot embedded,
every log tail inline, no network and no sibling files needed to read it.

WHY A COMPARISON AND NOT TWO REPORTS. "Glasspole launches 61 titles" means
nothing on its own; the question is always "out of how many that WORK". qemu
is the reference implementation of this workload — decades old, and the thing
every one of these titles was already known to run under — so the honest
score is a ratio against it, measured on the same 110 titles, the same
firmware, the same day, with one variable changed.

THE TRANSITION MATRIX IS THE FINDING. A verdict split tells you how many
broke; the matrix tells you WHICH WAY each title moved, and that is where the
work is. A title that crashes under both is a title qemu never ran either —
not Glasspole's bug, and counting it as one would make the emulator look
worse than it is while hiding the regressions that matter. The cell that
earns attention is ok -> crash.

Sweeps are produced by tools/compat-swarm.sh; see COMPAT_EMU there for how
the two runs are told apart.
"""
import argparse
import base64
import html
import io
import os
import re
import sys
from collections import Counter, OrderedDict

try:
    from PIL import Image
except ImportError:
    Image = None

THUMB = (256, 145)
LOG_TAIL = 50               # lines, as asked for: enough to see the last act

VERDICTS = OrderedDict([
    ("ok",      ("Launches",  "Drew a real screen and stayed up")),
    ("partial", ("Partial",   "Drew something, then went blank")),
    ("blank",   ("Blank",     "Ran without crashing, drew nothing")),
    ("crash",   ("Crash",     "Died before it drew anything")),
])

# Every glasspole diagnostic worth clustering on, and what each one MEANS.
# The label is the point: "GUEST FAULT" counted 19 times is a number, whereas
# "unmapped memory" counted 19 times is a bug report.
FAULT_KINDS = [
    (re.compile(r"HOST FAULT"),
     "Emulator bug", "Touched memory outside the guest's address space entirely."),
    (re.compile(r"GUEST FAULT"),
     "Unmapped memory", "The guest read or wrote an address that was never mapped."),
    (re.compile(r"could not translate"),
     "Unimplemented instruction", "dynarmic has no encoding for an ARM instruction here."),
    (re.compile(r"exception \d+ at pc"),
     "CPU exception", "The core refused the instruction at that address."),
    (re.compile(r"could not load"),
     "Never started", "The ELF loader could not bring the program up at all."),
    (re.compile(r"clone without CLONE_THREAD"),
     "fork()", "Process creation, which Glasspole does not implement."),
    # THE BIG ONE, and it hid inside "blank" until the logs were read. uClibc's
    # abort() raises SIGABRT with tkill/tgkill and, when the raise does not
    # kill it, falls through to _exit(127). Glasspole implements neither call,
    # so the guest aborts, survives its own abort, and exits quietly — no
    # signal, no crash.log, no diagnosis. Under qemu the same titles raise
    # SIGABRT, the shim catches it, and you get a backtrace.
    (re.compile(r"guest exited with status 127|unimplemented syscall (238|268)\b"),
     "abort() with no signal",
     "The title called abort(). tkill/tgkill are unimplemented, so no signal "
     "was delivered and it exited 127 instead of crashing — which is why these "
     "look blank rather than broken."),
    (re.compile(r"unimplemented syscall"),
     "Missing syscall", "Glasspole has no implementation for a call the title makes."),
    (re.compile(r"WATCHDOG"),
     "Spinning", "200M instructions with no syscall — a lock that never comes free."),
]

# The lines of a fault report, which are the register dump and belong with it.
FAULT_CONT = re.compile(r"^\s+(thread |block pc|pc=|r\d)")


def read_tsv(path):
    rows = []
    if not os.path.isfile(path):
        return rows
    with open(path, "r", errors="replace") as fh:
        head = fh.readline().rstrip("\n").split("\t")
        for line in fh:
            if not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            f += [""] * (len(head) - len(f))
            rows.append(dict(zip(head, f)))
    return rows


def thumb_data_uri(path):
    """Downscale to a data: URI. 46 MB of full-size PNGs will not fit in a page."""
    if not path or not os.path.isfile(path) or Image is None:
        return None
    try:
        im = Image.open(path).convert("RGB")
        # NEAREST, not a smooth filter: this is a 480x272 panel of pixel art and
        # UI text, and bilinear turns small glyphs into grey mush — exactly the
        # detail someone squints at a thumbnail to check.
        im = im.resize(THUMB, Image.NEAREST)
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=74, optimize=True)
        return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()
    except Exception:
        return None


def shot_for(root, pkg):
    """The LATE shot, falling back to the early one.

    Late first because it is the settled picture; early only when the title
    died before the late sample, in which case it is the last thing that was
    ever on screen and the most useful frame there is.
    """
    for which in ("late.png", "early.png"):
        p = os.path.join(root, "shots", pkg, which)
        if os.path.isfile(p):
            return p
    return None


def log_tail(root, pkg, n=LOG_TAIL):
    p = os.path.join(root, "shots", pkg, "run.log")
    if not os.path.isfile(p):
        return ""
    try:
        with open(p, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return ""
    return "".join(lines[-n:]).rstrip()


def dump_for(root, pkg):
    """The stack dump, from whichever of the two emulators produced one.

    qemu runs give the shim's crash.log — a real backtrace, because the guest
    caught its own SIGSEGV. Glasspole gives a register dump in the run log
    instead, because it reports faults rather than delivering guest signals.
    Different shapes, same job, so both are collected here and the page does
    not have to care which emulator it is describing.
    """
    out = []
    crash = os.path.join(root, "shots", pkg, "crash.log")
    if os.path.isfile(crash):
        try:
            with open(crash, "r", errors="replace") as fh:
                t = fh.read().strip()
            if t:
                out.append(t)
        except OSError:
            pass

    run = os.path.join(root, "shots", pkg, "run.log")
    if os.path.isfile(run):
        try:
            with open(run, "r", errors="replace") as fh:
                lines = fh.read().splitlines()
        except OSError:
            lines = []
        # BY PRIORITY, then by position — same reasoning as the sweep's
        # emu_fault. The first fault in the file is usually not the one that
        # killed the title, and a dump of the wrong fault is worse than none:
        # it sends you to the wrong address with full confidence.
        for rx, _label, _why in FAULT_KINDS:
            hits = [i for i, line in enumerate(lines) if rx.search(line)]
            if not hits:
                continue
            block = []
            # Up to four occurrences: abort() prints its tkill and its tgkill
            # and then the exit status, and all three together are the story.
            for i in hits[:4]:
                block.append(lines[i])
                for cont in lines[i + 1:i + 24]:
                    if FAULT_CONT.match(cont):
                        block.append(cont)
                    else:
                        break
            out.append("\n".join(block))
            break
    return "\n\n".join(out).strip()


def fault_kind(text):
    for rx, label, why in FAULT_KINDS:
        if text and rx.search(text):
            return label, why
    return None, None


CSS = """
*,*::before,*::after{box-sizing:border-box}
/* LIGHT IS THE BASE SET. Every colour the page uses is named here; the two
   blocks below redefine only these tokens. Nothing downstream may declare a
   colour of its own, or it would apply in one theme and not the other. */
:root{
  --bg:#f2f4f6; --surface:#ffffff; --surface-2:#e9edf1;
  --ink:#171b1f; --muted:#5c6771; --faint:#8d99a3; --line:#d8dee4;
  --ok:#2e7d4f; --ok-bg:#e2f0e7;
  --crash:#b8342b; --crash-bg:#f8e3e1;
  --blank:#9a6b12; --blank-bg:#f6ecd8;
  --partial:#39628f; --partial-bg:#e3ebf5;
  --glass:#2f7ea8; --glass-bg:#e0eff6;
  --shadow:0 1px 2px rgba(20,26,32,.07),0 4px 14px rgba(20,26,32,.05);
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme="light"]){
    --bg:#0e1114; --surface:#161a1e; --surface-2:#1e242a;
    --ink:#e7ecf0; --muted:#98a4ae; --faint:#6c7883; --line:#262d34;
    --ok:#63c98d; --ok-bg:#173225;
    --crash:#f08a80; --crash-bg:#341c1a;
    --blank:#e0b258; --blank-bg:#332715;
    --partial:#8fb6e6; --partial-bg:#18242f;
    --glass:#71c0e2; --glass-bg:#152c38;
    --shadow:0 1px 2px rgba(0,0,0,.4),0 4px 16px rgba(0,0,0,.3);
  }
}
:root[data-theme="dark"]{
  --bg:#0e1114; --surface:#161a1e; --surface-2:#1e242a;
  --ink:#e7ecf0; --muted:#98a4ae; --faint:#6c7883; --line:#262d34;
  --ok:#63c98d; --ok-bg:#173225;
  --crash:#f08a80; --crash-bg:#341c1a;
  --blank:#e0b258; --blank-bg:#332715;
  --partial:#8fb6e6; --partial-bg:#18242f;
  --glass:#71c0e2; --glass-bg:#152c38;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 4px 16px rgba(0,0,0,.3);
}
:root{
  --mono:ui-monospace,"DejaVu Sans Mono","Liberation Mono","Menlo",monospace;
  --sans:system-ui,-apple-system,"DejaVu Sans","Liberation Sans",sans-serif;
}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);font-family:var(--sans);
     font-size:15px;line-height:1.55}
.wrap{max-width:1180px;margin:0 auto;padding:32px 22px 80px}

.mast{display:flex;flex-wrap:wrap;gap:16px;align-items:baseline;
      justify-content:space-between;border-bottom:2px solid var(--ink);
      padding-bottom:14px}
.mast h1{font-family:var(--mono);font-size:1.45rem;letter-spacing:.02em;
         margin:0;font-weight:700;text-wrap:balance}
.mast .sub{font-family:var(--mono);font-size:.76rem;color:var(--muted);
           text-transform:uppercase;letter-spacing:.11em}

.hero{display:grid;grid-template-columns:minmax(200px,auto) 1fr;gap:32px;
      align-items:center;margin:34px 0 6px}
@media (max-width:760px){.hero{grid-template-columns:1fr}}
.score{font-family:var(--mono);font-variant-numeric:tabular-nums;line-height:1}
.score b{font-size:4.2rem;font-weight:700;letter-spacing:-.03em;color:var(--glass)}
.score .pc{font-size:1.8rem;color:var(--glass)}
.score .cap{display:block;margin-top:10px;font-size:.74rem;color:var(--muted);
            text-transform:uppercase;letter-spacing:.11em;max-width:26ch;
            line-height:1.5}
.hero p{margin:0;color:var(--muted);max-width:60ch}
.hero p b{color:var(--ink);font-family:var(--mono)}

.rows{display:grid;gap:14px;margin:26px 0 0}
.erow{display:grid;grid-template-columns:120px 1fr;gap:14px;align-items:center}
@media (max-width:560px){.erow{grid-template-columns:1fr}}
.erow .who{font-family:var(--mono);font-size:.8rem;font-weight:700;
           text-transform:uppercase;letter-spacing:.08em}
.erow .who small{display:block;font-weight:400;color:var(--faint);
                 letter-spacing:.04em;text-transform:none;font-size:.72rem}
.bar{display:flex;height:32px;border-radius:3px;overflow:hidden;
     border:1px solid var(--line)}
.bar span{display:block;min-width:0}
.bar .s-ok{background:var(--ok)} .bar .s-crash{background:var(--crash)}
.bar .s-blank{background:var(--blank)} .bar .s-partial{background:var(--partial)}
.legend{display:flex;flex-wrap:wrap;gap:16px;margin-top:12px;
        font-family:var(--mono);font-size:.78rem}
.legend i{display:inline-block;width:10px;height:10px;border-radius:2px;
          margin-right:6px}
.legend b{font-variant-numeric:tabular-nums}
.legend span{color:var(--muted)}

h2{font-family:var(--mono);font-size:.82rem;text-transform:uppercase;
   letter-spacing:.13em;color:var(--muted);margin:46px 0 14px;
   padding-bottom:8px;border-bottom:1px solid var(--line)}
.lede{margin:-4px 0 18px;color:var(--muted);max-width:70ch}

.matrix{overflow-x:auto;border:1px solid var(--line);border-radius:5px;
        background:var(--surface);box-shadow:var(--shadow)}
.matrix table{border-collapse:collapse;width:100%;min-width:520px}
.matrix th,.matrix td{padding:10px 14px;text-align:center;
                      border-bottom:1px solid var(--line);font-family:var(--mono);
                      font-variant-numeric:tabular-nums}
.matrix th{font-size:.72rem;text-transform:uppercase;letter-spacing:.1em;
           color:var(--muted);background:var(--surface-2);font-weight:600}
.matrix td:first-child,.matrix th:first-child{text-align:left}
.matrix tr:last-child td{border-bottom:0}
.matrix .zero{color:var(--faint)}
.matrix .keep{color:var(--ok);font-weight:700}
.matrix .lost{color:var(--crash);font-weight:700}
.matrix .gain{color:var(--glass);font-weight:700}

.clusters{display:grid;gap:12px;
          grid-template-columns:repeat(auto-fit,minmax(250px,1fr))}
.cl{background:var(--surface);border:1px solid var(--line);border-radius:5px;
    padding:14px 16px;box-shadow:var(--shadow)}
.cl .n{font-family:var(--mono);font-size:1.9rem;font-weight:700;line-height:1;
       font-variant-numeric:tabular-nums;color:var(--crash)}
.cl .site{font-family:var(--mono);font-size:.82rem;margin-top:6px;font-weight:700}
.cl .why{font-size:.86rem;color:var(--muted);margin-top:8px}

.filters{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 16px}
.filters button{font-family:var(--mono);font-size:.78rem;letter-spacing:.06em;
  text-transform:uppercase;padding:7px 13px;border-radius:3px;cursor:pointer;
  background:var(--surface);color:var(--muted);border:1px solid var(--line)}
.filters button:hover{color:var(--ink);border-color:var(--faint)}
.filters button[aria-pressed="true"]{background:var(--ink);color:var(--bg);
  border-color:var(--ink)}
.filters button:focus-visible{outline:2px solid var(--ink);outline-offset:2px}

.card{background:var(--surface);border:1px solid var(--line);border-radius:5px;
      box-shadow:var(--shadow);margin-bottom:12px;overflow:hidden}
.card.hide{display:none}
.chead{display:grid;grid-template-columns:1fr auto;gap:14px;align-items:start;
       padding:14px 16px}
.chead .title{font-weight:600;text-wrap:balance}
.chead .pkg{font-family:var(--mono);font-size:.7rem;color:var(--faint);
            margin-top:2px;word-break:break-all}
.chead .kind{font-family:var(--mono);font-size:.72rem;color:var(--muted);
             text-transform:uppercase;letter-spacing:.07em;margin-top:4px}
.verdicts{display:flex;gap:8px;align-items:center;white-space:nowrap}
.verdicts .arrow{color:var(--faint);font-family:var(--mono)}
.pill{display:inline-block;font-family:var(--mono);font-size:.72rem;
      font-weight:700;text-transform:uppercase;letter-spacing:.07em;
      padding:3px 9px;border-radius:3px;white-space:nowrap}
.p-ok{color:var(--ok);background:var(--ok-bg)}
.p-crash{color:var(--crash);background:var(--crash-bg)}
.p-blank{color:var(--blank);background:var(--blank-bg)}
.p-partial{color:var(--partial);background:var(--partial-bg)}
.p-none{color:var(--faint);background:var(--surface-2)}

.shots{display:grid;grid-template-columns:1fr 1fr;gap:1px;background:var(--line);
       border-top:1px solid var(--line);border-bottom:1px solid var(--line)}
@media (max-width:560px){.shots{grid-template-columns:1fr}}
.shots figure{margin:0;background:var(--surface);padding:12px 16px 14px}
.shots figcaption{font-family:var(--mono);font-size:.7rem;color:var(--muted);
                  text-transform:uppercase;letter-spacing:.09em;margin-bottom:8px}
.shots img{display:block;width:100%;max-width:256px;height:auto;border-radius:3px;
           border:1px solid var(--line);background:#000}
.shots .noimg{width:100%;max-width:256px;aspect-ratio:256/145;border-radius:3px;
  border:1px dashed var(--line);display:grid;place-items:center;
  font-family:var(--mono);font-size:.7rem;color:var(--faint)}
.shots .stat{font-family:var(--mono);font-size:.7rem;color:var(--faint);
             margin-top:6px;font-variant-numeric:tabular-nums}

details{border-top:1px solid var(--line)}
summary{cursor:pointer;padding:10px 16px;font-family:var(--mono);font-size:.74rem;
        text-transform:uppercase;letter-spacing:.09em;color:var(--muted);
        user-select:none}
summary:hover{color:var(--ink)}
summary::marker{color:var(--faint)}
details pre{margin:0;padding:12px 16px 16px;background:var(--surface-2);
            font-family:var(--mono);font-size:.72rem;line-height:1.45;
            overflow-x:auto;white-space:pre;color:var(--ink)}
details pre.dump{color:var(--crash)}
footer{margin-top:44px;padding-top:16px;border-top:1px solid var(--line);
       font-family:var(--mono);font-size:.74rem;color:var(--faint);max-width:74ch}
footer p{margin:0 0 8px}
"""

JS = """
(function(){
  var btns=document.querySelectorAll('.filters button');
  var cards=document.querySelectorAll('.card');
  btns.forEach(function(b){
    b.addEventListener('click',function(){
      var f=b.dataset.f;
      btns.forEach(function(o){o.setAttribute('aria-pressed',o===b?'true':'false');});
      cards.forEach(function(c){
        var show = f==='all' ? true
                 : f==='regress' ? c.dataset.regress==='1'
                 : f==='gain'    ? c.dataset.gain==='1'
                 : c.dataset.b===f;
        c.classList.toggle('hide', !show);
      });
    });
  });
})();
"""


def pill(v):
    if not v:
        return "<span class='pill p-none'>absent</span>"
    return "<span class='pill p-%s'>%s</span>" % (v, html.escape(v))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="reference sweep (qemu)")
    ap.add_argument("--b", required=True, help="sweep under test (glasspole)")
    ap.add_argument("--a-label", default="Tadpole (qemu-arm)")
    ap.add_argument("--b-label", default="Glasspole")
    ap.add_argument("-o", "--out", default=None)
    # A page body with no document around it, for hosts that supply their own
    # <head>. Publishing the standalone file to one of those nests a whole
    # second <html> inside the first, which browsers "fix" by discarding the
    # inner <head> — taking the stylesheet with it, so the report arrives as
    # unstyled text and nothing says why.
    ap.add_argument("--fragment", action="store_true",
                    help="emit the body only, no doctype/html/head/body")
    args = ap.parse_args(argv[1:])

    A_rows = {r["pkg"]: r for r in read_tsv(os.path.join(args.a, "results.tsv"))}
    B_rows = {r["pkg"]: r for r in read_tsv(os.path.join(args.b, "results.tsv"))}
    if not A_rows or not B_rows:
        sys.stderr.write("need results.tsv in both runs\n")
        return 1

    pkgs = sorted(set(A_rows) | set(B_rows),
                  key=lambda p: (B_rows.get(p) or A_rows[p]).get("name", "").lower())

    ca = Counter(r["verdict"] for r in A_rows.values())
    cb = Counter(r["verdict"] for r in B_rows.values())
    ok_a, ok_b = ca.get("ok", 0), cb.get("ok", 0)
    pct = (100.0 * ok_b / ok_a) if ok_a else 0.0

    # The transition matrix, and the two numbers that come out of it.
    matrix = Counter()
    for p in pkgs:
        va = A_rows.get(p, {}).get("verdict", "")
        vb = B_rows.get(p, {}).get("verdict", "")
        matrix[(va, vb)] += 1
    regressed = [p for p in pkgs
                 if A_rows.get(p, {}).get("verdict") == "ok"
                 and B_rows.get(p, {}).get("verdict") not in ("ok",)]
    gained = [p for p in pkgs
              if B_rows.get(p, {}).get("verdict") == "ok"
              and A_rows.get(p, {}).get("verdict") not in ("ok",)]

    # Cluster the regressions by what Glasspole said, not by title.
    clusters = Counter()
    cluster_why = {}
    for p in pkgs:
        b = B_rows.get(p, {})
        if b.get("verdict") == "ok":
            continue
        label, why = fault_kind(b.get("emufault") or b.get("site") or "")
        if not label:
            label, why = "No diagnostic", ("Ran, drew nothing, and said nothing "
                                           "about why — the hardest kind to chase.")
        clusters[label] += 1
        cluster_why[label] = why

    out = []
    W = out.append
    if not args.fragment:
        W('<!doctype html><html lang="en"><head><meta charset="utf-8">')
        W('<meta name="viewport" content="width=device-width,initial-scale=1">')
        W("<title>Glasspole vs Tadpole — title compatibility</title>")
        W("</head><body>")
    W("<style>%s</style>" % CSS)
    W("<div class='wrap'>")

    W("<header class='mast'><div>")
    W("<h1>Glasspole vs Tadpole</h1>")
    W("<div class='sub'>LeapPad2 &middot; firmware 4.6.0.784 &middot; %d titles</div>"
      % len(pkgs))
    W("</div><div class='sub'>%s</div></header>"
      % html.escape(os.path.basename(args.b)))

    # ---- the headline ------------------------------------------------------
    W("<section class='hero'><div class='score'>")
    W("<b>%.0f</b><span class='pc'>%%</span>" % pct)
    W("<span class='cap'>as compatible as Tadpole</span></div><div>")
    W("<p><b>%s</b> launches <b>%d</b> of these %d titles to a real screen. "
      "<b>%s</b> launches <b>%d</b> &mdash; %.0f%% of what the reference "
      "manages, on the same firmware, the same titles and the same day.</p>"
      % (html.escape(args.a_label), ok_a, len(pkgs),
         html.escape(args.b_label), ok_b, pct))
    W("</div></section>")

    W("<div class='rows'>")
    for label, counts, sub in ((args.a_label, ca, "the reference"),
                               (args.b_label, cb, "under test")):
        tot = sum(counts.values()) or 1
        W("<div class='erow'><div class='who'>%s<small>%s</small></div><div>"
          % (html.escape(label), html.escape(sub)))
        W("<div class='bar'>")
        for v in VERDICTS:
            n = counts.get(v, 0)
            if n:
                W("<span class='s-%s' style='flex:%d' title='%s: %d'></span>"
                  % (v, n, v, n))
        W("</div><div class='legend'>")
        for v, (lab, _d) in VERDICTS.items():
            W("<div><i style='background:var(--%s)'></i><b>%d</b> <span>%s</span></div>"
              % (v, counts.get(v, 0), html.escape(lab)))
        W("</div></div></div>")
    W("</div>")

    # ---- transition matrix -------------------------------------------------
    W("<h2>What moved</h2>")
    W("<p class='lede'>Rows are how a title did under %s; columns are how the "
      "same title did under %s. The diagonal is agreement. The cell that "
      "matters is <b>ok &rarr; crash</b>: titles that worked and now do not. "
      "A title that crashes in both columns is not a Glasspole bug &mdash; "
      "it never ran here to begin with.</p>"
      % (html.escape(args.a_label), html.escape(args.b_label)))
    W("<div class='matrix'><table><thead><tr><th>%s &darr; / %s &rarr;</th>"
      % (html.escape(args.a_label), html.escape(args.b_label)))
    for v in VERDICTS:
        W("<th>%s</th>" % v)
    W("<th>total</th></tr></thead><tbody>")
    for va in VERDICTS:
        row_total = sum(matrix[(va, vb)] for vb in VERDICTS)
        if not row_total:
            continue
        W("<tr><td><b>%s</b></td>" % va)
        for vb in VERDICTS:
            n = matrix[(va, vb)]
            cls = "zero" if not n else ("keep" if va == vb == "ok"
                                        else "lost" if va == "ok"
                                        else "gain" if vb == "ok" else "")
            W("<td class='%s'>%s</td>" % (cls, n if n else "&middot;"))
        W("<td>%d</td></tr>" % row_total)
    W("</tbody></table></div>")

    W("<p class='lede' style='margin-top:14px'><b>%d</b> title%s regressed "
      "(ran under %s, does not under %s). <b>%d</b> went the other way.</p>"
      % (len(regressed), "" if len(regressed) == 1 else "s",
         html.escape(args.a_label), html.escape(args.b_label), len(gained)))

    # ---- clusters ----------------------------------------------------------
    W("<h2>Why the failures fail</h2>")
    W("<p class='lede'>Every title that did not reach a screen under %s, "
      "grouped by what the emulator itself reported. This is the actual work "
      "queue: these are not %d separate bugs.</p>"
      % (html.escape(args.b_label), sum(clusters.values())))
    W("<div class='clusters'>")
    for label, n in clusters.most_common():
        W("<div class='cl'><div class='n'>%d</div>" % n)
        W("<div class='site'>%s</div>" % html.escape(label))
        W("<div class='why'>%s</div></div>" % html.escape(cluster_why.get(label, "")))
    W("</div>")

    # ---- the titles --------------------------------------------------------
    W("<h2>Every title</h2>")
    W("<p class='lede'>Both framebuffers, side by side, with the last %d lines "
      "of the Glasspole log and its fault dump under each. The screenshot is "
      "the late sample where there was one, and the early sample where the "
      "title died before it.</p>" % LOG_TAIL)
    W("<div class='filters'>")
    W("<button data-f='all' aria-pressed='true'>All %d</button>" % len(pkgs))
    W("<button data-f='regress'>Regressed %d</button>" % len(regressed))
    if gained:
        W("<button data-f='gain'>Only on %s %d</button>"
          % (html.escape(args.b_label), len(gained)))
    for v, (lab, _d) in VERDICTS.items():
        if cb.get(v):
            W("<button data-f='%s'>%s %d</button>" % (v, html.escape(lab), cb[v]))
    W("</div>")

    for p in pkgs:
        ra, rb = A_rows.get(p, {}), B_rows.get(p, {})
        name = rb.get("name") or ra.get("name") or p
        kind = rb.get("kind") or ra.get("kind") or ""
        va, vb = ra.get("verdict", ""), rb.get("verdict", "")
        is_reg = va == "ok" and vb != "ok"
        is_gain = vb == "ok" and va != "ok"

        W("<article class='card' data-b='%s' data-regress='%d' data-gain='%d'>"
          % (html.escape(vb or "none"), 1 if is_reg else 0, 1 if is_gain else 0))
        W("<div class='chead'><div>")
        W("<div class='title'>%s</div>" % html.escape(name))
        W("<div class='pkg'>%s</div>" % html.escape(p))
        W("<div class='kind'>%s</div></div>" % html.escape(kind))
        W("<div class='verdicts'>%s<span class='arrow'>&rarr;</span>%s</div>"
          % (pill(va), pill(vb)))
        W("</div>")

        W("<div class='shots'>")
        for label, root, row in ((args.a_label, args.a, ra),
                                 (args.b_label, args.b, rb)):
            W("<figure><figcaption>%s</figcaption>" % html.escape(label))
            uri = thumb_data_uri(shot_for(root, p))
            if uri:
                W("<img src='%s' alt='%s under %s' loading='lazy'>"
                  % (uri, html.escape(name), html.escape(label)))
            else:
                W("<div class='noimg'>no frame</div>")
            if row:
                W("<div class='stat'>lit %s%% &middot; %s colours</div>"
                  % (html.escape(row.get("late_lit", "0")),
                     html.escape(row.get("late_col", "0"))))
            W("</figure>")
        W("</div>")

        dump = dump_for(args.b, p)
        if dump:
            W("<details><summary>Fault dump</summary><pre class='dump'>%s</pre></details>"
              % html.escape(dump))
        tail = log_tail(args.b, p)
        if tail:
            W("<details><summary>Last %d log lines &mdash; %s</summary><pre>%s</pre></details>"
              % (LOG_TAIL, html.escape(args.b_label), html.escape(tail)))
        W("</article>")

    W("<footer>")
    W("<p>Every title was started straight into its own binary, with no home "
      "screen and no player signed in. That is what makes a hundred launches "
      "practical, and it is a caveat: a title that wants profile or save data "
      "may fail here and be fine launched normally.</p>")
    W("<p>Screens are sampled twice, at 12 s and 35 s, and a title that still "
      "looks empty at the deadline is given another round before being called "
      "blank &mdash; a loaded machine slows the guest without slowing the "
      "clock. &ldquo;Drew a real screen&rdquo; means at least 3%% of pixels "
      "lit across at least 64 distinct colours, which a flat fill cannot "
      "reach.</p>")
    W("<p>a: %s &middot; b: %s</p>"
      % (html.escape(args.a), html.escape(args.b)))
    W("</footer>")
    W("</div><script>%s</script>" % JS)
    if not args.fragment:
        W("</body></html>")

    dest = args.out or os.path.join(args.b, "index.html")
    with open(dest, "w") as fh:
        fh.write("\n".join(out))
    size = os.path.getsize(dest)
    print("%s  (%.1f MB)" % (dest, size / 1048576.0))
    print("  %s: %d ok of %d" % (args.a_label, ok_a, len(pkgs)))
    print("  %s: %d ok of %d  -> %.0f%% as compatible"
          % (args.b_label, ok_b, len(pkgs), pct))
    print("  regressed %d, gained %d" % (len(regressed), len(gained)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
