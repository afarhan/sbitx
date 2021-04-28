#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"

char audio_card[32];

int tx_shift = 512;
#define TX_LINE 4
#define BAND_SELECT 5

float fft_bins[MAX_BINS]; // spectrum ampltiudes  
fftw_complex *fft_spectrum;
fftw_plan plan_spectrum;
float spectrum_window[MAX_BINS];

// spectrum display
fftw_complex *spectrum_fft;

fftw_complex *fft_out;		// holds the incoming samples in freq domain (for rx as well as tx)
fftw_complex *fft_in;			// holds the incoming samples in time domain (for rx as well as tx) 
fftw_complex *fft_m;			// holds previous samples for overlap and discard convolution 
fftw_plan plan_fwd, plan_tx;
int bfo_freq = 27025570;
int freq_hdr = 7050000;

static int tuning_method = TUNE_SI5351;

static double volume 	= 100000.0;
static double mic_gain = 200000000.0;
static int tx_power = 100;
static int rx_gain = 100;
static int rx_vol = 100;
static int tx_gain = 100;
static double spectrum_speed = 0.1;
static int in_tx = 0;
struct vfo tone_a, tone_b; //these are audio tone generators

struct rx *rx_list = NULL;
struct filter *tx_filter;	//convolution filter

/* radio control. We only need two functions to control the radio hardware:
- tune the radio
- turn the tx on/off
*/

#define CMD_FREQ (1)
#define CMD_TX (2)
#define CMD_RX (3)

#define MDS_LEVEL (-135)
int fserial = 0;

void radio_tune_to(u_int32_t f){
	u_int8_t cmd[5];

	f -= 600;
	cmd[0] = CMD_FREQ;
	memcpy(cmd+1, &f, 4);
	for (int i = 0; i < 5; i++)
		serialPutchar(fserial, cmd[i]);
}

void radio_tx(int turn_on){
	u_int8_t cmd[5];
	if (turn_on){
		cmd[0] = CMD_TX;
	}
	else{
		cmd[0] = CMD_RX;
	}
	
	for (int i = 0; i < 5; i++)
		serialPutchar(fserial, cmd[i]);
	printf("tx is %d\n", (int)cmd[0]);
}

/*
//ffts for transmit, we only transmit one channel at a time
fftw_plan tx_plan_rev;
fftw_complex *tx_fft_freq;
fftw_complex *tx_fft_time;
*/

void fft_init(){
	int mem_needed;

	//printf("initializing the fft\n");
	fflush(stdout);

	mem_needed = sizeof(fftw_complex) * MAX_BINS;
	printf("fft needs %d bytes\n", mem_needed);

	fft_m = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS/2);
	fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_spectrum = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);

	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS/2);

	plan_fwd = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
	plan_spectrum = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_spectrum, FFTW_FORWARD, FFTW_ESTIMATE);

	//zero up the previous 'M' bins
	for (int i= 0; i < MAX_BINS/2; i++){
		__real__ fft_m[i]  = 0.0;
		__imag__ fft_m[i]  = 0.0;
	}

	make_hann_window(spectrum_window, MAX_BINS);
}

int mag2db(double mag){
	int m = abs(mag) * 10000000;
	
	int c = 31;
	int p = 0x80000000;
	while(c > 0){
		if (p & m)
			break;
		c--;
		p = p >> 1;
	}
	return c;
}

void set_spectrum_speed(int speed){
	spectrum_speed = speed;
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_reset(){
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_update(){
	//we are only using the lower half of the bins, so this copies twice as many bins, 
	//it can be optimized. leaving it here just in case someone wants to try I Q channels 
	//in hardware
	for (int i = 0; i < MAX_BINS; i++){
		fft_bins[i] = ((1.0 - spectrum_speed) * fft_bins[i]) + 
			(spectrum_speed * cabs(fft_spectrum[i]));
	}

  redraw();
}

void set_lpf(int frequency){
	static int prev_lpf = -1;
	int lpf = 0;

	if (frequency < 2000000)
		lpf = 5;
	else if (frequency < 4000000)		
		lpf = 4;
	else if (frequency < 8000000)		
		lpf = 3;
	else if (frequency < 10500000)
		lpf = 3;
	else if (frequency < 15000000)
		lpf = 2;
	else if (frequency < 21500000)
		lpf = 1;

	if (lpf == prev_lpf){
		puts("LPF not changed");
		return;
	}

	printf("Setting LPF to %d\n", lpf);
	//reset the 4017
	digitalWrite(BAND_SELECT, HIGH);
	delay(5);
	digitalWrite(BAND_SELECT, LOW);
	delay(1);

	//go to the band
	for (int i = 0; i < lpf; i++){
		digitalWrite(BAND_SELECT, HIGH);
		delayMicroseconds(200);
		digitalWrite(BAND_SELECT, LOW);
		delayMicroseconds(200);
	}
	prev_lpf = lpf;
}

void set_lo(int frequency){
/*	si570_freq(frequency + bfo_freq);
	freq_hdr = frequency;
	printf("freq: %d\n", frequency);
	set_lpf(frequency);*/
	radio_tune_to(frequency);
}

void set_rx1(int frequency){
	//si570_freq(frequency + bfo_freq - ((rx_list->tuned_bin * 96000)/MAX_BINS));
	radio_tune_to(frequency);
	freq_hdr = frequency;
	printf("freq: %d\n", frequency);
	set_lpf(frequency);
}

void set_volume(double v){
	volume = v;	
}

int test_tone = 4000;

/*
The sound process is called by the duplex sound system for each block of samples
In this demo, we read and equivalent block from the file instead of processing from
the input I and Q signals.
*/

int32_t in_i[MAX_BINS];
int32_t in_q[MAX_BINS];
int32_t	out_i[MAX_BINS];
int32_t out_q[MAX_BINS];
short is_ready = 0;

void tx_init(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	tx_filter = filter_new(1024, 1025);
	filter_tune(tx_filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);
}

struct rx *add_rx(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	struct rx *r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512; 

	//create fft complex arrays to convert the frequency back to time
	r->fft_time = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->fft_freq = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->plan_rev = fftw_plan_dft_1d(MAX_BINS, r->fft_freq, r->fft_time, FFTW_BACKWARD, FFTW_ESTIMATE);

	r->output = 0;
	r->next = NULL;
	r->mode = mode;
	
	r->filter = filter_new(1024, 1025);
	filter_tune(r->filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);

	if (abs(bpf_high - bpf_low) < 1000){
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}
	else {
		r->agc_speed = 5;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}

	r->next = rx_list;
	rx_list = r;
}

int count = 0;

double agc(struct rx *r){
	int i;
	r->signal_strength = 0.0;

	//we only look at the first 80 bins as the audio doesn't go beyond 4 KHz
	// find the largest amplitude
	for (i=0; i < 80; i++){
		double s = cabs(r->fft_time[i+(MAX_BINS/2)]);
		if (r->signal_strength < s) 
			r->signal_strength = s;
	}

	//store the signal strenght in a buffer of last r->agc_speed blocks of 
	if (r->agc_loop >= r->agc_speed)
		r->agc_loop = 0;

	//we now try to maintain a level of signal to 1000,000 
	r->agc_reading[r->agc_loop++] = r->signal_strength;

	//find the gain reduction needed
	double gain = 1000.0;
	for (i = 0; i < r->agc_speed; i++)
		if (gain > 100 / r->agc_reading[i])
			gain = 100 / r->agc_reading[i];

	//if (gain > 200)
	//	gain = 200;

	//we have to ramp up the gain from r->agc_gain to the new gain in 100 steps
	double gain_step = (gain - r->agc_gain) / 100.0;
	for (i = 0; i < 100; i++)
		__imag__ r->fft_time[i+(MAX_BINS/2)] = cimag(r->fft_time[i+(MAX_BINS/2)]) * (r->agc_gain + gain_step);

	for (i= 100; i < MAX_BINS/2; i++)
		__imag__ r->fft_time[i+(MAX_BINS/2)] = cimag(r->fft_time[i+(MAX_BINS/2)]) * gain;


	//convert signal strength, in-place, to dbm
	r->signal_strength *= r->signal_strength;
	r->signal_strength = power2dB(r->signal_strength);
	r->signal_strength += MDS_LEVEL;
	if ((count++ % 20) == 0){
	//	for (i = 0; i < 10; i++)
	//		printf("%g ", r->agc_reading[i]);
	//	printf("speed %d, hang %d, step %g # %g - %g dBm\n", 
	//		r->agc_speed, r->agc_loop, gain_step, gain, r->signal_strength);
	}
	//printf("gain %f, new gain %f, step %f\n", r->agc_gain, gain, gain_step); 	
	r->agc_gain = gain;
	return gain;
}

void rx_process(int32_t *input_rx,  int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, int n_samples)
{
	int i, j = 0;
	double i_sample, q_sample;


	//STEP 1: first add the previous M samples to
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	//STEP 2: then add the new set of samples
	// m is the index into incoming samples, starting at zero
	// i is the index into the time samples, picking from 
	// the samples added in the previous step
	int m = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){
		i_sample = (1.0  *input_rx[j])/200000000.0;
		q_sample = 0;

		j++;

		__real__ fft_m[m] = i_sample;
		__imag__ fft_m[m] = q_sample;

		__real__ fft_in[i]  = i_sample;
		__imag__ fft_in[i]  = q_sample;
		m++;
	}

	// STEP 3: convert the time domain samples to  frequency domain
	fftw_execute(plan_fwd);

	//STEP 3B: this is a side line, we use these frequency domain
	// values to paint the spectrum in the user interface
	// I discovered that the raw time samples give horrible spectrum
	// and they need to be multiplied wiht a window function 
	// they use a separate fft plan
	// NOTE: the spectrum update has nothing to do with the actual
	// signal processing. If you are not showing the spectrum or the
	// waterfall, you can skip these steps
	for (i = 0; i < MAX_BINS; i++)
			__real__ fft_in[i] *= spectrum_window[i];
	fftw_execute(plan_spectrum);

	// the spectrum display is updated
	spectrum_update();


	// ... back to the actual processing, after spectrum update  

	// we may add another sub receiver within the pass band later,
	// hence, the linkced list of receivers here
	// at present, we handle just the first receiver
	struct rx *r = rx_list;
	
	//STEP 4: we rotate the bins around by r-tuned_bin
	for (i = 0; i < MAX_BINS; i++){
		int b =  i + r->tuned_bin;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[i] = fft_out[b];
	}

	// STEP 5:zero out the other sideband
	if (r->mode == MODE_LSB || r->mode == MODE_CWR) 
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}
	else 
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}

	// STEP 6: apply the filter to the signal,
	// in frequency domain we just multiply the filter
	// coefficients with the frequency domain samples
	for (i = 0; i < MAX_BINS; i++)
		r->fft_freq[i] *= r->filter->fir_coeff[i];

	//STEP 7: convert back to time domain	
	fftw_execute(r->plan_rev);

	//STEP 8 : AGC
	//double gain = agc(r);
	
	//STEP 9: send the output back to where it needs to go
	if (rx_list->output == 0)
		for (i= 0; i < MAX_BINS/2; i++){
			output_speaker[i] = cimag(r->fft_time[i+(MAX_BINS/2)]) * 1000000;
			//keep transmit buffer empty
			output_tx[i] = 0;
		}
}


/*
	MAX_BINS is 2048,
	the number of samples coming in each time is 1024 (half)
	so, we overlap the last 1024 samples with these 1024 samples
	to assemble the 2048 samples to be ffted
*/
void tx_2tone(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i, j, m;
	double i_sample, q_sample;
	struct rx *r = rx_list;

	// these are the sample from previous block that are 
	// use as a par of overlap-and discard filtering (read about it in dspguid,com
	m = 0;
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	m = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		//i_sample = (1.0 * vfo_read(&tone_a)) / 2000000000.0;
		i_sample = (1.0 *  (vfo_read(&tone_b)  + vfo_read(&tone_a) )) / 20000000000.0;
		q_sample = 0;

		j++;

		//these are stored for the overlap-and-dicard of the next block
		__real__ fft_m[m] = i_sample;
		__imag__ fft_m[m] = q_sample;

		__real__ fft_in[i]  = i_sample;
		__imag__ fft_in[i]  = q_sample;
		m++;
	}	


	//convert to frequency
	fftw_execute(plan_fwd);

	//now we apply the tx filter
	for (int i = 0; i < MAX_BINS; i++)
		fft_out[i] *= tx_filter->fir_coeff[i];

	// zero out the other sideband
	if (r->mode == MODE_LSB || r->mode == MODE_CWR)
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}
	else 
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}

	//now we rotate this 
	for (i = 0; i < MAX_BINS; i++){
		// we shift back because we are operating on the -ve
		// frequencies from N/2 to N,
		// the original signal was from N-1, down at the very edge
		// so we have to shift it to the middle of the passband
		// this effectively inverts the sideband 
		int b = i - tx_shift;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[b] = fft_out[i];
	}

	//show on the spectrum display
	for (i = 0; i < MAX_BINS; i++)
		fft_spectrum[i] = r->fft_freq[i];
		
	// the spectrum display is updated
	spectrum_update();

	//convert back to time domain	
	fftw_execute(r->plan_rev);

	//send the output back to where it needs to go
	for (i= 0; i < MAX_BINS/2; i++){
		output_tx[i] = creal(rx_list->fft_time[i+(MAX_BINS/2)]) * volume;
		output_speaker[i] = output_tx[i]; 
	}
}

void tx_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i;
	double i_sample, q_sample;

	// we are reusing the rx structure, we should
	// have a seperate tx structure to work with	
	struct rx *r = rx_list;

	//first add the previous M samples
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	int m = 0;
	int j = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		//i_sample = (1.0 * vfo_read(&tone_a)) / 2000000000.0;
		if(r->mode == MODE_2TONE)
			i_sample = (1.0 * (vfo_read(&tone_a) + vfo_read(&tone_b))) / 20000000.0;
		else
			i_sample = (1.0 * input_mic[j]) / 2000000000.0;
		q_sample = 0;

		j++;

		__real__ fft_m[m] = i_sample;
		__imag__ fft_m[m] = q_sample;

		__real__ fft_in[i]  = i_sample;
		__imag__ fft_in[i]  = q_sample;
		m++;
	}

	//convert to frequency
	fftw_execute(plan_fwd);


	// NOTE: fft_out holds the fft output (in freq domain) of the 
	// incoming mic samples 
	// the naming is unfortunate

	// apply the filter
	for (i = 0; i < MAX_BINS; i++)
		fft_out[i] *= tx_filter->fir_coeff[i];

	// the usb extends from 0 to MAX_BINS/2 - 1, 
	// the lsb extends from MAX_BINS - 1 to MAX_BINS/2 (reverse direction)
	// zero out the other sideband

	// TBD: Something strange is going on, this should have been the otherway


	if (r->mode == MODE_LSB || r->mode == MODE_CWR)
		// zero out the LSB
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
			__real__ fft_out[i] = 0;
			__imag__ fft_out[i] = 0;	
		}
	else  
		// zero out the USB
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ fft_out[i] = 0;
			__imag__ fft_out[i] = 0;	
		}

	//now rotate to the tx_bin 
	for (i = 0; i < MAX_BINS; i++){
		int b = i + tx_shift;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[b] = fft_out[i];
	}

	// the spectrum display is updated
	spectrum_update();
	//convert back to time domain	
	fftw_execute(r->plan_rev);

	//send the output back to where it needs to go
	for (i= 0; i < MAX_BINS/2; i++){
		output_tx[i] = creal(rx_list->fft_time[i+(MAX_BINS/2)]) * volume;
		output_speaker[i] = 0; 
	}
}

/*
	This is called each time there is a block of signal samples ready 
	either from the mic or from the rx IF 
*/	
void sound_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	if (in_tx){
		if (rx_list->mode == MODE_2TONE)
			tx_2tone(input_rx, input_mic, output_speaker, output_tx, n_samples);
		else
			tx_process(input_rx, input_mic, output_speaker, output_tx, n_samples);
	}
	else
		rx_process(input_rx, input_mic, output_speaker, output_tx, n_samples);
}


/* 
Write code that mus repeatedly so things, it is called during the idle time 
of the event loop 
*/
void loop(){
	delay(10);
}

void setup_audio_codec(){

	strcpy(audio_card, "hw:0");

	//configure all the channels of the mixer
	sound_mixer(audio_card, "Input Mux", 0);
	sound_mixer(audio_card, "Line", 1);
	sound_mixer(audio_card, "Mic", 0);
	sound_mixer(audio_card, "Mic Boost", 0);
	sound_mixer(audio_card, "Playback Deemphasis", 0);
 
	sound_mixer(audio_card, "Master", 90);
	sound_mixer(audio_card, "Output Mixer HiFi", 1);
	sound_mixer(audio_card, "Output Mixer Mic Sidetone", 0);
}

/* 
This is the one-time initialization code 
*/
void setup(){

	fft_init();

	vfo_init_phase_table();

	add_rx(7000000, MODE_LSB, -3000, -300);
	rx_list->tuned_bin = 512;
	tx_init(7000000, MODE_LSB, -3000, -300);

	sound_thread_start("hw:0,0");
	setup_audio_codec();

	sleep(1);
	vfo_start(&tone_a, 700, 0);
	vfo_start(&tone_b, 1900, 0);

	fserial = serialOpen("/dev/ttyUSB0", 38400);
	if (fserial == -1){
		fserial = serialOpen("/dev/ttyUSB1", 38400);
		if (!fserial){
			puts("uBITX not connected");
			exit(-1);
		}
	}
	delay(2000);	
//	si570_init();
//	set_lo(7021000);
}

void sdr_request(char *request, char *response){
	char cmd[100], value[1000];


	printf("[%s]\n", request);

	char *p = strchr(request, '=');
	int n = p - request;
	if (!p)
		return;
	strncpy(cmd, request, n);
	cmd[n] = 0;
	strcpy(value, request+n+1);

	//at present, we handle only the requests to tune and adjust volume
/*	if (!strcmp(cmd, "r1:gain")){
		int d = atoi(value);
		volume = d * 100;
		set_volume(volume);
		printf("Volume set to %d\n", volume);
		strcpy(response, "ok");	
	}
	else*/ if (!strcmp(cmd, "xit")){
		int d = atoi(value);
		set_lo(d);
		if (d > 0 && d < 2048)
			tx_shift = -d;
		printf("xit set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strcmp(cmd, "r1:freq")){
		int d = atoi(value);
		set_rx1(d);
		printf("Frequency set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strcmp(cmd, "r1:mode")){
		if (!strcmp(value, "USB"))
				rx_list->mode = MODE_USB;
		else if (!strcmp(value, "LSB"))
				rx_list->mode = MODE_LSB;
		else if (!strcmp(value, "CW"))
			rx_list->mode = MODE_CW;
		else if (!strcmp(value, "CWR"))
			rx_list->mode = MODE_CWR;
		else if (!strcmp(value, "2TONE")){
			rx_list->mode = MODE_2TONE;
		}

		// An interesting but non-essential note:
		// the sidebands inverted twice, to come out correctly after all
		// conisder that the second oscillator is set to 27.025 MHz and 
		// a 7 MHz signal is tuned in by a 34 Mhz oscillator.
		// The first IF will be 25 Mhz, converted to a second IF of 25 KHz
		// Now, imagine that the signal at 7 Mhz moves up by 1 Khz
		// the IF now is going to be 34 - 7.001 MHz = 26.999 MHz which 
		// converts to a second IF of 26.999 - 27.025 = 26 KHz
		// Effectively, if a signal moves up, so does the second IF

		if (rx_list->mode == MODE_USB || rx_list->mode == MODE_CW){
			filter_tune(rx_list->filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
			puts("\n\n\ntx filter ");
			filter_tune(tx_filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
		}
		else { 
			filter_tune(rx_list->filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
			puts("\n\n\ntx filter ");
			filter_tune(tx_filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
		}
		
		printf("mode set to %d\n", rx_list->mode);
		strcpy(response, "ok");
	}
	else if (!strcmp(cmd, "txmode")){
		puts("\n\n\n\n###### tx filter #######");
		if (!strcmp(value, "LSB") || !strcmp(value, "CWR"))
			filter_tune(tx_filter, (1.0*-3000)/96000.0, (1.0 * -300)/96000.0, 5);
		else
			filter_tune(tx_filter, (1.0*300)/96000.0, (1.0*3000)/96000.0, 5);
	}
	else if (!strcmp(cmd, "tx")){
		if (!strcmp(value, "on")){
			in_tx = 1;
			digitalWrite(TX_LINE, HIGH);
			radio_tx(1);	
			sound_mixer(audio_card, "Master", tx_power);
			sound_mixer(audio_card, "Capture", tx_gain);
			strcpy(response, "ok");
			spectrum_reset();
		}
		else {
			in_tx = 0;
			strcpy(response, "ok");
			digitalWrite(TX_LINE, LOW);
			radio_tx(0);
			sound_mixer(audio_card, "Master", rx_vol);
			sound_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
		}
	}
	else if (!strcmp(cmd, "tx_gain")){
		tx_gain = atoi(value);
		if(in_tx)
			sound_mixer(audio_card, "Capture", tx_gain);
	}
	else if (!strcmp(cmd, "tx_power")){
		tx_power = atoi(value) - 25;
		if(in_tx)	
			sound_mixer(audio_card, "Master", tx_power);
	}
	else if(!strcmp(cmd, "r1:gain")){
		rx_gain = atoi(value);
		if(!in_tx)
			sound_mixer(audio_card, "Capture", rx_gain);
	}
	else if (!strcmp(cmd, "r1:volume")){
		rx_vol = atoi(value);
		if(!in_tx)	
			sound_mixer(audio_card, "Master", rx_vol);
	}
	else
		printf("*Error request[%s] not accepted\n", request);
}

