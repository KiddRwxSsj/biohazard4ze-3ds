.SUFFIXES:

export DEVKITPRO ?= $(CURDIR)/tools/root/opt/devkitpro
export DEVKITARM ?= $(DEVKITPRO)/devkitARM
export PATH := $(DEVKITARM)/bin:$(DEVKITPRO)/tools/bin:$(CURDIR)/tools/bin:$(PATH)

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := re4z
BUILD       := build
SOURCES     := source
INCLUDES    := source
APP_TITLE   := Resident Evil 4 Zeebo Edition
APP_DESCRIPTION := Resident Evil 4 Zeebo Edition
APP_AUTHOR  := KiddRwxSsj
ICON        := meta/icon.png

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections -fno-strict-aliasing $(ARCH) $(INCLUDE) -D__3DS__ $(EXTRA)
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
PICAFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))

export LD := $(CC)
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES_BIN := $(PICAFILES:.v.pica=.shbin.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES := $(PICAFILES:.v.pica=_shbin.h)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ICON_ART ?= private/art/icon.webp
BANNER_ART ?= private/art/banner.png

.PHONY: all elf clean cia meta lang

all: elf meta/icon.png meta/banner.png
	@bannertool makebanner -i meta/banner.png -a meta/silence.wav -o $(BUILD)/banner.bnr
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(ICON) -o $(BUILD)/icon.icn
	@mkdir -p release
	@makerom -f cia -o release/$(TARGET).cia -elf $(TARGET).elf -rsf meta/re4z.rsf -target t -exefslogo \
		-icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr
	@echo built release/$(TARGET).cia

cia: all

elf: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

meta/icon.png meta/banner.png:
	@echo "no menu art yet: make meta ICON_ART=icon.png BANNER_ART=banner.png"; exit 1

meta:
	@$(CURDIR)/.venv/bin/python meta/make_meta.py "$(ICON_ART)" "$(BANNER_ART)" meta

# english patch table from re/lang/en.txt. MOD = a zeebo dump's mod/276675, only offsets and
# hashes of the pt strings come from it, no pt text gets copied
lang:
	@test -n "$(MOD)" || { echo "make lang MOD=/path/to/mod/276675"; exit 1; }
	@$(CURDIR)/.venv/bin/python re/lang.py build "$(MOD)" re/lang/en.txt source/lang_en.h

clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf release/$(TARGET).cia

else

$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES): $(HFILES)

.PRECIOUS: %.shbin
%.shbin.o %_shbin.h: %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

endif
