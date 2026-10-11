# Stones — Go for TrimUI Brick (NextUI, tg5040)
SRC := src/main.c src/go.c src/ai.c src/gfx.c src/audio.c
HDR := $(wildcard src/*.h)
SDK_DIR ?= sdk
TG_TC ?= $(SDK_DIR)/tc/aarch64-linux-gnu-7.5.0-linaro/bin/aarch64-linux-gnu-gcc
TG_SDK ?= $(SDK_DIR)/usr/usr
BUILD := build

all: host tg5040

sdk:
	./scripts/bootstrap_sdk.sh

fonts:
	python3 tools/mkfont.py src/font_data.h

host: $(BUILD)/stones-host
$(BUILD)/stones-host: $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	cc -O2 -g -Wall -Wextra -Wno-unused-parameter $(shell sdl2-config --cflags) -o $@ $(SRC) $(shell sdl2-config --libs) -lpthread -lm

tg5040: $(BUILD)/stones
$(BUILD)/stones: $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(TG_TC) -O2 -mcpu=cortex-a53 -Wall -Wno-unused-parameter -I$(TG_SDK)/include/SDL2 -D_REENTRANT \
		-o $@ $(SRC) -L$(TG_SDK)/lib -Wl,-rpath-link,$(TG_SDK)/lib -lSDL2 -lpthread -lm
	$(dir $(TG_TC))aarch64-linux-gnu-strip $@

pak: tg5040
	rm -rf $(BUILD)/pak dist && mkdir -p $(BUILD)/pak/Tools/tg5040/Stones.pak dist
	cp $(BUILD)/stones packaging/launch.sh packaging/pak.json packaging/README.txt LICENSE $(BUILD)/pak/Tools/tg5040/Stones.pak/
	chmod +x $(BUILD)/pak/Tools/tg5040/Stones.pak/launch.sh $(BUILD)/pak/Tools/tg5040/Stones.pak/stones
	cd $(BUILD)/pak && zip -qr ../../dist/Stones.pakz Tools
	cd $(BUILD)/pak && zip -qr ../../dist/Stones-sdcard.zip Tools

test: tests/test_engine.c src/go.c src/ai.c
	cc -O2 -o $(BUILD)/test_engine tests/test_engine.c src/go.c src/ai.c -lpthread -lm && $(BUILD)/test_engine rules

clean:
	rm -rf $(BUILD) dist
.PHONY: all sdk fonts host tg5040 pak test clean
