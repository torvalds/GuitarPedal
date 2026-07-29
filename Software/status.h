#ifndef STATUS_H
#define STATUS_H

static const char *current_status = "Booting";

static inline void report_status(const char *msg)
{
	current_status = msg;
}

// The difference between "report status" and "report info" is that
// informational messages will not overwrite existing pending
// messages. So they update the current status only if it was NULL.
static inline void report_info(const char *msg)
{
	const char *no_message = NULL;
	__atomic_compare_exchange_n(&current_status, &no_message, msg,
		false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

//
// Three things want the one LED's attention, and they get it.
//
// 'output_clipped' is the output hitting full scale.  'samples_dropped'
// is the audio core missing the DMA deadline.  'attention_preview' is you turning
// the attention brightness up in the settings, where the only way to
// see what you are setting is for the LED to do it.
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

static const char *get_status(void)
{
	return __atomic_exchange_n(&current_status, NULL, __ATOMIC_RELAXED);
}

#endif
