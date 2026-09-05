# Host test suite. This builds with the SYSTEM compiler, not devkitARM - the
# modules under test are deliberately free of 3DS headers so their logic can be
# proven on a PC in a second rather than on hardware in a minute.
#
#   make            build and run everything
#   make icy CAP=/path/to/capture.bin   run the demuxer against a real stream
#
# Nothing here is part of the 3DS build; the console Makefile globs `source/`
# only, so these never end up in the app.

# NOT `CC ?= gcc`: make predefines CC as `cc`, so `?=` never fires and the
# build dies with "cc: No such file or directory" on any machine without a cc
# symlink - which includes this one.
HOSTCC  ?= gcc
CFLAGS  := -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-result
BUILD   := build

# The capture is optional and deliberately NOT committed: it would be a few
# hundred KB of somebody else's broadcast. Grab one with:
#   curl -H "Icy-MetaData: 1" --max-filesize 400000 \
#        http://media-ice.musicradio.com/ClassicFMMP3 -o capture.bin
CAP ?= capture.bin

# A saved directory response is also optional, for the same reason - it is a
# snapshot of somebody else's database. Grab one with:
#   curl "http://de1.api.radio-browser.info/json/stations/search?codec=MP3&hls=0&limit=30&hidebroken=true" -o real.json
JSON ?= real.json

TESTS := icy ring directory

.PHONY: all clean $(TESTS)

all: $(TESTS)

$(BUILD):
	@mkdir -p $(BUILD)

icy: $(BUILD)
	@$(HOSTCC) $(CFLAGS) -o $(BUILD)/test_icy test_icy.c ../source/net/icy.c
	@./$(BUILD)/test_icy $(CAP)

ring: $(BUILD)
	@$(HOSTCC) $(CFLAGS) -o $(BUILD)/test_ring test_ring.c ../source/audio/ring.c
	@./$(BUILD)/test_ring

directory: $(BUILD)
	@$(HOSTCC) $(CFLAGS) -o $(BUILD)/test_directory test_directory.c ../source/net/directory_parse.c
	@./$(BUILD)/test_directory $(JSON)

clean:
	@rm -rf $(BUILD)
