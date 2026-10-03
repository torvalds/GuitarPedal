/*
 * A test pattern for the i2s link between the RP2354 and the radio, which
 * both sides compile, the way they both compile link.h.
 *
 * 64 stereo frames, repeated for ever.  The left channel is a sine at half
 * of full scale, exactly one period of the 64, so 750 Hz at 48 kHz.  The
 * right is a word that names the frame, with bits set across the whole of
 * it.  A receiver checks every bit: a frame that matches with left and
 * right exchanged, or shifted by a bit, says what is wrong with the format
 * rather than only that something is.
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

#endif /* I2STEST_H */
