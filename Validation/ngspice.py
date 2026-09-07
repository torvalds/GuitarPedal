#!/usr/bin/env python3
#
# Drive ngspice on the netlists in spice/, and get numbers back.
#
# This is the ground truth, and it is the level that is too slow to
# develop against: a six-window transient on one setting is seconds where
# the bench is milliseconds, so it is run when a model and the circuit
# disagree, not on every change.  compare-spice.py is what asks it.
#
# What this file adds over an afternoon with a simulator is that the
# question is asked from a netlist in the repository.  The condition a
# measurement was taken under ends up in a file rather than in somebody's
# memory of which switch was where, and it can be asked again.
#
# WHY THIS IS NOT CALLED spice.py
#
# The netlists live in spice/, and a module called spice.py beside a
# directory called spice/ is a coin toss about which one `import spice`
# finds - Python will happily treat the directory as a namespace package.
# That is the same class of bug as Validation/wave.py shadowing the
# standard library, which is issue 291 and which quietly killed
# audio.wav().  One is enough.
#
# WHAT NGSPICE NEEDS THAT IS NOT OBVIOUS
#
# Two things cost time to find and are worth keeping written down:
#
#   - `ngspice -b` on a netlist with a .control block will sit there
#     forever unless the block ends in `quit`.  It is not hung and it is
#     not slow; it is waiting for you.  Every control block here ends in
#     quit.
#   - a single-point `ac lin 1 F F` is what hangs it in the first place.
#     Sweeps here always span a range and the caller picks points out.
#
import hashlib
import os
import re
import subprocess
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SPICE = os.path.join(HERE, "spice")

#
# Transients are cached on disk, and that is what makes ngspice usable as
# the only oracle rather than as an occasional second opinion.
#
# A run is a pure function of the deck it is handed: the netlist text
# after parameter substitution, the stimulus, the node asked for, the
# rate, the settle and the control line.  None of those change while a
# *model* is being worked on, which is the whole of the development loop,
# so the first run of a battery pays for ngspice and every run after it
# pays for a file read.
#
# Hashing the substituted source rather than the file plus the parameters
# is deliberate: it covers both, and it covers them at the granularity
# that actually matters, since a parameter is formatted into the text
# before it can affect anything.
#
# CACHE_VERSION is in the key because the control line and the way the
# stimulus is written are part of the question being asked, and changing
# either changes the answer without changing anything else here.
#
CACHE = os.path.join(HERE, "spice-cache")
CACHE_VERSION = 1


class SpiceError(Exception):
    pass


def netlist(name):
    """The text of a netlist in spice/, with its .end removed."""
    path = name if os.path.sep in name else os.path.join(SPICE, name + ".cir")
    with open(path) as f:
        src = f.read()
    return re.sub(r"^\.end\s*$", "", src, flags=re.M)


#
# Every substitution in this file goes through here.
#
# re.sub() on a pattern that matches nothing returns the string
# unchanged and tells nobody.  Editing a netlist that way means a
# parameter override can silently not happen: the deck still solves, the
# run still produces numbers, and they are the numbers for the deck as
# written rather than the one that was asked for.  There is no error to
# notice and nothing in the output that looks wrong.
#
# A substitution that must match is an assertion, so it is written as one.
#
def _sub(pattern, repl, src, what):
    out, n = re.subn(pattern, repl, src, count=1, flags=re.M)
    if n != 1:
        raise SpiceError("%s: matched %d times, wanted 1" % (what, n))
    return out


def _params(src, params):
    """Override .param lines. Unknown names are an error, not a no-op."""
    for k, v in (params or {}).items():
        pat = re.compile(r"^\.param\s+%s\s*=.*$" % re.escape(k), re.M)
        if not pat.search(src):
            raise SpiceError("%s is not a .param in this netlist" % k)
        src = _sub(pat.pattern, ".param %s=%r" % (k, float(v)), src,
                   ".param %s" % k)
    return src


def _run(src, control, columns):
    """Run one control block, and read back what wrdata wrote."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.dat")
        deck = "%s\n.control\n%s\nwrdata %s %s\nquit\n.endc\n.end\n" % (
            src, control, out, " ".join(columns))
        cir = os.path.join(tmp, "deck.cir")
        log = os.path.join(tmp, "deck.log")
        with open(cir, "w") as f:
            f.write(deck)
        r = subprocess.run(["ngspice", "-b", "-o", log, cir],
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=1800)
        if not os.path.exists(out):
            with open(log) as f:
                tail = f.read()[-2000:]
            raise SpiceError("ngspice wrote nothing:\n%s\n%s" % (r.stderr, tail))
        return np.loadtxt(out)


#
# wrdata repeats the sweep variable in front of every column it is asked
# for, so a request for three nodes comes back six columns wide.  Undo
# that here rather than at every call site.
#
def _columns(data, n):
    data = np.atleast_2d(data)
    x = data[:, 0]
    return x, [data[:, 2 * i + 1] for i in range(n)]


def op(name, params=None, nodes=()):
    """The DC operating point.  Returns {node: volts}."""
    src = _params(netlist(name), params)
    cols = ["v(%s)" % n for n in nodes]
    data = _run(src, "op", cols)
    data = np.atleast_1d(data)
    # an op has one row, so wrdata gives one flat line of index/value pairs
    return {n: float(data[2 * i + 1]) for i, n in enumerate(nodes)}


def ac(name, node, freqs, params=None, per_decade=200):
    """Small-signal gain in dB at each of 'freqs', from a 1 V AC input."""
    src = _params(netlist(name), params)
    lo, hi = min(freqs) * 0.9, max(freqs) * 1.1
    data = _run(src, "ac dec %d %g %g" % (per_decade, lo, hi), ["v(%s)" % node])
    f = data[:, 0]
    mag = np.hypot(data[:, 1], data[:, 2])
    db = 20 * np.log10(np.maximum(mag, 1e-30))
    return np.interp(np.asarray(freqs, dtype=float), f, db)


#
# A transient with our own stimulus, which is the whole reason this file
# exists rather than four hand-run sweeps.
#
# THE STIMULUS GOES IN INLINE, AND NOT BECAUSE IT IS TIDIER
#
# ngspice documents `PWL file="..."` and this build does not accept it -
# absolute path, relative path, with and without quotes, with and without
# a leading DC term, all give "parameter value out of range or the wrong
# type" on the source line.  So the breakpoints are written into the deck
# itself, `+`-continued.
#
# That puts a real limit on how much can be pushed through here: a second
# of 48 kHz audio is 48,000 breakpoints and about a megabyte of deck.  It
# is not a reason to work around - ngspice is the level of the chain that
# runs rarely and on short windows, by design - but a caller asking for
# ten seconds should be told rather than left waiting, which is what
# MAX_TRAN_SAMPLES is for.
#
# 'maxstep' is pinned to the sample period.  Without it ngspice picks its
# own timestep, which is the right thing for accuracy and the wrong thing
# for a comparison - the returned samples have to land where the pedal's
# samples land, and interpolating a curve ngspice drew coarsely is not
# the same as asking it for those instants.
#
MAX_TRAN_SAMPLES = 200000


def tran(name, node, x, fs=48000.0, params=None, settle=0.2,
         drive=None, offset=0.0):
    """Push a sample array through the netlist.  Returns the same length.

    'drive' cuts the netlist open: instead of the input, the stimulus is
    applied to that node by an ideal source, with 'offset' volts of DC
    under it so the node stays where its operating point put it.  That
    is how a part of the pedal gets measured on its own: asking what one
    stage does to a signal means driving that stage's input rather than
    the guitar jack, and what is upstream of it then has no vote.

    An ideal source there is not necessarily a simplification: it is
    often exactly what the model being compared against already assumes,
    a stage being a function applied to a number with nothing upstream
    having an opinion about the load.  Whether that assumption is right
    is a different question, and driving the node is how it gets asked.
    """
    x = np.asarray(x, dtype=float)
    n = len(x)
    if n > MAX_TRAN_SAMPLES:
        raise SpiceError("%d samples is %.1f s of deck; ngspice is the level "
                         "of the chain that runs on short windows (limit %d)"
                         % (n, n / fs, MAX_TRAN_SAMPLES))
    src = _params(netlist(name), params)

    t = (np.arange(n) + settle * fs) / fs
    pts = ["0 %.9g" % offset]
    pts += ["%.9g %.9g" % (ti, offset + xi) for ti, xi in zip(t, x)]
    body = "PWL\n" + "\n".join("+ " + p for p in pts)

    if drive:
        #
        # The original source stays, at DC 0, because its node needs a
        # path to ground - ngspice will not solve a floating one - and
        # the new source overrides whatever the circuit was doing at the
        # node it is put on.
        #
        src += "\nVdrv %s 0 DC %.9g %s\n" % (drive, offset, body)
    else:
        src = _sub(r"^Vin\s+in\s+0\s+.*$",
                   lambda _m: "Vin in 0 " + body, src, "the input source")

    stop = settle + n / fs
    ctl = "tran %.9g %.9g 0 %.9g" % (1.0 / fs, stop, 1.0 / fs)

    key = hashlib.sha256()
    for part in (str(CACHE_VERSION), src, ctl, node,
                 "%.9g" % fs, "%.9g" % settle):
        key.update(part.encode())
        key.update(b"\0")
    key.update(np.ascontiguousarray(t, dtype=np.float64).tobytes())
    path = os.path.join(CACHE, key.hexdigest()[:32] + ".npy")

    if not os.environ.get("NGSPICE_NO_CACHE") and os.path.exists(path):
        return np.load(path)

    data = _run(src, ctl, ["v(%s)" % node])
    tt, cols = _columns(data, 1)
    #
    # ngspice can stop early and still write what it has.  A run that
    # gives up at "timestep too small" leaves a file that loads, and
    # np.interp() extends the last sample flat all the way to the end -
    # so a truncated run comes back as a signal that fades into a
    # constant, which reads as a plausible measurement and is not one.
    #
    # It cost an afternoon: a 2-second RAT stimulus came back with the
    # tail a straight line, the level looked right because the rms was
    # taken over the whole window, and the harmonics came out as
    # division by zero.
    #
    if tt[-1] < stop - 1.5 / fs:
        raise SpiceError("ngspice stopped at %.4f s of %.4f; the run did "
                         "not converge" % (tt[-1], stop))
    out = np.interp(t, tt, cols[0])

    if not os.environ.get("NGSPICE_NO_CACHE"):
        os.makedirs(CACHE, exist_ok=True)
        #
        # Written beside and renamed, so an interrupted run leaves no
        # half a file for the next one to trust.
        #
        tmp = path + ".%d" % os.getpid()
        np.save(tmp, out)
        os.replace(tmp + ".npy" if not tmp.endswith(".npy") else tmp, path)
    return out
