cmake_minimum_required(VERSION 3.13...3.27)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico-sdk/pico_sdk_init.cmake)

project(rp2350)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(blink)

set(SYS_CLK_KHZ 172800)
set(SAMPLES_PER_SEC 48000.0)
set(SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

pico_set_binary_type(blink copy_to_ram)

# Math table generation
set(POW2_STEP_SHIFT 8)
set(POW2_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pow2.h")
add_custom_command(
	OUTPUT "${POW2_HEADER}"
	COMMAND "${SCRIPT_DIR}/pow2.py"
		"${POW2_HEADER}"
		"${POW2_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/pow2.py"
)
target_sources(blink PRIVATE "${POW2_HEADER}")

set(LOG2_STEP_SHIFT 8)
set(LOG2_HEADER "${CMAKE_CURRENT_BINARY_DIR}/log2.h")
add_custom_command(
	OUTPUT "${LOG2_HEADER}"
	COMMAND "${SCRIPT_DIR}/log2.py"
		"${LOG2_HEADER}"
		"${LOG2_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/log2.py"
)
target_sources(blink PRIVATE "${LOG2_HEADER}")

set(SINE_STEP_SHIFT 8)
set(SINE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/quarter_sine.h")
add_custom_command(
	OUTPUT "${SINE_HEADER}"
	COMMAND "${SCRIPT_DIR}/quarter_sine.py"
		"${SINE_HEADER}"
		"${SINE_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/quarter_sine.py"
)
target_sources(blink PRIVATE "${SINE_HEADER}")

set(EQ_W0_HEADER "${CMAKE_CURRENT_BINARY_DIR}/eq-w0.h")
add_custom_command(
	OUTPUT "${EQ_W0_HEADER}"
	COMMAND "${SCRIPT_DIR}/eq-w0.py"
		"${EQ_W0_HEADER}"
		"${SAMPLES_PER_SEC}"
	DEPENDS "${SCRIPT_DIR}/eq-w0.py"
)
target_sources(blink PRIVATE "${EQ_W0_HEADER}")

set(EFFECTS gate preamp compressor boost phaser flanger vibrato echo reverb pitch tremolo eq usb)
set(EFFECT_MAP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/effect_map.h")
set(EFFECT_JS "${CMAKE_CURRENT_SOURCE_DIR}/WebMIDI/effects.js")

add_custom_command(
	OUTPUT "${EFFECT_MAP_HEADER}" "${EFFECT_JS}"
	COMMAND "${SCRIPT_DIR}/gen_effects.py"
		"${CMAKE_CURRENT_SOURCE_DIR}/effects"
		"${EFFECT_MAP_HEADER}"
		"${EFFECT_JS}"
		${EFFECTS}
	DEPENDS "${SCRIPT_DIR}/gen_effects.py"
)
target_sources(blink PRIVATE "${EFFECT_MAP_HEADER}")

set(PIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pio")

pico_generate_pio_header(blink ${PIO_DIR}/ws2812.pio)
pico_generate_pio_header(blink ${PIO_DIR}/debounce.pio)
pico_generate_pio_header(blink ${PIO_DIR}/rotary.pio)
pico_generate_pio_header(blink ${PIO_DIR}/i2s.pio)

set(USB_MODE "DEVICE" CACHE STRING "USB Mode: DEVICE (default) or HOST")
set_property(CACHE USB_MODE PROPERTY STRINGS DEVICE HOST)

target_compile_definitions(blink PRIVATE
	USB_MODE_${USB_MODE}=1
	SAMPLES_PER_SEC=${SAMPLES_PER_SEC}f
	CFG_TUSB_MCU=OPT_MCU_RP2040
	CFG_TUSB_OS=OPT_OS_PICO
	CFG_TUSB_DEBUG=0)

target_sources(blink PRIVATE
	blink.c
	"${SINE_HEADER}"
	tinyusb/src/tusb.c
	tinyusb/src/common/tusb_fifo.c
	tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
)

if(USB_MODE STREQUAL "HOST")
	target_sources(blink PRIVATE
		usb-host.c
		tinyusb/src/host/usbh.c
		tinyusb/src/host/hub.c
		tinyusb/src/class/midi/midi_host.c
		tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c)
else()
	target_sources(blink PRIVATE
		usb-device.c
		tinyusb/src/device/usbd.c
		tinyusb/src/class/audio/audio_device.c
		tinyusb/src/class/midi/midi_device.c
		tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c)
endif()

target_link_libraries(blink
	pico_stdlib
	pico_multicore
	hardware_gpio
	hardware_pwm
	hardware_pio
	hardware_i2c
	hardware_dma)

target_include_directories(blink PRIVATE
	include
	tinyusb/src
	"${CMAKE_CURRENT_BINARY_DIR}")

#
# We really don't care about IEEE precision, nor
# do we want double-precision math just because
# of some random constant (and adding ".0f" to them
# everywhere is just cray-cray)
#
# Add -fsingle-precision-constant / -Wdouble-promotion
# when starting to do FP for audio
#
target_compile_options(blink PRIVATE
	-Wall -O2
	-ffast-math
	-fsingle-precision-constant
	-Wfloat-conversion)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(blink)

