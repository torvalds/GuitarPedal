#
# The radio's firmware.  Nordic's toolchain, not the pedal's.
#
# Nothing here is reached by the top-level build and 'make prep' does not
# fetch any of it: this is several gigabytes of Zephyr, and a clone that
# only wants a .uf2 should not pay for it.  Same bargain as
# Hardware/Models.
#
# 'sdk-nrf' is a submodule and is the version pin - it is the one thing
# git records.  Everything else west clones from the manifest inside it,
# so upstream stays the authority on which revisions go together.
#
TOPDIR		:= $(CURDIR)
VENV		:= $(TOPDIR)/.venv
WEST		:= $(VENV)/bin/west
MANIFEST	:= nrf

BOARD		:= minimal/nrf54l10/cpuapp
APP		:= app
BUILD		:= $(TOPDIR)/build
BUILD_NOBT	:= $(TOPDIR)/build-nobt

SOURCES		:= $(wildcard $(APP)/src/*.c $(APP)/src/*.h $(APP)/*.conf \
			      boards/pedal/minimal/*)

all: $(BUILD)/app/zephyr/zephyr.hex

#
# One target that gets a machine from nothing to able to build.  It is
# split into stamps rather than one script so that an interrupted fetch
# resumes instead of starting again.
#
fetch: .stamp-sdk

$(VENV)/bin/python:
	python3 -m venv $(VENV)

$(WEST): $(VENV)/bin/python
	$(VENV)/bin/pip install --upgrade pip west

$(MANIFEST)/west.yml:
	git submodule update --init $(MANIFEST)

#
# The manifest's own groups, less the two this build has no use for:
# 'optional' is several gigabytes of things nothing here selects, and
# 'testing' is a unit-test framework for code we do not build.
#
# cmock is in neither group and has to be named.  It is a mocking
# library, so nothing here wants it either, and it cannot be cloned on
# a machine with transfer.fsckobjects set: one of its own submodules
# has a malformed URL upstream, which git refuses to follow.  Excluding
# the project is the fix rather than turning the check off.
#
.stamp-west: $(WEST) $(MANIFEST)/west.yml
	$(WEST) init -l $(MANIFEST)
	$(WEST) config manifest.group-filter -- -testing,-optional
	$(WEST) config manifest.project-filter -- -cmock
	touch $@

.stamp-update: .stamp-west
	$(WEST) update
	touch $@

#
# Zephyr's own python dependencies.  They are not west's: 'west build'
# and 'west sdk' are extension commands that come from the zephyr
# repository, so neither exists until the workspace is cloned and these
# are installed.
#
.stamp-deps: .stamp-update
	$(VENV)/bin/pip install -r zephyr/scripts/requirements-base.txt
	touch $@

#
# Only the ARM toolchain.  The full Zephyr SDK carries a dozen
# architectures this project will never build for.
#
.stamp-sdk: .stamp-deps
	$(WEST) sdk install -t arm-zephyr-eabi
	touch $@

$(BUILD)/app/zephyr/zephyr.hex: .stamp-sdk $(SOURCES)
	$(WEST) build -b $(BOARD) -d $(BUILD) $(APP) -- -DBOARD_ROOT=$(TOPDIR)

#
# The same program without the radio stack.  It exists for bring-up:
# MPSL initialises ahead of the UART, so a board that says nothing has
# either failed before MPSL or inside it, and this image is what tells
# those two apart.
#
nobt: $(BUILD_NOBT)/app/zephyr/zephyr.hex

$(BUILD_NOBT)/app/zephyr/zephyr.hex: .stamp-sdk $(SOURCES)
	$(WEST) build -b $(BOARD) -d $(BUILD_NOBT) $(APP) -- \
		-DBOARD_ROOT=$(TOPDIR) -DEXTRA_CONF_FILE=$(TOPDIR)/$(APP)/nobt.conf

clean:
	rm -rf $(BUILD) $(BUILD_NOBT)

#
# Deliberately not 'clean': it throws away the download.
#
distclean: clean
	rm -rf $(VENV) .west .stamp-* bootloader modules nrfxlib tools zephyr

.PHONY: all nobt fetch clean distclean
