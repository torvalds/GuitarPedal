#ifndef PEDAL_SINK_H
#define PEDAL_SINK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * LE Audio in, from a broadcast (bcast.c) or a phone's unicast stream
 * (unicast.c), decoded by sink.c and sent to the pedal by audio.c; and
 * out, to a phone, by source.c.
 *
 * sink_start() is called once Bluetooth is up and starts both sources,
 * sink_poll() from the main loop decodes, and sink_pcm() is what audio.c
 * asks for.  The rest is between sink.c and the two sources.
 */
#ifdef CONFIG_BT
#include <zephyr/bluetooth/audio/audio.h>

void sink_start(void);
void sink_poll(void);
bool sink_pcm(int32_t *out, int frames);

/*
 * What the radio is available for, each way, which unicast.c also
 * advertises: music in, and both halves of a call.
 */
#define SINK_AVAILABLE	(BT_AUDIO_CONTEXT_TYPE_MEDIA | \
			 BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)
#define SOURCE_AVAILABLE BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL

enum sink_from { SINK_BCAST, SINK_UNICAST, SINK_FROMS };

#define SINK_FRAME_MAX	155	/* the largest LC3 frame at 48 kHz */

/*
 * Which channels a source is sending, as bits: 1 is left, 2 is right.
 * Zero when it is sending nothing.  A source sending one channel is heard
 * in both.
 */
void sink_feeding(enum sink_from from, unsigned int chans, int rate);

/* One LC3 frame of one channel; no bytes for one that was lost */
void sink_put(enum sink_from from, int ch, const uint8_t *data, uint16_t len);

/*
 * source.c: LE Audio out, what the pedal sends the radio over i2s,
 * encoded for a phone that hears it as a headset's microphone.
 */
struct bt_bap_stream;

int source_start(void);
void source_put(int32_t left, int32_t right);
void source_poll(void);
void source_started(struct bt_bap_stream *stream, int rate, int octets);
void source_stopped(struct bt_bap_stream *stream);
void source_sent(struct bt_bap_stream *stream);

int bcast_start(void);
void bcast_pause(bool pause);
int unicast_start(void);
void unicast_poll(void);
#else
static inline void sink_start(void) { }
static inline void sink_poll(void) { }
static inline bool sink_pcm(int32_t *out, int frames)
{
	(void)out;
	(void)frames;
	return false;
}
static inline void source_put(int32_t left, int32_t right) { }
#endif

#endif /* PEDAL_SINK_H */
