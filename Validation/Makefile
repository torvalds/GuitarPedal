CC = gcc
CFLAGS = -Wall -O2 -I../Software -I../Software/build/ \
	 -ffast-math -fsingle-precision-constant -Wfloat-conversion

visualize: test-fft
	python visualize.py DADGAD.wav

test-fft: test-fft.c

test-midi-cin: test-midi-cin.c

#
# The effect bench: the pedal's own audio core, built for the host.
#
# Everything it needs is generated here rather than taken from
# ../Software/build, so it does not need the firmware to have been built
# and never writes into the source tree.  The three math tables carry
# the shift values from Software/CMakeLists.txt; if those move, these
# have to move with them, and the bench will be measuring a different
# pedal until they do.
#
# The include paths want a word.  effect_map.h is generated into gen/
# and reaches its effects with '#include "../effects/x.h"', which is
# relative to wherever effect_map.h ends up - so ../Software/audio is on
# the path purely so that ../Software/audio/../effects resolves.  A
# symlink would be the other way to do it and would be a file in the
# tree pretending to be a directory.
#
BENCH_GEN = bench/gen
BENCH_HDRS = $(BENCH_GEN)/effect_map.h $(BENCH_GEN)/pow2.h \
	     $(BENCH_GEN)/log2.h $(BENCH_GEN)/quarter_sine.h
BENCH_CFLAGS = -Wall -Wno-unused-function -O2 \
	       -ffast-math -fsingle-precision-constant -Wfloat-conversion \
	       -DSAMPLES_PER_SEC=48000.0f \
	       -Ibench/shim -I$(BENCH_GEN) -I../Software -I../Software/audio

$(BENCH_GEN)/effect_map.h: $(wildcard ../Software/effects/*.h) \
			   ../Software/scripts/gen_effects.py
	@mkdir -p $(BENCH_GEN)
	python3 ../Software/scripts/gen_effects.py ../Software/effects/ \
		$(BENCH_GEN)/effect_map.h $(BENCH_GEN)/effects.js \
		$(BENCH_GEN)/dummy_map.md

$(BENCH_GEN)/pow2.h: ../Software/scripts/pow2.py
	@mkdir -p $(BENCH_GEN)
	python3 $< $@ 8

$(BENCH_GEN)/log2.h: ../Software/scripts/log2.py
	@mkdir -p $(BENCH_GEN)
	python3 $< $@ 8

$(BENCH_GEN)/quarter_sine.h: ../Software/scripts/quarter_sine.py
	@mkdir -p $(BENCH_GEN)
	python3 $< $@ 8

bench/bench: bench/bench.c $(BENCH_HDRS) \
	     $(wildcard bench/shim/*/*.h bench/shim/*/*/*.h) \
	     $(wildcard ../Software/audio/*.h) ../Software/effect-state.h \
	     ../Software/status.h
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lm

bench: bench/bench

#
# What the bench is for is looking at one effect, which is a
# conversation and not a target.  What belongs in 'check' is only the
# part that has a right answer: the two controls that say whether the
# instrument is working at all.  A bench that passes these and an effect
# that then reads strangely is a finding; a bench that fails these makes
# every other number in the session worthless.
#
check-effects: bench/bench
	./test-effects.py

#
# The other half of the bench: whether it agrees with a real pedal.
#
# Separate from check-effects because it needs hardware, and separate
# from check-hw because it needs no signal generator - the stimulus is
# [TESTTONE] and the capture is the pedal's own USB audio, so the path
# is digital end to end.  Skips when there is no pedal plugged in.
#
check-bench: bench/bench
	./test-bench.py

#
# The analog half, which needs a patch cable from the pedal's output
# back to its own input and nothing else.  One board, one cable, so the
# DAC and the ADC are the same codec on the same clock and there is no
# drift to chase.
#
# Reports and never fails.  What it measures is a converter and a cable
# rather than any code of ours, and 95 is the issue about a bench
# measurement that carries no record of the bench - so this prints
# numbers and lets a person decide whether the cable moved.
#
check-analog:
	./test-analog.py

#
# The web app gets checked too, which needs node.
#
# Skipped rather than fatal when node isn't installed: it is the only
# thing here that needs anything beyond a C compiler and python, and
# 'make check' should still be worth running without it.  It says so when
# it skips, so it can't quietly stop testing anything.
#
NODE := $(shell command -v node 2>/dev/null)

#
# The hardware half.  Wants a pedal on the USB and a signal generator on
# its input, so it is a separate target rather than part of 'check' -
# and it skips rather than fails when there is nothing plugged in, the
# same bargain the node check makes below.
#
# The generator cannot be set from here.  --ptp and --freq say what it
# is set to, and every number that depends on it is reported against
# what was declared.
#
check-hw:
	./test-audio.py --ptp 0.100 --freq 440

#
# Separate from check-hw because it reflashes: planting a scene with
# channel routing in it means a trip through BOOTSEL, since nothing sets
# that over MIDI yet.  It leaves two scenes behind in the save area.
#
check-split:
	./test-split.py

#
# Two pedals, patched output to input in both directions, one generating
# and the other measured.  Separate from check-hw because it needs the
# second board, and because what it is for is different: check-hw asks
# whether the pedal does the right thing to a signal somebody else
# provides, and this asks the questions that need the signal to be under
# the test's control - what the analog link does, how quiet a properly
# driven input is, and whether the audio survives the pedal's own sysex.
#
check-loop:
	./test-loop.py

#
# The hardware MIDI jacks, through a USB-MIDI adapter on the other end.
#
# Reports and never fails.  The adapter is a cheap CH345 and the path has
# shown intermittent trouble nobody has pinned down, so wiring it into a
# gate would only teach us to ignore the gate.  What is wanted is the
# rate: run it, watch the number, and let the data say whether the
# flakiness is the dongle, the parser, or nothing.
#
check-midi:
	./test-midi.py

#
# Cold power cycles on one board, tallied.  Needs a hand on the USB plug
# - there is no switched hub here - so it is as far from 'check' as a
# target gets, and it takes a --target because cycling the wrong board
# yields a number indistinguishable from a real one.
#
#	make check-boot TARGET=7989 JSON=boot-A.json
#
TARGET ?=
JSON ?=
check-boot:
	./test-boot.py $(if $(TARGET),--target $(TARGET)) \
		       $(if $(JSON),--json $(JSON)) $(BOOTARGS)

#
# The same hang, chased unattended through BOOTSEL instead of through the
# plug.  Cheap enough to leave running, and blind to whatever needs a
# genuinely cold crystal - a clean run here is not a fixed pedal.  See
# the header of test-reboot.py before believing a result either way.
#
check-reboot:
	./test-reboot.py $(if $(TARGET),--target $(TARGET)) \
			 $(if $(JSON),--json $(JSON)) $(BOOTARGS)

check: test-midi-cin
	./test-midi-cin
ifeq ($(NODE),)
	@echo "test-webmidi: SKIPPED - no node in PATH"
else
	@#
	@# effects.js is generated, so generate it - into a temp directory,
	@# because a test has no business writing into the source tree, and
	@# this way it doesn't matter whether the firmware has been built.
	@#
	@t=`mktemp -d` && \
	python3 ../Software/scripts/gen_effects.py ../Software/effects/ \
		$$t/dummy_map.h $$t/effects.js $$t/dummy_map.md && \
	$(NODE) test-webmidi.js $$t/effects.js; \
	r=$$?; rm -rf $$t; exit $$r
endif

.PHONY: bench check check-analog check-bench check-boot check-effects
.PHONY: check-hw check-loop
.PHONY: check-midi check-reboot check-split
