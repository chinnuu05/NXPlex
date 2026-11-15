# NXPlex Tesla overlay (remote control + browser) -> nxplex.ovl
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

# DEVKITA64 can point at another copy of the toolchain (see tools/setup-toolchain.sh).
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
PREFIX    := $(DEVKITA64)/bin/aarch64-none-elf-
CXX       := $(PREFIX)g++
LIBNX     := $(DEVKITPRO)/libnx
PORTLIBS  := $(DEVKITPRO)/portlibs/switch

TARGET      := nxplex
APP_TITLE   := NXPlex
APP_AUTHOR  := NXPlex contributors
APP_VERSION := $(shell cat ../VERSION)

SRCS      := source/main.cpp
OBJS      := $(addprefix build/,$(notdir $(SRCS:.cpp=.o)))
vpath %.cpp source

ARCH      := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS  := -g -Wall -Wno-format-truncation -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
             -I$(LIBNX)/include -Ilib/libtesla/include -I../common -std=c++20 -fno-exceptions
LDFLAGS   := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,build/$(TARGET).map
LIBS      := -L$(LIBNX)/lib -lnx

all: $(TARGET).ovl

build/%.o: %.cpp ../common/nxplex_ipc.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

build/$(TARGET).elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

build/$(TARGET).nacp:
	@mkdir -p build
	nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).ovl: build/$(TARGET).elf build/$(TARGET).nacp
	elf2nro $< $@ --nacp=build/$(TARGET).nacp
	@echo "built $@ ($$(stat -c %s $@) bytes)"

clean:
	rm -f build/*.o build/*.d build/*.elf build/*.nacp build/*.map $(TARGET).ovl

-include build/*.d
.PHONY: all clean
