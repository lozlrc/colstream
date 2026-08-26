# colstream build. Library is std + POSIX only; zlib for the DEFLATE codec.
# Reference liblz4 is linked into the cross-verification test only, when found.

CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Iinclude
BUILD    := build

LIB_SRCS := src/table.cpp src/pack.cpp src/lz4.cpp src/deflate.cpp \
            src/publisher.cpp src/subscriber.cpp
LIB_OBJS := $(LIB_SRCS:src/%.cpp=$(BUILD)/%.o)
HDRS     := $(wildcard include/colstream/*.hpp)

LZ4_HDR  := /opt/homebrew/include/lz4.h
HAVE_LZ4 := $(wildcard $(LZ4_HDR))

TARGETS := $(BUILD)/test_main $(BUILD)/bench_main
ifneq ($(HAVE_LZ4),)
TARGETS += $(BUILD)/test_lz4_cross
endif

all: $(TARGETS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_main: tests/test_main.cpp $(LIB_OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) tests/test_main.cpp $(LIB_OBJS) -o $@ -lz

$(BUILD)/bench_main: bench/bench_main.cpp $(LIB_OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) bench/bench_main.cpp $(LIB_OBJS) -o $@ -lz

$(BUILD)/test_lz4_cross: tests/test_lz4_cross.cpp $(LIB_OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) -I/opt/homebrew/include tests/test_lz4_cross.cpp $(LIB_OBJS) \
	  -o $@ -L/opt/homebrew/lib -llz4 -lz

test: all
	$(BUILD)/test_main
ifneq ($(HAVE_LZ4),)
	$(BUILD)/test_lz4_cross
else
	@echo "SKIP: liblz4 not found at $(LZ4_HDR), cross-verification skipped"
endif

bench: $(BUILD)/bench_main
	$(BUILD)/bench_main | tee bench/results.txt

clean:
	rm -rf $(BUILD)

.PHONY: all test bench clean
