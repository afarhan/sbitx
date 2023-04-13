prefix=/usr
CC=gcc

CFLAGS=`pkg-config --cflags gtk+-3.0` -std=gnu11 -fstack-protector
LDFLAGS=`pkg-config --libs gtk+-3.0` -lwiringPi -lasound -lm -lfftw3 -lfftw3f -pthread -lncurses -lsqlite3

all: sbitx

SOURCES=vfo.c si570.c sbitx_sound.c fft_filter.c sbitx_gtk.c sbitx_utils.c i2cbb.c si5351v2.c ini.c hamlib.c queue.c modems.c logbook.c telnet.c macros.c modem_ft8.c remote.c mongoose.c webserver.c sbitx.c

OBJ=$(SOURCES:.c=.o)

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

ft8_lib/libft8.a:
	$(MAKE) -C ft8_lib
	$(AR) rc ft8_lib/libft8.a ft8_lib/ft8/constants.o ft8_lib/ft8/encode.o ft8_lib/ft8/pack.o ft8_lib/ft8/text.o ft8_lib/common/wave.o ft8_lib/ft8/crc.o ft8_lib/fft/kiss_fftr.o ft8_lib/fft/kiss_fft.o ft8_lib/ft8/decode.o ft8_lib/ft8/ldpc.o ft8_lib/ft8/unpack.o

sbitx: $(OBJ) ft8_lib/libft8.a
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: clean install

install: sbitx
	install -D sbitx $(DESTDIR)$(prefix)/bin/sbitx

clean:
	rm -rf sbitx *.o ft8_lib/*.o ft8_lib/ft8/*.o ft8_lib/libft8.a
