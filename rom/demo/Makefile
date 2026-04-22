# Build a minimal Snap Station HLE demo ROM using libdragon.
# Invoked inside the libdragon-local Docker container, which has
# libdragon installed at $(N64_INST) = /n64_toolchain.
#
# Layout:
#   rom/snapstation.h + snapstation.c  (library: ROM-side driver)
#   rom/demo/main.c + Makefile         (sample consumer: this file)

BUILD_DIR  = build
SOURCE_DIR = .

# VPATH lets make find snapstation.c in the parent rom/ directory when
# the implicit $(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c rule tries to locate
# it. Without this we'd need an explicit per-source rule.
VPATH = ..

N64_ROM_TITLE      = "Snap Station Demo"
N64_ROM_SAVETYPE   = none
N64_ROM_REGIONFREE = true

include $(N64_INST)/include/n64.mk

# Add the parent rom/ directory to the include path so both sources can
# use #include "snapstation.h" regardless of where they live.
CFLAGS += -I.. -I.

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/snapstation.o

all: snapstation_demo.z64

snapstation_demo.z64: $(BUILD_DIR)/snapstation_demo.elf
$(BUILD_DIR)/snapstation_demo.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) snapstation_demo.z64 snapstation_demo.elf* \
	       *.sym *.map *.stripped

.PHONY: all clean

-include $(wildcard $(BUILD_DIR)/*.d)
