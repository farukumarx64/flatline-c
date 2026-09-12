CC ?= cc
PYTHON ?= python3
INTEGRATION_ARGS ?=
SANITIZE ?= 0

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O0 -g
PROJECT_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
	-Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZER_FLAGS :=

ifeq ($(SANITIZE),1)
BUILD_DIR := build/sanitize
SANITIZER_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
	-fno-sanitize-recover=all
else
BUILD_DIR := build/debug
endif

COMMON_SOURCES := $(wildcard src/common/*.c)
COMMON_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMMON_SOURCES))
MAIN_OBJECTS := $(BUILD_DIR)/coordinator/main.o \
	$(BUILD_DIR)/worker/main.o $(BUILD_DIR)/cli/main.o
TEST_NAMES := test_protocol test_messages test_net
TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .o,$(TEST_NAMES)))
OBJECTS := $(COMMON_OBJECTS) $(MAIN_OBJECTS) $(TEST_OBJECTS)
PROGRAMS := $(BUILD_DIR)/faultline-coordinator \
	$(BUILD_DIR)/faultline-worker $(BUILD_DIR)/faultline
TEST_PROGRAMS := $(addprefix $(BUILD_DIR)/tests/,$(TEST_NAMES))

.PHONY: all sanitize test test-unit test-integration test-sanitize clean

all: $(PROGRAMS)

sanitize:
	$(MAKE) SANITIZE=1 all

test: test-unit test-integration

test-unit: $(TEST_PROGRAMS)
	./$(BUILD_DIR)/tests/test_protocol
	./$(BUILD_DIR)/tests/test_messages
	./$(BUILD_DIR)/tests/test_net

test-integration: all
	$(PYTHON) tests/integration/test_ping.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-sanitize:
	$(MAKE) SANITIZE=1 test

$(BUILD_DIR)/faultline-coordinator: $(BUILD_DIR)/coordinator/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/faultline-worker: $(BUILD_DIR)/worker/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/faultline: $(BUILD_DIR)/cli/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_PROGRAMS): $(BUILD_DIR)/tests/%: $(BUILD_DIR)/tests/%.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
