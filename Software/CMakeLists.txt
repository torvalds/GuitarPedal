cmake_minimum_required(VERSION 3.13...3.27)

#
# What we build for, and how we pass floats around.
#
# The part is an RP2354B - an RP2350 with the flash on the die.  The
# default float ABI is softfp, which is a compatibility mode for parts
# that might not have an FPU.  This one does, and the whole firmware is
# built around 32-bit floats, so passing them in core registers is pure
# loss - a 'sample_t' is two floats, and softfp hands it back through
# memory rather than in s0/s1.
#
# The two belong together: asking for hard float only makes sense
# because of the platform we asked for.  Both are read by the toolchain
# file, so they have to be set before the SDK picks one - which is also
# why they are not the usual cache options further down.
#
set(PICO_PLATFORM rp2350)
set(PICO_HARD_FLOAT_ABI 1)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico-sdk/pico_sdk_init.cmake)

project(rp2350)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(blink)

#
# What is actually on the board.
#
# Both of these describe hardware rather than taste, so they default to
# the boards being built now and stay options only so that the older
# ones can still be built.  Getting one wrong is quiet: a 64kbit eeprom
# addressed as a 2kbit one takes the write and puts it somewhere else,
# so the pedal runs perfectly and just forgets everything on reboot.
#
# The MIDI pins used to be shared with a second rotary encoder, which is
# why this was ever off; there is no second rotary any more.
#
set(MIDI_HW ON CACHE BOOL "Enable MIDI HW (not just MIDI over USB-C) support")

set(SAMPLES_PER_SEC 48000.0)
set(SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

#
# Only the audio core runs from RAM: everything marked with
# __audio_func() lands in a '.time_critical.audio_*' section, which the
# default linker script already pulls out of flash. The rest is cold
# enough to run from XIP.
#

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

file(GLOB EFFECT_HEADERS CONFIGURE_DEPENDS "effects/*.h")
set(EFFECT_MAP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/effect_map.h")
set(EFFECT_JS "${CMAKE_CURRENT_SOURCE_DIR}/WebMIDI/effects.js")
set(EFFECT_MD "${CMAKE_CURRENT_SOURCE_DIR}/MIDI_CC_MAP.md")

# Written alongside effect_map.h rather than named on the command line -
# the generator puts it next to it - but it is still an output, and the
# build graph should know that rather than getting away with it.
set(EFFECT_SCHEMA "${CMAKE_CURRENT_BINARY_DIR}/midi_schema.h")

add_custom_command(
	OUTPUT "${EFFECT_MAP_HEADER}" "${EFFECT_SCHEMA}" "${EFFECT_JS}" "${EFFECT_MD}"
	COMMAND "${SCRIPT_DIR}/gen_effects.py"
		"${CMAKE_CURRENT_SOURCE_DIR}/effects"
		"${EFFECT_MAP_HEADER}"
		"${EFFECT_JS}"
		"${EFFECT_MD}"
		${EFFECT_HEADERS}
	DEPENDS "${SCRIPT_DIR}/gen_effects.py" ${EFFECT_HEADERS}
)
target_sources(blink PRIVATE "${EFFECT_MAP_HEADER}")

set(PIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pio")

pico_generate_pio_header(blink ${PIO_DIR}/ws2812.pio)
pico_generate_pio_header(blink ${PIO_DIR}/debounce.pio)
pico_generate_pio_header(blink ${PIO_DIR}/rotary.pio)
pico_generate_pio_header(blink ${PIO_DIR}/i2s.pio)

target_compile_definitions(blink PRIVATE
	SYS_CLK_KHZ=153600
	PLL_SYS_VCO_FREQ_HZ=768000000
	PLL_SYS_POSTDIV1=5
	PLL_SYS_POSTDIV2=1
	MIDI_HW=$<BOOL:${MIDI_HW}>
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

target_sources(blink PRIVATE
	usb-device.c
	tinyusb/src/device/usbd.c
	tinyusb/src/class/audio/audio_device.c
	tinyusb/src/class/midi/midi_device.c
	tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c)

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

# Our own sources only - we don't get to pick the SDK's warnings
set_source_files_properties(blink.c usb-device.c PROPERTIES
	COMPILE_OPTIONS -Wdouble-promotion)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(blink)

# Nothing on the audio core is allowed to call out to a library
add_custom_command(TARGET blink POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-audio.py"
		"$<TARGET_FILE:blink>.map"
		"$<TARGET_FILE:blink>"
		"${CMAKE_OBJDUMP}"
	VERBATIM)

# ...and nothing anywhere is allowed to use a double
add_custom_command(TARGET blink POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-float.py"
		"$<TARGET_FILE:blink>"
		"${CMAKE_NM}"
	VERBATIM)

