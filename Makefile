#
# Trivial Makefile for a simple educational project
#

BASE_CFLAGS ?= -std=c++17 -ggdb -Wall -Wextra -Wno-unused-parameter
TARGET = mylang

.PHONY: all clean

# Default target
all: $(TARGET)

OBJECTS = $(patsubst %.cpp, %.o, $(wildcard *.cpp))
HEADERS = $(wildcard *.h)

%.o: %.cpp $(HEADERS)
	$(CXX) $(BASE_CFLAGS) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LFLAGS)

clean:
	-rm -f *.o
	-rm -f $(TARGET)
