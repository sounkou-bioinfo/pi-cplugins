CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -fPIC -Iinclude
BUILD_DIR := .build/conformance
PLUGIN_SRC := fixture/conformance_plugin.c
HOST_SRC := fixture/conformance_host.c
HOST_BIN := $(BUILD_DIR)/pi_cplugins_conformance

.PHONY: all readme check-abi check-node check clean

all: check

readme:
	quarto render README.qmd --to gfm --output README.md

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(HOST_BIN): $(BUILD_DIR) $(HOST_SRC) $(PLUGIN_SRC) include/pi_plugin.h
	$(CC) $(CFLAGS) -o $@ $(HOST_SRC) $(PLUGIN_SRC)

check-abi: $(HOST_BIN)
	$(HOST_BIN)

check-node:
	npm run check

check: check-abi check-node

clean:
	rm -rf $(BUILD_DIR)
