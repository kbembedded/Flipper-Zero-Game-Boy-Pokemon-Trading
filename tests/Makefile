CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I..
CFLAGS += -I../gba/helper/include

BUILD = build
TESTS = $(basename $(notdir $(wildcard test_*.c)))
HELPER_SRCS = $(wildcard ../gba/helper/src/*.c)
SRCS = $(filter-out %_furi.c,$(wildcard ../src/gen3/*.c)) $(wildcard fake_*.c) $(HELPER_SRCS)

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(wildcard *.h) $(SRCS) $(wildcard ../src/gen3/*.h) $(wildcard ../gba/helper/include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -DTEST_NAME='"$*"' $< $(SRCS) -o $@

run-%: $(BUILD)/%
	./$<

clean:
	rm -rf $(BUILD)
