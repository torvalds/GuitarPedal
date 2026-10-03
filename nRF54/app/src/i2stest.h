/*
 * A test pattern for the i2s link between the RP2354 and the radio, which
 * both sides compile, the way they both compile link.h.
 *
 * 64 stereo frames, repeated for ever.  The left channel is a sine at half
 * of full scale, exactly one period of the 64, so 750 Hz at 48 kHz.  The
 * right is a word that names the frame, with bits set across the whole of
 * it.  A receiver checks every bit, with i2stest_check() below: a frame
 * that matches with left and right exchanged, or shifted by a bit, says
 * what is wrong with the format rather than only that something is.
 *
 * The sine is a table of integers rather than sinf(), because the two
 * sides build against different C libraries and a difference in the last
 * bit would read as a fault on the link.
 */
#ifndef I2STEST_H
#define I2STEST_H

#include <stdint.h>

#define I2STEST_SHIFT	6
#define I2STEST_FRAMES	(1 << I2STEST_SHIFT)
#define I2STEST_MASK	(I2STEST_FRAMES - 1)

static const int32_t i2stest_left[I2STEST_FRAMES] = {
	0, 105245103, 209476638, 311690799,
	410903207, 506158392, 596538995, 681174602,
	759250125, 830013654, 892783698, 946955747,
	992008094, 1027506862, 1053110176, 1068571464,
	1073741824, 1068571464, 1053110176, 1027506862,
	992008094, 946955747, 892783698, 830013654,
	759250125, 681174602, 596538995, 506158392,
	410903207, 311690799, 209476638, 105245103,
	0, -105245103, -209476638, -311690799,
	-410903207, -506158392, -596538995, -681174602,
	-759250125, -830013654, -892783698, -946955747,
	-992008094, -1027506862, -1053110176, -1068571464,
	-1073741824, -1068571464, -1053110176, -1027506862,
	-992008094, -946955747, -892783698, -830013654,
	-759250125, -681174602, -596538995, -506158392,
	-410903207, -311690799, -209476638, -105245103,
};

static inline int32_t i2stest_right(unsigned int frame)
{
	frame &= I2STEST_MASK;
	return (int32_t)(0x5A000000u | frame << 16 |
			 (~frame & I2STEST_MASK) << 8 | 0xC3);
}

/*
 * Checking what arrives, frame by frame.  A frame that is merely wrong
 * keeps the count going, so one damaged sample is one wrong frame; one that
 * is a different frame of the pattern is a slip, frames dropped or
 * repeated.  A wrong frame that would match with left and right exchanged,
 * or with the right a bit early or late, is counted as that as well.
 */
struct i2stest_rx {
	int phase;		/* the frame expected next, or -1 */
	uint32_t frames;
	uint32_t wrong;
	uint32_t swapped;
	uint32_t shifted;
	uint32_t slips;
};

static inline void i2stest_rx_init(struct i2stest_rx *t)
{
	*t = (struct i2stest_rx){ .phase = -1 };
}

static inline int i2stest_is(int p, int32_t l, int32_t r)
{
	return l == i2stest_left[p] && r == i2stest_right(p);
}

static inline void i2stest_classify(struct i2stest_rx *t, int32_t l,
				    int32_t r)
{
	for (int p = 0; p < I2STEST_FRAMES; p++) {
		uint32_t want = (uint32_t)i2stest_right(p);

		if (l == (int32_t)want && r == i2stest_left[p]) {
			t->swapped++;
			return;
		}
		if (((uint32_t)r << 1) == (want & ~1u) ||
		    ((uint32_t)r >> 1) == (want & 0x7fffffffu)) {
			t->shifted++;
			return;
		}
	}
}

static inline void i2stest_check(struct i2stest_rx *t, int32_t l, int32_t r)
{
	t->frames++;
	if (t->phase >= 0 && i2stest_is(t->phase, l, r)) {
		t->phase = (t->phase + 1) & I2STEST_MASK;
		return;
	}
	for (int p = 0; p < I2STEST_FRAMES; p++) {
		if (i2stest_is(p, l, r)) {
			if (t->phase >= 0)
				t->slips++;
			t->phase = (p + 1) & I2STEST_MASK;
			return;
		}
	}
	t->wrong++;
	i2stest_classify(t, l, r);
	if (t->phase >= 0)
		t->phase = (t->phase + 1) & I2STEST_MASK;
}

#endif /* I2STEST_H */
