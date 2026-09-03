#!/usr/bin/env python3
#
# Listening to [CAB], for ears rather than for the FFT.
#
# It writes one wav per setting and a page that switches between them
# without losing the playback position, which is the only way a small
# tonal difference is audible as a difference rather than as a restart.
#
# Level matching is the whole reason this is a script and not a handful
# of commands.  The rows do not have the same output level, they do not
# have the same level *as a function of input* either, and louder wins
# every uncontrolled A/B ever run.  Everything below is matched to the
# same RMS over the same passage before it is written out, and the gain
# that took is printed so it is visible rather than hidden.
#
# It began as the head-to-head that chose between the biquad cab sim and
# this one, and the level matching is what it learned there: the first
# comparison it ran was decided entirely by one of them being 3 dB up.
#
# The source defaults to Inputs/BassForLinus.mp3, which is a DI
# recording - no cab on it already - so it is a fair thing to put a cab
# sim in front of.  It is also a bass, which is not what most of this is
# voiced for; pass --source for something better.
#
import argparse
import base64
import os
import random
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench as B
import pots as P

FS = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
GATE = ["--pot", "Signal Chain:Gate=0"]


def decode(path, seconds, offset):
    """ffmpeg to mono float32 at 48k - the same trick analyse-compressor uses.

    'offset' of None means find the music, which is what the default is.
    """
    cmd = ["ffmpeg", "-v", "error"]
    if offset is not None:
        cmd += ["-ss", str(offset), "-t", str(seconds)]
    cmd += ["-i", path, "-ac", "1", "-ar", str(FS), "-f", "f32le", "-"]
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    x = np.frombuffer(out, dtype=np.float32).astype(np.float64)
    if offset is not None:
        return x, offset

    start = find_onset(x)
    end = min(start + int(seconds * FS), len(x))
    return x[start:end], start / FS


def find_onset(x, margin_db=30.0, hold=0.1, lead=0.1):
    """Where the playing starts, in samples.

    A recording of an instrument usually opens with the room, the hum and
    somebody picking the thing up, and none of that is a fair thing to
    put a cab sim in front of: it is matched to the same RMS as every
    other take, so a passage that is mostly noise floor comes back as
    loud noise floor.  Inputs/Dry-Guitar.wav does not start playing until
    13.6 s, and this script's old hardcoded 8 s landed squarely in it.

    The threshold is relative to the *loudest* 20 ms of the recording
    rather than absolute, so it does not care how hot the file is.  It
    has to hold for 'hold' seconds, or one click of a lead touching a
    jack picks the start; and it backs off by 'lead' so the first note
    keeps its attack.  Measured on the two recordings in the tree,
    anything from peak-20 dB to peak-40 dB picks the same moment to
    within a fifth of a second, so the exact margin is not delicate.
    """
    b = FS // 50                                   # 20 ms
    n = len(x) // b
    if n < 2:
        return 0
    e = np.sqrt((x[:n * b].reshape(n, b) ** 2).mean(axis=1))
    db = 20 * np.log10(e + 1e-12)
    loud = db > db.max() - margin_db

    run = max(int(hold * 50), 1)
    for i in range(len(loud) - run):
        if loud[i:i + run].all():
            return max(0, int((i / 50.0 - lead) * FS))
    return 0


ROWS = ["Small-Combo", "American-1x12", "British-4x12",
        "Modern-4x12", "Bass-15"]


def cab(row, **over):
    """One cabinet row at its defaults, except for what is named."""
    a = ["--route", "Cabinet", "--mix", "Cabinet=120",
         "--pot", f"Cabinet:Cabinet={ROWS.index(row)}"]
    for k, val in over.items():
        a += P.arg("Cabinet", k, val)
    return GATE + a


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
#bar{height:5px;background:#222831;border-radius:3px;margin:1.2rem 0 .4rem;overflow:hidden}
#pos{height:100%;width:0;background:#0072b2}
small{color:#6f7986}kbd{background:#222831;border:1px solid #39414d;border-radius:4px;padding:0 .35rem}
</style></head><body>
<h1>__TITLE__</h1>
<p>Switching keeps the playback position, so the change is audible as a change
rather than as a restart. Number keys pick a take; <kbd>space</kbd> plays and pauses.</p>
<div id="btns"></div>
<div id="bar"><div id="pos"></div></div>
<small id="now"></small>
<script>
const TAKES = __TAKES__;
const els = TAKES.map(t => { const a = new Audio(t.src); a.preload='auto'; a.loop=true; return a; });
let cur = 0, playing = false;
const btns = document.getElementById('btns');
TAKES.forEach((t,i) => {
  const b = document.createElement('button');
  b.textContent = (i+1) + '. ' + t.name;
  b.onclick = () => pick(i);
  btns.appendChild(b);
});
function paint(){
  [...btns.children].forEach((b,i) => b.className = i===cur ? 'on' : '');
  document.getElementById('now').textContent = TAKES[cur].note || '';
}
function pick(i){
  if (i === cur) return;
  const t = els[cur].currentTime;
  els[cur].pause();
  cur = i;
  els[cur].currentTime = t;
  if (playing) els[cur].play();
  paint();
}
function toggle(){
  playing = !playing;
  if (playing) els[cur].play(); else els[cur].pause();
}
document.addEventListener('keydown', e => {
  if (e.code === 'Space'){ e.preventDefault(); toggle(); return; }
  const n = parseInt(e.key, 10);
  if (n >= 1 && n <= TAKES.length) pick(n-1);
});
setInterval(() => {
  const a = els[cur];
  if (a.duration) document.getElementById('pos').style.width =
      (100*a.currentTime/a.duration) + '%';
}, 80);
paint();
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--offset", default="auto",
                    help="where in the recording to start, in seconds; "
                         "'auto' skips the quiet lead-in")
    ap.add_argument("--peak", type=float, default=0.35,
                    help="input peak, in the pedal's own scale")
    ap.add_argument("--out", default="cab-shootout",
                    help="prefix for the wavs and the page")
    ap.add_argument("--blind", action="store_true",
                    help="shuffle the labels and print the key at the end")
    ap.add_argument("--source", default=os.path.join(HERE, "Inputs",
                                                     "BassForLinus.mp3"))
    ap.add_argument("--pre", metavar="EFFECT", default=None,
                    help="route a drive in front of the cab - a cab sim's "
                         "stated job is taming the fizz off a distorted amp, "
                         "and a clean signal never asks it to")
    ap.add_argument("--pre-pot", action="append", default=[],
                    metavar="POT=VAL", help="a raw 0..120 pot on --pre")
    ap.add_argument("--trim", type=float, default=None,
                    help="[CHAIN] Trim in dB, for a guitar that is not hot")
    ap.add_argument("--row", default=None, choices=ROWS,
                    help="with --ladder, which cabinet to walk Drive on")
    ap.add_argument("--drive", type=float, default=15.0,
                    help="Drive for the row comparison, in dB")
    ap.add_argument("--ladder", action="store_true",
                    help="one row at five Drive settings, which is where "
                         "it stops being a filter and starts breaking up")
    args = ap.parse_args()

    #
    # The drive goes in front of every take including the dry one, so
    # that what is being compared is still only the cab.
    #
    if args.trim is not None:
        GATE.extend(P.arg("Signal Chain", "Trim", args.trim))

    pre = []
    if args.pre:
        pre = ["--route", args.pre, "--mix", f"{args.pre}=120"]
        for kv in args.pre_pot:
            pre += ["--pot", f"{args.pre}:{kv}"]

    off = None if args.offset == "auto" else float(args.offset)
    dry, off = decode(args.source, args.seconds, off)
    dry = dry / max(np.abs(dry).max(), 1e-9) * args.peak
    print(f"source: {os.path.basename(args.source)} "
          f"{off:.1f}..{off + len(dry) / FS:.1f}s"
          f"{' (found)' if args.offset == 'auto' else ''}, "
          f"peak {args.peak}, {len(dry)} samples")

    #
    # What is actually in the source, per octave, relative to its
    # loudest octave.  A cab sim's differences above 5kHz do not matter
    # on material that has nothing there, and which of those is true is
    # a measurement rather than an opinion.
    #
    spec = np.abs(np.fft.rfft(dry * np.hanning(len(dry)))) ** 2
    freq = np.fft.rfftfreq(len(dry), 1.0 / FS)
    edges = [20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480]
    band = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (freq >= lo) & (freq < hi)
        band.append(10 * np.log10(spec[m].sum() + 1e-30))
    top = max(band)
    print("source content, dB re its loudest octave:")
    print("  " + " ".join(f"{lo:>7}" for lo in edges[:-1]))
    print("  " + " ".join(f"{v - top:7.1f}" for v in band))

    if args.ladder:
        row = args.row or "Modern-4x12"
        takes = [("nocab", "no cab at all", GATE + pre)]
        for db in (0.0, 10.0, 18.0, 24.0, 30.0):
            takes.append((f"drive{int(db)}", f"{row}, Drive {db:+.0f} dB",
                          GATE + pre + cab(row, Drive=db)[len(GATE):]))
    else:
        #
        # Every row at the same Drive.  They are all at the same
        # small-signal level by construction - norm() divides the
        # sensitivity out - so what is left is the voicing and how early
        # each one lets go, which is the whole of what a row is.
        #
        takes = [("nocab", "no cab at all", GATE + pre)]
        for row in ROWS:
            takes.append((row.lower(), f"{row}, Drive {args.drive:+.0f} dB",
                          GATE + pre + cab(row, Drive=args.drive)[len(GATE):]))

    ref = rms(dry)
    rows = []
    for key, name, argv in takes:
        y, _, info = B.run(argv, dry.astype(np.float32), warmup=B.settle())
        y = np.asarray(y, dtype=np.float64)[-len(dry):]
        raw = rms(y)
        g = ref / max(raw, 1e-12)
        y = y * g
        pk = float(np.abs(y).max())
        if pk > 0.99:                      # matched loudness can still clip a wav
            y = y * (0.99 / pk)
        rows.append((key, name, y, 20 * np.log10(g), pk, info))

    order = list(range(len(rows)))
    if args.blind:
        random.shuffle(order)

    print()
    print(f"{'take':14} {'match gain':>11} {'peak after':>11} {'clipped':>8}")
    manifest = []
    for slot, idx in enumerate(order):
        key, name, y, gdb, pk, info = rows[idx]
        path = f"{args.out}-{'take%d' % (slot + 1) if args.blind else key}.wav"
        write_wav(path, y)
        label = f"Take {slot + 1}" if args.blind else name
        manifest.append({"name": label, "src": os.path.basename(path),
                         "note": "" if args.blind else name})
        print(f"{key:14} {gdb:+10.2f} dB {pk:11.3f} {info.get('clipped', 0):8.0f}")

    page = PAGE.replace("__TITLE__", "Cabinet - one source, several speakers") \
               .replace("__TAKES__", repr(manifest).replace("'", '"'))
    html = f"{args.out}.html"
    open(html, "w").write(page)
    print(f"\npage: {os.path.abspath(html)}")
    if args.blind:
        print("\nkey (look after listening):")
        for slot, idx in enumerate(order):
            print(f"  Take {slot + 1}  =  {rows[idx][1]}")


if __name__ == "__main__":
    main()
