#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
TARGET      := skywave
BUILD       := build
# One directory per layer. The globbing below is per-directory, so a new module
# only has to land in one of these to be compiled - but note that means every
# .c under them is built, tests included. That is why the host tests live in
# tests/ rather than beside the code they cover.
SOURCES     := source source/app source/audio source/audio/aac source/net source/store \
               source/ui source/update
DATA        := data
# source/audio/aac is on the include path so the vendored Helix decoder's own
# #include "aacXXX.h" (quote-form, resolved by the compiler against the
# including file's directory first) and player.c's #include "aac/aacdec.h"
# (angle-path-style relative include) both resolve, and so the two
# Skywave-authored shim headers in that directory (Arduino.h, pgmspace.h)
# satisfy Helix's own angle-bracket #include <Arduino.h>/<pgmspace.h> without
# editing a single byte of the vendored source - see source/audio/aac/README.md.
INCLUDES    := source source/audio/aac
# Holds one file: the certificate authority bundle the updater verifies against.
# It has to be a real file rather than a header baked into the binary because
# mbedtls parses it from a path, and because a user can drop a newer bundle at
# sdmc:/config/ssl/cacert.pem without waiting for a Skywave release - which is
# the point of that convention.
ROMFS       := romfs
APP_TITLE   := Skywave
APP_DESCRIPTION := Internet radio for the Nintendo 3DS
APP_AUTHOR  := Skywave

#---------------------------------------------------------------------------------
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -Wextra -O2 -mword-relocations \
           -ffunction-sections \
           $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# mpg123 comes from the devkitPro 3ds-mpg123 package and lives in portlibs, not
# in libctru - hence the second entry in LIBDIRS. Link order matters: mpg123
# has to come before -lm because it is what pulls the maths in.
#
# mbedtls is the app's own TLS, replacing the console's ssl:C for everything
# except random numbers. The three libraries have to be in this order between
# themselves, and all three ahead of -lctru: devkitPro's mbedcrypto gets its
# entropy from sslcGenerateRandomData, which lives in libctru.
LIBS    := -lmpg123 -lcitro2d -lcitro3d -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm
LIBDIRS := $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES     := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(strip $(ROMFS)),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

MAKEROM    := makerom
BANNERTOOL := bannertool

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# "all" is listed first so the elf exists before the CIA rules are considered.
cia: all $(OUTPUT).cia

$(OUTPUT).banner : cia/banner.png cia/banner.wav
	@echo banner ...
	@$(BANNERTOOL) makebanner -i cia/banner.png -a cia/banner.wav -o $@

# APP_ROMFS is substituted into the RSF's RomFs RootPath. It is passed as an
# absolute path because makerom resolves a relative one against its own working
# directory, not against the RSF, which is the kind of difference that only
# shows up as a CIA with no romfs in it and no error anywhere.
$(OUTPUT).cia : $(OUTPUT).elf $(OUTPUT).smdh $(OUTPUT).banner cia/$(TARGET).rsf
	@echo $(notdir $@) ...
	@$(MAKEROM) -f cia -o $@ -elf $(OUTPUT).elf -rsf cia/$(TARGET).rsf \
		-DAPP_ROMFS=$(CURDIR)/$(ROMFS) \
		-icon $(OUTPUT).smdh -banner $(OUTPUT).banner -exefslogo -target t
	@echo built ... $(notdir $@)

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf \
		$(TARGET).cia $(TARGET).banner

#---------------------------------------------------------------------------------
else

DEPENDS := $(OFILES:.o=.d)

ifeq ($(strip $(NO_SMDH)),)
$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
else
$(OUTPUT).3dsx : $(OUTPUT).elf
endif

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

endif
