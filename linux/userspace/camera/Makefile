# SPDX-License-Identifier: MIT
# C sources are shared by the release runtime builder and the Nix package.
OUT ?= ../../../build/camera
CC ?= cc
CXX ?= c++
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
YUV_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libyuv)
YUV_LIBS ?= $(shell $(PKG_CONFIG) --libs libyuv)
JPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libjpeg)
JPEG_LIBS ?= $(shell $(PKG_CONFIG) --libs libjpeg)
QUAD_INCLUDE ?= quad/include

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
LICENSEDIR ?= $(PREFIX)/share/licenses/piano-camera

# Dependencies are prepared by the caller, not downloaded by make. Nix supplies
# its Rust package's static library. The release builder prepares the same
# locked crate in its build directory before invoking this local Cargo rule.
ifndef QUAD_LIB
CARGO ?= cargo
CARGO_TARGET ?=
QUAD_TARGET_DIR := $(abspath $(OUT)/quad)
QUAD_LIB := $(QUAD_TARGET_DIR)/$(if $(CARGO_TARGET),$(CARGO_TARGET)/)release/libpiano_quad.a

$(QUAD_LIB): quad/Cargo.toml quad/Cargo.lock quad/src/lib.rs
	$(CARGO) build --manifest-path quad/Cargo.toml --target-dir "$(QUAD_TARGET_DIR)" \
	  --release --lib --locked --offline $(if $(CARGO_TARGET),--target $(CARGO_TARGET))
endif

DAEMON_SOURCES = piano-camerad.c quad-worker.c loopback-output.c \
  photo/capture.c photo/protocol.c photo/timing.c
CLI_SOURCES = piano-camera-ctl.c photo/protocol.c photo/jpeg.c
DAEMON_OBJECTS = $(addprefix $(OUT)/,$(DAEMON_SOURCES:.c=.o))
CLI_OBJECTS = $(addprefix $(OUT)/,$(CLI_SOURCES:.c=.o))
OBJECTS = $(sort $(DAEMON_OBJECTS) $(CLI_OBJECTS))
TEST_NAMES = piano-camerad quad-worker loopback-output photo-capture \
  photo-timing photo-owner piano-camera-ctl
TEST_OBJECTS = $(addprefix $(OUT)/tests/,$(addsuffix Test.o,$(TEST_NAMES)))
TEST_PROGRAMS = $(addprefix $(OUT)/tests/,$(TEST_NAMES))
CAMERA_HELPERS = $(filter-out $(OUT)/piano-camerad.o,$(DAEMON_OBJECTS))

.PHONY: all check install clean
.DEFAULT_GOAL := all
all: $(OUT)/piano-camerad $(OUT)/piano-camera-ctl

$(OUT)/%.o: %.c
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -pthread -I"$(QUAD_INCLUDE)" \
	  $(YUV_CFLAGS) $(JPEG_CFLAGS) -MMD -MP -c "$<" -o "$@"

# libyuv is C++; use the selected target C++ driver for its static dependencies.
$(OUT)/piano-camerad: $(DAEMON_OBJECTS) $(QUAD_LIB)
	$(CXX) $(LDFLAGS) $(DAEMON_OBJECTS) $(QUAD_LIB) \
	  $(YUV_LIBS) -pthread -ldl -lm -o "$@"

$(OUT)/piano-camera-ctl: $(CLI_OBJECTS)
	$(CC) $(LDFLAGS) $(CLI_OBJECTS) $(JPEG_LIBS) -o "$@"

$(OUT)/tests/piano-camerad: $(OUT)/tests/piano-cameradTest.o $(CAMERA_HELPERS) $(QUAD_LIB)
	$(CXX) $(LDFLAGS) $^ $(YUV_LIBS) -pthread -ldl -lm \
	  -Wl,--wrap=ioctl,--wrap=loopback_output_publish,--wrap=quad_worker_start -o "$@"

$(OUT)/tests/quad-worker: $(OUT)/tests/quad-workerTest.o $(OUT)/quad-worker.o $(QUAD_LIB)
	$(CXX) $(LDFLAGS) $^ -pthread -ldl -lm \
	  -Wl,--wrap=piano_quad_render,--wrap=pthread_join -o "$@"

$(OUT)/tests/loopback-output: $(OUT)/tests/loopback-outputTest.o $(OUT)/loopback-output.o
	$(CC) $(LDFLAGS) $^ -Wl,--wrap=ioctl,--wrap=mmap,--wrap=munmap -o "$@"

$(OUT)/tests/photo-capture: $(OUT)/tests/photo-captureTest.o $(OUT)/photo/capture.o \
  $(OUT)/photo/protocol.o $(OUT)/photo/jpeg.o
	$(CC) $(LDFLAGS) $^ $(JPEG_LIBS) -o "$@"

$(OUT)/tests/photo-timing: $(OUT)/tests/photo-timingTest.o $(OUT)/photo/timing.o
	$(CC) $(LDFLAGS) $^ -o "$@"

$(OUT)/tests/photo-owner: $(OUT)/tests/photo-ownerTest.o $(CAMERA_HELPERS) $(QUAD_LIB)
	$(CXX) $(LDFLAGS) $^ $(YUV_LIBS) -pthread -ldl -lm \
	  -Wl,--wrap=open,--wrap=ioctl -o "$@"

$(OUT)/tests/piano-camera-ctl: $(OUT)/tests/piano-camera-ctlTest.o \
  $(OUT)/photo/protocol.o $(OUT)/photo/jpeg.o
	$(CC) $(LDFLAGS) $^ $(JPEG_LIBS) -pthread \
	  -Wl,--wrap=socket,--wrap=connect,--wrap=send -o "$@"

check: all $(TEST_PROGRAMS)
	"$(OUT)/piano-camerad" --help
	"$(OUT)/piano-camera-ctl" --help
	@set -e; for program in $(TEST_PROGRAMS); do "$$program"; done

install: all
	install -Dm755 "$(OUT)/piano-camerad" "$(DESTDIR)$(LIBEXECDIR)/piano-camerad"
	install -Dm755 "$(OUT)/piano-camera-ctl" "$(DESTDIR)$(BINDIR)/piano-camera-ctl"
	install -Dm644 LICENSE-MIT "$(DESTDIR)$(LICENSEDIR)/LICENSE-MIT"

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TEST_OBJECTS) $(TEST_OBJECTS:.o=.d) \
	  $(TEST_PROGRAMS) $(OUT)/piano-camerad $(OUT)/piano-camera-ctl

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
