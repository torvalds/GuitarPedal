#include "pico/stdlib.h"
#include "pico.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"

#include "board.h"
#include "tusb.h"
#include "pico/usb_reset.h"

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
	ITF_NUM_RESET,
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
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO20_HEADSET_STEREO_DESC_LEN + TUD_MIDI_DESC_LEN + IAD_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

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
	STRID_MIDI_INTERFACE,
	STRID_RESET_INTERFACE
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

	TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, STRID_MIDI_INTERFACE, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, CFG_TUD_MIDI_EP_BUFSIZE),

	// Nine bytes and no endpoints: a vendor interface that exists
	// only to be recognised.  picotool finds it by class, subclass
	// and protocol and sends one control request to it.  No
	// endpoint, so it cannot compete with the isochronous audio.
	TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STRID_RESET_INTERFACE)
};

//
// The length in the header has to be the length of the thing.  Get
// it wrong and the host reads the descriptor short, which does not
// fail anywhere near here.
//
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
		 "CONFIG_TOTAL_LEN disagrees with desc_configuration");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+


//
// What this pedal calls itself.
//
// Neither of these is known at build time.  The product string names the
// codec, and which codec is fitted is something probe_hardware() works out
// on the i2c bus; the serial is the chip's own unique id.  Both are here
// because two pedals on one desk otherwise enumerate identically, and then
// lsusb, the sequencer port list, the ALSA card list and the app's port
// selector are all unable to say which one you are looking at.
//
// The default is what a pedal says before anything has told it otherwise,
// which is the truthful answer for the moment before the bus is probed.
//
static const char *usb_product = "Pedal";
static char usb_serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1] = "0";

void usb_set_product(const char *name)
{
	usb_product = name;
}

//
// A string descriptor is its own two-byte header - the descriptor type in
// the high byte and the total length in the low one - followed by the text
// as UTF-16, all in one buffer.  That is why the strings here are plain
// ASCII and get converted on the way out rather than being u"" literals: a
// literal supplies the payload and leaves nowhere to put the header, so the
// length ends up hand-counted next to the text it has to agree with.
//
// One buffer serves all of them.  The host asks for one descriptor at a
// time and cannot begin the next request before this transfer completes,
// and tinyusb passes the pointer straight through without copying, so the
// buffer has to outlive the call and does not have to be per-string.
//
static uint16_t desc_str[48];

static const uint16_t *utf16_desc(const char *s)
{
	unsigned int i = 0;

	while (s[i] && i < ARRAY_SIZE(desc_str) - 1) {
		desc_str[i + 1] = (unsigned char) s[i];
		i++;
	}
	desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * (i + 1));
	return desc_str;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	// Not text, so it does not go through the conversion above
	static const uint16_t langid_desc[] = {
		(TUSB_DESC_STRING << 8) | 4, 0x0409	// English
	};

	switch (index) {
	case STRID_LANGID:
		return langid_desc;
	case STRID_MANUFACTURER:
		return utf16_desc("Linus");
	case STRID_PRODUCT:
		return utf16_desc(usb_product);
	case STRID_SERIAL:
		return utf16_desc(usb_serial);
	case STRID_AUDIO_INTERFACE:
		return utf16_desc("UAC2");
	case STRID_MIDI_INTERFACE:
		return utf16_desc("MIDI");
	case STRID_RESET_INTERFACE:
		return utf16_desc("Reset");
	}
	return NULL;
}

//--------------------------------------------------------------------+
// Audio Callbacks
//--------------------------------------------------------------------+

int init_usb(void)
{
	pico_get_unique_board_id_string(usb_serial, sizeof(usb_serial));

	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO
	};

	//
	// The return value was being dropped, and dropping it is the
	// reason a whole class of failure here is invisible.
	//
	// usbd_init() is a run of TU_ASSERTs - the descriptor count, the
	// queue, a driver with no init function - and any one of them
	// makes it return false *before* dcd_init(), so D+ is never pulled
	// up.  The pedal then boots, plays, blinks and answers its
	// switches, and simply is not on the USB: no device, no BOOTSEL,
	// and nothing in the host's log, because from the host's side
	// nothing was ever plugged in.
	//
	// So it asks to be reflashed instead.  This is not a runtime
	// hazard dressed up as a recovery: tusb_init() failing is a
	// property of the image, the same every boot, so a pedal that
	// does this was never going to work and could not have said so.
	// Being findable by picotool is the only useful thing left, and
	// it is what makes a bad USB change cost a reflash rather than a
	// trip to the BOOTSEL button.
	//
	if (!tusb_init(0, &dev_init))
		reset_usb_boot(0, 0);

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

//
// Feature unit controls, per channel: 0 is the master, 1 and 2 are
// left and right.
//
// The channel number arrives inside the host's control request, so it
// can be anything at all.  Hand out a pointer or NULL and check it in
// one place, rather than having every call site index the arrays and
// hope for the best.
//
#define AUDIO_CHANNELS 3

static bool mute[AUDIO_CHANNELS];
static int16_t volume[AUDIO_CHANNELS];
static uint32_t sampFreq = 48000;
static uint8_t clkValid = 1;

static bool *channel_mute(uint8_t ch)
{
	return ch < AUDIO_CHANNELS ? &mute[ch] : NULL;
}

static int16_t *channel_volume(uint8_t ch)
{
	return ch < AUDIO_CHANNELS ? &volume[ch] : NULL;
}

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
				bool *mutep = channel_mute(channelNum);

				if (!mutep)
					return false;
				*mutep = ((audio20_control_cur_1_t *) pBuff)->bCur;
				return true;
			} else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
				int16_t *volp = channel_volume(channelNum);

				if (!volp)
					return false;
				*volp = (int16_t) ((audio20_control_cur_2_t *) pBuff)->bCur;
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
			bool *mutep = channel_mute(channelNum);

			if (!mutep)
				return false;
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, mutep, 1);
		} else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
			if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
				int16_t *volp = channel_volume(channelNum);

				if (!volp)
					return false;
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, volp, sizeof(*volp));
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
static raw_sample_t usb_rx_buf[USB_RX_BUF_SIZE];
static unsigned usb_rx_head;
static unsigned usb_rx_tail;

void usb_audio_task(void)
{
	tu_fifo_t *ff = tud_audio_get_ep_in_ff();
	if (ff) {
		unsigned bytes_available = tu_fifo_remaining(ff);
		unsigned max_samples_to_write = bytes_available / (sizeof(int32_t) * 2);

		if (max_samples_to_write > 0) {
			//
			// 49, not 48.  48 is the nominal rate; the extra
			// one is how a device whose clock runs slightly
			// fast catches up, and without it the surplus is
			// discarded instead - see the endpoint size in
			// tusb_config.h.
			//
			if (max_samples_to_write > 49) {
				max_samples_to_write = 49;
			}

			int32_t buf[49 * 2];
			unsigned nr = get_audio_samples(buf, max_samples_to_write);

			if (nr > 0) {
				tud_audio_write((uint8_t *)buf, nr * 2 * sizeof(int32_t));
			}
		}
	}

	uint16_t rx_avail = tud_audio_available();
	if (rx_avail) {
		raw_sample_t temp_buf[48];
		if (rx_avail > sizeof(temp_buf))
			rx_avail = sizeof(temp_buf);
		uint16_t bytes_read = tud_audio_read(temp_buf, rx_avail);
		unsigned samples_read = bytes_read / sizeof(raw_sample_t);

		for (unsigned i = 0; i < samples_read; i++) {
			unsigned head = usb_rx_head;
			unsigned next_head = (head + 1) % USB_RX_BUF_SIZE;

			if (next_head == smp_load_acquire(&usb_rx_tail))
				break;
			usb_rx_buf[head] = temp_buf[i];
			smp_store_release(&usb_rx_head, next_head);
		}
	}
}

sample_t __audio_func(get_usb_audio_input)(void)
{
	unsigned tail = usb_rx_tail;

	if (tail != smp_load_acquire(&usb_rx_head)) {
		raw_sample_t sample = usb_rx_buf[tail];
		smp_store_release(&usb_rx_tail, (tail + 1) % USB_RX_BUF_SIZE);

		return (sample_t) {
			.left = sample.left * (1.0f / 2147483648.0f),
			.right = sample.right * (1.0f / 2147483648.0f)
		};
	}
	return (sample_t) { 0, 0 };
}

//
// Incoming MIDI is handled from the main loop, not from a callback.
//
// There is no tud_midi_rx_cb() here on purpose.  That callback runs
// inside tud_task(), and tud_task() is exactly what usb_midi_write()
// spins on when the transmit fifo is full - so handling a message
// there meant an incoming routing change or parameter write could land
// in the middle of any main-loop code that was part-way through
// sending something and reading state as it went.  A state dump walking
// effects[] was the worst of it, but every sender had the same hole.
//
// So nothing asynchronous, apart from the audio core: packets queue up
// and the main loop deals with them, one at a time, in order.
//
// The queue is tinyusb's own rx fifo, which is what tud_midi_rx_cb()
// was only ever a notification about.  Leaving packets in it costs
// nothing and gets the flow control for free: when it fills, tinyusb
// stops accepting from the endpoint and the host waits.  Nothing is
// dropped, which matters most for SysEx, where losing one packet of a
// stream corrupts the whole message rather than one value.
//
void usb_midi_poll(void)
{
	uint8_t packet[4];

	while (tud_midi_packet_read(packet)) {
		// MIDI Thru: Echo to hardware UART if not for us
		if (!handle_midi_packet(packet))
			uart_midi_write(packet);
	}
}

//
// How long to wait for the host to make room, per packet.
//
// tud_midi_mounted() only says the interface is enumerated.  It says
// nothing about whether anything is reading MIDI IN, and a host that
// enumerates and then never reads used to hang core 0 outright - audio
// carried on over on core 1, while the UI, the eeprom writes, USB audio
// and the UART all stopped for good.
//
// A host that is reading drains every USB frame, so waiting is normally
// a matter of a frame or two.  This is meant to be far beyond that and
// still short enough that a dead host costs a stutter rather than the
// pedal.
//
#define MIDI_TX_TIMEOUT_MS 20

bool usb_midi_write(const uint8_t packet[4])
{
	if (!tud_midi_mounted())
		return false;

	absolute_time_t deadline = make_timeout_time_ms(MIDI_TX_TIMEOUT_MS);

	// Nothing calls this from inside tud_task() any more, so the
	// spin cannot recurse into it.
	while (!tud_midi_packet_write(packet)) {
		if (time_reached(deadline))
			return false;
		tud_task();
	}
	return true;
}
