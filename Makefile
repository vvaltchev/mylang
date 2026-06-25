# SPDX-License-Identifier: BSD-2-Clause

BUILD_DIR ?= build
PROJ_ROOT:=$(shell pwd)

OPT ?= 1
LTO ?= $(OPT)
FL_LANG ?= -std=c++17
FL_DBG ?= -ggdb
FL_WARN ?= -Wall -Wextra -Wno-unused-parameter

# The codebase intentionally calls `move`/`forward` unqualified (see
# `using std::move;` in defs.h), which recent clang reports under
# -Wunqualified-std-cast-call. Silence it for clang only: GCC has no such
# option and would note an unrecognized flag.
ifneq (,$(findstring clang,$(shell $(CXX) --version 2>/dev/null)))
	FL_WARN += -Wno-unqualified-std-cast-call
endif

FL_OTHER ?= -fwrapv
FL_INC = -I$(PROJ_ROOT)/src
BASE_FLAGS ?= $(FL_INC) $(FL_LANG) $(FL_DBG) $(FL_WARN) $(FL_OTHER)

ifdef OPT
	ifeq ($(OPT),1)
		BASE_FLAGS += -O3
	endif
endif

# Whole-program (link-time) optimization. On by default for optimized builds
# (LTO defaults to OPT): ~7% smaller binary and ~8-9% faster on the bench suite.
# Build with LTO=0 to disable. -flto=auto is accepted by both GCC and clang, and
# must be present at both compile and link (the link line passes BASE_FLAGS).
ifeq ($(LTO),1)
	BASE_FLAGS += -flto=auto
endif

# AddressSanitizer / UndefinedBehaviorSanitizer. Both default ON for a debug
# build (OPT=0) and OFF for an optimized build (OPT=1); override either
# explicitly, e.g. `make ASAN=0` (debug without ASan) or `make OPT=1 UBSAN=1`.
# The flags live in BASE_FLAGS so they reach both the compile and link lines.
# UBSan excludes signed-integer-overflow: the project builds with -fwrapv and
# *relies* on signed wraparound (see defs.h / the README), so that overflow is
# defined behavior here, not a bug to flag.
ifeq ($(OPT),0)
	ASAN ?= 1
	UBSAN ?= 1
else
	ASAN ?= 0
	UBSAN ?= 0
endif

ifeq ($(ASAN),1)
	BASE_FLAGS += -fsanitize=address
endif

ifeq ($(UBSAN),1)
	BASE_FLAGS += -fsanitize=undefined -fno-sanitize=signed-integer-overflow
endif

ifneq (,$(filter 1,$(ASAN) $(UBSAN)))
	BASE_FLAGS += -fno-omit-frame-pointer
endif

# ASSERTS: the defense-in-depth assertion net - both the C `assert()` and the
# project's ML_CHECK() invariant checks (defs.h). Default ON for EVERY build
# type (debug AND release), so every build exercises the full net. Set
# ASSERTS=0 to compile them all away (defines NDEBUG); use that on an optimized
# build to measure the assert overhead, e.g. `make OPT=1 ASSERTS=0` vs the
# default `make OPT=1`. (NDEBUG was never defined here before, so the existing
# asserts already ran in release; this just makes that a switch.)
ASSERTS ?= 1
ifeq ($(ASSERTS),0)
	BASE_FLAGS += -DNDEBUG
endif

# Adversarial Construct allocator (a LIFO free-list that hands a just-freed
# node's address straight back), so any "pointer used as a stable identity"
# bug manifests DETERMINISTICALLY under -rt instead of only on a hostile
# allocator like MSVC's. Off by default; `make RECYCLE=1 TESTS=1`. See
# syntax.cpp / the "Invariants & hazards" section in CLAUDE.md.
ifeq ($(RECYCLE),1)
	BASE_FLAGS += -DRECYCLE_ALLOC
endif

ifdef TESTS
	ifeq ($(TESTS),1)
		BASE_FLAGS += -DTESTS
	endif
endif

DEPDIR := .d
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d
SOURCES:=$(wildcard src/*.cpp)
OBJECTS=$(SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)

TARGET = mylang

$(shell mkdir -p $(DEPDIR) > /dev/null)
$(shell mkdir -p $(BUILD_DIR) > /dev/null)

# Default target
all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/%.o : src/%.cpp
$(BUILD_DIR)/%.o : src/%.cpp $(DEPDIR)/%.d
	@echo Compiling $(<:src/%.cpp=%.cpp)...
	@$(COMPILE) $(CXX) -o $@ $(DEPFLAGS) $(BASE_FLAGS) $(CFLAGS) -c $<
	@$(POSTCOMPILE)

$(BUILD_DIR)/$(TARGET): $(OBJECTS)
	@echo Linking $(TARGET)...
	@$(CXX) $(BASE_FLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/$(TARGET)
	rm -rf $(DEPDIR)

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d
.PHONY: all clean

-include $(patsubst src/%,$(DEPDIR)/%.d,$(basename $(SOURCES)))
