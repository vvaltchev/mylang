#
# Trivial Makefile for a simple educational project
#

BASE_CFLAGS ?= -std=c++17 -ggdb -Wall -Wextra -Wno-unused-parameter
TARGET = mylang

all:
	$(CXX) $(BASE_CFLAGS) $(CFLAGS) -o $(TARGET) *.cpp

clean:
	rm $(TARGET)
