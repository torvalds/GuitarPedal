#!/usr/bin/env python3
#
# Do the effect analyses still describe the effects?
#
# Documentation/effects/*.md have measured numbers in them, written by
# hand around the output of the analyse-*.py scripts.  The prose is worth
# writing by hand; the numbers are not worth trusting once anything under
# them changes, and the last analysis went silently wrong the moment
# single_pole_freq() was fixed.
#
# So the pages are not generated - they are checked.  Every mermaid
# 'line [...]' in a page is a series that came from a measurement, and
# this re-measures and compares.  Same pattern as check-readme.py: the
# document stays hand-written and human, and something else says when it
# has started lying.
#
# It compares exactly.  bench/bench is deterministic - same binary, same
# input, same bytes out - so a tolerance would only be hiding drift.
# What it cannot do is notice the *prose* going stale, which is why the
# numbers matter: a sentence is usually wrong in the same direction as
# the number it was written about.
#
# Warns rather than fails, like check-readme.py.  A page is documentation
# and is allowed to lag a commit; the firmware build should not stop for
# it.
#
# Called as: check-analysis.py [page.md ...]   (default: all of them)
#
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGES = HERE.parent / "Documentation" / "effects"


def series_in(path):
    """Every mermaid 'line [...]' in a page, with the line it is on.

    The written precision is kept alongside the value.  A page says
    14.13 where the bench says 14.125, and the question to ask is not
    'are these within some epsilon' - it is 'does the measurement round
    to what the page claims', which has an exact answer and needs no
    tolerance to be argued about.
    """
    out = []
    for n, text in enumerate(path.read_text().split("\n"), 1):
        m = re.match(r"\s*line\s*\[([^\]]*)\]\s*$", text)
        if not m:
            continue
        vals = [v.strip() for v in m.group(1).split(",")]
        out.append((n, [(float(v), len(v.partition(".")[2])) for v in vals]))
    return out


def matches(measured, written):
    """Every measurement rounds to the figure the page prints.

    Half an ulp of the written precision, inclusive - not round() itself,
    which is banker's on a tie and would reject a page that wrote 14.13
    where the bench said 14.125.  Either rounding of a tie is a correct
    rendering of the measurement and neither is worth a warning.
    """
    return len(measured) == len(written) and all(
        abs(a - v) <= 0.5 * 10 ** -dp + 1e-9
        for a, (v, dp) in zip(measured, written))


def bracketed(stdout):
    """The '[...]' lists the analyser prints, which are already series."""
    out = []
    for text in stdout.split("\n"):
        m = re.search(r"\[([-0-9.,\s]+)\]\s*$", text)
        if m and "x-axis" not in text:
            out.append([float(v) for v in m.group(1).split(",")])
    return out


#
# A column of a fixed-width table is the same measurement in a different
# shape, and the pages plot those too - the gain sweep is printed as a
# table and drawn as two curves.
#
def columns_of(stdout):
    """Numeric columns out of the fixed-width tables, as lists."""
    cols, run = [], []
    for text in stdout.split("\n"):
        f = text.split()
        if f and all(re.fullmatch(r"-?\d+\.?\d*", v) for v in f):
            run.append([float(v) for v in f])
        elif run:
            cols.extend(list(map(list, zip(*run))))
            run = []
    if run:
        cols.extend(list(map(list, zip(*run))))
    return cols


#
# Whether the charts draw at all, which the source cannot tell you.
#
# A mermaid block with a syntax error is not a smaller chart or an ugly
# one - GitHub renders an error box where the curve should be, and
# nothing locally says so.  That was worth two screenshot round trips
# before this existed: the colour directive was written as
# xyChart.plotColorPalette, which parses and is silently ignored, and
# the page went out claiming a colour it did not have.
#
# mmdc takes the whole markdown file and draws every block in it, which
# is one browser start rather than one per chart - about a second for a
# page.  Skipped rather than fatal when there is no way to run it, the
# same bargain 'make check' makes about node: this is the only thing
# here that wants anything beyond python and a C compiler.
#
def renderer():
    if shutil.which("mmdc"):
        return ["mmdc"]
    if shutil.which("npx"):
        # Downloads the package the first time and caches it after.
        return ["npx", "-y", "-p", "@mermaid-js/mermaid-cli", "mmdc"]
    return None


def renders(path, how):
    """Draw every mermaid block on the page, into a directory we throw away."""
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run(how + ["-i", str(path.resolve()), "-o", "out.md"],
                           cwd=d, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True,
                           timeout=600)
        return r.returncode == 0, r.stdout


#
# Mermaid ties one x-axis category to one data point, and silently
# collapses categories that are equal: two points sharing a label land
# on the same x and the line doubles back on itself.  So a page can
# render perfectly, pass every number, and still draw nonsense.
#
# That happened.  Thinning a crowded axis by labelling every third point
# and leaving " " for the rest gave eleven readable labels and a chart
# that zigzagged, because all the blanks were one category.  Whitespace
# of any length is trimmed to the same thing, and so are the Unicode
# spaces - both were tried.
#
# Drawing it was not enough to catch that, because it *did* draw.  This
# is the cheap check that does: no two categories on an axis may be
# equal after trimming.
#
def duplicate_labels(path):
    out = []
    for n, text in enumerate(path.read_text().split("\n"), 1):
        m = re.match(r'\s*x-axis\s+(?:"[^"]*"\s+)?\[(.*)\]\s*$', text)
        if not m:
            continue
        labels = [v.strip().strip('"').strip() for v in m.group(1).split(",")]
        dupes = sorted({v for v in labels if labels.count(v) > 1})
        if dupes:
            out.append((n, dupes))
    return out


def check_renders(path, how):
    for lineno, dupes in duplicate_labels(path):
        shown = ", ".join(repr(d) for d in dupes[:3])
        print(f"check-analysis: WARNING - {path.name}:{lineno} repeats "
              f"x-axis labels ({shown}).  Mermaid collapses those onto one "
              f"x position and the line doubles back.")

    blocks = len(re.findall(r"^```mermaid\s*$", path.read_text(), re.M))
    if not blocks:
        return 0
    if how is None:
        print(f"check-analysis: {path.name}, {blocks} charts NOT DRAWN - "
              f"no mmdc and no npx to fetch one")
        return 0

    ok, log = renders(path, how)
    if ok:
        print(f"check-analysis: {path.name}, {blocks} charts all draw")
        return 0

    print(f"check-analysis: WARNING - {path.name} has a chart that does not "
          f"draw.  On GitHub that is an error box, not a curve.")
    for text in [l for l in log.split("\n") if "rror" in l][:3]:
        print(f"    {text.strip()}")
    return 1


def check(path):
    name = path.stem
    script = HERE / f"analyse-{name}.py"
    if not script.exists():
        print(f"check-analysis: {path.name} has no analyse-{name}.py, skipped")
        return 0

    r = subprocess.run([sys.executable, str(script)], stdout=subprocess.PIPE,
                       cwd=HERE, universal_newlines=True)
    if r.returncode:
        print(f"check-analysis: WARNING - analyse-{name}.py failed")
        return 1

    have = bracketed(r.stdout) + columns_of(r.stdout)
    want = series_in(path)
    if not want:
        print(f"check-analysis: {path.name} has no mermaid series to check")
        return 0

    bad = 0
    for lineno, series in want:
        if any(matches(h, series) for h in have):
            continue
        #
        # Nearest first, or the suggestion is noise: with several series
        # of the same length in one page, the first one that happens to
        # match in length is almost never the one that was meant.
        #
        near = sorted((h for h in have if len(h) == len(series)),
                      key=lambda h: sum(abs(a - v) for a, (v, _) in
                                        zip(h, series)))
        print(f"check-analysis: WARNING - {path.name}:{lineno} does not match "
              f"any measurement")
        print(f"    page says  {[v for v, _ in series]}")
        if near:
            print(f"    nearest    {[round(v, 2) for v in near[0]]}")
        bad += 1

    if not bad:
        print(f"check-analysis: {path.name}, {len(want)} series, all reproduced")
    return bad


def main():
    args = sys.argv[1:]
    pages = [Path(a) for a in args] if args else sorted(PAGES.glob("*.md"))
    if not pages:
        print("check-analysis: no pages in Documentation/effects")
        return 0
    how = renderer()
    for p in pages:
        check(p)
        check_renders(p, how)
    return 0


if __name__ == "__main__":
    sys.exit(main())
