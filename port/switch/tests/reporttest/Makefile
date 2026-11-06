# The Switch report sender on its own: make OUT=<folder with no spaces>
# Needs switch-dev, switch-mbedtls, switch-zlib.
OUT ?= build
CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
INC := -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include
CXXFLAGS := -std=gnu++20 -O2 -g -ffunction-sections $(ARCH) -D__SWITCH__ $(INC)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib
LIBS := -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx

$(OUT)/reporttest.nro: $(OUT)/reporttest.elf
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

$(OUT)/reporttest.elf: main.cpp ../../nx_report.cpp ../../nx_report.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) main.cpp ../../nx_report.cpp $(LDFLAGS) $(LIBS) -o $@
