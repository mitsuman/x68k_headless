# Convenience wrapper around CMake.
BUILD_DIR ?= build
CMAKE ?= cmake

.PHONY: all build test clean install

all: build

build:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	X68K_BIN=$(BUILD_DIR)/x68k python3 tests/smoke_test.py --bin $(BUILD_DIR)/x68k

clean:
	rm -rf $(BUILD_DIR)

install: build
	$(CMAKE) --install $(BUILD_DIR)
