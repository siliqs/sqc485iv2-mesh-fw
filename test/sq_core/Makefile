# firmware_core host tests — pure C11, no external dependencies.
#
#   make test          build and run (address + UB sanitizers on)
#   make test SAN=0    without sanitizers
#   make golden        regenerate the blob fixtures from the spec script
#   make clean
#
# The point of having no dependencies is that this suite runs anywhere the
# firmware is edited: no PlatformIO, no Docker, no portduino toolchain.

CORE   := ../../src/siliqs/firmware_core
BUILD  := build
BIN    := $(BUILD)/sq_core_tests

CC     ?= cc
SAN    ?= 1

CORE_SRC := $(CORE)/config.c $(CORE)/modbus.c $(CORE)/poll.c $(CORE)/sqcmd.c
MOCK_SRC := mock/mock_board.c mock/mock_board_check.c mock/mock_serial.c mock/mock_store.c mock/mock_time.c
TEST_SRC := golden.c test_crc.c test_config.c test_blob.c test_modbus.c test_poll.c test_sqcmd.c main.c
SRC      := $(CORE_SRC) $(MOCK_SRC) $(TEST_SRC)

CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow \
          -I$(CORE)/include -Imock -I. \
          -DSQ_GOLDEN_DIR='"$(CURDIR)/golden"'

ifeq ($(SAN),1)
  CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
endif

.PHONY: all test golden clean
all: $(BIN)

$(BIN): $(SRC) $(wildcard *.h) $(wildcard mock/*.h) tests.def | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

test: $(BIN)
	@./$(BIN)

golden:
	@python3 golden/make_golden.py

clean:
	@rm -rf $(BUILD)
