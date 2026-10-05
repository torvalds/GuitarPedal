#
# Which board this tree is about lives in board.local, not here and not
# in the cmake cache - see the comment at the top of CMakeLists.txt.
# One line, one board name, written once.
#
# 'make' and 'make flash' are about that board.  The other one is an
# explicit target - 'make split', 'make flash-split' - which builds and
# flashes it without changing what the tree is about, because
# spot-checking an old board should not cost you your setting.
#
BOARDS := split unified minimal ble
BOARD := $(shell cat board.local 2>/dev/null)

# Built from Firmware/picotool with USB support - see CMakeLists.txt.
PICOTOOL := build/picotool/picotool

build:
	cmake --build build

$(BOARDS):
	cmake --build build --target pedal-$@

all-boards:
	cmake --build build --target all-boards

usb-device:
	cmake -B build
	cmake --build build

#
# Nothing here flashes build/pedal.elf, because there is no such file on
# purpose: every artifact is named for the board it is for.
#
flash: build
	@test -n "$(BOARD)" || { echo "no board.local - see 'make prep'"; exit 1; }
	$(PICOTOOL) load build/pedal-$(BOARD).elf && $(PICOTOOL) reboot

flash-%:
	cmake --build build --target pedal-$*
	$(PICOTOOL) load build/pedal-$*.elf && $(PICOTOOL) reboot

#
# The submodules the pedal's own build needs, by name.
#
# Not every submodule: nRF54/nrf is the Nordic SDK, a quarter of a
# gigabyte on its own and several more once west has resolved it, and
# nobody building a .uf2 should pay for the radio's toolchain.
# 'make -C nRF54 fetch' is where that is bought, deliberately.
#
prep:
	git submodule update --init --recursive \
		Firmware/pico-sdk Firmware/tinyusb Firmware/picotool
	cmake -S . -B build

.PHONY: build all-boards usb-device flash prep $(BOARDS)
