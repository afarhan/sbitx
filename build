#!/bin/sh
F=$@
WORKING_DIRECTORY=`pwd`
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
date
mkdir -p "/home/pi/sbitx/audio"
mkdir -p "/home/pi/sbitx/data"
#cd /home/pi/sbitx
mkdir -p "/home/pi/sbitx/web"

if test -f "data/sbitx.db"; then
	echo "database is intact"
else
	echo "database doesn't exist, it will be created"
	cd data
	sqlite3 sbitx.db < create_db.sql
	cd ..
fi

if [ "$F" != "sbitx" ]; then
  echo "compiling $F in $WORKING_DIRECTORY"
else
  VERSION=`grep VER sdr_ui.h|awk 'FNR==1{print $4}'|sed -e 's/"//g'`
  echo "compiling $F version $VERSION in $WORKING_DIRECTORY"
fi
gcc -g -o $F \
	 vfo.c si570.c sbitx_sound.c fft_filter.c  sbitx_gtk.c sbitx_utils.c \
    i2cbb.c si5351v2.c ini.c hamlib.c queue.c modems.c logbook.c \
		modem_cw.c \
		telnet.c macros.c modem_ft8.c remote.c mongoose.c webserver.c $F.c  \
		ft8_lib/libft8.a  \
	-lwiringPi -lasound -lm -lfftw3 -lfftw3f -pthread -lncurses -lsqlite3\
	`pkg-config --cflags gtk+-3.0` `pkg-config --libs gtk+-3.0`
echo "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
