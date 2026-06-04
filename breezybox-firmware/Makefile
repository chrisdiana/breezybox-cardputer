PORT ?= /dev/cu.usbmodem1101
IDF_PY ?= idf.py

.PHONY: help check-idf build final-package launcher-package flash app-flash fs-flash monitor flash-monitor clean fullclean erase

help:
	@echo "Targets:"
	@echo "  make build           Reconfigure and build firmware plus LittleFS image"
	@echo "  make final-package    Build the full flash image (.bin)"
	@echo "  make launcher-package Build the app-only Launcher-compatible image"
	@echo "  make flash           Flash firmware to $(PORT)"
	@echo "  make app-flash       Flash only the app partition and preserve /root"
	@echo "  make fs-flash        Flash only the /root LittleFS partition"
	@echo "  make monitor         Open serial monitor on $(PORT)"
	@echo "  make flash-monitor   Flash and open monitor"
	@echo "  make erase           Erase flash"
	@echo "  make clean           Remove build output"
	@echo "  make fullclean       Full clean via idf.py"
	@echo ""
	@echo "Override serial port with:"
	@echo "  make flash PORT=/dev/cu.usbmodemXXXX"

check-idf:
	@command -v $(IDF_PY) >/dev/null 2>&1 || { \
		echo "error: $(IDF_PY) not found. Activate ESP-IDF first, e.g.:"; \
		echo "  source ~/esp/esp-idf/export.sh"; \
		exit 1; \
	}

build: check-idf
	$(IDF_PY) reconfigure build

final-package: build
	python3 ../tools/build_launcher_bin.py --build-dir build --firmware-dir . --out build/breezybox-cardputer.bin

launcher-package: build
	python3 ../tools/build_launcher_bin.py --build-dir build --firmware-dir . --out build/breezybox-cardputer.bin

flash: check-idf
	$(IDF_PY) -p $(PORT) reconfigure flash

app-flash: build
	$(IDF_PY) -p $(PORT) app-flash

fs-flash: build
	python3 -m esptool --chip esp32s3 --port $(PORT) --baud 460800 --before default_reset --after hard_reset write_flash 0x4f0000 build/spiffs.bin

monitor: check-idf
	$(IDF_PY) -p $(PORT) monitor

flash-monitor: check-idf
	$(IDF_PY) -p $(PORT) reconfigure flash monitor

erase: check-idf
	$(IDF_PY) -p $(PORT) erase-flash

clean: check-idf
	$(IDF_PY) clean

fullclean: check-idf
	$(IDF_PY) fullclean
