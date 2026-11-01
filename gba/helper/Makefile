# The GBA helper (multiboot-and-save.md §4): built with the ufbt toolchain's arm-none-eabi-gcc,
# never as part of a FAP. Its image, files/gen3_helper.mb, is committed: `make install` writes it,
# and `make check` rebuilds it from scratch and compares it with the committed file.
#
#   make            build/helper.mb
#   make size       its section sizes and length (at most MAX_LEN)
#   make install    copy it to files/gen3_helper.mb
#   make check      a fresh build is byte-identical to files/gen3_helper.mb
#   make clean
#
# The image must build byte-identically with every toolchain that passes the version check;
# CROSS selects one, e.g. make check CROSS=<other ufbt home>/toolchain/current/bin/arm-none-eabi-

CROSS ?= $(HOME)/.ufbt/toolchain/current/bin/arm-none-eabi-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE = $(CROSS)size
PYTHON ?= python3
GCC_VERSION = 12.3.1
# The helper side's one copy of the image cap, passed to fix_header.py. It must equal
# GEN3_MB_MAX_LEN (src/gen3/gen3_mb.h), the Flipper sender's cap; tests/test_helper_image.c
# checks that the two agree.
MAX_LEN = 12288

BUILD = build
IMAGE = ../../files/gen3_helper.mb

CFLAGS = -mcpu=arm7tdmi -mthumb -mthumb-interwork -Os -ffreestanding -fno-builtin \
         -ffunction-sections -Wall -Wextra -DHP_GBA -Iinclude
ASFLAGS = -mcpu=arm7tdmi -Iinclude
LDFLAGS = -mcpu=arm7tdmi -mthumb -mthumb-interwork -nostartfiles -nostdlib \
          -T helper.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments
LIBS = -lc_nano -lgcc

# Sorted, so the link order (and the image) doesn't depend on the make version.
SRCS = $(sort $(wildcard src/*.c)) $(sort $(wildcard gba/*.c))
OBJS = $(BUILD)/crt0.o $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
HDRS = $(wildcard include/*.h)

.PHONY: all toolchain size install check clean

all: toolchain $(BUILD)/helper.mb

toolchain:
	@if [ "$$($(CC) -dumpversion 2>/dev/null)" != "$(GCC_VERSION)" ]; then \
		echo "gba/helper needs the ufbt toolchain (arm-none-eabi-gcc $(GCC_VERSION)); set CROSS=…/bin/arm-none-eabi-" >&2; \
		exit 1; \
	fi
	@if [ -f "$(dir $(CROSS))../VERSION" ]; then \
		echo "ufbt toolchain VERSION $$(cat "$(dir $(CROSS))../VERSION"): $(CC)"; \
	fi

$(BUILD)/crt0.o: crt0.S $(HDRS) | toolchain
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c $(HDRS) | toolchain
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/helper.elf: $(OBJS) helper.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD)/helper.bin: $(BUILD)/helper.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/helper.mb: $(BUILD)/helper.bin fix_header.py Makefile
	$(PYTHON) -I fix_header.py $< $@ $(MAX_LEN)

size: all
	$(SIZE) $(BUILD)/helper.elf
	@n=$$(wc -c < $(BUILD)/helper.mb | tr -d ' '); \
	echo "$(BUILD)/helper.mb: $$n bytes (at most $(MAX_LEN))"; \
	[ "$$n" -le $(MAX_LEN) ]

install: all
	cp $(BUILD)/helper.mb $(IMAGE)

# A fresh build in its own directory, so that objects from another toolchain are never reused.
check: toolchain
	rm -rf $(BUILD)/check
	$(MAKE) --no-print-directory BUILD=$(BUILD)/check $(BUILD)/check/helper.mb
	cmp $(BUILD)/check/helper.mb $(IMAGE)
	@echo "$(IMAGE): identical to a fresh build with $(CC)"

clean:
	rm -rf $(BUILD)
