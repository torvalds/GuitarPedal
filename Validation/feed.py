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
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio
import bench as B
import pedal
import pots as P

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))

#
# Thrown away at each end of the passage before anything is
# measured.  The first 250 ms is wrong in every run - the effect
# is still crossfading in over EFF_ENABLE_STEPS and the capture
# stream is still opening - and the ends do not correspond anyway,
# the bench starting from a settled filter and the board from
# whatever it was doing.
#
EDGE = RATE // 3

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


def sub_sample(a, b, guard=4800):
    """Line b up with a to a fraction of a sample, by fitting the delay.

    Worth doing because the stakes are high and invisible: on this
    material a *purely* fractional offset of 0.05 samples, with nothing
    else wrong at all, leaves a residual 48.5 dB below the signal, and
    0.2 samples leaves 36.4.  A null taken on a sloppy alignment is a
    measurement of the alignment.

    On the USB path it comes out at exactly zero, every time, which is
    the answer rather than a disappointment: in over USB, through the
    DSP, out over USB is a digital path end to end and nobody resamples
    it.  That is worth having measured rather than assumed, and it stops
    being true the moment this is pointed at the analog loop.

    The delay goes on 'a' as a phase ramp, which is exact for a band-
    limited signal.  'guard' samples are dropped from each end because
    that ramp is circular and the wrap does not belong in the residual -
    and because the ends do not correspond anyway: the bench starts from
    a settled filter and the capture starts from a stream opening.
    Trimming them is what took the null from -52 dB to the noise floor,
    which was briefly and wrongly credited to this function.
    """
    n = len(a)
    A = np.fft.rfft(a)
    k = np.arange(len(A))
    c = slice(guard, n - guard)
    bc = b[c]
    bb = float(np.dot(bc, bc))

    def resid(d):
        aa = np.fft.irfft(A * np.exp(-2j * np.pi * k * d / n), n)[c]
        g = float(np.dot(aa, bc) / max(np.dot(aa, aa), 1e-30))
        r = bc - g * aa
        return float(np.dot(r, r) / max(bb, 1e-30)), g

    best = min((resid(d)[0], d) for d in np.arange(-0.6, 0.6, 0.02))[1]
    best = min((resid(d)[0], d)
               for d in np.arange(best - 0.02, best + 0.02, 0.0005))[1]
    _, g = resid(best)
    aa = np.fft.irfft(A * np.exp(-2j * np.pi * k * best / n), n)
    return best, g, aa


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

    #
    # Put both effects where the bench starts from, pot by pot, and only
    # then change the ones under test.
    #
    # Setting just the interesting pots is what the first version did,
    # and it measured this machine's Signal Chain defaults against
    # whatever Trim and Volume the last session had left on the board.
    # Trim lands *ahead* of the nonlinearity, so that is not a level
    # error to be divided out afterwards - it is a different signal - and
    # it read as a 9.3 dB gain difference and a null of only -25 dB.
    #
    def pots_of(effect_id, effect_name, override=()):
        order = P.labels(effect_name)
        want = dict(P.defaults(effect_name))
        want.update(override)
        return [(0x03, effect_id, order.index(lab) + 1, raw)
                for lab, raw in want.items()]

    #
    # One invocation rather than nineteen.  send() falls back to
    # aplaymidi when the raw device is taken - which it is whenever the
    # web app is open - and that costs about two seconds a message.
    #
    pedal.send_many(
        p,
        (0x03, settings, pedal.SETTINGS_USB_IN, pedal.USB_IN_PRE_FX),
        (0x03, settings, pedal.SETTINGS_USB_OUT, pedal.USB_OUT_WET),
        *pots_of(pedal.CHAIN, "Signal Chain", {"Gate": 0}),
        (0x08, cab),                                    # routing: the cab alone
        (0x03, cab, 0, 120),                            # mix, fully wet
        *pots_of(cab, "Cabinet", {
            "Cabinet": row,
            "Drive": P.to_pot("Cabinet", "Drive", args.drive),
            "Resonance": P.to_pot("Cabinet", "Resonance", args.resonance),
            "Axis": P.to_pot("Cabinet", "Axis", args.axis),
        }),
    )

    #
    # And then wait, because routing an effect in crossfades it over
    # EFF_ENABLE_STEPS - a tenth of a second - and a passage that starts
    # inside that fade is not the passage the bench computed.
    #
    # This was learned by deleting it.  An earlier version measured the
    # noise floor in a separate capture first, which happened to give the
    # fade all the time it needed; folding the floor into the main
    # capture removed the delay along with it and the null went from -60
    # dB to -26, which reads exactly like a broken effect.
    #
    time.sleep(0.5)

    blob = stereo_s32(dry)
    seconds = len(dry) / RATE

    got = audio.capture(seconds + 2.0, card,
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
    # The floor comes out of this same capture, after the passage has
    # finished, rather than out of a capture of its own.
    #
    # Because it is not stationary.  An instrument left plugged in picks
    # up whatever the room and the mains are doing at that moment, and
    # measuring it a few seconds earlier gave a null that moved 5.6 dB
    # between two runs with nothing changed - which reads as the pedal
    # being inconsistent when it is the room being a room.  A quarter of
    # a second of guard, so the passage's own tail is not counted as
    # noise.
    #
    quiet = got[lag + len(want) + RATE // 4:]
    if len(quiet) < RATE // 10:
        sys.exit("feed: no quiet tail in the capture to measure the floor "
                 "against - ask for a shorter --seconds")
    floor = quiet
    floor_db = 20 * np.log10(audio.rms(floor) + 1e-30)

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

    #
    # Null block by block, each aligned on its own.
    #
    # Because the endpoint is asynchronous: the host sends at its clock
    # and the board consumes at its own, and when the two have drifted a
    # whole sample apart something has to give one back.  Everything
    # before that instant lines up and everything after it is off by one,
    # which as a single whole-passage null reads as the pedal being
    # wrong.  Measured per 250 ms, a run with a slip in it is -45 dB for
    # four seconds and then -21 dB for the rest; a run without one holds
    # -60 throughout.
    #
    # So a slip is not noise to be averaged away or a fault to be fixed -
    # it is the interface working as designed - and the honest summary is
    # the typical block plus a count of how many times it happened.
    #
    blk = RATE // 2
    nb = (len(want) - 2 * EDGE) // blk
    if nb < 2:
        sys.exit("feed: passage too short to block up - raise --seconds")

    lags, nulls, gains = [], [], []
    for i in range(nb):
        o = EDGE + i * blk
        a, b = want[o:o + blk], y[o:o + blk]
        fine = align(a, b, 8)
        b2 = y[o + fine:o + fine + blk]
        if len(b2) < blk:
            break
        frac, g, aligned = sub_sample(a, b2, guard=blk // 8)
        c = slice(blk // 8, blk - blk // 8)
        r = b2[c] - g * aligned[c]
        lags.append(fine)
        gains.append(g)
        nulls.append(20 * np.log10(audio.rms(r) / max(audio.rms(a[c]), 1e-30)
                                   + 1e-30))

    nulls = np.array(nulls)
    slips = int(np.count_nonzero(np.diff(lags)))
    null = float(np.median(nulls))
    g = float(np.median(gains))
    sig = max(audio.rms(want[EDGE:EDGE + nb * blk]), 1e-30)
    resid_rms = sig * 10 ** (null / 20.0)
    excess = resid_rms ** 2 - audio.rms(floor) ** 2
    beyond = (10 * np.log10(max(excess, 1e-30) / sig ** 2)
              if excess > 0 else None)

    #
    # The floor in the same units as the null, because that is the
    # comparison worth making and dBFS is not: what matters is not how
    # quiet the room is, it is how quiet it is *relative to the passage*,
    # since that is the level the residual is measured against.
    #
    # Pre-FX adds to the input jack, so an instrument left plugged in
    # contributes here even when nobody is playing it.
    #
    floor_rel = 20 * np.log10(audio.rms(floor) / sig + 1e-30)

    print()
    print(f"  latency         {lag:5d} samples, {1000.0*lag/RATE:.2f} ms")
    print(f"  clock slips     {slips:5d} in {nb} half-second blocks")
    print(f"  drift over {seconds:.0f}s  {late_note}")
    print(f"  level           {20*np.log10(abs(g)+1e-30):+6.2f} dB")
    print(f"  analog floor    {floor_db:6.1f} dBFS, "
          f"{floor_rel:.1f} dB below the signal")
    print(f"  null            {null:6.1f} dB below the signal "
          f"(median block; worst {nulls.max():.1f}, best {nulls.min():.1f})")
    print(f"  bench clipped    {info.get('clipped', 0):.0f}")
    print()
    if beyond is None or beyond < floor_rel:
        print("The null is at the analog floor: what is left over is no")
        print("bigger than what the input jack was picking up with nothing")
        print("playing, so this measures the room and not the pedal.")
        print("Unplug the instrument to see past it.")
    else:
        print(f"Taking the floor out leaves {beyond:.1f} dB below the signal.")
        print("The two are the same source through two compilers for two")
        print("instruction sets, reaching each other over USB - so what is")
        print("left is float32 landing differently, and whatever the host and")
        print("the board disagree about at the edges of the passage.")


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
