#include "pico/stdlib.h"
#include "pico.h"

#include "board.h"
#include "tusb.h"

#include "midi.h"

#include "usb-audio.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device =
{
	.bLength		= sizeof(tusb_desc_device_t),
	.bDescriptorType	= TUSB_DESC_DEVICE,
	.bcdUSB			= 0x0200,

	// Use Interface Association Descriptor (IAD) for Audio
	.bDeviceClass		= TUSB_CLASS_MISC,
	.bDeviceSubClass	= MISC_SUBCLASS_COMMON,
	.bDeviceProtocol	= MISC_PROTOCOL_IAD,
	.bMaxPacketSize0	= CFG_TUD_ENDPOINT0_SIZE,

	.idVendor		= 0xFFFF,
	.idProduct		= 0x0003, // Changed to Composite Device
	.bcdDevice		= 0x0100,

	.iManufacturer		= 0x01,
	.iProduct		= 0x02,
	.iSerialNumber		= 0x03,

	.bNumConfigurations	= 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
	return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum {
	ITF_NUM_AUDIO_CONTROL = 0,
	ITF_NUM_AUDIO_STREAMING_SPK,
	ITF_NUM_AUDIO_STREAMING_MIC,
	ITF_NUM_MIDI,
	ITF_NUM_MIDI_STREAMING,
	ITF_NUM_TOTAL
};

// Unit numbers are arbitrary selected
#define UAC2_ENTITY_CLOCK               0x04
// Speaker path
#define UAC2_ENTITY_SPK_INPUT_TERMINAL  0x05
#define UAC2_ENTITY_SPK_FEATURE_UNIT    0x06
#define UAC2_ENTITY_SPK_OUTPUT_TERMINAL 0x07
// Microphone path
#define UAC2_ENTITY_MIC_INPUT_TERMINAL  0x01
#define UAC2_ENTITY_MIC_FEATURE_UNIT    0x02
#define UAC2_ENTITY_MIC_OUTPUT_TERMINAL 0x03

#define TUD_AUDIO20_HEADSET_STEREO_DESC_LEN	\
	(TUD_AUDIO20_DESC_IAD_LEN +		\
	 TUD_AUDIO20_DESC_STD_AC_LEN +		\
	 TUD_AUDIO20_DESC_CS_AC_LEN +		\
	 TUD_AUDIO20_DESC_CLK_SRC_LEN +		\
	 TUD_AUDIO20_DESC_INPUT_TERM_LEN +	\
	 TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) +	\
	 TUD_AUDIO20_DESC_OUTPUT_TERM_LEN +	\
	 TUD_AUDIO20_DESC_INPUT_TERM_LEN +	\
	 TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) +	\
	 TUD_AUDIO20_DESC_OUTPUT_TERM_LEN +	\
	/* Interface 1, Alternate 0 */		\
	 TUD_AUDIO20_DESC_STD_AS_LEN +		\
	/* Interface 1, Alternate 1 */		\
	 TUD_AUDIO20_DESC_STD_AS_LEN +		\
	 TUD_AUDIO20_DESC_CS_AS_INT_LEN +	\
	 TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN +	\
	 TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN +	\
	 TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN +	\
	/* Interface 2, Alternate 0 */		\
	 TUD_AUDIO20_DESC_STD_AS_LEN +		\
	/* Interface 2, Alternate 1 */		\
	 TUD_AUDIO20_DESC_STD_AS_LEN +		\
	 TUD_AUDIO20_DESC_CS_AS_INT_LEN +	\
	 TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN +	\
	 TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN +	\
	 TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define TUD_AUDIO20_HEADSET_STEREO_DESCRIPTOR(_stridx, _epout, _epin, _epsize) \
	/* Standard Interface Association Descriptor (IAD) */		\
	TUD_AUDIO20_DESC_IAD(						\
		/*_firstitf*/ ITF_NUM_AUDIO_CONTROL,			\
		/*_nitfs*/ 3,				\
		/*_stridx*/ 0x00),					\
	/* Standard AC Interface Descriptor(4.7.1) */			\
	TUD_AUDIO20_DESC_STD_AC(					\
		/*_itfnum*/ ITF_NUM_AUDIO_CONTROL,			\
		/*_nEPs*/ 0x00,						\
		/*_stridx*/ _stridx),					\
	/* Class-Specific AC Interface Header Descriptor(4.7.2) */	\
	TUD_AUDIO20_DESC_CS_AC(						\
		/*_bcdADC*/ 0x0200,					\
		/*_category*/ AUDIO20_FUNC_HEADSET,			\
		/*_totallen*/ TUD_AUDIO20_DESC_CLK_SRC_LEN +		\
			TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) +		\
			TUD_AUDIO20_DESC_INPUT_TERM_LEN +		\
			TUD_AUDIO20_DESC_OUTPUT_TERM_LEN +		\
			TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) +		\
			TUD_AUDIO20_DESC_INPUT_TERM_LEN +		\
			TUD_AUDIO20_DESC_OUTPUT_TERM_LEN,		\
		/*_ctrl*/ AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS),	\
	/* Clock Source Descriptor(4.7.2.1) */				\
	TUD_AUDIO20_DESC_CLK_SRC(					\
		/*_clkid*/ UAC2_ENTITY_CLOCK,				\
		/*_attr*/ AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,		\
		/*_ctrl*/ (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), \
		/*_assocTerm*/ 0x00,					\
		/*_stridx*/ 0x00),					\
	/* Input Terminal Descriptor(4.7.2.4) */			\
	TUD_AUDIO20_DESC_INPUT_TERM(					\
		/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,		\
		/*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING,		\
		/*_assocTerm*/ 0x00,					\
		/*_clkid*/ UAC2_ENTITY_CLOCK,				\
		/*_nchannelslogical*/ 0x02,				\
		/*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,	\
		/*_idxchannelnames*/ 0x00,				\
		/*_ctrl*/ 0 * (AUDIO20_CTRL_R << AUDIO20_IN_TERM_CTRL_CONNECTOR_POS), \
		/*_stridx*/ 0x00),					\
	/* Feature Unit Descriptor(4.7.2.8) */				\
	TUD_AUDIO20_DESC_FEATURE_UNIT(					\
		/*_unitid*/ UAC2_ENTITY_SPK_FEATURE_UNIT,		\
		/*_srcid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,		\
		/*_stridx*/ 0x00,					\
		/*_ctrlch0master*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
		/*_ctrlch1*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
		/*_ctrlch2*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)), \
	/* Output Terminal Descriptor(4.7.2.5) */			\
	TUD_AUDIO20_DESC_OUTPUT_TERM(					\
		/*_termid*/ UAC2_ENTITY_SPK_OUTPUT_TERMINAL,		\
		/*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES,		\
		/*_assocTerm*/ 0x00,					\
		/*_srcid*/ UAC2_ENTITY_SPK_FEATURE_UNIT,		\
		/*_clkid*/ UAC2_ENTITY_CLOCK,				\
		/*_ctrl*/ 0x0000,					\
		/*_stridx*/ 0x00),					\
	/* Input Terminal Descriptor(4.7.2.4) */			\
	TUD_AUDIO20_DESC_INPUT_TERM(					\
		/*_termid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL,		\
		/*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC,		\
		/*_assocTerm*/ 0x00,					\
		/*_clkid*/ UAC2_ENTITY_CLOCK,				\
		/*_nchannelslogical*/ 0x02,				\
		/*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,	\
		/*_idxchannelnames*/ 0x00,				\
		/*_ctrl*/ AUDIO20_CTRL_R << AUDIO20_IN_TERM_CTRL_CONNECTOR_POS, \
		/*_stridx*/ 0x00),					\
	/* Feature Unit Descriptor(4.7.2.8) */				\
	TUD_AUDIO20_DESC_FEATURE_UNIT(					\
		/*_unitid*/ UAC2_ENTITY_MIC_FEATURE_UNIT,		\
		/*_srcid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL,		\
		/*_stridx*/ 0x00,					\
		/*_ctrlch0master*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
		/*_ctrlch1*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
		/*_ctrlch2*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS \
				| AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)), \
	/* Output Terminal Descriptor(4.7.2.5) */			\
	TUD_AUDIO20_DESC_OUTPUT_TERM(					\
		/*_termid*/ UAC2_ENTITY_MIC_OUTPUT_TERMINAL,		\
		/*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING,		\
		/*_assocTerm*/ 0x00,					\
		/*_srcid*/ UAC2_ENTITY_MIC_FEATURE_UNIT,		\
		/*_clkid*/ UAC2_ENTITY_CLOCK,				\
		/*_ctrl*/ 0x0000,					\
		/*_stridx*/ 0x00),					\
	/* Standard AS Interface Descriptor(4.9.1) */			\
	/* Interface 1, Alternate 0 - default alternate setting with 0 bandwidth */ \
	TUD_AUDIO20_DESC_STD_AS_INT(					\
		/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_SPK),	\
		/*_altset*/ 0x00,					\
		/*_nEPs*/ 0x00,						\
		/*_stridx*/ _stridx),					\
	/* Standard AS Interface Descriptor(4.9.1) */			\
	/* Interface 1, Alternate 1 - alternate interface for data streaming */ \
	TUD_AUDIO20_DESC_STD_AS_INT(					\
		/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_SPK),	\
		/*_altset*/ 0x01,					\
		/*_nEPs*/ 0x01,						\
		/*_stridx*/ _stridx),					\
	/* Class-Specific AS Interface Descriptor(4.9.2) */		\
	TUD_AUDIO20_DESC_CS_AS_INT(					\
		/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,		\
		/*_ctrl*/ AUDIO20_CTRL_NONE,				\
		/*_formattype*/ AUDIO20_FORMAT_TYPE_I,			\
		/*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM,		\
		/*_nchannelsphysical*/ 0x02,				\
		/*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,	\
		/*_stridx*/ 0x00),					\
	/* Type I Format Type Descriptor(2.3.1.6 - Audio Formats) */	\
	TUD_AUDIO20_DESC_TYPE_I_FORMAT(					\
		CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,	\
		CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),		\
	/* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */ \
	TUD_AUDIO20_DESC_STD_AS_ISO_EP(					\
		/*_ep*/ _epout,						\
		/*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS	\
			| (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS		\
			| (uint8_t)TUSB_ISO_EP_ATT_DATA),		\
		/*_maxEPsize*/ _epsize,					\
		/*_interval*/ 0x01),					\
	/* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */ \
	TUD_AUDIO20_DESC_CS_AS_ISO_EP(					\
		/*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, \
		/*_ctrl*/ AUDIO20_CTRL_NONE,				\
		/*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, \
		/*_lockdelay*/ 0x0000),					\
	/* Standard AS Interface Descriptor(4.9.1) */			\
	/* Interface 2, Alternate 0 - default alternate setting with 0 bandwidth */ \
	TUD_AUDIO20_DESC_STD_AS_INT(					\
		/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_MIC),	\
		/*_altset*/ 0x00,					\
		/*_nEPs*/ 0x00,						\
		/*_stridx*/ _stridx),					\
	/* Standard AS Interface Descriptor(4.9.1) */			\
	/* Interface 2, Alternate 1 - alternate interface for data streaming */ \
	TUD_AUDIO20_DESC_STD_AS_INT(					\
		/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_MIC),	\
		/*_altset*/ 0x01,					\
		/*_nEPs*/ 0x01,						\
		/*_stridx*/ _stridx),					\
	/* Class-Specific AS Interface Descriptor(4.9.2) */		\
	TUD_AUDIO20_DESC_CS_AS_INT(					\
		/*_termid*/ UAC2_ENTITY_MIC_OUTPUT_TERMINAL,		\
		/*_ctrl*/ AUDIO20_CTRL_NONE,				\
		/*_formattype*/ AUDIO20_FORMAT_TYPE_I,			\
		/*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM,		\
		/*_nchannelsphysical*/ 0x02,				\
		/*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,	\
		/*_stridx*/ 0x00),					\
	/* Type I Format Type Descriptor(2.3.1.6 - Audio Formats) */	\
	TUD_AUDIO20_DESC_TYPE_I_FORMAT(					\
		CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX,	\
		CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX),		\
	/* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */ \
	TUD_AUDIO20_DESC_STD_AS_ISO_EP(					\
		/*_ep*/ _epin,						\
		/*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS	\
			| (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS		\
			| (uint8_t)TUSB_ISO_EP_ATT_DATA),		\
		/*_maxEPsize*/ _epsize,					\
		/*_interval*/ 0x01),					\
	/* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */ \
	TUD_AUDIO20_DESC_CS_AS_ISO_EP(					\
		/*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, \
		/*_ctrl*/ AUDIO20_CTRL_NONE,				\
		/*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, \
		/*_lockdelay*/ 0x0000)

#define IAD_DESC_LEN 8
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO20_HEADSET_STEREO_DESC_LEN + TUD_MIDI_DESC_LEN + IAD_DESC_LEN)

#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_IN 0x81
#define EPNUM_MIDI_OUT 0x02
#define EPNUM_MIDI_IN 0x82

enum {
	STRID_LANGID = 0,
	STRID_MANUFACTURER,
	STRID_PRODUCT,
	STRID_SERIAL,
	STRID_AUDIO_INTERFACE,
	STRID_MIDI_INTERFACE
};

uint8_t const desc_configuration[] =
{
	// Config number, interface count, string index, total length, attribute, power in mA
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

	// Interface number, string index, EP Out & EP In address, EP size
	TUD_AUDIO20_HEADSET_STEREO_DESCRIPTOR(
		/*_stridx*/ 0,
		/*_epout*/ EPNUM_AUDIO_OUT,
		/*_epin*/ EPNUM_AUDIO_IN,
		/*_epsize*/ CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX),

	// Interface Association Descriptor for MIDI
	// bLength, bDescriptorType, bFirstInterface, bInterfaceCount, bFunctionClass, bFunctionSubClass, bFunctionProtocol, iFunction
	8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_MIDI, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_MIDI_STREAMING, 0, 0,

	TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, STRID_MIDI_INTERFACE, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, CFG_TUD_MIDI_EP_BUFSIZE)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+


#define DESC_STRING(len) (2*((len)+1)+(TUSB_DESC_STRING<<8))
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	static const uint16_t reply[6][15] = {
		{ DESC_STRING(1), 0x0409 }, // English
		{ DESC_STRING(5), 'L', 'i', 'n', 'u', 's' },
		{ DESC_STRING(11), 'L', 'i', 'n', 'u', 's', ' ', 'P', 'e', 'd', 'a', 'l' },
		{ DESC_STRING(1), '0' },
		{ DESC_STRING(4), 'U', 'A', 'C', '2' },
		{ DESC_STRING(4), 'M', 'I', 'D', 'I' },
	};
	if (index >= 6)
		return NULL;
	return reply[index];
}

//--------------------------------------------------------------------+
// Audio Callbacks
//--------------------------------------------------------------------+

int init_usb(void)
{
	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO
	};
	tusb_init(0, &dev_init);
	return 0;
}

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *pBuff)
{
	(void) rhport; (void) p_request; (void) pBuff;
	return false; // We don't support EP requests
}

// Invoked when audio class specific set request received for an interface
bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *pBuff)
{
	(void) rhport; (void) p_request; (void) pBuff;
	return false;
}

static bool mute[3];
static int16_t volume[3];
static uint32_t sampFreq = 48000;
static uint8_t clkValid = 1;

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *pBuff)
{
	(void) rhport;
	uint8_t channelNum = TU_U16_LOW(p_request->wValue);
	uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
	uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

	if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
		if (entityID == UAC2_ENTITY_MIC_FEATURE_UNIT || entityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
			if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
				mute[channelNum] = ((audio20_control_cur_1_t *) pBuff)->bCur;
				return true;
			} else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
				volume[channelNum] = (int16_t) ((audio20_control_cur_2_t *) pBuff)->bCur;
				return true;
			}
		} else if (entityID == UAC2_ENTITY_CLOCK) {
			if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
				return true;
			}
		}
	}
	return false;
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
	(void) rhport; (void) p_request;
	return false;
}

// Invoked when audio class specific get request received for an interface
bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
	(void) rhport; (void) p_request;
	return false;
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
	uint8_t channelNum = TU_U16_LOW(p_request->wValue);
	uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
	uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

	if (entityID == UAC2_ENTITY_MIC_INPUT_TERMINAL || entityID == UAC2_ENTITY_SPK_INPUT_TERMINAL) { // Input Terminal
		if (ctrlSel == AUDIO20_TE_CTRL_CONNECTOR) {
			audio20_desc_channel_cluster_t ret;
			ret.bNrChannels = 2;
			ret.bmChannelConfig = (audio20_channel_config_t) 0;
			ret.iChannelNames = 0;
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));
		}
	} else if (entityID == UAC2_ENTITY_MIC_FEATURE_UNIT || entityID == UAC2_ENTITY_SPK_FEATURE_UNIT) { // Feature Unit
		if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);
		} else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
			if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume[channelNum], sizeof(volume[channelNum]));
			} else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
				audio20_control_range_2_n_t(1) ret;
				ret.wNumSubRanges = 1;
				ret.subrange[0].bMin = -90 * 256;	// -90 dB (1/256 dB per step)
				ret.subrange[0].bMax = 90 * 256;	// +90 dB
				ret.subrange[0].bRes = 1 * 256;		// 1 dB steps
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));
			}
		}
	} else if (entityID == UAC2_ENTITY_CLOCK) { // Clock Source
		if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
			if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sampFreq, sizeof(sampFreq));
			} else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
				audio20_control_range_4_n_t(1) sampleFreqRng;
				sampleFreqRng.wNumSubRanges = 1;
				sampleFreqRng.subrange[0].bMin = 48000;
				sampleFreqRng.subrange[0].bMax = 48000;
				sampleFreqRng.subrange[0].bRes = 0;
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &sampleFreqRng, sizeof(sampleFreqRng));
			}
		} else if (ctrlSel == AUDIO20_CS_CTRL_CLK_VALID) {
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));
		}
	}

	return false;
}

#define USB_RX_BUF_SIZE 512
static int32_t usb_rx_buf[USB_RX_BUF_SIZE * 2]; // stereo buffer
static volatile unsigned usb_rx_head;
static volatile unsigned usb_rx_tail;

void usb_audio_task(void)
{
	tu_fifo_t *ff = tud_audio_get_ep_in_ff();
	if (ff) {
		unsigned bytes_available = tu_fifo_remaining(ff);
		unsigned max_samples_to_write = bytes_available / (sizeof(int32_t) * 2);

		if (max_samples_to_write > 0) {
			if (max_samples_to_write > 48) {
				max_samples_to_write = 48; // Max per ms at 48kHz
			}

			int32_t buf[48 * 2];
			unsigned nr = get_audio_samples(buf, max_samples_to_write);

			if (nr > 0) {
				tud_audio_write((uint8_t *)buf, nr * 2 * sizeof(int32_t));
			}
		}
	}

	uint16_t rx_avail = tud_audio_available();
	if (rx_avail) {
		int32_t temp_buf[48 * 2];
		if (rx_avail > sizeof(temp_buf))
			rx_avail = sizeof(temp_buf);
		uint16_t bytes_read = tud_audio_read(temp_buf, rx_avail);
		unsigned samples_read = bytes_read / (sizeof(int32_t) * 2);

		for (unsigned i = 0; i < samples_read; i++) {
			unsigned head = usb_rx_head;
			unsigned next_head = (head + 1) % USB_RX_BUF_SIZE;
			if (next_head != usb_rx_tail) {
				usb_rx_buf[head * 2] = temp_buf[i * 2];
				usb_rx_buf[head * 2 + 1] = temp_buf[i * 2 + 1];
				usb_rx_head = next_head;
			}
		}
	}
}

float get_usb_audio_input(void)
{
	unsigned tail = usb_rx_tail;
	if (tail != usb_rx_head) {
		int32_t l = usb_rx_buf[tail * 2];
		int32_t r = usb_rx_buf[tail * 2 + 1];
		usb_rx_tail = (tail + 1) % USB_RX_BUF_SIZE;

		float val_l = l * (1.0f / 2147483648.0f);
		float val_r = r * (1.0f / 2147483648.0f);
		return (val_l + val_r) * 0.5f;
	}
	return 0.0f;
}

void tud_midi_rx_cb(uint8_t itf)
{
	(void)itf;
	uint8_t packet[4];
	while (tud_midi_packet_read(packet)) {
		// MIDI Thru: Echo to hardware UART if not for us
		if (!handle_midi_packet(packet))
			uart_midi_write(packet);
	}
}

void usb_midi_write(const uint8_t packet[4])
{
	if (tud_midi_mounted())
		tud_midi_packet_write(packet);
}
