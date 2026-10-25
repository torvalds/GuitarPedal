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

prep:
	git submodule update --init --recursive
	cmake -S . -B build

#
# Tools that stay outside the tree but have to be set up once per
# machine.  The scripts say what each one needs and where it goes.
#
probe-rs:
	scripts/setup-probe-rs

build123d:
	scripts/setup-build123d

.PHONY: build all-boards usb-device flash prep probe-rs build123d $(BOARDS)
