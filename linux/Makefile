CC       ?= gcc
PKG_CFG  ?= pkg-config
PREFIX   ?= /usr/local

BUILD_DIR := build
BIN       := $(BUILD_DIR)/dkchatebras
TEST_BIN  := $(BUILD_DIR)/test_markdown

SRCS := \
	src/main.c \
	src/str.c \
	src/json.c \
	src/markdown.c \
	src/store.c \
	src/secret.c \
	src/net.c \
	src/gfx.c \
	src/layout.c \
	src/ui.c

CORE_SRCS := \
	src/str.c \
	src/json.c \
	src/markdown.c \
	src/store.c

PKG_CFLAGS := $(shell $(PKG_CFG) --cflags x11 xext freetype2 fontconfig)
PKG_LIBS   := $(shell $(PKG_CFG) --libs x11 xext freetype2 fontconfig)

COMMON_CFLAGS   := -std=c17 -Wall -Wextra -Wshadow -Wformat=2 -Isrc
RELEASE_CFLAGS  := -O3 -flto=auto -ffunction-sections -fdata-sections -DNDEBUG
RELEASE_LDFLAGS := -flto=auto -Wl,--gc-sections -Wl,--as-needed

DEBUG_CFLAGS  := -O0 -g3
DEBUG_LDFLAGS :=
ifeq ($(SANITIZE),1)
DEBUG_CFLAGS  += -fsanitize=address,undefined
DEBUG_LDFLAGS += -fsanitize=address,undefined
endif

LIBS := $(PKG_LIBS) -lm -lpthread -ldl

.PHONY: all release debug test test-asan install clean

all: release

release: CFLAGS := $(COMMON_CFLAGS) $(RELEASE_CFLAGS) $(PKG_CFLAGS)
release: LDFLAGS := $(RELEASE_LDFLAGS)
release: $(BIN)
	strip --strip-all $(BIN)

debug: CFLAGS := $(COMMON_CFLAGS) $(DEBUG_CFLAGS) $(PKG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: $(BIN)

$(BIN): $(SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN) $(LDFLAGS) $(LIBS)

$(TEST_BIN): $(CORE_SRCS) tests/test_markdown.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -O2 $(CORE_SRCS) tests/test_markdown.c -o $(TEST_BIN) -lm -lpthread

test: $(TEST_BIN)
	./$(TEST_BIN)

test-asan: | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -O0 -g3 -fsanitize=address,undefined $(CORE_SRCS) tests/test_markdown.c -o $(BUILD_DIR)/test_markdown_asan -fsanitize=address,undefined -lm -lpthread
	./$(BUILD_DIR)/test_markdown_asan

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

install: release
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/dkchatebras"
	install -Dm644 data/io.github.dkchatebras.DKChatebras.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/io.github.dkchatebras.DKChatebras.desktop"
	install -Dm644 data/io.github.dkchatebras.DKChatebras.metainfo.xml \
		"$(DESTDIR)$(PREFIX)/share/metainfo/io.github.dkchatebras.DKChatebras.metainfo.xml"
	install -Dm644 data/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg"

clean:
	rm -rf $(BUILD_DIR)
