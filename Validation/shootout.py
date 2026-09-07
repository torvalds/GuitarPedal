#!/usr/bin/env python3
#
# Listening to two versions of the same thing, side by side.
#
# Matching the levels, writing the wavs, and building a page that
# switches between them without losing the playback position.  Nothing
# in here knows what is being compared: it takes named takes and a
# title, so the same page serves any question of the form "does this
# change sound different from that one".
#
# LEVEL MATCHING IS THE WHOLE REASON THIS IS CODE
#
# Takes do not come out at the same level, they do not have the same
# level as a function of input either, and louder wins every
# uncontrolled A/B ever run.  Everything is matched
# to the same RMS over the same passage before it is written out, and the
# gain that took is printed so it is visible rather than hidden.  The
# first comparison compare-cabs.py ever ran was decided entirely by one
# take being 3 dB up.
#
# BLIND IS WORTH IT
#
# Everyone knows which take is supposed to win.  --blind shuffles the
# labels and prints the key afterwards, so the answer arrives after the
# listening rather than during it.
#
import os
import random

import numpy as np

FS = 48000


def rms(x):
    return float(np.sqrt(np.mean(np.asarray(x, dtype=np.float64) ** 2)))


def write_wav(path, x):
    """RIFF by hand, because `import wave` does not get the standard
    library here - Validation/wave.py is the waveform viewer and it
    shadows the stdlib module for anything with this directory on its
    path, which is everything in here."""
    q = np.clip(np.asarray(x) * 32767.0, -32768, 32767).astype("<i2")
    d = q.tobytes()
    le = lambda v, n: int(v).to_bytes(n, "little")
    hdr = (b"RIFF" + le(36 + len(d), 4) + b"WAVEfmt " + le(16, 4)
           + le(1, 2) + le(1, 2) + le(FS, 4) + le(FS * 2, 4)
           + le(2, 2) + le(16, 2) + b"data" + le(len(d), 4))
    with open(path, "wb") as f:
        f.write(hdr + d)
    return len(hdr) + len(d)


PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>__TITLE__</title><style>
body{background:#14161a;color:#e6e6e6;font:15px/1.5 system-ui,sans-serif;
     margin:0;padding:2rem;max-width:46rem}
h1{font-size:1.2rem;margin:0 0 .3rem}p{color:#9aa4b2;margin:.2rem 0 1.4rem}
button{background:#222831;color:#e6e6e6;border:1px solid #39414d;border-radius:6px;
  padding:.7rem 1rem;font:inherit;cursor:pointer;margin:0 .4rem .4rem 0;min-width:9rem}
button.on{background:#0072b2;border-color:#0072b2;color:#fff}
button:disabled{opacity:.4;cursor:default}
#bar{height:5px;background:#222831;border-radius:3px;margin:1.2rem 0 .4rem;overflow:hidden}
#pos{height:100%;width:0;background:#0072b2}
small{color:#6f7986}kbd{background:#222831;border:1px solid #39414d;border-radius:4px;padding:0 .35rem}
</style></head><body>
<h1>__TITLE__</h1>
<p>Every take plays at once, sample-locked, and the buttons cross-fade between
them in 8 ms. There is no seek and no gap, so what you hear when you switch is
the difference and nothing else. Click a take or press its number to play and to
switch; <kbd>space</kbd> pauses.</p>
<div id="btns"></div>
<div id="bar"><div id="pos"></div></div>
<small id="now">loading...</small>
<script>
//
// Web Audio rather than <audio> elements, and the reason is the whole
// point of the page.  Switching an <audio> element means pausing one,
// seeking another to the same position and starting it - a seek that is
// not sample accurate, followed by a decode, followed by a gap of tens
// of milliseconds.  Against a difference this small that is louder than
// the thing being listened for.
//
// Here every take is decoded up front, all sources start together off
// one clock, and picking a take only moves gain.  The 8 ms ramp is to
// stop the switch itself clicking; it is short enough not to be a fade.
//
const TAKES = __TAKES__;
const ctx = new (window.AudioContext || window.webkitAudioContext)();
const btns = document.getElementById('btns');
const now = document.getElementById('now');
let bufs = [], gains = [], srcs = [], cur = 0, playing = false, t0 = 0, dur = 0;

//
// An AudioContext built before the page has been clicked is born
// suspended, and stays that way until something asks it not to be.
// Chrome's autoplay policy, and it is not optional or detectable up
// front: the constructor succeeds, decodeAudioData succeeds, the
// sources start, the clock does not run and nothing comes out.
//
// This page had no resume() at all, so it was silent for everyone who
// opened it by double-clicking - and audible under browser automation,
// which is launched with the policy disabled.  That is the worst
// possible split, because it means testing it the convenient way
// reports success.
//
function resume(){
  if (ctx.state !== 'running')
    ctx.resume().then(paint, () => {
      now.textContent = 'the browser will not start audio here';
    });
}

TAKES.forEach((t,i) => {
  const b = document.createElement('button');
  b.textContent = (i+1) + '. ' + t.name;
  b.disabled = true;
  b.onclick = () => pick(i);
  btns.appendChild(b);
});

Promise.all(TAKES.map(t =>
  fetch(t.src).then(r => r.arrayBuffer()).then(a => ctx.decodeAudioData(a))
)).then(bs => {
  bufs = bs;
  dur = bs[0].duration;
  [...btns.children].forEach(b => b.disabled = false);
  paint();
}).catch(e => { now.textContent = 'could not load: ' + e; });

function start(offset){
  srcs = bufs.map((b,i) => {
    const s = ctx.createBufferSource();
    const g = ctx.createGain();
    s.buffer = b; s.loop = true;
    g.gain.value = (i === cur) ? 1 : 0;
    s.connect(g).connect(ctx.destination);
    gains[i] = g;
    s.start(0, offset % b.duration);
    return s;
  });
  t0 = ctx.currentTime - offset;
}
function stop(){ srcs.forEach(s => { try { s.stop(); } catch(e){} }); srcs = []; }
function at(){ return playing ? (ctx.currentTime - t0) : 0; }

//
// Picking a take also starts the page, rather than only moving gain.
// Clicking a take is the first thing anyone does, and a version that
// highlights the button, prints no error and plays nothing leaves the
// space bar as the only way in - discoverable by reading the paragraph
// above, which is to say not discoverable.
//
function pick(i){
  if (!bufs.length) return;
  resume();
  const old = cur; cur = i;
  if (!playing){ playing = true; start(0); }
  else if (gains.length && old !== i){
    const t = ctx.currentTime, R = 0.008;
    gains[old].gain.setTargetAtTime(0, t, R/3);
    gains[cur].gain.setTargetAtTime(1, t, R/3);
  }
  paint();
}
function toggle(){
  if (!bufs.length) return;
  resume();
  if (playing){ stop(); playing = false; }
  else { playing = true; start(0); }
  paint();
}
function paint(){
  [...btns.children].forEach((b,i) => b.className = i===cur ? 'on' : '');
  now.textContent = !bufs.length ? 'loading...'
    : (TAKES[cur].note || '')
      + (playing && ctx.state === 'running' ? ''
         : '  (click a take, or space, to play)');
}
document.addEventListener('keydown', e => {
  if (e.code === 'Space'){ e.preventDefault(); toggle(); return; }
  const n = parseInt(e.key, 10);
  if (n >= 1 && n <= TAKES.length) pick(n-1);
});
setInterval(() => {
  if (playing && dur)
    document.getElementById('pos').style.width =
      (100*((at() % dur)/dur)) + '%';
}, 80);
</script></body></html>
"""


def publish(takes, out, title, ref, blind=False, extra=None, inline=True,
            match="rms"):
    """Match, write and lay out.

    'takes' is [(key, name, samples)], 'ref' the RMS everything is
    matched to - usually the dry passage's, so a take is compared
    against the source rather than against whichever take came first.
    'extra' is an optional {key: text} of things to print beside each.

    'match' is how the takes are levelled against each other.  "rms"
    scales each to the reference and is right for comparing tone, since
    nothing should win by being louder.  "common" applies one gain to
    all of them and is right when the *level* is the thing in question -
    a gain error is exactly what per-take matching is built to remove,
    so a shootout that always matches can never show one.  That is not
    hypothetical: this page compared a model carrying 11-17 dB of
    excess gain against one that does not, and the two sounded
    "fairly similar", because the matching had already fixed the defect
    before anyone heard it.

    'inline' embeds the audio in the page as data: URIs.  It is the
    default because the page is handed over as a path, and a path is
    opened as a file:// URL, and the loader here is fetch() - which
    file:// refuses on every current browser.  So the page written
    beside its four .wav files looked perfectly correct, said
    "could not load: TypeError: Failed to fetch", and needed a web
    server nobody was told to start.  data: URIs are fetchable, so
    this is one line at the loader's end and none at all in the
    JavaScript.

    The .wav files are still written: they are the artifact worth
    keeping, they can be dropped into anything else, and at four takes
    of twelve seconds the inline copy costs about 6 MB of page, which
    is a local file and not a download.  Pass inline=False for a
    comparison long or wide enough for that to stop being true, and
    serve the directory over HTTP.
    """
    import base64
    rows = []
    for key, name, y in takes:
        y = np.asarray(y, dtype=np.float64)
        g = ref / max(rms(y), 1e-12) if match == "rms" else 1.0
        rows.append([key, name, y * g, 20 * np.log10(g)])

    #
    # One gain across all of them, chosen so the loudest take peaks just
    # under full scale.
    #
    # It has to be common, and the per-take "if it clipped, turn that one
    # down" this replaces was not: it un-matched exactly the takes that
    # were loudest, which is the one thing this whole function exists to
    # prevent.  A common scale cannot change what is being compared, and
    # without it a drive matched to a dry passage lands 21 dB down and is
    # judged quiet - which is its own kind of unfair, since fizz is
    # easier to miss quietly.
    #
    peak = max(float(np.abs(y).max()) for _k, _n, y, _g in rows)
    norm = 0.89 / max(peak, 1e-12)
    for r in rows:
        r[2] = r[2] * norm
    print("common normalisation: %+.2f dB" % (20 * np.log10(norm)))

    order = list(range(len(rows)))
    if blind:
        random.shuffle(order)

    print()
    print("%-16s %11s %11s   %s" % ("take", "match gain", "peak after",
                                    "" if extra is None else "note"))
    manifest = []
    for slot, idx in enumerate(order):
        key, name, y, gdb = rows[idx]
        pk = float(np.abs(y).max())
        path = "%s-%s.wav" % (out, ("take%d" % (slot + 1)) if blind else key)
        write_wav(path, y)
        label = "Take %d" % (slot + 1) if blind else name
        if inline:
            src = "data:audio/wav;base64," + base64.b64encode(
                open(path, "rb").read()).decode("ascii")
        else:
            src = os.path.basename(path)
        manifest.append({"name": label, "src": src,
                         "note": "" if blind else name})
        print("%-16s %+10.2f dB %11.3f   %s"
              % (key, gdb, pk, "" if extra is None else extra.get(key, "")))

    page = (PAGE.replace("__TITLE__", title)
                .replace("__TAKES__", repr(manifest).replace("'", '"')))
    html = "%s.html" % out
    open(html, "w").write(page)
    print("\npage: %s%s" % (os.path.abspath(html),
                            "" if inline else "  (needs a web server: the"
                            " loader is fetch(), which file:// refuses)"))
    if blind:
        print("\nkey (look after listening):")
        for slot, idx in enumerate(order):
            print("  Take %d  =  %s" % (slot + 1, rows[idx][1]))
    return html
