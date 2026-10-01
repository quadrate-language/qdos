# Building is Meson's job; see README.md. This is here for the housekeeping
# that wants one spelling across both repositories.
#
#   make format     reformat in place
#   make fmtcheck   report what is not formatted, change nothing
#   make esp32-qemu build the ESP32-S3 firmware and boot it under QEMU

# The .clang-format is Quadrate's, copied so the two trees read alike. QDOS is
# mostly C where Quadrate is C++, so this sweeps .c as well.
#
# The generated font tables are not listed here: they carry `clang-format off`
# themselves, emitted by tools/genfont.py and tools/genpadfont.py, so an editor
# formatting on save leaves them alone too.
SOURCES = $(shell find src include tests examples -type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null)

.PHONY: format fmtcheck esp32-qemu esp32-qemu-build

format:
	clang-format -i $(SOURCES)

fmtcheck:
	@clang-format --dry-run --Werror $(SOURCES)

# The ESP32-S3 port under Espressif's QEMU; see esp32/README.md. idf.py is used
# if it is on the PATH, otherwise the espressif/idf image, with the Quadrate
# tree beside this one. The panel is a window and the keys are typed in the
# terminal, as at the serial console; QEMU_DISPLAY=none for no window. Quit
# with Ctrl-a x.
ESP32_BUILD = esp32/build-qemu
ESP32_FLASH = $(ESP32_BUILD)/flash.bin
IDF_IMAGE = espressif/idf:v5.5
IDF_ARGS = -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
	-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"

# The v5.5 image's QEMU predates the S3's octal PSRAM, so a release is fetched
QEMU_RELEASE = esp-develop-9.2.2-20260417
QEMU_ASSET = qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-$(shell uname -m)-linux-gnu.tar.xz
QEMU ?= $(ESP32_BUILD)/qemu/bin/qemu-system-xtensa
QEMU_DISPLAY ?= sdl

# $(call idf,dir under esp32,command)
ifneq ($(shell command -v idf.py),)
idf = cd esp32/$(1) && $(2)
else
idf = docker run --rm -u $$(id -u):$$(id -g) -e HOME=/tmp \
	-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
	-v $(abspath ..):/work -w /work/$(notdir $(CURDIR))/esp32/$(1) $(IDF_IMAGE) $(2)
endif

esp32-qemu: esp32-qemu-build $(QEMU)
	$(QEMU) -M esp32s3 -m 8M -global driver=ssi_psram,property=is_octal,value=true \
		-display $(QEMU_DISPLAY) -serial mon:stdio -drive file=$(ESP32_FLASH),if=mtd,format=raw

# The image holds the data partition too, so it is only remade when the
# firmware changes, and the session survives a reboot until then
esp32-qemu-build: $(ESP32_BUILD)/sdkconfig
	$(call idf,,idf.py $(IDF_ARGS) build)
	$(MAKE) --no-print-directory $(ESP32_FLASH)

# idf.py only reads the defaults for a key its sdkconfig lacks
$(ESP32_BUILD)/sdkconfig: esp32/sdkconfig.defaults esp32/sdkconfig.qemu
	rm -f $@

$(ESP32_FLASH): $(ESP32_BUILD)/qdos.bin $(ESP32_BUILD)/system.bin
	$(call idf,build-qemu,python -m esptool --chip esp32s3 merge_bin --fill-flash-size 8MB -o flash.bin @flash_args)

$(ESP32_BUILD)/qemu/bin/qemu-system-xtensa:
	mkdir -p $(ESP32_BUILD)
	curl -fL https://github.com/espressif/qemu/releases/download/$(QEMU_RELEASE)/$(QEMU_ASSET) \
		| tar xJ -C $(ESP32_BUILD)
