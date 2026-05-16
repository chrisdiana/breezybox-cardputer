PORT ?= /dev/cu.usbmodem1101
BAUD ?= 460800
BOARD ?= cardputer

CARDPUTER_DIR := /Users/chris/code/hardware/breezy-cardputer-2/cardputer-demo
PIO_CORE_DIR := $(CARDPUTER_DIR)/.pio-core
PIO := $(CARDPUTER_DIR)/.venv/bin/platformio
ESPTOOL := python3 /Users/chris/.platformio/packages/tool-esptoolpy/esptool.py
FIRMWARE_DIR := $(CURDIR)/breezybox-firmware
IDF_PY ?= idf.py
BUILD_DIR := $(FIRMWARE_DIR)/build-$(BOARD)
SDKCONFIG_DEFAULTS_FILE := $(FIRMWARE_DIR)/sdkconfig.defaults.$(BOARD)
SDKCONFIG_FILE := $(BUILD_DIR)/sdkconfig

ifeq ($(BOARD),sticks3)
IDF_TARGET := esp32s3
else
IDF_TARGET := esp32s3
endif

CARDPUTER_ENV := env PLATFORMIO_CORE_DIR=$(PIO_CORE_DIR)
CARDPUTER_BIN := $(CARDPUTER_DIR)/.pio/build/cardputer/cardputer_breezy.bin
FIRMWARE_BUILD_DIR := $(BUILD_DIR)
IDF_ARGS := -B $(BUILD_DIR) -DIDF_TARGET=$(IDF_TARGET) -DBREEZY_BOARD=$(BOARD) -DSDKCONFIG=$(SDKCONFIG_FILE) -DSDKCONFIG_DEFAULTS=$(SDKCONFIG_DEFAULTS_FILE)

.PHONY: help check-cardputer check-firmware build cardputer-build firmware-build launcher-package breezydemo-build cardputer-shell-build rebuild flash monitor erase clean cardputer-shell-flash cardputer-shell-monitor cardputer-shell-erase

help:
	@echo "Targets:"
	@echo "  make build              Build the BreezyBox Cardputer port"
	@echo "  make flash              Reconfigure, build, and flash breezybox-firmware to $(PORT)"
	@echo "  make monitor            Open the breezybox-firmware serial monitor on $(PORT)"
	@echo "  make launcher-package   Build a Launcher-compatible install image"
	@echo "  make erase              Erase flash on $(PORT)"
	@echo "  make cardputer-build    Alias for firmware-build"
	@echo "  make firmware-build     Build the BreezyBox Cardputer port only"
	@echo "  make breezydemo-build   Backward-compatible alias for firmware-build"
	@echo "  make rebuild            Fullclean and rebuild the BreezyBox Cardputer port"
	@echo "  make cardputer-shell-build   Build the legacy Arduino shell image"
	@echo "  make cardputer-shell-flash   Flash the legacy Arduino shell image"
	@echo "  make cardputer-shell-monitor Monitor the legacy Arduino shell serial port"
	@echo "  make clean              Remove build output"
	@echo ""
	@echo "Overrides:"
	@echo "  make flash PORT=/dev/cu.usbmodemXXXX"
	@echo "  make flash BAUD=115200"
	@echo "  make build BOARD=sticks3"

check-cardputer:
	@test -x $(PIO) || { \
		echo "error: $(PIO) not found or not executable"; \
		exit 1; \
	}

check-firmware:
	@command -v $(IDF_PY) >/dev/null 2>&1 || { \
		echo "error: $(IDF_PY) not found. Activate ESP-IDF first, e.g.:"; \
		echo "  source ~/esp/esp-idf/export.sh"; \
		exit 1; \
	}

build: firmware-build

cardputer-build: firmware-build

cardputer-shell-build: check-cardputer
	cd $(CARDPUTER_DIR) && $(CARDPUTER_ENV) $(PIO) run

firmware-build: check-firmware
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) reconfigure build

launcher-package: firmware-build
	python3 tools/build_launcher_bin.py --build-dir $(BUILD_DIR) --firmware-dir $(FIRMWARE_DIR) --out $(BUILD_DIR)/breezybox-$(BOARD)-launcher.bin

breezydemo-build: firmware-build

rebuild: check-firmware
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) fullclean
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) reconfigure build

flash: check-firmware
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) -p $(PORT) reconfigure flash

monitor: check-firmware
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) -p $(PORT) monitor

erase: check-firmware
	cd $(FIRMWARE_DIR) && $(IDF_PY) $(IDF_ARGS) -p $(PORT) erase-flash

cardputer-shell-flash: check-cardputer cardputer-shell-build
	$(ESPTOOL) --chip esp32s3 --port $(PORT) --baud $(BAUD) write_flash 0x0 $(CARDPUTER_BIN)

cardputer-shell-monitor: check-cardputer
	cd $(CARDPUTER_DIR) && $(CARDPUTER_ENV) $(PIO) device monitor -b 115200 -p $(PORT)

cardputer-shell-erase:
	$(ESPTOOL) --chip esp32s3 --port $(PORT) erase_flash

clean:
	rm -rf $(CARDPUTER_DIR)/.pio/build/cardputer
	rm -rf $(FIRMWARE_BUILD_DIR)
