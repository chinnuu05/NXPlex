# NXPlex background sysmodule -> exefs.nsp for atmosphere/contents/4200000000504C58/
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

# DEVKITA64 can point at another copy of the toolchain (see tools/setup-toolchain.sh).
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
PREFIX    := $(DEVKITA64)/bin/aarch64-none-elf-
CXX       := $(PREFIX)g++
LIBNX     := $(DEVKITPRO)/libnx
PORTLIBS  := $(DEVKITPRO)/portlibs/switch

TARGET    := nxplex-sys
VERSION   := $(shell cat ../VERSION)
CORE      := platform mkv decoders player thread config net json plex app
SRCS      := $(wildcard source/*.cpp) $(addprefix ../core/,$(addsuffix .cpp,$(CORE)))
OBJS      := $(addprefix build/,$(notdir $(SRCS:.cpp=.o)))
vpath %.cpp source ../core

ARCH      := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS  := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
             -I$(LIBNX)/include -I$(PORTLIBS)/include -I../core -I../common -Isource \
             -std=gnu++17 -fno-rtti -fno-exceptions -DNXP_SYS_VERSION='"$(VERSION)"'
LDFLAGS   := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,build/$(TARGET).map
LIBS      := -L$(PORTLIBS)/lib -L$(LIBNX)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lnx

all: $(TARGET).nsp

build/%.o: %.cpp $(wildcard ../core/*.hpp) $(wildcard source/*.hpp) ../common/nxplex_ipc.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

build/$(TARGET).elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

build/$(TARGET).nso: build/$(TARGET).elf
	elf2nso $< $@

build/main.npdm: $(TARGET).json
	@mkdir -p build
	npdmtool $< $@

$(TARGET).nsp: build/$(TARGET).nso build/main.npdm
	@mkdir -p build/exefs
	cp build/$(TARGET).nso build/exefs/main
	cp build/main.npdm build/exefs/main.npdm
	build_pfs0 build/exefs $@
	@echo "built $@ ($$(stat -c %s $@) bytes)"

clean:
	rm -f build/*.o build/*.d build/*.elf build/*.nso build/*.npdm build/*.map build/exefs/main build/exefs/main.npdm $(TARGET).nsp

-include build/*.d
.PHONY: all clean
