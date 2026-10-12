SHELL := /bin/bash
.DEFAULT_GOAL := release

APP_NAME ?= myapp
BUILD ?= release
BUILD_BASE_DIR ?= build
CC ?= cc
PYTHON ?= python3
GCOVR ?= gcovr
# Onyx needs only ASan by default; opt into UBSan where its runtime is installed.
SANITIZERS ?= address
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?=
LDLIBS ?=

APP_SRCS := $(sort $(shell find src -name '*.c'))
CORE_SRCS := $(filter-out src/main.c,$(APP_SRCS))
TEST_SRCS := $(filter-out tests/adapter-test.c,$(sort $(shell find tests -name '*.c')))
ADAPTER_TEST_SRCS := $(CORE_SRCS) tests/adapter-test.c tests/harness/unity.c

ifeq ($(BUILD),release)
  BUILD_DIR := $(BUILD_BASE_DIR)/release
  MODE_FLAGS := -O2 -DNDEBUG -fstack-protector-strong
  TARGET := $(BUILD_DIR)/$(APP_NAME)
  SRCS := $(APP_SRCS)
else ifeq ($(BUILD),debug)
  BUILD_DIR := $(BUILD_BASE_DIR)/debug
  MODE_FLAGS := -g3 -O0 -fno-omit-frame-pointer -fsanitize=$(SANITIZERS) -fno-pie
  MODE_LDFLAGS := -fsanitize=$(SANITIZERS) -no-pie
  TARGET := $(BUILD_DIR)/$(APP_NAME)_d
  SRCS := $(APP_SRCS)
else ifeq ($(BUILD),test)
  BUILD_DIR := $(BUILD_BASE_DIR)/tests
  MODE_FLAGS := -g -O0 --coverage
  MODE_LDFLAGS := --coverage
  TARGET := $(BUILD_DIR)/$(APP_NAME)_t
  SRCS := $(CORE_SRCS) $(TEST_SRCS)
else ifeq ($(BUILD),coverage)
  BUILD_DIR := $(BUILD_BASE_DIR)/coverage
  MODE_FLAGS := -g -O0 --coverage
  MODE_LDFLAGS := --coverage
  TARGET := $(BUILD_DIR)/$(APP_NAME)_coverage
  SRCS := $(APP_SRCS)
else ifeq ($(BUILD),adapter-test)
  BUILD_DIR := $(BUILD_BASE_DIR)/adapter-tests
  MODE_FLAGS := -g -O0 --coverage
  MODE_LDFLAGS := --coverage
  TARGET := $(BUILD_DIR)/$(APP_NAME)_adapter_t
  SRCS := $(ADAPTER_TEST_SRCS)
else ifeq ($(BUILD),debug-adapter-test)
  BUILD_DIR := $(BUILD_BASE_DIR)/debug-adapter-tests
  MODE_FLAGS := -g3 -O0 -fno-omit-frame-pointer -fsanitize=$(SANITIZERS) -fno-pie
  MODE_LDFLAGS := -fsanitize=$(SANITIZERS) -no-pie
  TARGET := $(BUILD_DIR)/$(APP_NAME)_adapter_td
  SRCS := $(ADAPTER_TEST_SRCS)
else ifeq ($(BUILD),debug-test)
  BUILD_DIR := $(BUILD_BASE_DIR)/debug-test
  MODE_FLAGS := -g3 -O0 -fno-omit-frame-pointer -fsanitize=$(SANITIZERS) -fno-pie
  MODE_LDFLAGS := -fsanitize=$(SANITIZERS) -no-pie
  TARGET := $(BUILD_DIR)/$(APP_NAME)_td
  SRCS := $(CORE_SRCS) $(TEST_SRCS)
else
  $(error Invalid BUILD=$(BUILD); choose release, debug, test, debug-test, coverage, adapter-test, or debug-adapter-test)
endif

OBJS := $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) $(MODE_LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MODE_FLAGS) -MMD -MP -c $< -o $@

.PHONY: all release debug test debug-test coverage adapter-test debug-adapter-test check report report-txt leak leak-test integration benchmark clean help print _build
_build: $(TARGET)
release:
	$(MAKE) BUILD=release _build
debug:
	$(MAKE) BUILD=debug _build
test:
	$(MAKE) BUILD=test _build
debug-test:
	$(MAKE) BUILD=debug-test _build
coverage:
	$(MAKE) BUILD=coverage _build
adapter-test:
	$(MAKE) BUILD=adapter-test _build
debug-adapter-test:
	$(MAKE) BUILD=debug-adapter-test _build
all: release debug test debug-test coverage adapter-test debug-adapter-test

check: test adapter-test
	./$(BUILD_BASE_DIR)/tests/$(APP_NAME)_t
	./$(BUILD_BASE_DIR)/adapter-tests/$(APP_NAME)_adapter_t

# Measure every production C file. No source, line, or branch exclusions.
# Explicit build directories prevent stale or alternate-compiler counters from
# leaking into the report. Real transfers complement deterministic fault tests.
report: test adapter-test coverage
	find $(BUILD_BASE_DIR)/tests $(BUILD_BASE_DIR)/adapter-tests $(BUILD_BASE_DIR)/coverage -name '*.gcda' -delete
	./$(BUILD_BASE_DIR)/tests/$(APP_NAME)_t
	./$(BUILD_BASE_DIR)/adapter-tests/$(APP_NAME)_adapter_t
	$(PYTHON) scripts/integration-test.py --app ./$(BUILD_BASE_DIR)/coverage/$(APP_NAME)_coverage
	$(PYTHON) scripts/io-edge-tests.py --app ./$(BUILD_BASE_DIR)/coverage/$(APP_NAME)_coverage --slow
	@mkdir -p $(BUILD_BASE_DIR)/report/html $(BUILD_BASE_DIR)/report/txt
	$(GCOVR) --root . --filter 'src/.*\.c$$' --fail-under-line 100 --fail-under-branch 100 --print-summary --json $(BUILD_BASE_DIR)/report/coverage.json --txt $(BUILD_BASE_DIR)/report/txt/coverage_report.txt --html-details $(BUILD_BASE_DIR)/report/html/coverage_report.html $(BUILD_BASE_DIR)/tests $(BUILD_BASE_DIR)/adapter-tests $(BUILD_BASE_DIR)/coverage
	@cat $(BUILD_BASE_DIR)/report/txt/coverage_report.txt
report-txt: report

leak-test: debug-test debug-adapter-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD_BASE_DIR)/debug-test/$(APP_NAME)_td
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD_BASE_DIR)/debug-adapter-tests/$(APP_NAME)_adapter_td
leak: debug
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD_BASE_DIR)/debug/$(APP_NAME)_d
integration: release
	$(PYTHON) scripts/integration-test.py --app ./$(BUILD_BASE_DIR)/release/$(APP_NAME)
	$(PYTHON) scripts/io-edge-tests.py --app ./$(BUILD_BASE_DIR)/release/$(APP_NAME)
benchmark: release
	$(PYTHON) scripts/benchmark.py --app ./$(BUILD_BASE_DIR)/release/$(APP_NAME)

clean:
	rm -rf -- $(BUILD_BASE_DIR)

help:
	@echo 'make [all|release|debug|test|debug-test|coverage|adapter-test|debug-adapter-test|check|report|leak|leak-test|integration|benchmark|clean]'
	@echo 'Default: release. Release executable: build/release/myapp'
	@echo 'report requires gcovr and enforces 100% line and branch coverage for every src/*.c file.'
	@echo 'benchmark runs the required 1 MiB file experiment (12 transfers).'
	@echo 'Default sanitizer: address. Optional: SANITIZERS=address,undefined BUILD_BASE_DIR=build/ubsan'
	@echo 'Use a fresh build directory or make clean when changing SANITIZERS or CC.'
print:
	@echo 'BUILD=$(BUILD) CC=$(CC) TARGET=$(TARGET)'
	@echo 'CPPFLAGS=$(CPPFLAGS) CFLAGS=$(CFLAGS) MODE_FLAGS=$(MODE_FLAGS)'
	@echo 'SRCS=$(SRCS)'

-include $(DEPS)
