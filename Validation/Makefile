CC = gcc
CFLAGS = -Wall -O2 -I../Software -I../Software/build/ \
	 -ffast-math -fsingle-precision-constant -Wfloat-conversion

visualize: test-fft
	python visualize.py DADGAD.wav

test-fft: test-fft.c

test-midi-cin: test-midi-cin.c

#
# The web app gets checked too, which needs node.
#
# Skipped rather than fatal when node isn't installed: it is the only
# thing here that needs anything beyond a C compiler and python, and
# 'make check' should still be worth running without it.  It says so when
# it skips, so it can't quietly stop testing anything.
#
NODE := $(shell command -v node 2>/dev/null)

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

.PHONY: check
