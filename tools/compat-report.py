#!/usr/bin/env python3
"""Turn a compat-sweep run into a single self-contained HTML page.

    ./tools/compat-report.py [build/compat/<date>]      -> that run's index.html

Reads results.tsv and the per-title screenshots, and writes index.html next to
them with every thumbnail embedded, so the file can be moved or sent on its own.

WHY THE CLUSTERS GET TOP BILLING. A compatibility list that is only a list
invites the wrong conclusion — "28 games are broken" — when the fault sites say
something far more useful: 19 of those 28 abort in the same place. One bug, not
nineteen. The table is the evidence; the cluster panel is the finding.
"""
import base64
import html
import io
import os
import sys
from collections import Counter, OrderedDict

try:
    from PIL import Image
except ImportError:
    Image = None

THUMB = (256, 145)          # half the panel, near enough; 480x272 is 16:9-ish

VERDICTS = OrderedDict([
    ("ok",      ("Launches",  "Drew a real screen and stayed up")),
    ("partial", ("Partial",   "Drew something, then went blank")),
    ("blank",   ("Blank",     "Ran without crashing, drew nothing")),
    ("crash",   ("Crash",     "Died before it drew anything")),
])


def read_tsv(path):
    rows = []
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
        im.save(buf, "JPEG", quality=78, optimize=True)
        return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()
    except Exception:
        return None


def short_site(site):
    if not site:
        return ""
    return site.replace("libuClibc-0.9.32.1-git.so", "libuClibc") \
               .replace("-0.9.32.1-git.so", "").replace(".so.6.0.14", "")


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
  --shadow:0 1px 2px rgba(0,0,0,.4),0 4px 16px rgba(0,0,0,.3);
}
/* MONOSPACE CARRIES THE PAGE. Every label, count and fault address is set in
   it; prose is the exception. The subject is a framebuffer measured in bytes
   and offsets, and an instrument readout is what that world looks like. */
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

.readout{display:grid;grid-template-columns:minmax(220px,auto) 1fr;gap:28px;
         align-items:center;margin:30px 0 8px}
@media (max-width:720px){.readout{grid-template-columns:1fr}}
.score{font-family:var(--mono);font-variant-numeric:tabular-nums;line-height:1}
.score b{font-size:3.6rem;font-weight:700;letter-spacing:-.02em}
.score .of{font-size:1.5rem;color:var(--faint)}
.score .cap{display:block;margin-top:8px;font-size:.74rem;color:var(--muted);
            text-transform:uppercase;letter-spacing:.11em}
.bar{display:flex;height:34px;border-radius:3px;overflow:hidden;
     border:1px solid var(--line)}
.bar span{display:block}
.bar .s-ok{background:var(--ok)} .bar .s-crash{background:var(--crash)}
.bar .s-blank{background:var(--blank)} .bar .s-partial{background:var(--partial)}
.legend{display:flex;flex-wrap:wrap;gap:16px;margin-top:12px;
        font-family:var(--mono);font-size:.78rem}
.legend i{display:inline-block;width:10px;height:10px;border-radius:2px;
          margin-right:6px;vertical-align:baseline}
.legend b{font-variant-numeric:tabular-nums}
.legend span{color:var(--muted)}

.note{margin:26px 0 0;padding:14px 16px;border-left:3px solid var(--blank);
      background:var(--surface);border-radius:0 4px 4px 0;box-shadow:var(--shadow)}
.note h3{margin:0 0 4px;font-family:var(--mono);font-size:.78rem;
         text-transform:uppercase;letter-spacing:.1em;color:var(--blank)}
.note p{margin:0;color:var(--muted);max-width:68ch}

h2{font-family:var(--mono);font-size:.82rem;text-transform:uppercase;
   letter-spacing:.13em;color:var(--muted);margin:44px 0 14px;
   padding-bottom:8px;border-bottom:1px solid var(--line)}

.clusters{display:grid;gap:12px;
          grid-template-columns:repeat(auto-fit,minmax(250px,1fr))}
.cl{background:var(--surface);border:1px solid var(--line);border-radius:5px;
    padding:14px 16px;box-shadow:var(--shadow)}
.cl .n{font-family:var(--mono);font-size:1.9rem;font-weight:700;line-height:1;
       font-variant-numeric:tabular-nums}
.cl .site{font-family:var(--mono);font-size:.8rem;margin-top:6px;
          word-break:break-all}
.cl .why{font-size:.86rem;color:var(--muted);margin-top:8px}

.filters{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 16px}
.filters button{font-family:var(--mono);font-size:.78rem;letter-spacing:.06em;
  text-transform:uppercase;padding:7px 13px;border-radius:3px;cursor:pointer;
  background:var(--surface);color:var(--muted);border:1px solid var(--line)}
.filters button:hover{color:var(--ink);border-color:var(--faint)}
.filters button[aria-pressed="true"]{background:var(--ink);color:var(--bg);
  border-color:var(--ink)}
.filters button:focus-visible{outline:2px solid var(--ink);outline-offset:2px}

.tablewrap{overflow-x:auto;border:1px solid var(--line);border-radius:5px;
           background:var(--surface);box-shadow:var(--shadow)}
table{border-collapse:collapse;width:100%;min-width:760px}
th{font-family:var(--mono);font-size:.72rem;text-transform:uppercase;
   letter-spacing:.1em;color:var(--muted);text-align:left;font-weight:600;
   padding:11px 14px;border-bottom:1px solid var(--line);
   background:var(--surface-2);position:sticky;top:0}
td{padding:10px 14px;border-bottom:1px solid var(--line);vertical-align:middle}
tr:last-child td{border-bottom:0}
tr.hide{display:none}
td.shot{width:148px;padding:8px 0 8px 14px}
td.shot img{display:block;width:132px;height:auto;border-radius:3px;
            border:1px solid var(--line);background:#000}
td.shot .noimg{width:132px;height:75px;border-radius:3px;border:1px dashed var(--line);
  display:grid;place-items:center;font-family:var(--mono);font-size:.66rem;
  color:var(--faint)}
.title{font-weight:600;text-wrap:balance}
.pkg{font-family:var(--mono);font-size:.7rem;color:var(--faint);
     margin-top:2px;word-break:break-all}
.kind{font-family:var(--mono);font-size:.72rem;color:var(--muted);
      text-transform:uppercase;letter-spacing:.07em}
.pill{display:inline-block;font-family:var(--mono);font-size:.72rem;
      font-weight:700;text-transform:uppercase;letter-spacing:.07em;
      padding:3px 9px;border-radius:3px;white-space:nowrap}
.p-ok{color:var(--ok);background:var(--ok-bg)}
.p-crash{color:var(--crash);background:var(--crash-bg)}
.p-blank{color:var(--blank);background:var(--blank-bg)}
.p-partial{color:var(--partial);background:var(--partial-bg)}
.detail{font-family:var(--mono);font-size:.74rem;color:var(--muted);
        max-width:40ch}
.detail .site{color:var(--ink)}
.detail .what{display:block;margin-top:3px;color:var(--crash)}
.metric{font-family:var(--mono);font-size:.74rem;color:var(--faint);
        font-variant-numeric:tabular-nums;white-space:nowrap}
footer{margin-top:44px;padding-top:16px;border-top:1px solid var(--line);
       font-family:var(--mono);font-size:.74rem;color:var(--faint)}
"""

JS = """
(function(){
  var btns=document.querySelectorAll('.filters button');
  var rows=document.querySelectorAll('tbody tr');
  btns.forEach(function(b){
    b.addEventListener('click',function(){
      var f=b.dataset.f;
      btns.forEach(function(o){o.setAttribute('aria-pressed',o===b?'true':'false');});
      rows.forEach(function(r){
        r.classList.toggle('hide', f!=='all' && r.dataset.v!==f);
      });
    });
  });
})();
"""


def main(argv):
    root = argv[0] if argv else None
    if not root:
        base = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "build", "compat")
        runs = sorted(os.listdir(base)) if os.path.isdir(base) else []
        if not runs:
            sys.stderr.write("no sweep found under build/compat/\n")
            return 1
        root = os.path.join(base, runs[-1])
    tsv = os.path.join(root, "results.tsv")
    rows = read_tsv(tsv)
    if not rows:
        sys.stderr.write("no rows in %s\n" % tsv)
        return 1

    counts = Counter(r["verdict"] for r in rows)
    total = len(rows)
    ok = counts.get("ok", 0)

    # Cluster the crashes by fault site — the finding, not the list.
    sites = Counter(short_site(r["site"]) for r in rows if r["verdict"] == "crash" and r["site"])
    whys = {}
    for r in rows:
        s = short_site(r["site"])
        if r["verdict"] == "crash" and s and s not in whys and r.get("note"):
            whys[s] = r["note"]

    out = []
    A = out.append
    A('<!doctype html><html lang="en"><head><meta charset="utf-8">')
    A('<meta name="viewport" content="width=device-width,initial-scale=1">')
    A("<title>Tadpole — LeapPad2 title compatibility</title>")
    A("<style>%s</style></head><body><div class='wrap'>" % CSS)

    A("<header class='mast'><div>")
    A("<h1>Title compatibility</h1>")
    A("<div class='sub'>Tadpole &middot; LeapPad2 &middot; firmware 4.6.0.784</div>")
    A("</div><div class='sub'>swept %s &middot; %d titles</div></header>"
      % (html.escape(os.path.basename(root).replace("-", " ")), total))

    A("<section class='readout'><div class='score'>")
    A("<b>%d</b><span class='of'>/%d</span>" % (ok, total))
    A("<span class='cap'>launched to a real screen</span></div><div>")
    A("<div class='bar'>")
    for v in VERDICTS:
        n = counts.get(v, 0)
        if n:
            A("<span class='s-%s' style='flex:%d' title='%s: %d'></span>" % (v, n, v, n))
    A("</div><div class='legend'>")
    for v, (label, _d) in VERDICTS.items():
        A("<div><i style='background:var(--%s)'></i><b>%d</b> <span>%s</span></div>"
          % (v, counts.get(v, 0), html.escape(label)))
    A("</div></div></section>")

    A("<div class='note'><h3>How these were launched</h3><p>Every title was "
      "started directly into its own binary, with no home screen and no player "
      "signed in. That is what makes a hundred launches practical, and it is a "
      "caveat: a title that expects profile or save data may fail here and be "
      "fine when launched normally. A crash below means <em>crashed this way</em>, "
      "not <em>broken</em>, until it has been checked both ways.</p></div>")

    if sites:
        A("<h2>Where the failures cluster</h2>")
        A("<div class='clusters'>")
        for site, n in sites.most_common(6):
            A("<div class='cl'><div class='n'>%d</div>" % n)
            A("<div class='site'>%s</div>" % html.escape(site))
            w = whys.get(site, "")
            if "what():" in w:
                w = w.split("what():", 1)[1].strip()
                A("<div class='why'>uncaught exception &mdash; %s</div>" % html.escape(w[:110]))
            elif n > 1:
                A("<div class='why'>same offset in %d titles &mdash; one bug, "
                  "not %d</div>" % (n, n))
            A("</div>")
        A("</div>")

    A("<h2>All titles</h2>")
    A("<div class='filters'>")
    A("<button data-f='all' aria-pressed='true'>All %d</button>" % total)
    for v, (label, _d) in VERDICTS.items():
        if counts.get(v):
            A("<button data-f='%s' aria-pressed='false'>%s %d</button>"
              % (v, html.escape(label), counts[v]))
    A("</div>")

    A("<div class='tablewrap'><table><thead><tr>")
    A("<th>Screen</th><th>Title</th><th>Type</th><th>Result</th>"
      "<th>Detail</th><th>Drawn</th></tr></thead><tbody>")

    order = {v: i for i, v in enumerate(VERDICTS)}
    rows.sort(key=lambda r: (order.get(r["verdict"], 9), r["name"].lower()))

    for r in rows:
        v = r["verdict"]
        d = os.path.join(root, "shots", r["pkg"])
        shot = None
        for cand in ("late.png", "early.png"):
            p = os.path.join(d, cand)
            if os.path.isfile(p):
                shot = thumb_data_uri(p)
                if shot:
                    break
        A("<tr data-v='%s'>" % v)
        if shot:
            A("<td class='shot'><img src='%s' alt='%s at 35 seconds' loading='lazy'></td>"
              % (shot, html.escape(r["name"])))
        else:
            A("<td class='shot'><div class='noimg'>no capture</div></td>")
        A("<td><div class='title'>%s</div><div class='pkg'>%s</div></td>"
          % (html.escape(r["name"] or "(unnamed)"), html.escape(r["pkg"])))
        A("<td class='kind'>%s</td>" % html.escape(r["kind"]))
        A("<td><span class='pill p-%s'>%s</span></td>" % (v, VERDICTS[v][0]))

        A("<td class='detail'>")
        if v == "crash":
            A("<span class='site'>%s</span>" % html.escape(short_site(r["site"]) or "?"))
            if r["signal"]:
                A(" %s" % html.escape(r["signal"]))
            if r["alive"]:
                A(" &middot; after %ss" % html.escape(r["alive"]))
            note = r.get("note", "")
            if "what():" in note:
                A("<span class='what'>%s</span>"
                  % html.escape(note.split("what():", 1)[1].strip()[:90]))
        elif v == "blank":
            A("no crash, nothing on screen")
        elif v == "partial":
            A("drew early, blank at 35s")
        else:
            A("&mdash;")
        A("</td>")
        A("<td class='metric'>%s%% lit<br>%s colours</td>"
          % (html.escape(r["late_lit"] or "0"), html.escape(r["late_col"] or "0")))
        A("</tr>")

    A("</tbody></table></div>")
    A("<footer>Screens captured 35&nbsp;s after launch (12&nbsp;s for titles that "
      "crashed first). &ldquo;Lit&rdquo; is the share of pixels that are not "
      "near-black; &ldquo;colours&rdquo; counts distinct 15-bit values &mdash; "
      "together they separate a drawn screen from a flat fill.</footer>")
    A("</div><script>%s</script></body></html>" % JS)

    dest = os.path.join(root, "index.html")
    with open(dest, "w") as fh:
        fh.write("\n".join(out))
    print("%s  (%.1f MB)" % (dest, os.path.getsize(dest) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
