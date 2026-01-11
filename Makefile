# Project settings
TARGET := nirinotify
SOURCES := main.c cJSON.c
OBJECTS := $(SOURCES:.c=.o)

# Compiler and flags
CC := gcc
CFLAGS := $(shell pkg-config --cflags libsystemd)
LDFLAGS := $(shell pkg-config --libs libsystemd)

# Build type specific flags
DEBUG_FLAGS := -g -O0 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -DDEBUG
RELEASE_FLAGS := -O2 -DNDEBUG

# Installation paths
PREFIX := /usr/local
BINDIR := $(PREFIX)/bin

# Default target
.PHONY: all
all: debug

# Release build
.PHONY: release
release: CFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

# Debug build
.PHONY: debug
debug: CFLAGS += $(DEBUG_FLAGS)
debug: LDFLAGS += -fsanitize=address -fsanitize=undefined
debug: clean $(TARGET)

# Link the target
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Install target
.PHONY: install
install: release
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

# Uninstall target
.PHONY: uninstall
uninstall:
	rm -f $(BINDIR)/$(TARGET)

# Clean build artifacts
.PHONY: clean
clean:
	rm -f $(OBJECTS) $(TARGET)

# Help target
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all (default) - Build release version"
	@echo "  release       - Build optimized release version"
	@echo "  debug         - Build with debug symbols"
	@echo "  install       - Install binary to $(BINDIR)"
	@echo "  uninstall     - Remove installed binary"
	@echo "  clean         - Remove build artifacts"


