PROJECT := kilix-pdf
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
INSTALL ?= install
PKG_CONFIG ?= pkg-config

SESSION_DIR ?= third_party/kitty-terminal-session
SESSION_BUILD_DIR ?= $(abspath $(BUILD_DIR)/terminal-session)
SESSION_LIB := $(SESSION_BUILD_DIR)/libkitty-terminal-session.a
FRAMEBUFFER_DIR := $(SESSION_DIR)/third_party/kitty-framebuffer
INPUT_DIR := $(SESSION_DIR)/third_party/kitty-input
KEYBOARD_DIR := $(INPUT_DIR)/third_party/kitty_keyboard

PDF_PACKAGES := poppler-glib cairo glib-2.0 gobject-2.0
PDF_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags $(PDF_PACKAGES) 2>/dev/null)
PDF_LIBS ?= $(shell $(PKG_CONFIG) --libs $(PDF_PACKAGES) 2>/dev/null)

CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Iinclude \
	-I$(SESSION_DIR)/include -I$(FRAMEBUFFER_DIR)/include \
	-I$(INPUT_DIR)/include -I$(KEYBOARD_DIR)/include $(PDF_CFLAGS)
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -pthread $(WARNINGS)
LDLIBS += $(PDF_LIBS) -lz -pthread -lm

CORE := $(BUILD_DIR)/kilix-pdf-core
OBJECTS := $(BUILD_DIR)/main.o $(BUILD_DIR)/kilix_pdf_layout.o
TEST_LAYOUT := $(BUILD_DIR)/test-layout
SANITIZE_TEST := $(BUILD_DIR)/test-layout-sanitize

.DEFAULT_GOAL := all

all:
	@if $(PKG_CONFIG) --exists $(PDF_PACKAGES); then \
		$(MAKE) --no-print-directory native; \
	else \
		printf '%s\n' \
		  'native PDF development files not found; Evince fallback remains available'; \
	fi

native: check-deps $(CORE)

check-deps:
	@$(PKG_CONFIG) --exists $(PDF_PACKAGES) || { \
		printf '%s\n' \
		  'kilix-pdf: install the Poppler GLib, Cairo, and GLib development files' >&2; \
		exit 1; \
	}

$(BUILD_DIR):
	mkdir -p $@

$(SESSION_LIB):
	$(MAKE) -C $(SESSION_DIR) BUILD_DIR=$(SESSION_BUILD_DIR) all

$(BUILD_DIR)/main.o: src/main.c include/kilix_pdf_layout.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kilix_pdf_layout.o: src/kilix_pdf_layout.c \
	include/kilix_pdf_layout.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(CORE): $(OBJECTS) $(SESSION_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(SESSION_LIB) $(LDLIBS)

$(TEST_LAYOUT): tests/test_layout.c src/kilix_pdf_layout.c \
	include/kilix_pdf_layout.h | $(BUILD_DIR)
	$(CC) -D_POSIX_C_SOURCE=200809L -Iinclude $(CFLAGS) \
		-o $@ tests/test_layout.c src/kilix_pdf_layout.c -lm

test: $(TEST_LAYOUT)
	$(TEST_LAYOUT)
	tests/test_wrapper.sh

test-native: native
	$(CORE) --probe
	$(CORE) --render-page tests/minimal.pdf $(BUILD_DIR)/smoke.png
	test -s $(BUILD_DIR)/smoke.png
	$(CORE) --benchmark tests/minimal.pdf
	KILIX_PDF_CORE=$(CORE) tests/test_terminal_protocol.py

$(SANITIZE_TEST): tests/test_layout.c src/kilix_pdf_layout.c \
	include/kilix_pdf_layout.h | $(BUILD_DIR)
	$(CC) -D_POSIX_C_SOURCE=200809L -Iinclude -std=c11 -O1 -g3 \
		$(WARNINGS) -fno-omit-frame-pointer -fsanitize=address,undefined \
		-o $@ tests/test_layout.c src/kilix_pdf_layout.c \
		-lm -fsanitize=address,undefined

sanitize: $(SANITIZE_TEST)
	ASAN_OPTIONS=detect_leaks=1 $(SANITIZE_TEST)
	tests/test_wrapper.sh

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 0755 kilix-pdf-viewer $(DESTDIR)$(PREFIX)/bin/
	@if test -x $(CORE); then \
		$(INSTALL) -m 0755 $(CORE) $(DESTDIR)$(PREFIX)/bin/; \
	fi

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all check-deps clean install native sanitize test test-native
