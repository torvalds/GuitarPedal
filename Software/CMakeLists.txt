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

add_executable(pedal)

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
# How much flash there actually is.
#
# PICO_BOARD is not set, so the SDK defaults it to 'pico2' - a board
# with 4MB of flash hanging off the QSPI pins.  This is an RP2354B and
# the 2MB is on the die.  Nothing has cared so far, because nothing has
# addressed flash by anything but a symbol.
#
# It starts mattering the moment something works backwards from the end
# of flash, because 'the end' would be an address that is not there.
# flash_range_erase() guards itself with
#
#	hard_assert(flash_offs + count <= PICO_FLASH_SIZE_BYTES)
#
# so getting this right is also what makes the SDK's own bounds check
# mean something.
#
set(FLASH_SIZE_BYTES 0x200000)

#
# The save area: sixty-four 4kB slots at the top of flash.
#
# 4kB because that is the erase size, and there is no reason to be
# cleverer when the part has 2MB and the firmware uses 84kB.  Sixty-four
# rather than the thirty-two that would do, because slots are expected
# to be live at once - one per scene, plus whatever else - and a region
# with no spare slots cannot rotate, so 'enough for one copy of
# everything' is the one size that does not work.
#
set(SAVE_SLOT_SIZE 0x1000)
set(SAVE_SLOT_COUNT 64)
math(EXPR SAVE_AREA_SIZE "${SAVE_SLOT_SIZE} * ${SAVE_SLOT_COUNT}"
	OUTPUT_FORMAT HEXADECIMAL)
math(EXPR SAVE_AREA_OFFSET "${FLASH_SIZE_BYTES} - ${SAVE_AREA_SIZE}"
	OUTPUT_FORMAT HEXADECIMAL)

target_compile_definitions(pedal PRIVATE
	PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
	SAVE_SLOT_SIZE=${SAVE_SLOT_SIZE}
	SAVE_SLOT_COUNT=${SAVE_SLOT_COUNT}
	SAVE_AREA_OFFSET=${SAVE_AREA_OFFSET})

#
# Only the audio core runs from RAM: everything marked with
# __audio_func() lands in a '.time_critical.audio_*' section, which the
# default linker script already pulls out of flash. The rest is cold
# enough to run from XIP.
#
# Its constant data has to be there too, marked __not_in_flash("audio"),
# because the audio core keeps playing while core 0 writes flash with
# XIP switched off.  check-audio.py enforces both halves.
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
target_sources(pedal PRIVATE "${POW2_HEADER}")

set(LOG2_STEP_SHIFT 8)
set(LOG2_HEADER "${CMAKE_CURRENT_BINARY_DIR}/log2.h")
add_custom_command(
	OUTPUT "${LOG2_HEADER}"
	COMMAND "${SCRIPT_DIR}/log2.py"
		"${LOG2_HEADER}"
		"${LOG2_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/log2.py"
)
target_sources(pedal PRIVATE "${LOG2_HEADER}")

set(SINE_STEP_SHIFT 8)
set(SINE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/quarter_sine.h")
add_custom_command(
	OUTPUT "${SINE_HEADER}"
	COMMAND "${SCRIPT_DIR}/quarter_sine.py"
		"${SINE_HEADER}"
		"${SINE_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/quarter_sine.py"
)
target_sources(pedal PRIVATE "${SINE_HEADER}")

set(EQ_W0_HEADER "${CMAKE_CURRENT_BINARY_DIR}/eq-w0.h")
add_custom_command(
	OUTPUT "${EQ_W0_HEADER}"
	COMMAND "${SCRIPT_DIR}/eq-w0.py"
		"${EQ_W0_HEADER}"
		"${SAMPLES_PER_SEC}"
	DEPENDS "${SCRIPT_DIR}/eq-w0.py"
)
target_sources(pedal PRIVATE "${EQ_W0_HEADER}")

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
target_sources(pedal PRIVATE "${EFFECT_MAP_HEADER}")

#
# The WS2812B waveform, as a 256-entry table.
#
# The protocol used to live in ws2812.pio, in delay slots and a branch,
# with a '.clock_div 18' that quietly meant a different bit rate on
# every system clock this firmware has run at.  It is a table now, which
# can be checked with a diff on a machine that has no PIO in it - see
# pixels.h and pio/bitstream.pio.
#
set(WS2812_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ws2812_table.h")
add_custom_command(
	OUTPUT "${WS2812_TABLE}"
	COMMAND "${SCRIPT_DIR}/ws2812_table.py" "${WS2812_TABLE}"
	DEPENDS "${SCRIPT_DIR}/ws2812_table.py"
)
target_sources(pedal PRIVATE "${WS2812_TABLE}")

set(PIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pio")

pico_generate_pio_header(pedal ${PIO_DIR}/bitstream.pio)
pico_generate_pio_header(pedal ${PIO_DIR}/debounce.pio)
pico_generate_pio_header(pedal ${PIO_DIR}/rotary.pio)
pico_generate_pio_header(pedal ${PIO_DIR}/i2s.pio)

target_compile_definitions(pedal PRIVATE
	SYS_CLK_KHZ=153600
	PLL_SYS_VCO_FREQ_HZ=768000000
	PLL_SYS_POSTDIV1=5
	PLL_SYS_POSTDIV2=1

	#
	# Wait longer for the crystal before believing it.
	#
	# One board came up dead on roughly a quarter of cold power cycles -
	# no USB, no BOOTSEL, nothing in the host's log - and was rock solid
	# once running.  It hangs in pll_init(), spinning on PLL_CS_LOCK,
	# which is before main() and before any watchdog tick exists, so
	# nothing in this firmware could see it and no reflash could recover
	# it.
	#
	# The PLL was not the problem; its reference was.  The counter that
	# gates XOSC_STATUS_STABLE is clocked by the crystal itself, so it
	# measures oscillation and not amplitude - a crystal that is still
	# building trips STABLE anyway, and pll_init() gets handed a
	# reference too poor to lock to.  At the default multiplier of 6 that
	# is about 6ms.  This makes it about 64ms.
	#
	# 20 cold power cycles out of 20 clean afterwards, against about 5 in
	# 20 before.  The cost is 58ms once per boot, which nobody can see
	# next to USB enumeration.
	#
	# The default is what is wrong here, not the crystal.  The part is
	# the one the datasheet asks for, with the load caps and the damping
	# resistor it asks for, and this has been seen on more than one board
	# - so "that one crystal is marginal" does not survive contact with
	# the evidence.  Nor does "our layout", entirely: 18 board files in
	# the SDK override this, from eight different vendors, and 17 of them
	# pick exactly 64.  Everyone who builds something that is not the
	# reference design and looks at this raises it.  What none of them
	# do is leave it at 6 - and pico.h and pico2.h, which never had to
	# care, are where the 6 comes from.
	#
	# What this does NOT do is make a dead crystal start; nothing here
	# can, and a board whose crystal never oscillates still hangs in
	# xosc_init() instead.  Surviving that needs a bounded wait in place
	# of the SDK's two unbounded ones, which means replacing the __weak
	# runtime_init_clocks() rather than setting anything.
	#
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
	MIDI_HW=$<BOOL:${MIDI_HW}>
	SAMPLES_PER_SEC=${SAMPLES_PER_SEC}f
	CFG_TUSB_MCU=OPT_MCU_RP2040
	CFG_TUSB_OS=OPT_OS_PICO
	CFG_TUSB_DEBUG=0
	#
	# The reset interface picotool looks for.  The sdk picks defaults
	# for these from whether LIB_TINYUSB_* is defined, which comes from
	# linking its tinyusb target - and this builds the submodule's
	# sources directly, so they all land on the branch meant for a
	# project not using tinyusb at all.  Stated rather than inherited.
	#
	# The baud-rate escape is the 1200-baud trick and wants a CDC
	# interface to have a baud rate on.  There isn't one.
	#
	PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
	PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0
	PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=1)

target_sources(pedal PRIVATE
	pedal.c
	"${SINE_HEADER}"
	tinyusb/src/tusb.c
	tinyusb/src/common/tusb_fifo.c
	tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
)

target_sources(pedal PRIVATE
	usb-device.c
	tinyusb/src/device/usbd.c
	tinyusb/src/class/audio/audio_device.c
	tinyusb/src/class/midi/midi_device.c
	tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c
	#
	# Compiled in rather than linked as pico_usb_reset, which would
	# drag in the sdk's own tinyusb alongside the submodule this
	# builds against.  The file includes "tusb.h" and gets whichever
	# one is on the path.
	#
	pico-sdk/src/rp2_common/pico_usb_reset/usb_reset.c)

target_link_libraries(pedal
	pico_stdlib
	pico_multicore
	hardware_gpio
	hardware_pwm
	hardware_pio
	hardware_i2c
	hardware_adc
	hardware_dma
	hardware_flash
	pico_unique_id
	hardware_watchdog
	pico_sha256)

target_include_directories(pedal PRIVATE
	include
	tinyusb/src
	pico-sdk/src/rp2_common/pico_usb_reset/include
	pico-sdk/src/common/pico_usb_reset_interface_headers/include
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
target_compile_options(pedal PRIVATE
	-Wall -O2
	-ffast-math
	-fsingle-precision-constant
	-Wfloat-conversion)

# Our own sources only - we don't get to pick the SDK's warnings
set_source_files_properties(pedal.c usb-device.c PROPERTIES
	COMPILE_OPTIONS -Wdouble-promotion)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(pedal)

# Nothing on the audio core is allowed to call out to a library
add_custom_command(TARGET pedal POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-audio.py"
		"$<TARGET_FILE:pedal>.map"
		"$<TARGET_FILE:pedal>"
		"${CMAKE_OBJDUMP}"
	VERBATIM)

# ...and nothing anywhere is allowed to use a double
add_custom_command(TARGET pedal POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-float.py"
		"$<TARGET_FILE:pedal>"
		"${CMAKE_NM}"
	VERBATIM)

# ...and the image is not allowed to grow into the save area
add_custom_command(TARGET pedal POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-flash.py"
		"$<TARGET_FILE:pedal>"
		"${FLASH_SIZE_BYTES}"
		"${SAVE_AREA_OFFSET}"
		"${CMAKE_NM}"
	VERBATIM)

