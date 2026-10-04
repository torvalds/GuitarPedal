#ifndef HARDWARE_H
#define HARDWARE_H

//
// Bringing the board up, and finding out which board it is.
//
// include/board.h says *which pins*.  This says *how to start them*: the
// i2s state machines and their DMA, the WS2812 program, one debounce
// state machine per switch, the PWM the LEDs are dimmed with, and the
// rotary encoder's quadrature decoder.
//
// It also probes what is on the i2c bus, which is a different question
// and lives here because it is the same one: what is actually out there.
// A fixed build cannot adapt to the board it lands on and does not try -
// the answer goes out in the identity reply so that "is this the board
// this firmware was built for" can be asked of a running pedal.
//
// Ordering note, since this is one translation unit and include order is
// program order: this has to come before midi/sysex.h, because the
// identity reply reports what probe_hardware() found.
//

//
// What gets reported is what was *observed*.  Any inference from it -
// which board this is, how old - belongs to whoever is reading rather
// than in the wire format, so that being wrong about it later costs an
// app change and not a protocol one.
//
static struct {
	bool i2c_codec;		// a codec answered, so it is not strapped
	bool legacy_screen;	// SH1106, 0x3c - a design that is gone
#ifdef NRF54_SWDIO
	uint32_t radio;		// the nRF54's debug port id, or zero
#endif
} hardware;

//
// One i2s state machine's DMA.
//
// All four are the same channel with the direction flipped.  Memory is
// the side that increments and the side the ring wraps, and which of
// read or write that is depends on which way the samples are going.
//
static int i2s_dma_channel(uint sm, bool is_tx, raw_sample_t *buf,
			   uint ring_bits)
{
	int chan = dma_claim_unused_channel(true);
	dma_channel_config c = dma_channel_get_default_config(chan);

	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_read_increment(&c, is_tx);
	channel_config_set_write_increment(&c, !is_tx);
	channel_config_set_dreq(&c, pio_get_dreq(pio0, sm, is_tx));
	channel_config_set_ring(&c, !is_tx, ring_bits);	// the buffer

	pio_sm_clear_fifos(pio0, sm);

	if (is_tx)
		dma_channel_configure(chan, &c, &pio0->txf[sm], buf,
				      0xffffffff, false);
	else
		dma_channel_configure(chan, &c, buf, &pio0->rxf[sm],
				      0xffffffff, false);

	return chan;
}

#ifdef NRF54_SWDIO
#include "nRF54/app/src/i2stest.h"

//
// The radio's audio link, in rings the DMA loops round.  What goes out is
// what the audio core writes a few frames ahead of the DMA - audio, or the
// test pattern (i2stest.h) when Bluetooth Audio's Out is None - and starts
// as the pattern.  What comes back is audio or the radio's copy of the
// pattern, into a ring deep enough - 85 ms - that the main loop checks
// every frame of it.
//
#define NRF54_I2S_TX_SHIFT	8
#define NRF54_I2S_TX_FRAMES	(1 << NRF54_I2S_TX_SHIFT)
#define NRF54_I2S_TX_MASK	(NRF54_I2S_TX_FRAMES - 1)
#define NRF54_I2S_RX_SHIFT	12
#define NRF54_I2S_RX_FRAMES	(1 << NRF54_I2S_RX_SHIFT)
#define NRF54_I2S_RX_MASK	(NRF54_I2S_RX_FRAMES - 1)

static raw_sample_t __attribute__((aligned(sizeof(raw_sample_t) *
					 NRF54_I2S_TX_FRAMES)))
	nrf54_i2s_tx[NRF54_I2S_TX_FRAMES];

// One repeat of the pattern, in RAM for the audio core
static raw_sample_t nrf54_i2s_pattern[I2STEST_FRAMES];
static raw_sample_t __attribute__((aligned(sizeof(raw_sample_t) *
					 NRF54_I2S_RX_FRAMES)))
	nrf54_i2s_rx[NRF54_I2S_RX_FRAMES];
static int nrf54_dma_tx, nrf54_dma_rx;

static struct {
	bool on;
	uint32_t tail;			// frames taken from the ring
	struct i2stest_rx rx;		// since the last report
	uint32_t reported_at;

	//
	// The same frames read as audio, for when it is not the pattern:
	// the peak of each channel, and how often each crosses zero going
	// up, which over a second of a tone is its frequency.
	//
	int32_t peak[2];
	uint32_t ups[2];
	bool was_neg[2];
} nrf54_i2s;

static void nrf54_i2s_audio(int ch, int32_t v)
{
	int32_t a = v < 0 ? -(v + 1) : v;

	if (a > nrf54_i2s.peak[ch])
		nrf54_i2s.peak[ch] = a;
	if (nrf54_i2s.was_neg[ch] && v >= 0)
		nrf54_i2s.ups[ch]++;
	nrf54_i2s.was_neg[ch] = v < 0;
}

// A peak as dBFS, to a tenth of a dB, for the report
static void nrf54_i2s_dbfs(int32_t peak)
{
	int db10 = peak ? (int)lrintf(200.0f *
				      log10f(peak / 2147483648.0f)) : -1200;

	if (db10 < 0) {
		dbg_puts("-");
		db10 = -db10;
	}
	dbg_dec(db10 / 10);
	dbg_puts(".");
	dbg_dec(db10 % 10);
	dbg_puts(" dBFS");
}

//
// Times core 1 has had to move its read or its write position in the
// radio's rings, which skips or repeats audio.  Written by core 1 only.
//
static volatile uint32_t nrf54_i2s_moves, nrf54_i2s_out_moves;

//
// A timing check on audio from the radio, for when a phone plays the
// tone Validation/vernier-tone.py makes: a sine of exactly 48 samples a
// cycle on the left and 47 on the right.
//
// Upward zero crossings on a channel fall on a grid one period apart.
// Samples dropped or repeated move the grid, by that many modulo the
// period, and the two channels together say how many outright, up to
// 1128 either way.  A crossing only counts after a cycle that reached
// -50 dBFS, so silence and noise do not make any.
//
// The grid moves only when the crossings agree on a new place for
// longer than an LC3 frame, 10 ms: a waveform the decoder made up for a
// lost packet can sit a whole number of samples off it for most of one.
// A crossing a fraction of a sample off is that, or the step a jump makes
// through zero, and is counted and passed over; only a run of them long
// enough to be something else - a pause, a resampler - starts a new grid.
//
#define VERNIER_JUMPS	8
#define VERNIER_CONFIRM	12		// crossings, about 12 ms

static const int vernier_period[2] = { 48, 47 };

static struct {
	uint32_t n;			// audio frames seen
	struct {
		int32_t prev, peak;
		bool have;		// a grid to measure against
		uint32_t at;		// a crossing on it: the frame before
		float frac;		// ...and how far past that frame
		int cand, cand_n;	// a whole-sample move, and times seen
		int rag_n;		// crossings off by a fraction, in a row
		int steady_n;		// crossings on the grid, in a row
		bool odd;		// moved, not yet paired with the other
		int d;			// ...by this many, modulo the period
	} ch[2];
	uint32_t steady;		// crossings on the grid
	uint32_t ragged;		// off it by a fraction of a sample
	uint32_t relocks;		// new grids after a run of those
	uint32_t moves;			// nrf54_i2s_moves at the last report
	int nj;
	int32_t jump[VERNIER_JUMPS];	// samples repeated (+) or dropped (-)
} vern;

static void vernier_jump(int32_t d)
{
	if (vern.nj < VERNIER_JUMPS)
		vern.jump[vern.nj] = d;
	vern.nj++;
}

// The one jump that is dl modulo 48 and dr modulo 47, nearest zero
static void vernier_solve(int dl, int dr)
{
	for (int32_t m = 0; m <= 24; m++)
		for (int sign = 1; sign >= -1; sign -= 2) {
			int32_t d = dl + sign * m * 48;

			if ((((d - dr) % 47) + 47) % 47 == 0) {
				vernier_jump(d);
				return;
			}
		}
}

static void vernier_crossing(int c, uint32_t at, float frac)
{
	int p = vernier_period[c];
	float off;
	int d;

	if (!vern.ch[c].have)
		goto anchor;

	// Where it falls against the grid, from -p/2 to p/2
	off = (float)((at - vern.ch[c].at) % p) + frac - vern.ch[c].frac;
	off -= p * floorf(off / p + 0.5f);
	d = (int)lrintf(off);

	if (fabsf(off) < 0.25f) {
		vern.steady++;
		vern.ch[c].steady_n++;
		vern.ch[c].cand_n = vern.ch[c].rag_n = 0;
		return;
	}
	vern.ch[c].steady_n = 0;
	if (fabsf(off - d) > 0.25f) {
		vern.ragged++;
		vern.ch[c].cand_n = 0;
		if (++vern.ch[c].rag_n < 2 * VERNIER_CONFIRM)
			return;
		vern.relocks++;
		vern.ch[c].odd = false;
		goto anchor;
	}
	// Modulo the period: half of one is +p/2 and -p/2 by turns
	d = (d % p + p) % p;
	vern.ch[c].rag_n = 0;
	if (vern.ch[c].cand_n && vern.ch[c].cand == d) {
		vern.ch[c].cand_n++;
	} else {
		vern.ch[c].cand = d;
		vern.ch[c].cand_n = 1;
	}
	if (vern.ch[c].cand_n < VERNIER_CONFIRM)
		return;

	// Moved: the sum of moves is right modulo the period
	vern.ch[c].d = vern.ch[c].odd ? vern.ch[c].d + d : d;
	vern.ch[c].odd = true;
anchor:
	vern.ch[c].at = at;
	vern.ch[c].frac = frac;
	vern.ch[c].have = true;
	vern.ch[c].cand_n = vern.ch[c].rag_n = vern.ch[c].steady_n = 0;
}

static void vernier_frame(int32_t l, int32_t r)
{
	int32_t v[2] = { l, r };

	vern.n++;
	for (int c = 0; c < 2; c++) {
		int32_t a = v[c] < 0 ? -(v[c] + 1) : v[c];

		if (a > vern.ch[c].peak)
			vern.ch[c].peak = a;
		if (vern.ch[c].prev < 0 && v[c] >= 0 &&
		    vern.ch[c].peak > (int32_t)(0.00316f * 2147483648.0f)) {
			vernier_crossing(c, vern.n - 1,
					 (float)-vern.ch[c].prev /
					 ((float)v[c] - (float)vern.ch[c].prev));
			vern.ch[c].peak = 0;
		}
		vern.ch[c].prev = v[c];
	}

	//
	// A jump, once both grids have moved.  Or one only, once the other
	// has been on its grid for as long as a move takes to confirm: a
	// jump that is a whole number of the other's periods.  A lost packet
	// can hold one channel's confirmation back by a frame, so how long
	// it has been is not enough.
	//
	if (vern.ch[0].odd && vern.ch[1].odd) {
		vernier_solve(vern.ch[0].d, vern.ch[1].d);
		vern.ch[0].odd = vern.ch[1].odd = false;
	}
	for (int c = 0; c < 2; c++)
		if (vern.ch[c].odd && !vern.ch[!c].odd &&
		    vern.ch[!c].steady_n >= VERNIER_CONFIRM) {
			vernier_solve(c ? 0 : vern.ch[0].d, c ? vern.ch[1].d : 0);
			vern.ch[c].odd = false;
		}
}

// Not the tone: start again when it is
static void vernier_lost(void)
{
	for (int c = 0; c < 2; c++)
		vern.ch[c].have = vern.ch[c].odd = false;
}

static void dbg_sdec(int32_t v)
{
	dbg_puts(v < 0 ? "-" : "+");
	dbg_dec(v < 0 ? -v : v);
}

static void vernier_report(void)
{
	uint32_t moves = nrf54_i2s_moves;

	if (!vern.steady && !vern.nj && !vern.ragged && !vern.relocks &&
	    moves == vern.moves)
		return;
	dbg_puts("vernier: ");
	dbg_dec(vern.steady);
	dbg_puts(" in step, ");
	dbg_dec(vern.ragged);
	dbg_puts(" ragged, ");
	dbg_dec(vern.relocks);
	dbg_puts(" relocks, ");
	dbg_dec(moves - vern.moves);
	dbg_puts(" moves, ");
	dbg_dec(vern.nj);
	dbg_puts(" jumps");
	for (int i = 0; i < vern.nj && i < VERNIER_JUMPS; i++) {
		dbg_puts(" ");
		dbg_sdec(vern.jump[i]);
	}
	dbg_puts("\n");
	vern.steady = vern.ragged = vern.relocks = 0;
	vern.nj = 0;
	vern.moves = moves;
}

//
// Check what the radio sent back, every frame of it, and say how it went
// on the debug port once a second.  A frame is only counted once both of
// its words are in: the write address rounded down to a whole frame.
//
static void nrf54_i2s_poll(void)
{
	uint32_t head, now;

	if (!nrf54_i2s.on)
		return;

	head = (dma_hw->ch[nrf54_dma_rx].write_addr - (uintptr_t)nrf54_i2s_rx) /
	       sizeof(raw_sample_t);
	// The ring is read after this, not before
	__dmb();
	while ((nrf54_i2s.tail & NRF54_I2S_RX_MASK) !=
	       (head & NRF54_I2S_RX_MASK)) {
		raw_sample_t *f = &nrf54_i2s_rx[nrf54_i2s.tail++ &
						NRF54_I2S_RX_MASK];

		i2stest_check(&nrf54_i2s.rx, f->left, f->right);
		nrf54_i2s_audio(0, f->left);
		nrf54_i2s_audio(1, f->right);
		if ((f->right & 0xff) == 0xc3)
			vernier_lost();
		else
			vernier_frame(f->left, f->right);
	}

	now = to_ms_since_boot(get_absolute_time());
	if (now - nrf54_i2s.reported_at < 1000)
		return;

	//
	// Mostly not the pattern: say what it is as audio instead.
	//
	vernier_report();

	// The audio core's write position, moved: audio skipped or repeated
	static uint32_t out_moves;

	if (nrf54_i2s_out_moves != out_moves) {
		dbg_puts("i2s to the radio: ");
		dbg_dec(nrf54_i2s_out_moves - out_moves);
		dbg_puts(" moves\n");
		out_moves = nrf54_i2s_out_moves;
	}

	if (nrf54_i2s.rx.wrong > nrf54_i2s.rx.frames / 2) {
		for (int ch = 0; ch < 2; ch++) {
			dbg_puts(ch ? ", right " : "i2s from the radio: audio, left ");
			dbg_dec(nrf54_i2s.ups[ch]);
			dbg_puts(" Hz peak ");
			nrf54_i2s_dbfs(nrf54_i2s.peak[ch]);
		}
		dbg_puts("\n");
		goto reset;
	}

	dbg_puts("i2s from the radio: ");
	dbg_dec(nrf54_i2s.rx.frames);
	dbg_puts(nrf54_i2s.rx.phase >= 0 ? " frames, in step, " :
					   " frames, not in step, ");
	dbg_dec(nrf54_i2s.rx.wrong);
	dbg_puts(" wrong (");
	dbg_dec(nrf54_i2s.rx.swapped);
	dbg_puts(" swapped, ");
	dbg_dec(nrf54_i2s.rx.shifted);
	dbg_puts(" shifted), ");
	dbg_dec(nrf54_i2s.rx.slips);
	dbg_puts(" slips\n");
reset:
	nrf54_i2s.peak[0] = nrf54_i2s.peak[1] = 0;
	nrf54_i2s.ups[0] = nrf54_i2s.ups[1] = 0;
	nrf54_i2s.rx.frames = nrf54_i2s.rx.wrong = 0;
	nrf54_i2s.rx.swapped = nrf54_i2s.rx.shifted = 0;
	nrf54_i2s.rx.slips = 0;
	nrf54_i2s.reported_at = now;
}

//
// What the radio sends, as core 1's input: a fixed few frames behind the
// ring's DMA, which runs off the same PIO clock as the codec's and so at
// exactly its rate.  Put back there whenever it is not, which is the
// first call and any time core 1 has fallen behind and lost frames.
//
// The test pattern is silence.  The radio sends audio with the low byte
// of each word zero, and the pattern's right word has 0xC3 there.
//
#define NRF54_I2S_LAG	8

static uint32_t nrf54_i2s_at;

sample_t __audio_func(get_radio_audio_input)(void)
{
	uint32_t head, d;
	raw_sample_t f;

	if (!nrf54_i2s.on)
		return (sample_t) { 0, 0 };

	head = (dma_hw->ch[nrf54_dma_rx].write_addr - (uintptr_t)nrf54_i2s_rx) /
	       sizeof(raw_sample_t);
	d = (head - nrf54_i2s_at) & NRF54_I2S_RX_MASK;
	if (d < NRF54_I2S_LAG / 2 || d > 4 * NRF54_I2S_LAG) {
		if (nrf54_i2s_at)
			nrf54_i2s_moves++;
		nrf54_i2s_at = head - NRF54_I2S_LAG;
	}

	f = nrf54_i2s_rx[nrf54_i2s_at++ & NRF54_I2S_RX_MASK];
	if ((f.right & 0xff) == 0xc3)
		return (sample_t) { 0, 0 };

	return (sample_t) {
		.left = f.left * (1.0f / 2147483648.0f),
		.right = f.right * (1.0f / 2147483648.0f)
	};
}

//
// What the pedal sends the radio, chosen like USB's: a fixed few frames
// ahead of the ring's DMA, put back there whenever it is not.  Audio goes
// with the low byte of each word clear, which is how the radio tells it
// from the pattern.
//
#define NRF54_I2S_LEAD	16

static uint32_t nrf54_i2s_out;

void __audio_func(put_radio_audio_output)(raw_sample_t wet, raw_sample_t dry)
{
	uint32_t tail, d;
	raw_sample_t f;

	if (!nrf54_i2s.on)
		return;

	tail = (dma_hw->ch[nrf54_dma_tx].read_addr - (uintptr_t)nrf54_i2s_tx) /
	       sizeof(raw_sample_t);
	d = (nrf54_i2s_out - tail) & NRF54_I2S_TX_MASK;
	if (d < NRF54_I2S_LEAD / 2 || d > 4 * NRF54_I2S_LEAD) {
		if (nrf54_i2s_out)
			nrf54_i2s_out_moves++;
		nrf54_i2s_out = tail + NRF54_I2S_LEAD;
	}

	switch (radioaudio.output) {
	case LR_None:
		f = nrf54_i2s_pattern[nrf54_i2s_out & I2STEST_MASK];
		break;
	case LR_Wet:
		f = wet;
		break;
	case LR_Dry:
		f = dry;
		break;
	default:
		f.left = wet.left;
		f.right = dry.left;
		break;
	}
	if (radioaudio.output != LR_None) {
		f.left &= ~0xff;
		f.right &= ~0xff;
	}
	nrf54_i2s_tx[nrf54_i2s_out++ & NRF54_I2S_TX_MASK] = f;
}
#else
sample_t get_radio_audio_input(void)
{
	return (sample_t) { 0, 0 };
}

void put_radio_audio_output(raw_sample_t wet, raw_sample_t dry)
{
}
#endif

//
// Every i2s link on the board, started together.
//
// This runs after probe_hardware() because the radio's half depends on
// what that found.  GPIO10 is the radio's data pin on this revision and
// was the footswitch on the one before it, and the two share a board
// file, so driving it on the older board would put a PIO output against
// a switch.
//
// Starting them together is what the one PIO block buys beyond the
// instruction memory: four state machines off one clock, enabled on the
// same cycle, so the two links share a starting edge rather than only a
// rate.
//
static void init_i2s(void)
{
	uint tx_offset = pio_add_program(pio0, &i2s_tx_program);
	uint rx_offset = pio_add_program(pio0, &i2s_rx_program);
	uint32_t sms, dmas;

	//
	// BCLK and FSYNC are the side-set, so they and only they have to be
	// adjacent.  Which of the two is lower is a board fact that reaches
	// the instructions rather than this call; see i2s.pio.
	//
	_Static_assert(I2S_BCLK == I2S_FSYNC + 1 || I2S_FSYNC == I2S_BCLK + 1,
		       "i2s BCLK and FSYNC must be adjacent for the side-set");
#ifdef I2S_FSYNC_BELOW_BCLK
	_Static_assert(I2S_FSYNC < I2S_BCLK,
		       "I2S_FSYNC_BELOW_BCLK disagrees with the pins");
#else
	_Static_assert(I2S_BCLK < I2S_FSYNC,
		       "the side-set wants I2S_FSYNC_BELOW_BCLK for these pins");
#endif

	i2s_tx_program_init(pio0, PIO0_I2S_TX_SM, tx_offset,
			    I2S_BCLK, I2S_FSYNC, I2S_DIN, I2S_DOUT);
	i2s_rx_program_init(pio0, PIO0_I2S_RX_SM, rx_offset,
			    I2S_FSYNC, I2S_DOUT);

	dma_rx = i2s_dma_channel(PIO0_I2S_RX_SM, false, i2s_dma_buf, 7);
	dma_tx = i2s_dma_channel(PIO0_I2S_TX_SM, true, i2s_dma_buf, 7);

	sms = (1u << PIO0_I2S_TX_SM) | (1u << PIO0_I2S_RX_SM);
	dmas = (1u << dma_rx) | (1u << dma_tx);

#ifdef NRF54_SWDIO
	if (hardware.radio) {
		_Static_assert(NRF54_I2S_BCLK == NRF54_I2S_FSYNC + 1 ||
			       NRF54_I2S_FSYNC == NRF54_I2S_BCLK + 1,
			       "the radio's BCLK and FSYNC must be adjacent too");
#ifdef I2S_FSYNC_BELOW_BCLK
		_Static_assert(NRF54_I2S_FSYNC < NRF54_I2S_BCLK,
			       "the radio's pins disagree with I2S_FSYNC_BELOW_BCLK");
#else
		_Static_assert(NRF54_I2S_BCLK < NRF54_I2S_FSYNC,
			       "the radio's pins disagree with I2S_FSYNC_BELOW_BCLK");
#endif
		i2s_tx_program_init(pio0, PIO0_NRF54_I2S_TX_SM, tx_offset,
				    NRF54_I2S_BCLK, NRF54_I2S_FSYNC,
				    NRF54_I2S_DIN, NRF54_I2S_DOUT);
		i2s_rx_program_init(pio0, PIO0_NRF54_I2S_RX_SM, rx_offset,
				    NRF54_I2S_FSYNC, NRF54_I2S_DOUT);

		for (int i = 0; i < I2STEST_FRAMES; i++) {
			nrf54_i2s_pattern[i].left = i2stest_left[i];
			nrf54_i2s_pattern[i].right = i2stest_right(i);
		}
		for (int i = 0; i < NRF54_I2S_TX_FRAMES; i++)
			nrf54_i2s_tx[i] = nrf54_i2s_pattern[i & I2STEST_MASK];
		nrf54_dma_rx = i2s_dma_channel(PIO0_NRF54_I2S_RX_SM, false,
					       nrf54_i2s_rx,
					       3 + NRF54_I2S_RX_SHIFT);
		i2stest_rx_init(&nrf54_i2s.rx);
		nrf54_i2s.on = true;
		_Static_assert(sizeof(raw_sample_t) == 1 << 3,
			       "the transmit ring is 2^3 bytes a frame");
		nrf54_dma_tx = i2s_dma_channel(PIO0_NRF54_I2S_TX_SM, true,
					       nrf54_i2s_tx,
					       3 + NRF54_I2S_TX_SHIFT);

		sms |= (1u << PIO0_NRF54_I2S_TX_SM) |
		       (1u << PIO0_NRF54_I2S_RX_SM);
		dmas |= (1u << nrf54_dma_rx) | (1u << nrf54_dma_tx);
	}
#endif

	//
	// The DMA first, so the transmit fifos have something in them
	// before the state machines start reading them.
	//
	// RX and TX start at the same point, together.  But TX will fill up
	// the PIO buffers and move ahead, while RX will be waiting for the
	// first samples to come in, so it naturally falls behind - and in a
	// circular buffer that is the same as being ahead.
	//
	dma_start_channel_mask(dmas);
	pio_enable_sm_mask_in_sync(pio0, sms);
}

static void init_ws2812(void)
{
#ifdef WS2812_GPIO
	pixels_init();
#endif
}

// Initialize a pin for input, pulled up
static void init_sw_pin(PIO pio, int pin)
{
	gpio_init(pin);
	gpio_set_dir(pin, false);
	gpio_pull_up(pin);
	pio_gpio_init(pio, pin);
}

// I have no good way to detect USB when in USB host mode.
//
// In a perfect world, I would have a GPIO that would tell
// me whether the power is provided by the 9V guitar power
// supply or the USB line, but ...
static inline bool usb_is_connected(void)
{
	return tud_ready();
}

// We use PIO1 for the switches.
//
// They share the same program, just a separate state machine
// for each pin - state machine N is switch id N, see switch.h.
static void switch_irq(void)
{
	PIO pio = pio1;

	for (int sw = 0; sw < NR_SWITCHES; sw++) {
		if (pio_sm_is_rx_fifo_empty(pio, sw))
			continue;

		int bit = pio_sm_get(pio, sw) ? LONGPRESS(sw) : sw;
		switch_val |= 1u << bit;
	}

	user_interaction = 1;
}


//
// What this firmware found itself running on.
//
// Probed once at boot, and the question it answers is not "which board
// is this" - the pin map already settled that at compile time, and a
// fixed build cannot adapt to landing on the wrong one anyway.  It is
// the narrower question of what is on the far end of the FFC, which the
// build genuinely does not know and must not guess.
//
// The audio-jacks board is a separate board joined by a cable, and it
// comes in two flavours: a TAC5112, which was never wired for stereo,
// and a TAC5242, which was.  Either can be paired with either MCU board,
// so which one is present is not a property of the build and cannot be.
// The TAC5112 needs its control registers set up over i2c0 regardless,
// so the firmware has to find out - and having found out, it can say so.
//
// **Mono against stereo is the difference a person actually notices**,
// and it is this one.  It is not the codec's doing - both parts are
// stereo-capable - it is that the older board only ever routed one
// channel.
//
// The SH1106 screen on i2c1 is the same kind of statement: it belonged
// to a design that is gone, the code for it went with it, and the part
// still answers when addressed.
//
// The eeprom used to be probed here too, and is not any more.  It was
// the scene store, which now lives in the RP2354's own flash; after that
// it survived a while as a hint about which board this was, and it was
// never a good one.  It sat on whichever board happened to carry it
// across a couple of revisions, so its presence identified nothing, and
// the reading was not even stable - see the issue list.

//
// Whether an effect has the board under it that it asked for.
//
// The generator wrote the list of who wants what; board.h turned each
// name into an answer.  Expanded here because this is the first place
// that has both - the effect headers are compiled by the host bench
// too, and there is no board under that.
//
// An absent effect is still in effects[] and still has its pots: taking
// it out would renumber every effect above it, and the ids are the
// wire.  What it loses is its card in the app and its pots in the
// state dump.
//
static bool effect_present(unsigned int id)
{
	switch (id) {
#define HW_NEEDS(id, what)	case id: return !!(HAVE_##what);
	EFFECT_HW_LIST
#undef HW_NEEDS
	default:
		return true;
	}
}

static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr)
{
	uint8_t byte;

	// One byte, harmless to anything that does answer, and a timeout
	// rather than a hang if the bus is being held down.
	return i2c_read_timeout_us(i2c, addr, &byte, 1, false, 2000) == 1;
}

static void probe_hardware(void)
{
	hardware.i2c_codec = i2c_probe(TAC5112_I2C);
#ifdef SH1106_I2C
	hardware.legacy_screen = i2c_probe(SH1106_I2C);
#endif

	//
	// Say what we are before USB exists, because the name is part of
	// how a person tells two pedals apart and the host may already be
	// attached and waiting.
	//
	// The board name is compile-time and what the probe means is not,
	// so this is where the two meet.
	//
	// **What that bit means is a board fact, so the board says it.**
	// The probe establishes one thing - a codec answered on i2c rather
	// than being strapped - and the consequence differs: on the split
	// family the i2c part is the TAC5112, which is mono; on minimal it
	// is the TAC5212, which is DC-coupled where the strapped one has a
	// corner at 10Hz.  Both are worth knowing and neither is the other.
	//
	// A board header that defines CODEC_I2C_PRODUCT gives its i2c-codec
	// build that name, followed by the serial's last four digits.
	//
#ifdef CODEC_I2C_PRODUCT
	if (hardware.i2c_codec) {
		static char product[sizeof(CODEC_I2C_PRODUCT) + 5];
		char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
		size_t len;

		pico_get_unique_board_id_string(id, sizeof(id));
		len = strlen(id);
		memcpy(product, CODEC_I2C_PRODUCT " ", sizeof(CODEC_I2C_PRODUCT));
		memcpy(product + sizeof(CODEC_I2C_PRODUCT), id + len - 4, 5);
		usb_set_product(product);
	} else
#endif
	usb_set_product(hardware.i2c_codec
			? PEDAL_BOARD_NAME " " CODEC_I2C_DESC " Pedal"
			: PEDAL_BOARD_NAME " " CODEC_STRAPPED_DESC " Pedal");

	//
	// Stated rather than judged, for the same reason the wire format
	// carries the observation: "early" is an inference and it is the
	// reader's to draw.
	//
	report_status(hardware.i2c_codec
		      ? "Codec on i2c: " CODEC_I2C_DESC
		      : "Codec strapped: " CODEC_STRAPPED_DESC);
	if (hardware.legacy_screen)
		report_status("A screen answered on i2c");

#ifdef NRF54_SWDIO
	//
	// After the i2c probes because it takes a millisecond of reset,
	// and this is the last thing before USB.
	//
	// It says the chip is there, not that it is running anything -
	// the debug port answers on a part that has never been
	// programmed, and that is most of what makes it worth asking.
	//
	hardware.radio = nrf54_probe();
	report_status(hardware.radio ? "The radio answered on SWD"
				    : "No radio on SWD");

	//
	// The MIDI port to the radio, which only exists when a radio
	// answered.  Its i2s waits for the same answer and is started by
	// init_i2s(), with the codec's.
	//
	if (hardware.radio) {
		nrf54_uart_init();
		nrf54_update_start();
	}
#endif
}
static uint debounce_offset;

static void init_sw_pins(void)
{
	PIO pio = pio1;

	debounce_offset = pio_add_program(pio, &debounce_program);


	//
	// Same PIO program for every switch, one state machine each,
	// walked in switch id order so that state machine N really is
	// switch N.  switch_irq() relies on that and has no other way
	// to know which pin a fifo entry came from.
	//
	// Only the ones soldered to this board.  Anything on the
	// expression jack is an accessory rather than bring-up, and waits
	// for init_exp_switches() and the setting that decides it.
	//
	for (int sw = 0; sw < NR_ONBOARD_SWITCHES; sw++) {
		init_sw_pin(pio, switch_gpio[sw]);
		debounce_program_init(pio, sw, debounce_offset, switch_gpio[sw]);
	}


	irq_set_exclusive_handler(PIO1_IRQ_0, switch_irq);
	irq_set_enabled(PIO1_IRQ_0, true);
}

// Any pin, not just an LED_GPIO: a board with smart LEDs still has an
// expression jack that may have something to light on it.  A board with
// neither has nothing to dim at all.
#if !defined(WS2812_GPIO) || defined(EXP_TIP_GPIO)
static void init_one_pwm_pin(int pin)
{
	unsigned int slice = pwm_gpio_to_slice_num(pin);

	gpio_set_function(pin, GPIO_FUNC_PWM);
	pwm_set_wrap(slice, PWM_WRAP);
	pwm_set_gpio_level(pin, 0);
	pwm_set_enabled(slice, true);
}
#endif

static void init_pwm_pins(void)
{
#ifdef WS2812_GPIO
	//
	// Nothing to dim.  A board with smart LEDs has no PWM one - and on
	// the usb-stomp board LED_GPIO is not an LED at all, it is the
	// stomp switch, shorting to ground with no series resistor.  So
	// this must not run there, and neither must the boot lamp in
	// main().  Both are left in place for the boards that do have it.
	//
	return;
#else
	init_one_pwm_pin(LED_GPIO);

	//
	// Full, not off, and it stays that way until the first UI tick.
	//
	// main() lights this pin as a plain GPIO before anything else runs,
	// and taking it over for PWM would drop it - so the level is put
	// back up here and the LED stays on across the handover.  What that
	// buys is a lamp that means "started, not finished yet": it comes on
	// at the first instruction and goes to its real brightness when
	// set_led() first runs, which is inside the main loop.
	//
	// So a pedal that hangs during boot sits there lit, and a pedal that
	// never got as far as main() sits there dark.  See the boot comment
	// in main() for why that distinction is the one worth having.
	//
	pwm_set_gpio_level(LED_GPIO, PWM_WRAP);
#endif
}

static void init_i2c_bus(i2c_inst_t *i2c, int kbps, int sda, int scl)
{
	i2c_init(i2c, kbps * 1000);
	gpio_set_function(sda, GPIO_FUNC_I2C);
	gpio_set_function(scl, GPIO_FUNC_I2C);
	gpio_pull_up(sda);
	gpio_pull_up(scl);
}

//
// The one rotary encoder.  Turning it changes the selected pot's value,
// and that is all a turn has ever meant to anything but the old EQ.
//
// Accumulated by the interrupt, drained by update_ui().  There used to
// be a second encoder for picking the effect; it is gone, and picking
// the effect is done over MIDI.
//
static volatile int rotary_value;

#ifdef ROTARY_A_GPIO
static void rotary_irq(void)
{
	// Initial impossible previous value
	static int prev_value = 4;
	static const int lookup[32] = {
		// CW: 00 -> 10 -> 11 -> 01 -> 00
		[2] = 1, [11] = 1, [13] = 1, [4] = 1,
		// CCW: 00 -> 01 -> 11 -> 10 -> 00
		[1] = -1, [7] = -1, [14] = -1, [8] = -1
	};

	while (!pio_sm_is_rx_fifo_empty(pio2, ROTARY_SM)) {
		int curr = pio_sm_get(pio2, ROTARY_SM) & 3;
		int prev = prev_value;

		int val = lookup[(prev << 2) | curr];
		prev_value = curr;

		if (!val)
			continue;

		rotary_value += val;
	}
	user_interaction = 1;
}

// We'll use a separate PIO program for the rotary
// encoder pins eventually
static void init_rotary_encoder(void)
{
	PIO pio = pio2;
	uint offset = pio_add_program(pio, &rotary_program);

	// The program reads both pins of the quadrature pair starting
	// at the one it is given, so A and B have to stay adjacent.
	_Static_assert(ROTARY_B_GPIO == ROTARY_A_GPIO + 1,
		       "the quadrature pair has to be adjacent");

	init_sw_pin(pio, ROTARY_A_GPIO);
	init_sw_pin(pio, ROTARY_B_GPIO);
	rotary_program_init(pio, ROTARY_SM, offset, ROTARY_A_GPIO);

	irq_set_exclusive_handler(PIO2_IRQ_0, rotary_irq);
	irq_set_enabled(PIO2_IRQ_0, true);
}
#endif


#endif
