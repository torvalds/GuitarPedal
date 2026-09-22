#ifndef USB_VOLUME_H
#define USB_VOLUME_H

#include "usb-audio.h"

//
// The host's volume and mute, and where they land.
//
// USB audio class 2 puts a feature unit at each end of the device.  The
// speaker unit is what a host turns down when it turns the music down;
// the microphone unit is what it turns down when it turns the capture
// down.  Each carries a master control and one per channel, and the two
// multiply.
//
// Everything here runs on core 0, in usb_audio_task(), where the samples
// cross between USB and the ring buffers.  Core 1 never sees any of it,
// so there is no cross-core publish and no barrier to get right.
//
// **Attenuation only.**  The declared range stops at 0 dB because this
// is a digital gain, and above unity it can only clip the capture or
// push the chain into its own ceiling.  The place to make a quiet pickup
// louder is Trim, which is ahead of everything and analog-referred.
//

//
// UAC2 carries the level in 1/256 dB and reserves this one value for
// silence rather than for a level, so it is checked before the
// conversion rather than handed to it.
//
#define USB_VOL_SILENT	(-32768)

//
// Slewed at the rate the chain slews its own Volume.  A host drags a
// slider through a great many one-dB steps and each one is a step in
// gain; 1/512 per sample settles in about 10ms, which is below a click
// and far quicker than anyone moves a slider.
//
#define USB_VOL_SLEW	(1.0f / 512)

enum usb_fu_channel {
	USB_FU_MASTER,
	USB_FU_LEFT,
	USB_FU_RIGHT,
	USB_FU_CHANNELS
};

struct usb_feature_unit {
	bool mute[USB_FU_CHANNELS];
	int16_t volume[USB_FU_CHANNELS];	// 1/256 dB, as the host sends it
	float gain[2];				// what each channel is heading for
	float slewed[2];			// ...and where it has got to
};

//
// Unity until the host says otherwise.  'volume' is zero, which is
// already 0 dB, but the two floats are not - and a zero gain at boot is
// a pedal that is silent over USB until something touches a slider.
//
static struct usb_feature_unit usb_spk = {
	.gain = { 1.0f, 1.0f }, .slewed = { 1.0f, 1.0f }
};
static struct usb_feature_unit usb_mic = {
	.gain = { 1.0f, 1.0f }, .slewed = { 1.0f, 1.0f }
};

static struct usb_feature_unit *usb_unit(unsigned unit)
{
	return unit == USB_FU_SPK ? &usb_spk : &usb_mic;
}

//
// One channel's gain: its own control plus the master, in dB, or nothing
// at all if either is muted.  Adding the two before the conversion is
// one pow2() rather than two and a multiply.
//
static float usb_fu_gain(const struct usb_feature_unit *fu, unsigned ch)
{
	if (fu->mute[USB_FU_MASTER] || fu->mute[ch])
		return 0.0f;
	if (fu->volume[USB_FU_MASTER] == USB_VOL_SILENT ||
	    fu->volume[ch] == USB_VOL_SILENT)
		return 0.0f;
	return db_to_level((fu->volume[USB_FU_MASTER] + fu->volume[ch])
			   * (1.0f / 256.0f));
}

static void usb_fu_recompute(struct usb_feature_unit *fu)
{
	fu->gain[0] = usb_fu_gain(fu, USB_FU_LEFT);
	fu->gain[1] = usb_fu_gain(fu, USB_FU_RIGHT);
}

int16_t *usb_volume_ptr(unsigned unit, unsigned ch)
{
	return ch < USB_FU_CHANNELS ? &usb_unit(unit)->volume[ch] : NULL;
}

bool *usb_mute_ptr(unsigned unit, unsigned ch)
{
	return ch < USB_FU_CHANNELS ? &usb_unit(unit)->mute[ch] : NULL;
}

bool usb_volume_set(unsigned unit, unsigned ch, int db_256)
{
	struct usb_feature_unit *fu = usb_unit(unit);

	if (ch >= USB_FU_CHANNELS)
		return false;
	//
	// Clamped rather than refused.  A host restores a value it stored
	// against some other range before it reads this one back, and the
	// honest answer to "louder than unity" is unity.
	//
	if (db_256 != USB_VOL_SILENT) {
		if (db_256 < USB_VOL_MIN_DB * 256)
			db_256 = USB_VOL_MIN_DB * 256;
		if (db_256 > USB_VOL_MAX_DB * 256)
			db_256 = USB_VOL_MAX_DB * 256;
	}
	fu->volume[ch] = db_256;
	usb_fu_recompute(fu);
	return true;
}

bool usb_mute_set(unsigned unit, unsigned ch, bool mute)
{
	struct usb_feature_unit *fu = usb_unit(unit);

	if (ch >= USB_FU_CHANNELS)
		return false;
	fu->mute[ch] = mute;
	usb_fu_recompute(fu);
	return true;
}

//
// Interleaved left/right pairs, in place.
//
// The gain never exceeds unity, so the result cannot leave the range the
// input came from and nothing has to saturate.  lrintf() rather than a
// cast because truncation toward zero is half an LSB of DC and a cast is
// what would quietly do it.
//
static void usb_fu_scale(struct usb_feature_unit *fu, s32 *buf, unsigned frames)
{
	for (unsigned i = 0; i < frames; i++) {
		for (unsigned ch = 0; ch < 2; ch++) {
			fu->slewed[ch] += (fu->gain[ch] - fu->slewed[ch])
					* USB_VOL_SLEW;
			buf[i * 2 + ch] = lrintf(buf[i * 2 + ch] * fu->slewed[ch]);
		}
	}
}

//
// int32_t here and s32 inside: the same 32 bits, but int32_t is 'long
// int' on this ABI and s32 is 'int', so the two do not assign without
// saying so.  Each side takes the type its caller already has.
//
void usb_scale_capture(int32_t *buf, unsigned frames)
{
	usb_fu_scale(&usb_mic, (s32 *)buf, frames);
}

void usb_scale_playback(raw_sample_t *buf, unsigned frames)
{
	usb_fu_scale(&usb_spk, &buf->left, frames);
}

#endif // USB_VOLUME_H
