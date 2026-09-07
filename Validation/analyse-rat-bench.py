#!/usr/bin/env python3
#
# What the 2026-09-06 Helios bench session measured, from the captures.
#
# Three tables, and this is where [RAT]'s diode constants came from -
# RAT_D_N, RAT_D_IS and RAT_D_RS are the third table's output:
#
#   asymmetry   duty cycle and H2 across the Distortion travel, which is
#               a hump with its peak at noon and not a trend
#   clamp       the clipping transfer curve, taken at Distortion minimum
#               where the op-amp is a follower and the input drives the
#               diodes through R5 alone
#   diode       N, Is and Rs fitted to that curve
#
# WHY THIS FILE EXISTS
#
# A fit that lives in a script in a temporary directory is a fit nobody
# can check, and constants that came from one are constants nobody can
# argue with.  The captures themselves are a few megabytes each and
# belong to one pedal on one evening, so they are not committed - but
# this is, so the tables can be rebuilt from whatever captures are to
# hand and the numbers in rat.h have somewhere to come from.
#
# WHY THE PEAK AND NOT THE FUNDAMENTAL
#
# The clamp curve wants the voltage on the diodes, and that is the peak
# of the waveform.  Reading it off the fundamental instead - the
# ladder's out_db - gives 154 mV/decade against the peak's 119, because
# the fundamental of a compressed sine is not its peak and the gap grows
# with the compression.  So the ladder's stored 'peak' is used, scaled by
# what the same ratio does in the linear region.
#
import argparse
import glob
import json
import os
import sys

import numpy as np

VPEAK = 1.41421356          # volts at digital full scale
R5 = 1000.0                 # the only thing between the drive and the diodes
VT = 0.02585


def load(path):
    d = json.load(open(path))
    L = sorted(d["ladder"], key=lambda r: r["dbfs"])
    lin = [r for r in L if r["dbfs"] <= -20]
    if not lin:
        raise SystemExit("%s: no rungs at or below -20 dBFS to take a "
                         "linear reference from" % os.path.basename(path))
    return dict(
        path=path, name=os.path.basename(path), setting=d.get("setting"),
        ladder=L, waves=d.get("waves", {}),
        # the two linear references, one per statistic
        gain=float(np.mean([r["out_db"] - r["dbfs"] for r in lin])),
        pk=float(np.mean([r["peak"] / 10 ** (r["dbfs"] / 20.0) for r in lin])),
        nlin=len(lin))


def asymmetry(c):
    """Duty cycle off a burst, and H2 off the ladder rung at the same level.

    The duty cycle is the metric to quote.  Peak-to-peak asymmetry is a
    single-sample statistic and moved 0.20 dB between two captures of
    the same setting three hours apart, where the duty cycle moved 0.01%.
    """
    w = c["waves"].get("burst-12")
    if w is None:
        return None
    b = np.array(w["back"])
    b = b[int(0.15 * len(b)):int(0.9 * len(b))]
    b = b - b.mean()                    # the capture's DC, not the pedal's
    rung = {r["dbfs"]: r for r in c["ladder"]}.get(-12.0)
    return (100.0 * np.mean(b > 0),
            rung["h"][0] if rung else float("nan"),
            rung["h"][1] if rung else float("nan"))


def clamp_curve(c):
    """Drive, node voltage and diode current, one row per rung.

    Only meaningful at Distortion minimum, where the op-amp's gain is 1
    and the drive is the input.  Everything after the clamp is linear, so
    the peak scaling measured in the linear region carries the output
    peak back to the node.
    """
    rows = []
    for r in c["ladder"]:
        drive = 10 ** (r["dbfs"] / 20.0) * VPEAK
        vcl = r["peak"] / c["pk"] * VPEAK
        rows.append((r["dbfs"], drive, vcl, (drive - vcl) / R5,
                     r["out_db"] - (r["dbfs"] + c["gain"])))
    return rows


def diode_fit(rows, floor=2e-6):
    """V = N*Vt*ln(I/Is) + I*Rs, fitted in log space.

    Is and Rs are ten orders of magnitude apart, so a naive fit sits
    still and hands back the starting guess with a worse residual than a
    straight line.  log10(Is) is the parameter, and x_scale carries Rs.
    """
    from scipy.optimize import least_squares
    v = np.array([r[2] for r in rows if r[3] > floor])
    i = np.array([r[3] for r in rows if r[3] > floor])
    if len(v) < 4:
        return None
    def resid(p):
        return p[0] * VT * np.log(i) - p[0] * VT * np.log(10) * p[1] \
            + i * p[2] - v
    f = least_squares(resid, [1.8, -9.0, 30.0],
                      bounds=([1.0, -18.0, 0.0], [3.0, -3.0, 2000.0]),
                      x_scale=[1.0, 1.0, 10.0])
    if not f.success:
        return None
    return dict(N=f.x[0], Is=10 ** f.x[1], Rs=f.x[2], n=len(v),
                rms=float(np.sqrt(np.mean(resid(f.x) ** 2))),
                lo=i.min(), hi=i.max())


def main():
    ap = argparse.ArgumentParser(
        description="Rebuild the RAT bench tables from capture-rat.py JSONs.")
    ap.add_argument("captures", nargs="*", default=None,
                    help="capture files; default is rat-*.json beside this")
    ap.add_argument("--table", action="append", default=None,
                    choices=("asymmetry", "clamp", "diode"))
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    paths = args.captures or sorted(glob.glob(os.path.join(here, "rat-*.json")))
    if not paths:
        raise SystemExit("analyse-rat-bench: no captures.  Make one with "
                         "./capture-rat.py - they are not in the repository.")
    caps = [load(p) for p in paths]
    want = args.table or ("asymmetry", "clamp", "diode")

    print("captures, and the setting each records:")
    for c in caps:
        print("   %-28s %s" % (c["name"], c["setting"]))

    if "asymmetry" in want:
        print("\nasymmetry, from burst-12 and the -12 dBFS rung")
        print("   %-28s %9s %8s %8s" % ("", "above 0", "H2", "H3"))
        for c in caps:
            a = asymmetry(c)
            if a:
                print("   %-28s %8.2f%% %8.1f %8.1f"
                      % (c["name"], a[0], a[1], a[2]))

    if "clamp" in want or "diode" in want:
        for c in caps:
            rows = clamp_curve(c)
            bend = [r for r in rows if r[3] > 2e-6]
            if not bend:
                continue
            if "clamp" in want:
                print("\nclamp curve: %s" % c["name"])
                print("   linear gain %+.3f dB, peak scaling %.5f, from %d rungs"
                      % (c["gain"], c["pk"], c["nlin"]))
                print("   %8s %10s %10s %11s %11s"
                      % ("in dBFS", "drive V", "Vcl V", "I_d A", "compress"))
                for x, drive, vcl, i, comp in bend:
                    print("   %8.1f %10.4f %10.4f %11.2e %10.2f dB"
                          % (x, drive, vcl, i, comp))
            if "diode" in want:
                f = diode_fit(rows)
                if f:
                    print("   diode fit over %d points, %.1f uA to %.0f uA:"
                          % (f["n"], f["lo"] * 1e6, f["hi"] * 1e6))
                    print("      N = %.3f   Is = %.2e A   Rs = %.0f ohm"
                          % (f["N"], f["Is"], f["Rs"]))
                    print("      %.1f mV/decade of junction, %.2f mV rms residual"
                          % (f["N"] * VT * np.log(10) * 1000, f["rms"] * 1000))
    return 0


if __name__ == "__main__":
    sys.exit(main())
