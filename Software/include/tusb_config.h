#include "pico.h"

#define CFG_TUSB_RHPORT0_MODE		OPT_MODE_DEVICE
#define CFG_TUD_ENABLED			1
#define CFG_TUD_AUDIO			1
#define CFG_TUD_MIDI			1
#define CFG_TUD_ENDPOINT0_SIZE		64
#define CFG_TUD_MIDI_RX_BUFSIZE		64
#define CFG_TUD_MIDI_TX_BUFSIZE		8192
#define CFG_TUD_MIDI_EP_BUFSIZE		64

//
// One sample more than nominal, and the extra one is the whole point.
//
// This is an asynchronous capture endpoint: the pedal samples on its own
// clock and the host frames on its own, and the two will never agree.
// The way that is supposed to work is that the device sends however many
// samples it actually has - 47, 48, 49 - and the host takes them.
//
// Sized at exactly 48 it can only ever send *fewer*, so a pedal running
// the tiniest bit fast has nowhere to put the surplus, the ring fills,
// and get_output_samples() throws away a chunk to catch up.  Which is
// audible as a step in the waveform, several times a second.
//
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX (49 * 4 * 2)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (48 * 4 * 2 * 2)
#define CFG_TUD_AUDIO_ENABLE_EP_IN (1)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX (4)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX (32)

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX (48 * 4 * 2)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (48 * 4 * 2 * 2)
#define CFG_TUD_AUDIO_ENABLE_EP_OUT (1)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX (4)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX (32)
