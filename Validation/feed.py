#!/usr/bin/env python3
#
# Play a recording into the pedal's USB audio input, on a loop.
#
# The pedal is a USB audio device in both directions and its 'USB L/R In'
# setting decides what happens to what arrives: Pre-FX adds it to the
# analog input ahead of the signal chain, which is the one that makes the
# board process a recording as though it had been played into the jack.
#
# Two reasons to want that.  The obvious one is that a fixed passage
# going round and round is the only way to hear what a knob does - move
# it in the web app and the difference is the knob, because nothing else
# changed.  A guitar cannot do that; you cannot play the same bar twice.
#
# The other is compare-cabs.py's blind spot.  That script runs the
# pedal's own DSP on this machine, which is the same source compiled by a
# different compiler for a different CPU - so it is a claim about the
# hardware rather than a measurement of it.  --verify closes that: play a
# passage in, capture what comes out, and null it against what the host
# bench predicts for the identical settings.
#
# Pre-FX *adds*, it does not replace - Audio/effect.h:618 is
# 'in.left += usb_in.left'.  So a plugged-in guitar, or just the noise an
# open jack picks up, sums into the recording.  For listening that is
# usually fine; for --verify it is the floor the null cannot go below,
# and the script measures it rather than assuming it.
#
import argparse
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio
import bench as B
import pedal
import pots as P

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))

# SysEx pot numbers: 0 is the mix, so an effect's own pots start at 1.
CAB_CABINET, CAB_DRIVE, CAB_RESONANCE, CAB_AXIS = 1, 2, 3, 4
ROWS = ["Small-Combo", "American-1x12", "British-4x12",
        "Modern-4x12", "Bass-15"]


def stereo_s32(x):
    """Mono float to the interleaved 32-bit stereo aplay wants."""
    q = np.clip(x, -1.0, 1.0) * (2 ** 31 - 1)
    return np.repeat(q.astype("<i4"), 2).tobytes()


def player(card, blob, repeats):
    """aplay on the raw device, fed a whole number of passes.

    One invocation rather than one per repeat: aplay reopens the device
    between runs, and that is a gap and a click at every wrap.
    """
    p = subprocess.Popen(
        ["aplay", "-D", f"hw:{card},0", "-f", "S32_LE", "-c", "2",
         "-r", str(RATE), "-t", "raw", "-q", "-"],
        stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        n = 0
        while repeats is None or n < repeats:
            p.stdin.write(blob)
            p.stdin.flush()
            n += 1
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        try:
            p.stdin.close()
        except (BrokenPipeError, OSError):
            pass
    err = p.stderr.read().decode().strip()
    p.wait()
    if p.returncode and err:
        print(f"feed: aplay said: {err}", file=sys.stderr)


def align(a, b, max_lag):
    """How far b is behind a, in samples, by cross-correlation.

    The USB endpoint is asynchronous, so the pedal runs on its own clock
    and this is not a constant over a long capture - which is why the
    caller measures it at both ends of the take rather than once.
    """
    n = 1
    while n < len(a) + max_lag:
        n *= 2
    c = np.fft.irfft(np.fft.rfft(b, n) * np.conj(np.fft.rfft(a, n)), n)
    c = np.concatenate([c[-max_lag:], c[:max_lag]])
    return int(c.argmax()) - max_lag


def capture_busy(card):
    """Whether something already owns the capture stream, and what.

    Worth naming rather than letting arecord say "Device or resource
    busy", because the usual cause is the sound server holding it for a
    loopback to the speakers - which is a thing somebody set up on
    purpose and would rather be told about than have taken away.
    """
    try:
        text = open(f"/proc/asound/card{card}/stream0").read()
    except OSError:
        return False
    tail = text.split("Capture:", 1)[-1]
    if "Status: Running" not in tail.split("Playback:")[0]:
        return False
    try:
        out = subprocess.run(["fuser", "-v", f"/dev/snd/pcmC{card}D0c"],
                             capture_output=True, text=True).stderr
        who = sorted({ln.split()[-1] for ln in out.splitlines()[1:] if ln.split()})
        return ", ".join(who) or True
    except OSError:
        return True


def verify(args, card, p, dry):
    """Play a passage in, capture it back, and null it against the bench.

    Everything the pedal is doing is set from here rather than read back,
    because a null against settings that were merely assumed is a null
    against nothing.
    """
    cab = pedal.effect_id("CAB")
    settings = pedal.settings_effect()
    if cab is None or settings is None:
        sys.exit("feed: no Cabinet in the built map - is build/ current?")

    row = ROWS.index(args.row)
    raw = {k: P.to_pot("Cabinet", k, v) for k, v in
           (("Drive", args.drive), ("Resonance", args.resonance),
            ("Axis", args.axis))}
    gate = P.to_pot("Signal Chain", "Gate", -100.0)

    pedal.set_pot(p, settings, pedal.SETTINGS_USB_IN, pedal.USB_IN_PRE_FX)
    pedal.set_pot(p, settings, pedal.SETTINGS_USB_OUT, pedal.USB_OUT_WET)
    pedal.set_pot(p, pedal.CHAIN, pedal.CHAIN_GATE, gate)
    pedal.set_routing(p, cab)
    pedal.set_pot(p, cab, 0, 120)                       # mix, fully wet
    pedal.set_pot(p, cab, CAB_CABINET, row)
    pedal.set_pot(p, cab, CAB_DRIVE, raw["Drive"])
    pedal.set_pot(p, cab, CAB_RESONANCE, raw["Resonance"])
    pedal.set_pot(p, cab, CAB_AXIS, raw["Axis"])

    busy = capture_busy(card)
    if busy:
        sys.exit(f"feed: card {card}'s capture stream is already running"
                 f"{' - held by ' + busy if busy is not True else ''}.\n"
                 f"      arecord wants the raw device to itself, so stop the "
                 f"loopback first.\n"
                 f"      (Playback is a separate stream, which is why the "
                 f"plain loop works anyway.)")

    print("this replaces the pedal's live routing and pot values; the scene")
    print("on flash is untouched, so a program change puts it back.")

    #
    # The floor first, with nothing being sent: whatever the analog input
    # is picking up goes through the same chain and comes back on the
    # same capture, and the null cannot beat it.  Reporting the null
    # without it would be reporting the room.
    #
    print("measuring the floor with nothing playing...")
    floor = audio.capture(2.0, card)[:, 0]
    floor_db = 20 * np.log10(audio.rms(floor) + 1e-30)

    blob = stereo_s32(dry)
    seconds = len(dry) / RATE

    got = audio.capture(seconds + 1.0, card,
                        during=lambda: player(card, blob, 1))[:, 0]

    want, _, info = B.run(bench_args(args, row), dry.astype(np.float32),
                          warmup=B.settle())
    want = np.asarray(want, dtype=np.float64)[-len(dry):]

    #
    # Align on the first second and again on the last, because an async
    # endpoint means the two clocks are not the same clock and the offset
    # at the end is not the offset at the start.
    #
    win = RATE
    lag = align(want[:win], got, len(got) - win)
    if lag < 0 or lag + len(want) > len(got):
        sys.exit(f"feed: could not find the passage in the capture "
                 f"(lag {lag}, capture {len(got)}, passage {len(want)})")
    y = got[lag:lag + len(want)]

    #
    # And again at the far end, over a small search window centred on
    # where the start alignment says the tail should be.  The difference
    # is the drift.
    #
    #
    # The window is offset by 'slack' so that a tail arriving *early* is
    # reachable, which means the search has to be twice as wide as the
    # offset or only late tails can ever be found.  The first version was
    # 'slack' wide and reported zero drift as -1 sample and +60 as -543,
    # because the answer was one past the end of what it looked at.
    #
    slack = 400
    tail = lag + len(want) - win
    seg = got[max(0, tail - slack):tail + win + slack]
    late = align(want[-win:], seg, 2 * slack) - slack
    if abs(late) >= slack:
        late_note = f"{late:+d} - at the edge of the search, treat as unknown"
    else:
        late_note = f"{late:+d} samples"

    g = float(np.dot(y, want) / max(np.dot(want, want), 1e-30))
    resid = y - g * want
    null = 20 * np.log10(audio.rms(resid) / max(audio.rms(want), 1e-30) + 1e-30)

    print()
    print(f"  latency          {lag} samples, {1000.0*lag/RATE:.2f} ms")
    print(f"  drift over {seconds:.0f}s   {late_note}")
    print(f"  level            {20*np.log10(abs(g)+1e-30):+.2f} dB")
    print(f"  analog floor    {floor_db:7.1f} dBFS")
    print(f"  null            {null:7.1f} dB below the signal")
    print(f"  bench clipped    {info.get('clipped', 0):.0f}")
    print()
    print("The two are the same source compiled twice, so what is left is")
    print("the converters, the analog input summing in, and float32 landing")
    print("differently on two instruction sets.")


def bench_args(args, row):
    a = ["--pot", "Signal Chain:Gate=0",
         "--route", "Cabinet", "--mix", "Cabinet=120",
         "--pot", f"Cabinet:Cabinet={row}"]
    a += P.arg("Cabinet", "Drive", args.drive)
    a += P.arg("Cabinet", "Resonance", args.resonance)
    a += P.arg("Cabinet", "Axis", args.axis)
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.path.join(HERE, "Inputs",
                                                     "Dry-Guitar.wav"))
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--offset", default="auto",
                    help="where in the recording to start, in seconds; "
                         "'auto' skips the quiet lead-in")
    ap.add_argument("--peak", type=float, default=0.35,
                    help="input peak in the pedal's own scale - the USB "
                         "input lands ahead of Trim, so this is what "
                         "decides whether Drive reads the same as it "
                         "does on the bench")
    ap.add_argument("--repeat", default="forever",
                    help="how many times round, or 'forever'")
    ap.add_argument("--pedal", default="", help="which board, by serial")
    ap.add_argument("--force", action="store_true",
                    help="write pots even if the board is running a "
                         "different build than this tree")
    ap.add_argument("--no-set", action="store_true",
                    help="leave the USB input mode alone")
    ap.add_argument("--verify", action="store_true",
                    help="play it in, capture it back, and null it "
                         "against the host bench")
    ap.add_argument("--row", default="Modern-4x12", choices=ROWS)
    ap.add_argument("--drive", type=float, default=15.0)
    ap.add_argument("--resonance", type=float, default=4.0)
    ap.add_argument("--axis", type=float, default=0.5)
    args = ap.parse_args()

    found = pedal.discover()
    if not found:
        sys.exit("feed: no pedal found")
    d = pedal.find(args.pedal, found) if args.pedal else found[0]
    if not d:
        sys.exit(f"feed: '{args.pedal}' is not exactly one of these boards")
    card, port, key = d["card"], d["port"], d["label"]
    if port is None:
        sys.exit(f"feed: {key} has no MIDI port - nothing can be set on it")

    #
    # Refuse to write a pot to a board that is not running this tree.
    #
    # Every effect index in here comes out of build/effect_map.h, and
    # adding or removing an effect renumbers everything after it.  A pot
    # write to the wrong effect sets a real pot on a real effect and says
    # nothing - see issue 285, which is that mistake found by accident
    # after it had been shipping for a while.
    #
    if not args.force:
        want = pedal.elf_build()
        got = (pedal.identity(port) or {}).get("build")
        if want and got and want != got:
            sys.exit(f"feed: the board is running {got!r} and this tree "
                     f"built {want!r}.\n"
                     f"      Effect numbering may have moved under it, and a "
                     f"pot write would land\n"
                     f"      somewhere silently wrong.  'make flash', or "
                     f"--force if you know better.")
        if want and not got:
            print("feed: could not read the board's build stamp - is the "
                  "web app holding the port?", file=sys.stderr)

    off = None if args.offset == "auto" else float(args.offset)
    dry, off = audio.decode(args.source, args.seconds, off)
    dry = dry / max(np.abs(dry).max(), 1e-9) * args.peak
    print(f"pedal {key}, card {card}, port {port}")
    print(f"source: {os.path.basename(args.source)} "
          f"{off:.1f}..{off + len(dry)/RATE:.1f}s"
          f"{' (found)' if args.offset == 'auto' else ''}, peak {args.peak}")

    if args.verify:
        verify(args, card, port, dry)
        return 0

    if not args.no_set:
        settings = pedal.settings_effect()
        if settings is None:
            sys.exit("feed: no settings effect in the built map")
        pedal.set_pot(port, settings, pedal.SETTINGS_USB_IN,
                      pedal.USB_IN_PRE_FX)
        print("USB L/R In set to Pre-FX (it adds to the jack, it does not "
              "replace it)")

    repeats = None if args.repeat == "forever" else int(args.repeat)
    print(f"playing {'forever' if repeats is None else repeats} - ctrl-C to stop")
    try:
        player(card, stereo_s32(dry), repeats)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
