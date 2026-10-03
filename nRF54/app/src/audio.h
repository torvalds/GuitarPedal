#ifndef PEDAL_AUDIO_H
#define PEDAL_AUDIO_H

/* audio.c: the i2s link from the RP2354, started once and polled each pass */
void audio_start(void);
void audio_poll(void);

#endif /* PEDAL_AUDIO_H */
