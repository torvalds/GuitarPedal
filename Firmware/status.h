#ifndef STATUS_H
#define STATUS_H

//
// One pending message, and the whole thing is one word.
//
// This is deliberately about as simple as a diagnostic channel can be,
// and the simplicity is the feature rather than a shortcut taken on the
// way to something better.
//
// It exists for the day something goes wrong in a way that cannot be
// reasoned about, and the answer is to sprinkle "got here" markers around
// and see which ones come back - the equivalent of a printf, without a
// printf.  Nothing uses it that way today and hopefully nothing ever
// will.  Which is exactly why it has to stay this small: an unused
// mechanism that is one store stays correct indefinitely, and an unused
// mechanism with rules rots quietly and is discovered to be broken on the
// one day it was needed.
//
// So it has to work from cpu1, the audio core, without locking of any
// kind - and it does, today, without anything being added: a store and a
// compare-exchange, both of which armv8-m does inline, so check-audio.py
// has nothing to object to.  There is no queue, no formatting and no
// second word.  If two things happen, one of them is lost.
//
// **The message must point at static storage** - a string literal, or
// something else that was already there and is not going to change.
// That is the contract, and it is what makes RELAXED the right ordering:
// there is nothing to publish but the pointer, because the bytes it
// points at were visible to both cores before either of them started.
// Formatting into a buffer and reporting that would need release/acquire
// to be correct, and is not supported - on the audio core it is not even
// possible, since check-audio.py refuses the call it would take.
//
// If it ever does need to be cleverer than this - a ring, a value
// alongside the string, per-cpu slots - then this comment is the thing to
// laugh at on the way past.
//
static const char *current_status = "Booting";

//
// Something happened, and it matters more than whatever was pending.
//
// Atomic only to keep the model honest: mixing a plain store with the
// compare-exchange below and the exchange in get_status() is a data race
// however obviously fine the codegen is.  RELAXED compiles to the same
// single 'str' a plain assignment does.
//
static inline void report_status(const char *msg)
{
	__atomic_store_n(&current_status, msg, __ATOMIC_RELAXED);
}

//
// Something happened, but do not talk over anybody.
//
// The difference from report_status() is that this will not overwrite a
// pending message - it speaks only into silence.  So a routine "did the
// thing" note cannot bury the interesting failure that was waiting to be
// read, and the caller decides which of the two it is making.
//
static inline void report_info(const char *msg)
{
	const char *no_message = NULL;
	__atomic_compare_exchange_n(&current_status, &no_message, msg,
		false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

//
// Things that want the one LED's attention, and get it.
//
// 'output_clipped' is the output hitting full scale.  'samples_dropped'
// is the audio core missing the DMA deadline.  'attention_preview' is you turning
// the attention brightness up in the settings, where the only way to
// see what you are setting is for the LED to do it.
//
// The effects' own 'intense' flags are the fourth, and reach the LED
// through show_status() rather than from here - see ui.h.  They used to
// go to a second LED that this board does not have, so a closed gate or
// a compressor working was invisible by any means.
//
// They are deliberately not told apart on the LED, and that is not the
// compromise it looks like.  The two failures sound completely
// different, so the ear does the disambiguating that one bit of light
// cannot: clipping tracks how hard you are playing and can be
// something you actually want, while sample loss means you have
// stacked up too many effects and everything has gone to mush and
// stays that way.  "Something is wrong, listen" is the useful signal;
// which of the two it is, you can hear.
//
// They stay separate *here* so that the code and the MIDI reporting
// still know the difference - the status CCs carry clipping and the drop
// count in their own bits, and the count stays a count.  A smart LED
// will have colours to spend on this and can start telling them apart.
//
// The timing is deliberate too, and worth stating because it looks
// like sloppiness otherwise.  These are set on the audio core at
// 48kHz and cleared by update_ui() at about 25Hz, and that asymmetry
// is the whole mechanism:
//
//  - a single clipped sample lasts 20us, which no eye will ever catch.
//    Holding the flag until the next UI tick stretches it to 40ms,
//    which is the shortest thing worth showing a human at all.
//
//  - clip one sample in a hundred and the LED simply stays lit, because
//    the audio core sets the flag far faster than the UI clears it.  So
//    "occasionally" and "constantly" look different without anyone
//    having to filter, count or average anything: how solid the light
//    looks is already a measure of how often it is happening.
//
// In other words the UI rate is not just where the LED happens to be
// updated - it is what turns an audio-rate event into something on a
// human timescale.  Anything that moves this to a faster loop breaks
// both of those properties.
//
static unsigned int output_clipped;
static unsigned int samples_dropped;
static unsigned int attention_preview;

// How long the LED holds the preview, in update_ui() ticks of ~40ms
#define ATTENTION_PREVIEW_TICKS 12

//
// Meters.  What the pedal can see about its own signal, which is more
// than it has ever been willing to say.
//
// Kept as levels rather than as anything already scaled for the wire,
// because the wire format is the reporting code's business and these are
// maintained at 48kHz by code that should not have to know about it.
//
// All of them decay on their own, so there is no reset handshake with
// cpu0 at all: the audio core keeps them current and whoever asks samples
// whatever is there.  That is also what a meter should do.  A peak that
// never falls tells you what happened once, which is no use at all while
// turning a knob and watching.
//
// Read across cores with no atomics, deliberately.  These are aligned
// 32-bit words, so a read cannot tear, and the worst available outcome is
// a meter reading forty milliseconds out of date.  Same bargain as
// 'samples_dropped' above, for the same reason.
//
static float meter_in;		// input peak, before Trim
static float meter_floor;	// and the quiet level underneath it
static float meter_out;		// output peak, after Volume
static float meter_load;	// of the sample period, the fraction spent working

static const char *get_status(void)
{
	return __atomic_exchange_n(&current_status, NULL, __ATOMIC_RELAXED);
}

#endif
