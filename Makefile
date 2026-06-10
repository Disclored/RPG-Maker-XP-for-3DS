.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM not set.")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET   := mkxp-3ds
BUILD    := build
SOURCES  := source
INCLUDES := source \
	    ruby/include \
	    libs/include \
	    libs/include/mruby
ROMFS    := romfs

APP_TITLE  := mkxp-3DS
APP_AUTHOR := mkxp-3ds-port
APP_DESC   := RPG Maker XP for Nintendo 3DS

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -O2 -mword-relocations \
	    -fomit-frame-pointer -ffunction-sections \
	    $(ARCH) $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -std=c++11 -fexceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := -lcitro2d -lcitro3d -lctru -lm -lmruby -lz

LIBDIRS  := $(CTRULIB) $(PORTLIBS) $(CURDIR)/libs

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
	          $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES         := $(OFILES_SOURCES)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	           $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	           -I$(PORTLIBS)/include \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@echo Cleaning...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).3dsx
$(OUTPUT).3dsx: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)
-include $(DEPENDS)
endif
