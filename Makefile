# GNU Makefile for sBitx SDR Transceiver
# Based on the original build script

# Compiler and flags
CC = gcc
CFLAGS = -g -Wall -Wextra
LDFLAGS = 
LIBS = -lwiringPi -lasound -lm -lfftw3 -lfftw3f -pthread -lncurses -lsqlite3

# GTK3 configuration
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

# FT8 library
FT8_LIB = ft8_lib/libft8.a

# Version extraction
VERSION = $(shell grep VER sdr_ui.h | awk 'FNR==1{print $$4}' | sed -e 's/"//g')

# Main executable
TARGET = sbitx

# Core source files (from build script, excluding target.c)
CORE_SRCS = vfo.c si570.c sbitx_sound.c fft_filter.c sbitx_gtk.c sbitx_utils.c \
            i2cbb.c si5351v2.c ini.c hamlib.c queue.c modems.c logbook.c \
            modem_cw.c settings_ui.c oled.c telnet.c macros.c modem_ft8.c \
            remote.c mongoose.c webserver.c

# Additional source files found but not in build script
EXTRA_SRCS = resampler.c store.c wsjtx.c ubitx.c

# All source files
ALL_SRCS = $(CORE_SRCS) $(TARGET).c

# Object files
OBJS = $(ALL_SRCS:.c=.o)

# Header dependencies
HEADERS = $(wildcard *.h)

# Directories to create
DIRS = audio data web

# Default target
.PHONY: all
all: setup $(TARGET)

# Setup directories and database
.PHONY: setup
setup: $(DIRS) data/sbitx.db

$(DIRS):
	@mkdir -p $@

data/sbitx.db: data/create_db.sql | data
	@if [ ! -f data/sbitx.db ]; then \
		echo "Creating database..."; \
		cd data && sqlite3 sbitx.db < create_db.sql; \
	else \
		echo "Database exists"; \
	fi

# Build FT8 library
$(FT8_LIB):
	@echo "Building FT8 library..."
	$(MAKE) -C ft8_lib

# Main executable
$(TARGET): $(OBJS) $(FT8_LIB)
	@echo "Linking $(TARGET) version $(VERSION)..."
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(FT8_LIB) $(LIBS) $(GTK_LIBS)
	@echo "Build complete: $(TARGET)"

# Compile C source files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

# Clean targets
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned object files and executable"

.PHONY: distclean
distclean: clean
	$(MAKE) -C ft8_lib clean
	rm -rf audio data web
	@echo "Full clean complete"

# Install target (basic)
.PHONY: install
install: $(TARGET)
	@echo "Installing $(TARGET)..."
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod +x /usr/local/bin/$(TARGET)

# Test build
.PHONY: test
test: $(TARGET)
	@echo "Testing $(TARGET) executable..."
	file $(TARGET)
	ldd $(TARGET)

# Show version
.PHONY: version
version:
	@echo "sBitx version: $(VERSION)"

# Development helpers
.PHONY: rebuild
rebuild: clean all

.PHONY: ft8-clean
ft8-clean:
	$(MAKE) -C ft8_lib clean

.PHONY: ft8-rebuild
ft8-rebuild: ft8-clean $(FT8_LIB)

# Help target
.PHONY: help
help:
	@echo "sBitx Makefile targets:"
	@echo "  all       - Build everything (default)"
	@echo "  clean     - Remove object files and executable"
	@echo "  distclean - Full clean including FT8 lib and directories"
	@echo "  install   - Install to /usr/local/bin (requires sudo)"
	@echo "  test      - Test the built executable"
	@echo "  rebuild   - Clean and build"
	@echo "  version   - Show version info"
	@echo "  help      - Show this help"
	@echo ""
	@echo "FT8 library targets:"
	@echo "  ft8-clean   - Clean FT8 library"
	@echo "  ft8-rebuild - Rebuild FT8 library"

# Debug target
.PHONY: debug
debug:
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "GTK_CFLAGS: $(GTK_CFLAGS)"
	@echo "LIBS: $(LIBS)"
	@echo "GTK_LIBS: $(GTK_LIBS)"
	@echo "VERSION: $(VERSION)"
	@echo "CORE_SRCS: $(CORE_SRCS)"
	@echo "OBJS: $(OBJS)"