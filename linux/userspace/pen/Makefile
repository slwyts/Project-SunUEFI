# SPDX-License-Identifier: Apache-2.0
# Offline diagnostic and C ABI library; no device service is installed.
OUT ?= ../../../build/pen-core
CC ?= cc
CXX ?= c++
AR ?= ar
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?=

OBJECTS = $(OUT)/piano-pen-frame.o $(OUT)/piano-pen-core.o $(OUT)/piano-pen-decoder.o $(OUT)/piano-pen-owner.o
DEPENDENCIES = $(OBJECTS:.o=.d) $(OUT)/piano-pen-offline.d

.PHONY: all clean
all: $(OUT)/libpiano-pen-core.a $(OUT)/piano-pen-offline

$(OUT):
	mkdir -p "$@"

$(OUT)/piano-pen-frame.o: piano-pen-frame.c piano-pen-frame.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/piano-pen-owner.o: piano-pen-owner.c piano-pen-owner.h piano-pen-core.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/%.o: %.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@

$(OUT)/libpiano-pen-core.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUT)/piano-pen-offline: $(OUT)/piano-pen-offline.o $(OUT)/libpiano-pen-core.a
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(OBJECTS) $(DEPENDENCIES) $(OUT)/piano-pen-offline.o \
	  $(OUT)/libpiano-pen-core.a $(OUT)/piano-pen-offline

-include $(DEPENDENCIES)
