# SPDX-License-Identifier: BSD-2-Clause

BUILD_DIR ?= build
PROJ_ROOT:=$(shell pwd)

FL_LANG ?= -std=c++17
FL_DBG ?= -ggdb
FL_WARN ?= -Wall -Wextra -Wno-unused-parameter
FL_OTHER ?= -fwrapv
FL_INC = -I$(PROJ_ROOT)
BASE_FLAGS ?= $(FL_INC) $(FL_LANG) $(FL_DBG) $(FL_WARN) $(FL_OTHER)

ifdef OPT
	ifeq ($(OPT),1)
		BASE_FLAGS += -O3
	endif
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
	@$(CXX) $(OBJECTS) -o $@ $(LFLAGS)

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/$(TARGET)
	rm -rf $(DEPDIR)

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d
.PHONY: all clean

-include $(patsubst src/%,$(DEPDIR)/%.d,$(basename $(SOURCES)))
