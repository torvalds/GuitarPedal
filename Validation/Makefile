CC = gcc
CFLAGS = -Wall -O2 -I../Software -I../Software/build/ \
	 -ffast-math -fsingle-precision-constant -Wfloat-conversion

visualize: test-fft
	python visualize.py DADGAD.wav

test-fft: test-fft.c

test-midi-cin: test-midi-cin.c

check: test-midi-cin
	./test-midi-cin

.PHONY: check
