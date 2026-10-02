//
// Learning the hum cuts, on core 0.
//
// A learn measures the hum on the left input, ahead of the cuts, and
// puts a peaking cut on each mains line below 2 kHz that is above the
// target, the deepest eight if more need one -
// exactly as deep as it takes to reach the target or the noise beside
// the line, whichever is higher, and as narrow as the line's frequency
// allows.  It keeps a running average and works the cuts out again
// after every block, three times a second, so the hum can be heard and
// seen going as it measures.  So can a string touched by mistake, which
// the measurement leaves out.  It measures through the analyzer in
// Audio/analyze.h, and the tuner taking that over stops a learn, which
// starts again once the tuner is off.
//
//	F0 7D 21 [target] F7		learn; target in -dBFS, 95 if none
//	F0 7D 21 00 F7			clear the cuts
//	F0 7D 22 F7			report the cuts
//
// all answered, and sent again whenever the cuts change, by
//
//	F0 7D 21 <version> <mains> <cuts> <updates> <learning> <target>
//		 <ignored>
//		 { <harmonic> <depth*4:14> <width*10:14> <before> <after> }
//		 F7
//
// with depth and width as fourteen-bit numbers, high seven bits first,
// the target and each line's level before and after in -dBFS - the
// after being what the cut leaves, worked out rather than measured - and
// how much of the signal was left out for something being played, in
// tenths of a second.  The report describes notches rather than filter
// sections, so that the app can draw whatever produced them.
//
// [DEHUM]'s Target pot, while the effect is on, recomputes the cuts at
// once from what was last measured and starts a learn.
//
#define HUM_BINS	(FFT_SIZE / 2 + 1)
#define HUM_BIN_HZ	ANALYZE_BIN_HZ
#define HUM_TOP_HZ	2000.0f
#define HUM_MAX_DEPTH	40.0f
#define HUM_LEARN_MS	5000
#define HUM_MIN_BLOCKS	4
#define HUM_AVG_BLOCKS	8		// about 2.7 s, at a block every 0.34 s

//
// One measurement: the average power spectrum of the analyzer's blocks.
// A plain average until HUM_AVG_BLOCKS are in, so the first cuts come
// from what little there is, and a running one after that, so what is
// in it stays recent.
//
static struct {
	bool running;
	bool taking;			// past the settling
	uint32_t start, until;
	unsigned int blocks;
	float level;			// average hum_block_level()
	float power[HUM_BINS];
} hum_fft;

static struct {
	unsigned int updates;
	float target;			// dBFS
	float mains;			// 0 when none was found
	unsigned int n;
	struct {
		unsigned int h;
		float depth, width, before, after;
	} cut[HUM_SECTIONS];
	unsigned int ignored;		// blocks left out
} hum_learn;

static bool hum_send;

static void hum_fft_start(uint32_t now, unsigned int settle_ms,
			  unsigned int ms)
{
	hum_fft.blocks = 0;
	hum_fft.running = true;
	hum_fft.taking = false;
	hum_fft.start = now + settle_ms;
	hum_fft.until = now + settle_ms + ms;
}

static bool hum_near_harmonic(int k, float mains)
{
	float f = k * HUM_BIN_HZ;
	float m = (float)(int)(f / mains + 0.5f) * mains;
	return m > 0.0f && fabsf(f - m) <= 2.5f * HUM_BIN_HZ;
}

static float hum_bin_power(int k)
{
	complex_t z = analyzer.fft[k];

	return __real__ z * __real__ z + __imag__ z * __imag__ z;
}

//
// A block's power between the mains lines of either frequency, where a
// string shows and the hum and its cuts do not.  Summed over that many
// bins, steady noise varies by well under a dB from block to block.
//
static float hum_block_level(void)
{
	float sum = 0.0f;

	for (int k = (int)(30.0f / HUM_BIN_HZ); k < HUM_TOP_HZ / HUM_BIN_HZ; k++) {
		if (hum_near_harmonic(k, 50.0f) || hum_near_harmonic(k, 60.0f))
			continue;
		sum += hum_bin_power(k);
	}
	return sum;
}

//
// Add the block to the average unless something was played in it, and
// say whether it was.
//
// A block 6 dB above the average so far has a string in it, and is left
// out.  One 6 dB below it means the blocks so far had one ringing out,
// and the average starts again from this one.  That way a strum before
// the first block is not taken for the quiet that the rest are compared
// with.
//
static bool hum_fft_block(void)
{
	float level = hum_block_level();

	if (hum_fft.blocks) {
		float avg = hum_fft.level;

		if (level > 4.0f * avg) {
			hum_learn.ignored++;
			hum_send = true;
			return false;
		}
		if (level < 0.25f * avg) {
			hum_learn.ignored += hum_fft.blocks;
			hum_send = true;
			hum_fft.blocks = 0;
		}
	}

	hum_fft.blocks++;
	float a = 1.0f / (hum_fft.blocks < HUM_AVG_BLOCKS ?
			  hum_fft.blocks : HUM_AVG_BLOCKS);

	for (int k = 0; k < HUM_BINS; k++)
		hum_fft.power[k] += a * (hum_bin_power(k) - hum_fft.power[k]);
	hum_fft.level += a * (level - hum_fft.level);
	return true;
}

//
// A harmonic's level, and the noise level in the same width beside it,
// both as dBFS of the equivalent sine.  The line is what its five-bin
// Hann lobe holds above that noise; the noise is taken 6 to 20 Hz
// either side, from bins that are not on a harmonic themselves.
//
// The scale: a sine of amplitude A puts A^2 * 1.5 into its lobe once
// |X| is divided by N/4.
//
static void hum_line(float mains, unsigned int h, float *line, float *noise_db)
{
	float scale = 1.0f / (FFT_SIZE / 4.0f) / (FFT_SIZE / 4.0f);
	int k0 = (int)(h * mains / HUM_BIN_HZ + 0.5f);
	float lobe = 0.0f, noise = 0.0f;
	int nn = 0;

	for (int k = k0 - 2; k <= k0 + 2; k++)
		lobe += hum_fft.power[k] * scale;

	for (int d = 4; d <= 14; d++) {
		for (int k = k0 - d; k <= k0 + d; k += 2 * d) {
			if (k <= 0 || k >= HUM_BINS - 1)
				continue;
			if (hum_near_harmonic(k, mains))
				continue;
			noise += hum_fft.power[k] * scale;
			nn++;
		}
	}
	noise = nn ? noise / nn * 5.0f : 0.0f;

	*noise_db = noise > 1e-30f ? 10.0f * log10f(noise / 1.5f) : -200.0f;
	*line = lobe > noise ? 10.0f * log10f((lobe - noise) / 1.5f) : -200.0f;
}

//
// Mains hum is mostly odd harmonics, so 50 against 60 Hz is decided by
// which set of odd harmonics clears its noise.
//
static float hum_score(float mains)
{
	float score = 0.0f;

	for (unsigned int h = 1; h <= 9; h += 2) {
		float line, noise;

		hum_line(mains, h, &line, &noise);
		if (line > noise + 6.0f)
			score += db_to_level(line);
	}
	return score;
}

//
// Each harmonic keeps the slot it had, because a filter's state belongs to
// its slot: putting 180 Hz where 60 Hz was would hand 180 Hz the 60 Hz
// section's state.  A new cut takes a free slot, and an empty slot below
// the last one passes the signal through untouched.
//
static unsigned int hum_slot_h[HUM_SECTIONS];

static void hum_publish(void)
{
	unsigned int next = !hum.active;
	struct hum_set *set = &hum.set[next];
	unsigned int slot_h[HUM_SECTIONS] = { 0 };
	int where[HUM_SECTIONS];
	unsigned int n = 0;

	for (unsigned int i = 0; i < hum_learn.n; i++) {
		where[i] = -1;
		for (unsigned int s = 0; s < HUM_SECTIONS; s++)
			if (hum_slot_h[s] == hum_learn.cut[i].h)
				where[i] = s;
		if (where[i] >= 0)
			slot_h[where[i]] = hum_learn.cut[i].h;
	}
	for (unsigned int i = 0; i < hum_learn.n; i++) {
		for (unsigned int s = 0; where[i] < 0 && s < HUM_SECTIONS; s++) {
			if (!slot_h[s]) {
				where[i] = s;
				slot_h[s] = hum_learn.cut[i].h;
			}
		}
	}

	for (unsigned int s = 0; s < HUM_SECTIONS; s++) {
		if (slot_h[s])
			n = s + 1;
		else
			_biquad_peaking(&set->c[s], 1000.0f, 1.0f, 1.0f);
	}
	for (unsigned int i = 0; i < hum_learn.n; i++) {
		float f = hum_learn.cut[i].h * hum_learn.mains;

		_biquad_peaking(&set->c[where[i]], f,
				f / hum_learn.cut[i].width,
				db_to_A(-hum_learn.cut[i].depth));
	}

	set->n = n;
	memcpy(hum_slot_h, slot_h, sizeof(slot_h));
	smp_store_release(&hum.active, next);
}

//
// Which mains, and a cut for each line that needs one, keeping the
// deepest HUM_SECTIONS if more do - from the average as it stands.
//
// The cuts go at exact multiples of the mains frequency, which holds far
// better than a bin can resolve.  Their width allows for it drifting by a
// few hundredths of a hertz, which a harmonic multiplies.
//
static void hum_choose(void)
{
	float s50 = hum_score(50.0f), s60 = hum_score(60.0f);

	hum_learn.n = 0;
	hum_learn.mains = 0.0f;
	if (s50 <= 0.0f && s60 <= 0.0f)
		return;
	hum_learn.mains = s60 >= s50 ? 60.0f : 50.0f;

	for (unsigned int h = 1; h * hum_learn.mains <= HUM_TOP_HZ; h++) {
		float line, noise;

		hum_line(hum_learn.mains, h, &line, &noise);

		float goal = noise > hum_learn.target ? noise : hum_learn.target;
		float depth = line - goal;
		if (depth < 1.0f)
			continue;
		if (depth > HUM_MAX_DEPTH)
			depth = HUM_MAX_DEPTH;

		unsigned int slot = hum_learn.n;
		if (slot == HUM_SECTIONS) {
			slot = 0;
			for (unsigned int i = 1; i < HUM_SECTIONS; i++)
				if (hum_learn.cut[i].depth < hum_learn.cut[slot].depth)
					slot = i;
			if (hum_learn.cut[slot].depth >= depth)
				continue;
		} else {
			hum_learn.n++;
		}

		hum_learn.cut[slot].h = h;
		hum_learn.cut[slot].depth = depth;
		hum_learn.cut[slot].width = 1.0f + 0.2f * h;
		hum_learn.cut[slot].before = line;
		hum_learn.cut[slot].after = line - depth;
	}
}

//
// A learn leaves the cuts it finds in place until the measurement
// replaces them, so learning again - Target moved - never lets the hum
// back in while it measures.
//
static void hum_learn_start(uint32_t now, unsigned int target)
{
	hum_learn.updates = 0;
	hum_learn.ignored = 0;
	hum_send = true;

	if (target == 128) {		// 0 on the wire: clear, and that is all
		hum_learn.n = 0;
		hum_learn.mains = 0.0f;
		hum_publish();
		return;
	}

	hum_learn.target = -(float)target;
	analyzer_set_mode(ANALYZE_HUM);
	hum_fft_start(now, 100, HUM_LEARN_MS);
}

static void hum_learn_done(void)
{
	hum_fft.running = false;
	analyzer_set_mode(ANALYZE_OFF);
	hum_send = true;
}

// The target the cuts are for, in -dBFS; 0 is none
static unsigned int hum_learnt_target;

static void hum_task(void)
{
	uint32_t now = to_ms_since_boot(get_absolute_time());

	//
	// [DEHUM] being routed is what switches the cuts in.  On, with a
	// target the cuts are not for - the first time, or Target moved -
	// the cuts follow at once from the spectrum last measured, and a
	// learn measures it again.  A learn already running just takes the
	// new target into its next update.
	//
	hum.on = !disable_all && effect_is_routed(&dehum_effect);
	if (hum.on && hum_want_target && hum_want_target != hum_learnt_target) {
		hum_learnt_target = hum_want_target;
		hum_learn.target = -(float)hum_want_target;
		if (hum_learn.mains) {
			hum_choose();
			hum_publish();
			hum_send = true;
		}
		if (!hum_fft.running && !hum_learn_request)
			hum_learn_request = hum_want_target;
	}

	if (hum_report_request) {
		hum_report_request = false;
		hum_send = true;
	}

	// A clear does not need the analyzer, but a learn waits for the tuner
	if (hum_learn_request && !hum_fft.running &&
	    (hum_learn_request == 128 || analyzer_mode == ANALYZE_OFF)) {
		unsigned int target = hum_learn_request;

		hum_learn_request = 0;
		if (target != 128)
			hum_learnt_target = target;
		hum_learn_start(now, target);
		return;
	}

	if (!hum_fft.running)
		return;

	// The tuner has taken the analyzer: stop, and learn again after it
	if (analyzer_mode != ANALYZE_HUM) {
		hum_fft.running = false;
		if (!hum_learn_request)
			hum_learn_request = hum_learnt_target;
		hum_send = true;
		return;
	}

	// Wait out the settling, then take only what comes in after it
	if (!hum_fft.taking) {
		if ((int32_t)(now - hum_fft.start) < 0)
			return;
		hum_fft.taking = true;
		analyzer_skip();
		return;
	}

	// Half-overlapped blocks, which the Hann window weights evenly
	if (analyze_next_block(FFT_SIZE / 2) && hum_fft_block()) {
		hum_choose();
		hum_publish();
		hum_learn.updates++;
		hum_send = true;
	}

	// Done once its time is up and enough quiet blocks are in it
	if ((int32_t)(now - hum_fft.until) >= 0 &&
	    hum_fft.blocks >= HUM_MIN_BLOCKS)
		hum_learn_done();
}

static uint8_t hum_db_byte(float db)
{
	int v = (int)(-db + 0.5f);
	return v < 0 ? 0 : v > 127 ? 127 : v;
}

static void sysex_send_hum(void)
{
	if (!hum_send)
		return;
	if (midi_tx_busy())
		return;

	static const uint8_t header[] = { 0xF0, 0x7D, 0x21 };
	static const uint8_t trailer[] = { 0xF7 };
	uint8_t body[7 + 7 * HUM_SECTIONS], *p = body;
	float ignored = hum_learn.ignored * (FFT_SIZE / 2) / ANALYZE_RATE;

	*p++ = 4;			// layout version
	*p++ = (uint8_t)hum_learn.mains;
	*p++ = hum_learn.n;
	*p++ = hum_learn.updates < 127 ? hum_learn.updates : 127;
	*p++ = hum_fft.running;
	*p++ = hum_db_byte(hum_learn.target);
	*p++ = ignored < 12.7f ? (uint8_t)(ignored * 10.0f + 0.5f) : 127;
	for (unsigned int i = 0; i < hum_learn.n; i++) {
		int depth = (int)(hum_learn.cut[i].depth * 4.0f + 0.5f);
		int width = (int)(hum_learn.cut[i].width * 10.0f + 0.5f);

		*p++ = hum_learn.cut[i].h;
		*p++ = depth >> 7;
		*p++ = depth & 127;
		*p++ = width >> 7;
		*p++ = width & 127;
		*p++ = hum_db_byte(hum_learn.cut[i].before);
		*p++ = hum_db_byte(hum_learn.cut[i].after);
	}

	sysex_tx_start();
	sysex_stream_write(header, sizeof(header));
	sysex_stream_write(body, p - body);
	sysex_stream_write(trailer, sizeof(trailer));
	if (sysex_tx_finish("Sent hum cuts"))
		hum_send = false;
}
