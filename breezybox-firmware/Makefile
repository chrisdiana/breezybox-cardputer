PORT ?= /dev/cu.usbmodem1101
IDF_PY ?= idf.py

.PHONY: help check-idf build flash monitor flash-monitor clean fullclean erase

help:
	@echo "Targets:"
	@echo "  make build           Reconfigure and build firmware plus LittleFS image"
	@echo "  make flash           Flash firmware to $(PORT)"
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

flash: check-idf
	$(IDF_PY) -p $(PORT) reconfigure flash

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
