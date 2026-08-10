cmake_minimum_required(VERSION 3.13...3.27)

#
# What we build for, and how we pass floats around.
#
# The part is an RP2354A - an RP2350A with the flash on the die.  The
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

#
# Which boards exist, and which one this tree is currently about.
#
# Every board is built as its own target with its own name, all of them
# at once, because they differ in ways that a single binary cannot paper
# over: the unified board shares four GPIOs with the split map and needs
# the ADC pins for its expression jack.  There is no "the" firmware any
# more, so there is no build/pedal.elf either - an artifact whose name
# does not say what it is for is exactly what goes on the wrong board.
#
# What is configured is only which one is the *default*: what a plain
# 'make' builds and what 'make flash' flashes.  Any other board is one
# explicit target away ('make split', 'make flash-split')
# without disturbing that, and 'make all-boards' builds the lot, which
# is how a change to shared code stops silently breaking a board nobody
# has selected for a month.
#
# There are only two, and the line between them is the only one that
# needs a build.  The split family has two MCU boards - one with a rotary
# encoder, one with MIDI jacks - whose pins do not clash, so one binary
# drives either.  Its audio boards differ too, mono against stereo, and
# that difference is discovered at runtime because the older codec needs
# i2c setup anyway.  Neither is a reason to compile twice.
#
# What is left is the pin map, which the unified board shares four GPIOs
# of and no more.  That is the whole of the build-time question.
#
set(PEDAL_BOARDS split unified)

# ...and the same list as prose, for the messages below.
list(JOIN PEDAL_BOARDS " " PEDAL_BOARDS_PRETTY)

#
# Set and forget, in a file rather than in the cache.
#
# A cmake cache variable is set-and-forget right up until somebody
# deletes build/, and that is not hypothetical: reconfiguring silently
# reset the eeprom size option once and the pedal ran perfectly and
# forgot every scene across a reboot.  board.local survives 'rm -rf
# build', is not tracked, and is read again whenever it changes.
#
# There is deliberately no default.  A wrong board should be a configure
# error you fix once, not a working build for hardware you do not have.
#
set(BOARD_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/board.local")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOARD_LOCAL}")

if (EXISTS "${BOARD_LOCAL}")
	file(STRINGS "${BOARD_LOCAL}" PEDAL_BOARD LIMIT_COUNT 1)
	string(STRIP "${PEDAL_BOARD}" PEDAL_BOARD)
endif()

if (NOT PEDAL_BOARD)
	message(FATAL_ERROR
		"No board selected.  Say which one this tree is about:\n"
		"    echo unified > ${BOARD_LOCAL}\n"
		"Known boards: ${PEDAL_BOARDS_PRETTY}")
endif()

if (NOT PEDAL_BOARD IN_LIST PEDAL_BOARDS)
	message(FATAL_ERROR
		"'${PEDAL_BOARD}' in ${BOARD_LOCAL} is not a board.\n"
		"Known boards: ${PEDAL_BOARDS_PRETTY}")
endif()

message("Default board: ${PEDAL_BOARD}")

set(SAMPLES_PER_SEC 48000.0)
set(SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

#
# How much flash there actually is.
#
# PICO_BOARD is not set, so the SDK defaults it to 'pico2' - a board
# with 4MB of flash hanging off the QSPI pins.  This is an RP2354A and
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
list(APPEND PEDAL_GENERATED "${POW2_HEADER}")

set(LOG2_STEP_SHIFT 8)
set(LOG2_HEADER "${CMAKE_CURRENT_BINARY_DIR}/log2.h")
add_custom_command(
	OUTPUT "${LOG2_HEADER}"
	COMMAND "${SCRIPT_DIR}/log2.py"
		"${LOG2_HEADER}"
		"${LOG2_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/log2.py"
)
list(APPEND PEDAL_GENERATED "${LOG2_HEADER}")

set(SINE_STEP_SHIFT 8)
set(SINE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/quarter_sine.h")
add_custom_command(
	OUTPUT "${SINE_HEADER}"
	COMMAND "${SCRIPT_DIR}/quarter_sine.py"
		"${SINE_HEADER}"
		"${SINE_STEP_SHIFT}"
	DEPENDS "${SCRIPT_DIR}/quarter_sine.py"
)
list(APPEND PEDAL_GENERATED "${SINE_HEADER}")

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
list(APPEND PEDAL_GENERATED "${EFFECT_MAP_HEADER}")

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
list(APPEND PEDAL_GENERATED "${WS2812_TABLE}")

#
# Everything above is generated once and shared by every board: none of
# it depends on which pins anything is on.
#
# It hangs off one target rather than being listed in each board's
# sources, which matters with more than one of them - two targets naming
# the same generated file can run its rule twice under 'make -j', and
# gen_effects.py writes into the source tree (effects.js, MIDI_CC_MAP.md)
# where doing that twice at once is a race with a real loser.
#
add_custom_target(pedal_generated DEPENDS ${PEDAL_GENERATED})

#
# The PIO programs are shared for the same reason - a .pio says what the
# state machine does, and which pin it does it on arrives at runtime in
# *_program_init().
#
# They go through an INTERFACE library because pico_generate_pio_header()
# writes to one fixed path per .pio file: calling it once per board would
# declare three rules for the same output and cmake refuses that outright.
# The SDK's own function has a branch for INTERFACE targets, so this is
# the supported way round rather than a trick.
#
# Our own sources only - we don't get to pick the SDK's warnings
set_source_files_properties(pedal.c usb-device.c PROPERTIES
	COMPILE_OPTIONS -Wdouble-promotion)

set(PIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pio")

add_library(pedal_pio INTERFACE)
pico_generate_pio_header(pedal_pio ${PIO_DIR}/bitstream.pio)
pico_generate_pio_header(pedal_pio ${PIO_DIR}/debounce.pio)
pico_generate_pio_header(pedal_pio ${PIO_DIR}/rotary.pio)
pico_generate_pio_header(pedal_pio ${PIO_DIR}/i2s.pio)

#
# ...and the header-generating targets it made, named after it, which
# each board depends on explicitly.  Linking an INTERFACE library does
# not reliably carry its add_dependencies() across, and "the header is
# not there yet" is not a failure worth debugging twice.
#
set(PEDAL_PIO_TARGETS
	pedal_pio_bitstream_pio_h
	pedal_pio_debounce_pio_h
	pedal_pio_rotary_pio_h
	pedal_pio_i2s_pio_h)

#
# One board, one target, one name.  Everything below here is per-board;
# everything above it is not.
#
function(add_pedal_board board)

set(tgt "pedal-${board}")

add_executable(${tgt})

add_dependencies(${tgt} pedal_generated ${PEDAL_PIO_TARGETS})
target_link_libraries(${tgt} pedal_pio)

#
# Which board this is, in the three places it has to be true at once:
# the pin map the compiler sees, the name the binary carries so picotool
# can answer for a file on disk, and the name the firmware can compare
# against what actually answers on the i2c bus.
#
target_compile_definitions(${tgt} PRIVATE
	PEDAL_BOARD_HEADER="boards/${board}.h"
	PEDAL_BOARD_NAME="${board}")

target_compile_definitions(${tgt} PRIVATE
	PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
	SAVE_SLOT_SIZE=${SAVE_SLOT_SIZE}
	SAVE_SLOT_COUNT=${SAVE_SLOT_COUNT}
	SAVE_AREA_OFFSET=${SAVE_AREA_OFFSET})

target_compile_definitions(${tgt} PRIVATE
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

target_sources(${tgt} PRIVATE
	pedal.c
	"${SINE_HEADER}"
	tinyusb/src/tusb.c
	tinyusb/src/common/tusb_fifo.c
	tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
)

target_sources(${tgt} PRIVATE
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

target_link_libraries(${tgt}
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

target_include_directories(${tgt} PRIVATE
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
target_compile_options(${tgt} PRIVATE
	-Wall -O2
	-ffast-math
	-fsingle-precision-constant
	-Wfloat-conversion)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(${tgt})

# Nothing on the audio core is allowed to call out to a library
add_custom_command(TARGET ${tgt} POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-audio.py"
		"$<TARGET_FILE:${tgt}>.map"
		"$<TARGET_FILE:${tgt}>"
		"${CMAKE_OBJDUMP}"
	VERBATIM)

# ...and nothing anywhere is allowed to use a double
add_custom_command(TARGET ${tgt} POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-float.py"
		"$<TARGET_FILE:${tgt}>"
		"${CMAKE_NM}"
	VERBATIM)

# ...and the image is not allowed to grow into the save area
add_custom_command(TARGET ${tgt} POST_BUILD
	COMMAND "${SCRIPT_DIR}/check-flash.py"
		"$<TARGET_FILE:${tgt}>"
		"${FLASH_SIZE_BYTES}"
		"${SAVE_AREA_OFFSET}"
		"${CMAKE_NM}"
	VERBATIM)


#
# Only the default board is in 'all', so a plain 'make' builds the one
# thing this tree is currently about.  The others are a named target
# away and cost nothing until asked for.
#
if (NOT board STREQUAL PEDAL_BOARD)
	set_target_properties(${tgt} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

endfunction()

foreach(board ${PEDAL_BOARDS})
	add_pedal_board(${board})
	list(APPEND PEDAL_BOARD_TARGETS "pedal-${board}")
endforeach()

#
# Build every board.  Nothing needs this to develop, and that is the
# point of having it: a change to shared code that breaks a board nobody
# has selected in a month is otherwise found by flashing it.
#
add_custom_target(all-boards DEPENDS ${PEDAL_BOARD_TARGETS})
