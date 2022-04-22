#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include "sdr.h"
#include "sdr_ui.h"

#define FT8_MAX_BUFF (12000 * 18) 
unsigned int wallclock = 0;
int32_t ft8_rx_buff[FT8_MAX_BUFF];
int ft8_rx_buff_index = 0;
int ft8_do_decode = 0;
pthread_t ft8_thread;

void *ft8_thread_function(void *ptr){
	char buff[200];

	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);
		if (!ft8_do_decode)
			continue;

		//create a temporary file of the ft8 samples
		FILE *pf = fopen("/tmp/ftrx.raw", "w");
		fwrite(ft8_rx_buff, sizeof(ft8_rx_buff), 1, pf);
		fclose(pf);

		//let the next batch begin
		ft8_do_decode = 0;
		ft8_rx_buff_index = 0;

		
		//now launch the decoder
		pf = popen("/home/pi/ft8_lib/decode_ft8 /tmp/ftrx.raw", "r");
		while(fgets(buff, sizeof(buff), pf)) 
			append_log(buff);
		
	}
}

// the ft8 sampling is at 12000, the incoming samples are at
// 96000 samples/sec
void ft8_rx(int32_t *samples, int count){

	int decimation_ratio = 96000/12000;

	//if there is an overflow, then reset to the begining
	if (ft8_rx_buff_index + (count/decimation_ratio) >= FT8_MAX_BUFF){
		ft8_rx_buff_index = 0;		
	}

	//down convert to 12000 Hz sampling rate
	for (int i = 0; i < count; i += decimation_ratio)
		ft8_rx_buff[ft8_rx_buff_index++] = samples[i];

	int now = time(NULL);
	if (now != wallclock)	
		wallclock = now;
	else 
		return;

	// do nothing unless we are on a 15 second boundary
	if (wallclock % 15)
		return;

	//we should have atleast 12 seconds of samples to decode
	if (ft8_rx_buff_index >= 14 * 12000)
		ft8_do_decode = 1;

	ft8_rx_buff_index = 0;	
}

void modem_rx(int mode, int32_t *samples, int count){
	switch(mode){
	case MODE_FT8:
		ft8_rx(samples, count);
		break;
	}
}

void modem_init(){
	// init the ft8
	ft8_rx_buff_index = 0;
	pthread_create( &ft8_thread, NULL, ft8_thread_function, (void*)NULL);
}
