//
// Quick test-program for a native build of exactly the
// FFT we use on the 32-bit arm rp2354
//
// NOTE! This does *not* use <math.h> very much on purpose,
// and uses our fastsincos() and other quick float approximations.
//
// This needs the Software build to have completed, and then
// this can be built with
//
//	gcc -Wall -O2
//		-I../Software -I../Software/build/
//		-ffast-math
//		-fsingle-precision-constant
//		-Wfloat-conversion
//		test-fft.c
//
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define SAMPLES_PER_SEC (48000.0f)

#include "audio/util.h"
#include "audio/analyze.h"
#include "audio/fft.h"

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

// 32-bit signed samples from WAV file
//
static int32_t samples[4*FFT_SIZE];
static complex_t fft_buf[FFT_SIZE];
static float magnitudes[FFT_SIZE/2];

// Just the rough header - this should be expanded
// to verify that it really is a mono pcm_s32le wav
// file: uncompressed linear PCM with just one audio
// channel and 48000 samples per second.
struct wav_header {
	unsigned int ID;	// 'RIFF'
	unsigned int size;	// not counting the first 8 bytes
	unsigned int format;	// 'WAVE'
	unsigned int subchunk;	// 'fmt '
	// .. and so on
};

// Map a file with raw 32-bit integer samples
// and do a fft over the range, outputting the
// FFT for that range
int main(int argc, char **argv)
{
	if (argc < 3) die("usage: %s <file> <offset>\n", argv[0]);

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) die("'%s': %s\n", argv[1], strerror(errno));

	errno = 0;
	unsigned long arg_offset = strtoul(argv[2], NULL, 0);
	if (errno) die("'%s': %s\n", argv[2], strerror(errno));

	struct stat st;
	if (fstat(fd, &st) < 0) die("'%s': %s\n", argv[1], strerror(errno));
	if (!S_ISREG(st.st_mode)) die("'%s' is not a regular file\n", argv[1]);

	char riff_wave[12];
	if (read(fd, riff_wave, 12) != 12) die("Couldn't read WAV header\n");
	if (memcmp(riff_wave, "RIFF", 4) || memcmp(riff_wave + 8, "WAVE", 4))
		die("Not a WAVE file\n");

	unsigned long data_offset = 12;
	while (1) {
		unsigned int chunk[2];
		if (pread(fd, chunk, 8, data_offset) != 8) die("Couldn't find data chunk\n");
		data_offset += 8;
		if (!memcmp(chunk, "data", 4))
			break;
		data_offset += chunk[1];
	}

	unsigned long byte_offset = data_offset + 4 * arg_offset;
	unsigned long len = st.st_size;

	if (byte_offset + sizeof(samples) > len) die("'%s' is too small\n", argv[1]);

	ssize_t res = pread(fd, samples, sizeof(samples), byte_offset);
	if (res != sizeof(samples)) die("Couldn't read sample data\n");

	// Convert the samples to floating point, do the
	// 4x downsampling, then do Hann window using our
	// hanning() function from "audio/analyze.h", and
	// finally do fft(fft_buf, FFT_SHIFT) and compute
	// compute the magnitudes, and then write them
	// out for analysis
	for (int i = 0; i < FFT_SIZE; i++) {
		float sum = (float)samples[4*i] + (float)samples[4*i+1] +
			    (float)samples[4*i+2] + (float)samples[4*i+3];
		sum *= (1.0f / 2147483648.0f);
		fft_buf[i] = sum * hanning(i);
	}

	fft(fft_buf, FFT_SHIFT);

	for (int i = 0; i < FFT_SIZE / 2; i++) {
		magnitudes[i] = __builtin_cabsf(fft_buf[i]);
	}

	uint32_t out_header[2] = { arg_offset, FFT_SIZE / 2 };
	if (write(STDOUT_FILENO, out_header, sizeof(out_header)) != sizeof(out_header))
		die("Failed to write output header\n");

	if (write(STDOUT_FILENO, magnitudes, sizeof(magnitudes)) != sizeof(magnitudes))
		die("Failed to write magnitudes\n");

	return 0;
}
