cmake_minimum_required(VERSION 3.13...3.27)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(/home/torvalds/src/pico-sdk/pico_sdk_init.cmake)

project(rp2354a)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(blink)

# A 5MHz PIO clock gives a 2.5MHz BCLK, gives a ~52kHz sample rate
target_compile_definitions(blink PRIVATE SAMPLES_PER_SEC=52083.33333f)

pico_generate_pio_header(blink ${CMAKE_CURRENT_SOURCE_DIR}/i2s.pio)

target_sources(blink PRIVATE blink.c flanger.c lfo.c i2s.pio.h
	util.h flanger.h lfo.h)
target_link_libraries(blink pico_stdlib
	hardware_pwm hardware_adc
	hardware_i2c hardware_watchdog
	hardware_pio)

#
# We really don't care about IEEE precision, nor
# do we want double-precision math just because
# of some random constant (and adding ".0f" to them
# everywhere is just cray-cray)
#
target_compile_options(blink PRIVATE
	-ffast-math
	-fsingle-precision-constant
	-Wdouble-promotion -Wfloat-conversion)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(blink)

