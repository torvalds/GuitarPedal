cmake_minimum_required(VERSION 3.13...3.27)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(/home/torvalds/src/pico-sdk/pico_sdk_init.cmake)

project(rp2354a)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# rest of your project
add_executable(blink
    blink.c
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(blink pico_stdlib hardware_adc)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(blink)

