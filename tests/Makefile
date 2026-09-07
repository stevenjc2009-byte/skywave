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

# Canonical list of host suites. tools/run_tests.sh parses this exact line
# and cross-checks it against its own SCRIPT_SUITES array before building
# anything, so a suite added to one and not the other stops the whole run
# with a loud mismatch instead of a quiet "ALL SUITES PASSED". Keep this line
# in the exact `TESTS := ...` form - no other assignment operator, nothing
# else on the same line - or that check's grep stops matching it.
TESTS := icy ring directory url httpmsg aac sbrgrid mirrors robust

# The sanitizers the robustness suite is built with. Overridable, and blank is
# a supported value: these host tests also get built on Windows with MinGW-w64,
# which ships no ASan runtime. `make robust SANFLAGS=` still runs every check,
# it just loses the memory-safety net - so the WSL run is the one that counts,
# and an off-by-one that lands inside a struct rather than across a redzone is
# caught by the exact-value assertions either way.
SANFLAGS ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

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

url: $(BUILD)
	@$(HOSTCC) $(CFLAGS) -o $(BUILD)/test_url test_url.c ../source/net/url.c
	@./$(BUILD)/test_url

httpmsg: $(BUILD)
	@$(HOSTCC) $(CFLAGS) -o $(BUILD)/test_httpmsg test_httpmsg.c ../source/net/httpmsg.c
	@./$(BUILD)/test_httpmsg

# Deliberately NOT one line like the targets above: the thing under test is
# Skywave's own aac_bridge.c, but it links against ~30 vendored Helix decoder
# files (RealNetworks, RPSL-licensed - see source/audio/aac/README.md) that
# this project does not own and only patches for narrow portability reasons.
# One of them (sbr.c) has a pre-existing printf format-specifier mismatch in
# an OOM-only diagnostic ('%d' against a sizeof() result) that is harmless but
# would be a hard failure under this file's own -Werror. Rather than adding a
# third portability patch to vendored source just to silence a warning that
# has nothing to do with correctness, the vendored files are compiled
# separately with relaxed warnings; Skywave's own code (aac_bridge.c,
# test_aac.c) still builds under the exact same $(CFLAGS)/-Werror as every
# other target in this file.
AAC_VENDOR_SRC := $(wildcard ../source/audio/aac/*.c)
AAC_VENDOR_OBJ := $(patsubst ../source/audio/aac/%.c,$(BUILD)/aacvendor_%.o,$(AAC_VENDOR_SRC))

$(BUILD)/aacvendor_%.o: ../source/audio/aac/%.c | $(BUILD)
	@$(HOSTCC) -std=c11 -O2 -g -Wall -I../source/audio/aac -c $< -o $@

aac: $(BUILD) $(AAC_VENDOR_OBJ)
	@$(HOSTCC) $(CFLAGS) -I../source/audio/aac -o $(BUILD)/test_aac \
		test_aac.c ../source/audio/aac_bridge.c $(AAC_VENDOR_OBJ)
	@./$(BUILD)/test_aac

# The SBR grid parser, guarding the numEnv clamps in sbrside.c. Two things make
# this target unlike `aac` above:
#
#   * UnpackSBRGrid is static, so test_sbrgrid.c INCLUDES sbrside.c to reach it.
#     That pulls vendored source into this translation unit, which cannot meet
#     $(CFLAGS)/-Werror - so this one TU is compiled at the same relaxed warning
#     level the vendored objects get. It is the file under test, not Skywave
#     code, so nothing of ours escapes -Werror by this.
#   * Because sbrside.c is compiled INTO the test, aacvendor_sbrside.o must be
#     filtered out of the link or every symbol in it is defined twice.
#
# Built with $(SANFLAGS) rather than plain: the bug this guards wrote one byte
# past a stack array, and ASan is what sees that directly. The value assertions
# in the suite still go red without a sanitizer, so the MinGW build (where
# SANFLAGS is blank) is still worth running.
SBRGRID_OBJ := $(filter-out $(BUILD)/aacvendor_sbrside.o,$(AAC_VENDOR_OBJ))

sbrgrid: $(BUILD) $(SBRGRID_OBJ)
	@$(HOSTCC) -std=c11 -O1 -g -Wall $(SANFLAGS) -I../source/audio/aac \
		-o $(BUILD)/test_sbrgrid test_sbrgrid.c $(SBRGRID_OBJ)
	@./$(BUILD)/test_sbrgrid

# The only target that compiles source/net/directory.c, which every other suite
# has to skip because it includes http.h -> tcp.h -> mbedtls. The trick is in
# tests/stub/http.h: `#include "http.h"` resolves next to the INCLUDING file
# before any -I path, so a -I here would lose to the real header. directory.c is
# therefore COPIED into the build directory alongside a copy of the stub, where
# the stub is the one it finds. Both copies are remade every run, so neither can
# drift from the tree - and nothing in source/ is modified to make this work.
mirrors: $(BUILD)
	@mkdir -p $(BUILD)/stub
	@cp stub/http.h ../source/net/directory.c $(BUILD)/stub/
	@$(HOSTCC) $(CFLAGS) -I../source/net -o $(BUILD)/test_mirrors \
		test_mirrors.c $(BUILD)/stub/directory.c ../source/net/directory_parse.c
	@./$(BUILD)/test_mirrors

# The hostile-input suite. Everything above feeds it well-formed data and checks
# the answer; this one feeds every parser that touches the network the worst
# input a public, community-editable station directory could hand it - truncated
# fields, embedded CR/LF, 30-digit numbers, NULs mid-record - and checks that
# nothing reads out of bounds, overflows, or quietly invents a record. It found
# six real bugs the first time it was run, two of them header injection, so it
# earns its place in the default run rather than living as a script somebody
# remembers to invoke.
#
# -O1 rather than -O2: at -O2 the sanitizers' reports lose frames worth having.
robust: $(BUILD)
	@$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-result $(SANFLAGS) \
		-o $(BUILD)/test_robust test_robust.c \
		../source/net/directory_parse.c ../source/net/url.c \
		../source/net/httpmsg.c ../source/net/icy.c ../source/store/favourites.c
	@./$(BUILD)/test_robust

clean:
	@rm -rf $(BUILD)
