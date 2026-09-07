#
# One look for every figure in Documentation/effects.
#
# The figures are a set before they are pictures.  A reader who has
# learnt that the dashed grey line is the input should not have to learn
# it again on the next page, so the vocabulary lives here rather than in
# whichever script drew the plot: model, netlist, hardware and input
# keep one colour and one dash pattern everywhere, and a script asks for
# a leg by name instead of picking a style.
#
# WHY PNG, WHEN EVERY OTHER FIGURE IN THIS TREE IS NUMBERS
#
# ISSUES 148 argued that the pages in Documentation/effects should be
# tables and mermaid: a number in a diff is legible and a curve in a
# diff is not, and the previous analysis went silently wrong the moment
# single_pole_freq() was fixed under it.  That argument is about the
# *record*, and it is right.
#
# It is not an argument about the picture, and mermaid xychart is a poor
# picture: no logarithmic axis, so a frequency response is eleven evenly
# spaced points with log-spaced labels; one x label per data point, so
# more resolution is more clutter; and no way to put five measurements
# of a pedal on top of a curve of the model, which for [RAT] is the most
# useful thing a page can show.
#
# So rat.md draws with these and keeps its numbers in tables that
# check-analysis.py re-measures, which is 148's requirement met by a
# different route.  klon.md and the rest still use mermaid and should
# probably follow, but that is a day's work per page and not this one.
#
# The price is real and unchanged: a changed figure shows up in review
# as "the file changed" and nothing more.  What stops that mattering is
# that the numbers are beside it and they do diff.
#
# ON BACKGROUNDS
#
# Explicitly light, never transparent.  GitHub renders markdown on white
# or on near-black depending on the reader's theme, and a transparent
# PNG with dark axes vanishes for half of them.
#
import os
import textwrap

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

#
# Four legs, and the eye should be able to tell them apart before it
# reads the legend.
#
# Colour alone is not enough - about one reader in twenty cannot use it,
# and a printed page has none - so each leg carries a dash as well, and
# the two encode the same thing rather than different things.  Hardware
# is the solid dark one because it is the thing the others are claiming
# to be.
#
LEGS = {
    "input":    dict(color="#8a8a8a", ls=(0, (1, 2)), lw=1.0, zorder=2),
    "model":    dict(color="#c2410c", ls=(0, (5, 2)), lw=1.6, zorder=4),
    "netlist":  dict(color="#1d4ed8", ls=(0, (2, 2)), lw=1.6, zorder=3),
    "hardware": dict(color="#111827", ls="-",         lw=1.4, zorder=5),
}

LABELS = {
    "input": "input",
    "model": "[RAT], on the pedal",
    "netlist": "ngspice, rat-helios.cir",
    "hardware": "the Helios itself",
}

#
# One leg at several knob positions is a different kind of series from
# model-against-hardware, and wants a different vocabulary: these are
# not three claims about the same thing, they are one thing at three
# settings, and the eye should read them as ordered rather than as
# rival.  So they share a hue and separate by lightness, darkest last,
# which also survives being printed.
#
# Chosen from Okabe-Ito like LEGS above, so a page that carries both
# kinds of chart does not look like two documents.
#
SWEEP = ("#9ecae1", "#4292c6", "#08519c")
SWEEP_DASH = ((0, (1, 1.5)), (0, (4, 2)), "-")


def sweep(ax, x, series, **kw):
    """Draw one leg at several settings, ordered light to dark."""
    n = len(series)
    out = []
    for i, (label, y) in enumerate(series):
        j = int(round(i * (len(SWEEP) - 1) / max(n - 1, 1)))
        style = dict(color=SWEEP[j], ls=SWEEP_DASH[j], lw=1.6, zorder=3 + i)
        style.update(kw)
        out.append(ax.plot(x, y, label=label, **style))
    return out


BG = "#fbfbfa"
GRID = "#d9d9d6"
FG = "#111827"


def figure(width=7.2, height=3.2, rows=1, cols=1, **kw):
    """One figure, styled, with the background painted rather than left."""
    fig, ax = plt.subplots(rows, cols, figsize=(width, height),
                           facecolor=BG, **kw)
    for a in (ax.flat if hasattr(ax, "flat") else [ax]):
        a.set_facecolor(BG)
        a.grid(True, color=GRID, lw=0.6, zorder=0)
        a.set_axisbelow(True)
        for spine in ("top", "right"):
            a.spines[spine].set_visible(False)
        for spine in ("left", "bottom"):
            a.spines[spine].set_color(GRID)
        a.tick_params(colors=FG, labelsize=8, length=3)
        for lbl in a.get_xticklabels() + a.get_yticklabels():
            lbl.set_color(FG)
    return fig, ax


def leg(ax, x, y, which, label=None, **kw):
    """Draw one leg of a comparison in its own permanent style."""
    style = dict(LEGS[which])
    style.update(kw)
    return ax.plot(x, y, label=LABELS[which] if label is None else label,
                   **style)


def finish(fig, ax, path, title=None, caption=None, legend=True, dpi=140):
    """Label it, say whether it can be rebuilt, and write it out.

    Every figure carries a line saying what made it.  A page of pictures
    whose provenance is in somebody's shell history is a page that
    cannot be checked, and the ones drawn from Dry-Guitar.wav cannot be
    rebuilt from the repository at all - which is exactly the sort of
    thing that has to be written on the picture rather than remembered.
    """
    axes = list(ax.flat) if hasattr(ax, "flat") else [ax]
    caption_frac = 0.0
    if title:
        axes[0].set_title(title, color=FG, fontsize=10, loc="left")
    if legend:
        lg = axes[0].legend(fontsize=8, framealpha=0.0, loc="best")
        for t in lg.get_texts():
            t.set_color(FG)
    if caption:
        #
        # Wrapped to the figure's own width rather than trusted to fit.
        # fig.text does not wrap, so a caption a few words too long
        # simply runs off the right edge and is gone - which is not
        # visible from the code and is very visible in the picture.
        #
        width = max(int(fig.get_size_inches()[0] * 15), 40)
        lines = []
        for para in caption.split("\n"):
            lines.extend(textwrap.wrap(para, width) or [""])
        fig.text(0.01, 0.01, "\n".join(lines), fontsize=7,
                 color="#6b7280", va="bottom")
        #
        # Room for the caption that is actually there, not for a nominal
        # two lines.  A four-line caption under a fixed margin sits on
        # top of the x-axis label, which is only visible in the picture.
        # Measured in inches and converted, because a fraction of the
        # figure means different things at different heights.
        #
        inches = 0.16 + 0.115 * len(lines)
        caption_frac = min(inches / fig.get_size_inches()[1], 0.45)
    fig.tight_layout(rect=(0, caption_frac, 1, 1))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    fig.savefig(path, dpi=dpi, facecolor=BG)
    plt.close(fig)
    return path
