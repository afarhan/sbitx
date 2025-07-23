# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

sBitx is a Software Defined Radio (SDR) transceiver project designed primarily for ham radio operation. It's built to run on Raspberry Pi with custom hardware and supports various amateur radio modes including CW, SSB, FT8, and digital modes.

## Build Commands

The project uses a simple compilation approach without a main Makefile:

```bash
# To build the main sbitx executable (inferred from existing binary):
gcc -o sbitx *.c -lwiringPi -lfftw3 -lm -lpthread -lasound -lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 -lcairo-glib -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0

# To build FT8 library components:
cd ft8_lib && make

# To run FT8 tests:
cd ft8_lib && make run_tests

# To clean FT8 build artifacts:
cd ft8_lib && make clean

# To reset database and settings:
./cleanup.sh
```

## Architecture

### Core Components

- **sbitx.c**: Main SDR engine and signal processing core
- **sbitx_gtk.c**: GTK-based GUI implementation 
- **sbitx_sound.c**: ALSA audio interface and sound card management
- **sbitx_utils.c**: Utility functions and helpers
- **webserver.c**: Built-in web interface using Mongoose library

### Digital Signal Processing

- Uses **FFTW3** library for FFT operations
- **fft_filter.c**: Digital filtering implementations
- Custom **resampler.c** for sample rate conversion
- **ft8_lib/**: Complete FT8/FT4 protocol implementation

### Hardware Interface

- **i2cbb.c**: I2C bit-banging for hardware control
- **si5351v2.c**: SI5351 frequency synthesizer control  
- **si570.c**: SI570 oscillator control
- **vfo.c**: VFO (Variable Frequency Oscillator) management
- Uses **wiringPi** library for GPIO control on Raspberry Pi

### User Interface

- Custom lightweight GUI framework (not using standard toolkit widgets)
- Single field-based control system defined in how_gui_is_organized.txt
- Touch/mouse interface optimized for 800x480 displays
- Web interface accessible on port 8080 (redirected to port 80)

### Communication Protocols

- **hamlib.c**: CAT control interface
- **logbook.c**: QSO logging and database management
- **wsjtx.c**: WSJT-X integration for digital modes
- **remote.c**: Remote control capabilities
- **telnet.c**: Telnet cluster interface

## Configuration

- **Settings**: Stored in INI format files in data/ directory
- **Database**: SQLite database for logbook (data/create_db.sql)
- **Audio Setup**: Requires ALSA loopback devices for external program integration
- **Hardware Config**: GPIO and hardware settings in /boot/config.txt

## Development Setup Requirements

Based on install.txt, the following dependencies are required:
- wiringPi library for GPIO control
- FFTW3 (both double and single precision)
- ALSA development libraries (libasound2-dev)
- GTK3 development libraries
- ncurses development libraries
- SQLite3 libraries

## Code Style

From coding_convention.txt:
- Use snake_case for functions and variables
- Keep lines within 800x480 display width
- Use static variables to limit scope instead of globals
- Opening braces on same line
- Prefer simple, readable code over complex optimizations
- Each module should have its own main() for testing (commented out in production)

## Command Interface

The system supports text commands (from commands.txt) including:
- Frequency control: `\freq` or `\f`
- Mode changes: `\mode` or `\m` 
- CW settings: `\cwdelay`, `\cwinput`, `\txpitch`
- Transmit/receive: `\t`, `\r`
- Telnet operations: `\topen`, `\tclose`, `\w`

## Testing

- FT8 library has comprehensive test suite: `cd ft8_lib && make run_tests`
- Each module designed with independent main() function for unit testing
- Use `./cleanup.sh` to reset to clean state for testing