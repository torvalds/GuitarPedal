typedef _Complex float complex_t;

static inline complex_t twiddle_factor(float phase)
{
	struct sincos sc = fastsincos(phase);
	return __builtin_complex(sc.cos, -sc.sin);
}

static inline unsigned reverse_n_bits(unsigned int i, unsigned int bits)
{
#ifdef __arm__
	// Newer versions of gcc have this as a builtin
	asm inline("rbit %0,%1":"=r" (i): "r" (i));
	return i >> (32-bits);
#else
	int res = 0;
	for (int j = 0; j < bits; j++) {
		res <<= 1;
		res |= i & 1;
		i >>= 1;
	}
	return res;
#endif
}

//
// Cooley-Tukey Radix-2 DIT FFT.
//
// Operates in-place on a complex array.
//
static inline void fft(complex_t *buf, int bits)
{
	int n = 1 << bits;

	// Bit-reversal permutation
	for (int i = 0; i < n - 1; i++) {
		int j = reverse_n_bits(i, bits);
		if (i < j) {
			complex_t tmp = buf[i];
			buf[i] = buf[j];
			buf[j] = tmp;
		}
	}

	// Cooley-Tukey butterfly
	for (int shift = 0; shift < bits; shift++) {
		int step = 1 << shift;
		for (int j = 0; j < step; j++) {
			complex_t w = twiddle_factor(u32_to_fraction(j << (31-shift)));

			for (int i = j; i < n; i += step * 2) {
				int match = i + step;
				complex_t tmp = w * buf[match];

				buf[match] =buf[i] - tmp;
				buf[i] += tmp;
			}
		}
	}
}
