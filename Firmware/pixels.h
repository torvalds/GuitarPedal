#ifndef PIXELS_H
#define PIXELS_H

//
// The status LEDs, as a DMA buffer that never stops.
//
// There is no "send a frame" here, and that is the point.  A DMA channel
// walks a circular buffer forever, handing words to a PIO state machine
// that does nothing but shift them onto a pin; the buffer holds the
// WS2812B waveform pre-encoded, tail-padded with zeroes that the chain
// reads as the inter-frame reset.  Changing a colour means storing three
// words.  Nothing is flushed, nothing is waited for, and there is no
// call that can be forgotten.
//
// Brought over from the knob test board, where it was written and tested
// against eight of these.  The version it replaces had the PIO encode
// the waveform from raw colour, which meant the reset gap - an absence
// of bits, not a pattern of them - could not be expressed at all: it
// lived in software as a deadline the caller had to honour, in a
// different file from the timing it was derived from.  Here it is words
// of zero like everything else.
//
// It also fixes a thing that had nothing to do with LEDs.  The old
// program baked '.clock_div 18' into the .pio file, which meant the bit
// rate moved whenever the system clock did, silently - it was written
// against 150MHz and this firmware has run at 172.8, 230.4 and 153.6.
// bitstream.pio takes the rate as a parameter and derives the divisor
// from clk_sys at run time.  At 153.6MHz the divisor comes out exactly
// 48, so there is no fractional divider and no jitter.
//
// LAYOUT.  One word is 32 output bits at 3.2MHz, so one word is one
// colour byte and, conveniently, exactly 10us:
//
//	words 0..8	three LEDs, three bytes each, green red blue
//	words 9..63	zero - 550us of low, the reset
//
// The size is not a taste.  The DMA's ring mode wraps by masking address
// bits, so the buffer must be a power of two bytes long and aligned to
// its own size.
//
#include "hardware/clocks.h"
#include "hardware/dma.h"

#include "bitstream.pio.h"
#include "ws2812_table.h"

//
// pio0 already runs the two i2s state machines; this is the third, and
// the assignment lives in pedal.c with the others.
//
#define PIXEL_PIO	pio0
#define PIXEL_SM	PIO0_WS2812_SM

//
// Four output bits per WS2812B bit, at the datasheet's 800kHz.
//
// Four rather than three because these are WS2812B-2020, which want a
// zero's high time in 220-380ns.  Three bits gives 417ns, which the
// 2020 reads as a *one* - so every byte arrives as 0xff, all three sit
// at white, and changing what you encode changes nothing at all,
// because none of it is getting through.  See scripts/ws2812_table.py.
//
// 153.6MHz / 3.2MHz is 48 exactly, so this is still an integer divisor.
//
#define PIXEL_BIT_RATE	(800000.0f * 4)

// One colour byte is four bits x eight, so exactly one word.
#define PIXEL_WORD_BITS	32

#define RING_ORDER	8			// log2 of the buffer in bytes
#define RING_WORDS	(1 << (RING_ORDER - 2))
#define PIXEL_WORDS	(NR_LEDS * 3)
#define RESET_WORDS	(RING_WORDS - PIXEL_WORDS)

// 32 bits at 3.2MHz, which comes out round enough to do the arithmetic in.
#define WORD_US		10

//
// How long the reset has to be.
//
// The original WS2812B asks for 50us.  The WS2812B-V5 die - and an
// unknowable assortment of clones - ask for 280us, and getting it wrong
// does not fail cleanly: it flickers, occasionally, under conditions
// that will not reproduce on the bench.  The parts here are marked
// WS2812B-2020-V6 and nobody has confirmed what is actually inside, so
// the buffer is sized for the pessimistic number.  With only three LEDs
// there is room to spare: 55 words of reset is 550us.
//
_Static_assert(RESET_WORDS * WORD_US >= 280, "reset gap too short for a V5 die");
_Static_assert(PIXEL_WORDS < RING_WORDS, "no room left for a reset gap");
_Static_assert((RING_WORDS & (RING_WORDS - 1)) == 0, "the DMA ring must be a power of two");

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

// Aligned to its own size, because the DMA wraps by masking, not counting.
static uint32_t pixel_ring[RING_WORDS] __attribute__((aligned(RING_WORDS * 4)));

//
// Set one LED.  Three stores, no synchronisation, no refresh.
//
// The DMA may be reading these very words as they are written, so a
// single frame can go out half old and half new.  At this frame rate
// that is a fraction of a millisecond of one wrong colour and no eye
// will catch it - but it is the one property this design gives up
// against a double-buffered one, so it is written down rather than left
// to be rediscovered.  Anything that ever needs a frame to be atomic
// needs a second buffer and a pointer swap, not a lock.
//
static void pixels_set(int led, float red, float green, float blue)
{
	if (led < 0 || led >= NR_LEDS)
		return;

	//
	unsigned int r = lrintf(red * 255);
	unsigned int g = lrintf(green * 255);
	unsigned int b = lrintf(blue * 255);

	uint32_t *w = pixel_ring + 3 * led;

	// A WS2812B wants green before red.
	w[0] = ws2812_encode[g];
	w[1] = ws2812_encode[r];
	w[2] = ws2812_encode[b];
}

static void pixels_clear(void)
{
	for (int i = 0; i < NR_LEDS; i++)
		pixels_set(i, 0, 0, 0);
}

//
// One channel, walking the ring forever.
//
// TRANS_COUNT on RP2350 is not a plain count.  The low 28 bits are the
// count and the top four are a mode, where 0xf means ENDLESS: the count
// stops decrementing and the channel simply never finishes.  So
// dma_encode_endless_transfer_count() is literally 0xffffffff, and
// dma_channel_configure() passes it through untouched.
//
// This is an RP2350 feature and did not exist on RP2040, where the idiom
// was two channels chained to each other - a channel cannot chain to
// itself - each re-running the buffer as the other completed.
//
static void pixel_dma_init(int chan)
{
	dma_channel_config c = dma_channel_get_default_config(chan);

	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, pio_get_dreq(PIXEL_PIO, PIXEL_SM, true));
	channel_config_set_ring(&c, false, RING_ORDER);

	dma_channel_configure(chan, &c, &PIXEL_PIO->txf[PIXEL_SM], pixel_ring,
			      dma_encode_endless_transfer_count(), true);
}

static void pixels_init(void)
{
	uint offset = pio_add_program(PIXEL_PIO, &bitstream_program);

	bitstream_program_init(PIXEL_PIO, PIXEL_SM, offset,
			       WS2812_GPIO, PIXEL_BIT_RATE, PIXEL_WORD_BITS);

	//
	// The reset is zero words; the colour region is *encoded* black,
	// which is not the same thing.  Zero is silence, which the chain
	// reads as a reset rather than as a dark LED - the two only look
	// alike from here.
	//
	for (int i = PIXEL_WORDS; i < RING_WORDS; i++)
		pixel_ring[i] = 0;

	pixels_clear();

	pixel_dma_init(dma_claim_unused_channel(true));
}

#endif
