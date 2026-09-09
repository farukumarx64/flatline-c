CC ?= cc
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
TEST_OBJECTS := $(BUILD_DIR)/tests/test_protocol.o
OBJECTS := $(COMMON_OBJECTS) $(MAIN_OBJECTS) $(TEST_OBJECTS)
PROGRAMS := $(BUILD_DIR)/faultline-coordinator \
	$(BUILD_DIR)/faultline-worker $(BUILD_DIR)/faultline
TEST_PROGRAM := $(BUILD_DIR)/tests/test_protocol

.PHONY: all sanitize test test-sanitize clean

all: $(PROGRAMS)

sanitize:
	$(MAKE) SANITIZE=1 all

test: $(TEST_PROGRAM)
	./$(TEST_PROGRAM)

test-sanitize:
	$(MAKE) SANITIZE=1 test

$(BUILD_DIR)/faultline-coordinator: $(BUILD_DIR)/coordinator/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/faultline-worker: $(BUILD_DIR)/worker/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/faultline: $(BUILD_DIR)/cli/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_PROGRAM): $(TEST_OBJECTS) $(COMMON_OBJECTS)
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
