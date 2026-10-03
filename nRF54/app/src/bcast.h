#ifndef PEDAL_BCAST_H
#define PEDAL_BCAST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * bcast.c: an LE Audio broadcast sink.  Started once Bluetooth is up, polled
 * from the main loop, and asked by audio.c for what to send the pedal.
 */
#ifdef CONFIG_BT
void bcast_start(void);
void bcast_poll(void);
bool bcast_pcm(int32_t *out, int frames);
#else
static inline void bcast_start(void) { }
static inline void bcast_poll(void) { }
static inline bool bcast_pcm(int32_t *out, int frames)
{
	(void)out;
	(void)frames;
	return false;
}
#endif

#endif /* PEDAL_BCAST_H */
