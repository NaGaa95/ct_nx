#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	ct_nx
APP_TITLE	:=	CHRONO TRIGGER
APP_AUTHOR	:=	naga, ppkantorski
APP_VERSION	:=	1.1.1
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source
#ROMFS	:=	romfs

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O3 -ffunction-sections -flto \
            -fuse-linker-plugin -fomit-frame-pointer -finline-small-functions \
            -fno-strict-aliasing -frename-registers -falign-functions=16 \
            -ftree-vectorize -ffast-math -fno-unwind-tables -fno-asynchronous-unwind-tables \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

# Enable Logging
#CFLAGS += -DEBUG_INSTR=0


CXXFLAGS	:= $(CFLAGS) -std=c++26 -Wno-dangling-else

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# SDL2 backs the OpenSL ES shim (audio). GLESv2/EGL/glapi/drm_nouveau are the
# shader pipeline cocos2d-x renders with (pulled in by mesa). FreeType+libpng
# implement the system-font label raster (createTextBitmapShadowStroke). ffmpeg
# (+codec deps) decodes the FMV cutscenes (assets/001.dat..008.dat, H.264/AAC).
LIBS	:= -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
			-Wl,--start-group -lavformat -lavcodec -lswresample -lswscale -lavutil \
			-ldav1d -lopus -lvorbisidec -lwebp -logg -Wl,--end-group \
			-Wl,--start-group -lfreetype -lharfbuzz -Wl,--end-group \
			-lpng -lbz2 -lz -lnx -lm

CXXFLAGS += -fno-exceptions -ffunction-sections -fdata-sections -fno-rtti
LDFLAGS += -Wl,--as-needed -Wl,--gc-sections

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(PORTLIBS)/include/freetype2 \
			-I$(PORTLIBS)/include/SDL2 \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all sdout

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# builds the .nro (via `all`), then stages an sd-card-ready layout under
# ./sdout/switch/ct/ (nro + font folder) and zips its contents to ct_nx.zip
#---------------------------------------------------------------------------------
sdout: all
	@echo packaging sdout ...
	@rm -fr sdout $(TARGET).zip
	@mkdir -p sdout/switch/ct
	@mkdir -p sdout/switch/ct/mods
	@mkdir -p sdout/switch/ct/saves
	@cp $(TARGET).nro sdout/switch/ct/
	@cp -r font sdout/switch/ct/
	@cd sdout && zip -r -q ../$(TARGET).zip .
	@echo created $(TARGET).zip

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf sdout $(TARGET).zip

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
