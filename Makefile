# Snap Station emulation - Linux/macOS build (tests only)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -Isrc
LDFLAGS ?=

SRC_LIBS   = src/sticker_sheet.c src/joybus_snapstation.c src/smart_card.c
TEST_BINS  = test_sticker_sheet test_joybus test_smart_card

.PHONY: all test clean

all: $(TEST_BINS)

test_sticker_sheet: src/sticker_sheet.c test/test_sticker_sheet.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_joybus: src/joybus_snapstation.c test/test_joybus.c
	$(CC) $(CFLAGS) -o $@ $^

test_smart_card: src/smart_card.c test/test_smart_card.c
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_BINS)
	@echo "--- test_sticker_sheet ---"
	./test_sticker_sheet sticker_sheet_demo.bmp > /tmp/ss_stk.log && tail -3 /tmp/ss_stk.log
	@echo "--- test_joybus ---"
	@./test_joybus | tail -10
	@echo "--- test_smart_card ---"
	@./test_smart_card | tail -5

clean:
	rm -f $(TEST_BINS) *.o *.bmp
