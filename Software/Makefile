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
BOARDS := split unified
BOARD := $(shell cat board.local 2>/dev/null)

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
	picotool load build/pedal-$(BOARD).elf && picotool reboot

flash-%:
	cmake --build build --target pedal-$*
	picotool load build/pedal-$*.elf && picotool reboot

prep:
	git submodule init
	git submodule update --init --recursive
	cmake -S . -B build

.PHONY: build all-boards usb-device flash prep $(BOARDS)
