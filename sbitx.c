#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"

/*
The incoming signals are time sampled as I and Q (as stereo audio). These are samples 
in signed 32-bit range.
FFT is a softeware that takes a the consecutive values of signal's voltage and converts them into a block of frequencies with how strong each frequency is and the exact position of the wwave (phase) wrt to others. 
The fft_out stores this and we can use this to pain the screen.
In individual signals can be picked off this waterfall and decoded. By limint ourselves... 
(tbd)
*/

char audio_card[32];

int tx_shift = -512;
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

int	fd;

static double volume 	= 100000.0;
static double mic_gain = 200000000.0;
static double spectrum_speed = 0.1;
static int in_tx = 0;
struct vfo tone_a, tone_b; //these are audio tone generators

struct rx *rx_list = NULL;
struct filter *tx_filter;	//convolution filter

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

	if (frequency < 3500000)
		lpf = 0;
	else if (frequency < 6000000)
		lpf = 1;
	else if (frequency < 8000000)		
		lpf = 2;
	else if (frequency < 15000000)
		lpf = 3;
	else if (frequency < 21500000)
		lpf = 4;

	if (lpf == prev_lpf)
		return;

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
	si570_freq(frequency + bfo_freq);
	freq_hdr = frequency;
	printf("freq: %d\n", frequency);
	set_lpf(frequency);
}

void set_rx1(int frequency){
	si570_freq(frequency + bfo_freq - ((rx_list->tuned_bin * 96000)/MAX_BINS));
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
	r->tuned_bin = 510; 

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
		r->agc_speed = 5;
		r->agc_threshold = -100;
	}
	else {
		r->agc_speed = 1;
		r->agc_threshold = -100;
	}

	r->next = rx_list;
	rx_list = r;
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
	if (r->mode == MODE_USB || r->mode == MODE_CW || r->mode == MODE_DIGITAL)
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

	//TO DO, we must add AGC here somewhere...

	//STEP 8: send the output back to where it needs to go
	if (rx_list->output == 0)
		for (i= 0; i < MAX_BINS/2; i++){
			output_speaker[i] = cimag(rx_list->fft_time[i+(MAX_BINS/2)]) * volume;
			//keep transmit buffer empty
			output_tx[i] = 0;
		}
}


void tx_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i, j = 0;
	double i_sample, q_sample;

	//first add the previous M samples
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	int m = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		//i_sample = (1.0 * vfo_read(&tone_a)) / 2000000000.0;
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

	// we are reusing the rx structure, we should
	// have a seperate tx structure to work with	
	struct rx *r = rx_list;

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
	if (in_tx)
		tx_process(input_rx, input_mic, output_speaker, output_tx, n_samples);
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
	vfo_start(&tone_a, 800, 0);
	si570_init();
	set_lo(7021000);
}

void sdr_request(char *request, char *response){
	//at present, we handle only the requests to tune and adjust volume
	printf("request: %s\n", request);
	if (!strncmp(request, "r1:gain=", 8)){
		int d = atoi(request+8);
		volume = d * 100;
		set_volume(volume);
		printf("Volume set to %d from %s\n", volume, request);
		strcpy(response, "ok");	
	}
	else if (!strncmp(request, "xit=",4)){
		int d = atoi(request+4);
		set_lo(d);
		if (d > 0 && d < 2048)
			tx_shift = -d;
		printf("xit set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strncmp(request, "r1:freq=",8)){
		int d = atoi(request+8);
		set_rx1(d);
		printf("Frequency set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strncmp(request, "r1:mode=", 8)){
		if (!strcmp(request, "r1:mode=USB"))
				rx_list->mode = MODE_USB;
		else if (!strcmp(request, "r1:mode=LSB"))
				rx_list->mode = MODE_LSB;
		else if (!strcmp(request, "r1:mode=CW"))
			rx_list->mode = MODE_CW;
		else if (!strcmp(request, "r1:mode=CWR"))
			rx_list->mode = MODE_CWR;


		// An interesting but non-essential note:
		// the sidebands inverted twice, to come out correctly after all
		// conisder that the second oscillator is set to 27.025 MHz and 
		// a 7 MHz signal is tuned in by a 34 Mhz oscillator.
		// The first IF will be 25 Mhz, converted to a second IF of 25 KHz
		// Now, imagine that the signal at 7 Mhz moves up by 1 Khz
		// the IF now is going to be 34 - 7.001 MHz = 26.999 MHz which 
		// converts to a second IF of 26.999 - 27.025 = 26 KHz
		// Effectively, if a signal moves up, so does the second IF

		if (rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR){
			filter_tune(rx_list->filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
		}
		else { 
			filter_tune(rx_list->filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
		}
		
		printf("mode set to %d\n", rx_list->mode);
		strcpy(response, "ok");
	}
	else if (!strncmp(request, "txmode", 6)){
		if (!strcmp(request+7, "LSB") || !strcmp(request+7, "CWR"))
			filter_tune(tx_filter, (1.0*-3000)/96000.0, (1.0 * -300)/96000.0, 5);
		else
			filter_tune(tx_filter, (1.0*300)/96000.0, (1.0*3000)/96000.0, 5);
	}
	else if (!strncmp(request, "tx:on", 5)){
		in_tx = 1;
		digitalWrite(TX_LINE, HIGH);
		strcpy(response, "ok");
	}
	else if (!strncmp(request, "tx:off", 5)){
		in_tx = 0;
		strcpy(response, "ok");
		digitalWrite(TX_LINE, LOW);
	}
}

