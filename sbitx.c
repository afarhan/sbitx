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
#include <linux/types.h>
#include <stdint.h>
#include <time.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"
#include "si5351.h"

char audio_card[32];
static int tx_shift = 512;

FILE *pf_debug = NULL;

//this is for processing FT8 decodes 
//unsigned int	wallclock = 0;

#define TX_LINE 4
#define BAND_SELECT 5
#define LPF_A 5
#define LPF_B 6
#define LPF_C 10
#define LPF_D 11

float fft_bins[MAX_BINS]; // spectrum ampltiudes  
fftw_complex *fft_spectrum;
fftw_plan plan_spectrum;
float spectrum_window[MAX_BINS];


fftw_complex *fft_out;		// holds the incoming samples in freq domain (for rx as well as tx)
fftw_complex *fft_in;			// holds the incoming samples in time domain (for rx as well as tx) 
fftw_complex *fft_m;			// holds previous samples for overlap and discard convolution 
fftw_plan plan_fwd, plan_tx;
int bfo_freq = 40035000;
int freq_hdr = 7000000;

static double volume 	= 100.0;
static int tx_power_watts = 40;
static int rx_gain = 100;
static int rx_vol = 100;
static int tx_gain = 100;
static int tx_compress = 0;
static double spectrum_speed = 0.1;
static int in_tx = 0;
static int rx_tx_ramp = 0;
static int sidetone = 2000000000;
struct vfo tone_a, tone_b; //these are audio tone generators
static int tx_use_line = 0;
struct rx *rx_list = NULL;
struct rx *tx_list = NULL;
struct filter *tx_filter;	//convolution filter
static double tx_amp = 0.0;

#define MUTE_MAX 6 
static int mute_count = 50;

FILE *pf_record;
int16_t record_buffer[1024];
int32_t modulation_buff[MAX_BINS];


#define CMD_TX (2)
#define CMD_RX (3)
//#define TUNING_SHIFT (-550)
#define TUNING_SHIFT (0)
#define MDS_LEVEL (-135)
int fserial = 0;


void radio_tune_to(u_int32_t f){
  si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT);
  //printf("Setting radio to %d\n", f);
}

void fft_init(){
	int mem_needed;

	//printf("initializing the fft\n");
	fflush(stdout);

	mem_needed = sizeof(fftw_complex) * MAX_BINS;

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

void fft_reset_m_bins(){
	//zero up the previous 'M' bins
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS/2);
	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
//	for (int i= 0; i < MAX_BINS/2; i++){
//		__real__ fft_m[i]  = 0.0;
//		__imag__ fft_m[i]  = 0.0;
//	}
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

 // redraw();
}

void set_lpf_40mhz(int frequency){
	static int prev_lpf = -1;
	int lpf = 0;

	if (frequency < 5500000)
		lpf = LPF_D;
	else if (frequency < 10500000)		
		lpf = LPF_C;
	else if (frequency < 18500000)		
		lpf = LPF_B;
	else if (frequency < 30000000)
		lpf = LPF_A; 

	if (lpf == prev_lpf){
		//puts("LPF not changed");
		return;
	}

	//printf("##################Setting LPF to %d\n", lpf);

  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);

  //printf("################ setting %d high\n", lpf);
  digitalWrite(lpf, HIGH); 
	prev_lpf = lpf;
}


void set_rx1(int frequency){
	radio_tune_to(frequency);
	freq_hdr = frequency;
	set_lpf_40mhz(frequency);
}

void set_volume(double v){
	volume = v;	
}

FILE *wav_start_writing(const char* path)
{
    char subChunk1ID[4] = { 'f', 'm', 't', ' ' };
    uint32_t subChunk1Size = 16; // 16 for PCM
    uint16_t audioFormat = 1; // PCM = 1
    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t sampleRate = 12000;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t byteRate = sampleRate * blockAlign;

    char subChunk2ID[4] = { 'd', 'a', 't', 'a' };
    uint32_t subChunk2Size = 0Xffffffff; //num_samples * blockAlign;

    char chunkID[4] = { 'R', 'I', 'F', 'F' };
    uint32_t chunkSize = 4 + (8 + subChunk1Size) + (8 + subChunk2Size);
    char format[4] = { 'W', 'A', 'V', 'E' };

    FILE* f = fopen(path, "w");

    // NOTE: works only on little-endian architecture
    fwrite(chunkID, sizeof(chunkID), 1, f);
    fwrite(&chunkSize, sizeof(chunkSize), 1, f);
    fwrite(format, sizeof(format), 1, f);

    fwrite(subChunk1ID, sizeof(subChunk1ID), 1, f);
    fwrite(&subChunk1Size, sizeof(subChunk1Size), 1, f);
    fwrite(&audioFormat, sizeof(audioFormat), 1, f);
    fwrite(&numChannels, sizeof(numChannels), 1, f);
    fwrite(&sampleRate, sizeof(sampleRate), 1, f);
    fwrite(&byteRate, sizeof(byteRate), 1, f);
    fwrite(&blockAlign, sizeof(blockAlign), 1, f);
    fwrite(&bitsPerSample, sizeof(bitsPerSample), 1, f);

    fwrite(subChunk2ID, sizeof(subChunk2ID), 1, f);
    fwrite(&subChunk2Size, sizeof(subChunk2Size), 1, f);
		
		return f;
}

void wav_record(int32_t *samples, int count){
	int16_t *w;
	int32_t *s;
	int i = 0, j = 0;
	int decimation_factor = 96000 / 12000; 

	if (!pf_record)
		return;

	w = record_buffer;
	while(i < count){
		*w++ = *samples / 32786;
		samples += decimation_factor;
		i += decimation_factor;	
		j++;
	}
	fwrite(record_buffer, j, sizeof(int16_t), pf_record);
}

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

struct rx *add_tx(int frequency, short mode, int bpf_low, int bpf_high){

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
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}
	else {
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}

	//the modems drive the tx at 12000 Hz, this has to be upconverted
	//to the radio's sampling rate

  r->next = tx_list;
  tx_list = r;
}

struct rx *add_rx(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	struct rx *r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512; 
	r->agc_gain = 0.0;

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
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
    r->signal_avg = 0;
	}
	else {
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
    r->signal_avg = 0;
	}

	// the modems are driven by 12000 samples/sec
	// the queue is for 20 seconds, 5 more than 15 sec needed for the FT8

	r->next = rx_list;
	rx_list = r;

}



int count = 0;

double agc2(struct rx *r){
	int i;
  double signal_strength, agc_gain_should_be;

	//do nothing if agc is off
  if (r->agc_speed == -1){
	  for (i=0; i < MAX_BINS/2; i++)
			__imag__ (r->fft_time[i+(MAX_BINS/2)]) *=10000000;
    return 10000000;
  }

  //find the peak signal amplitude
  signal_strength = 0.0;
	for (i=0; i < MAX_BINS/2; i++){
		double s = cimag(r->fft_time[i+(MAX_BINS/2)]) * 1000;
		if (signal_strength < s) 
			signal_strength = s;
	}
	//also calculate the moving average of the signal strength
  r->signal_avg = (r->signal_avg * 0.93) + (signal_strength * 0.07);
	if (signal_strength == 0)
		agc_gain_should_be = 10000000;
	else
		agc_gain_should_be = 100000000000/signal_strength;
	r->signal_strength = signal_strength;
//	printf("Agc temp, g:%g, s:%g, f:%g ", r->agc_gain, signal_strength, agc_gain_should_be);

	double agc_ramp = 0.0;

  // climb up the agc quickly if the signal is louder than before 
	if (agc_gain_should_be < r->agc_gain){
		r->agc_gain = agc_gain_should_be;
		//reset the agc to hang count down 
    r->agc_loop = r->agc_speed;
//  	printf("attack %g %d ", r->agc_gain, r->agc_loop);
  }
	else if (r->agc_loop <= 0){
		agc_ramp = (agc_gain_should_be - r->agc_gain) / (MAX_BINS/2);	
//  	printf("release %g %d ",  r->agc_gain, r->agc_loop);
	}
//	else if (r->agc_loop > 0)
//  	printf("hanging %g %d ", r->agc_gain, r->agc_loop);
 
	if (agc_ramp != 0){
//		printf("Ramping from %g ", r->agc_gain);
  	for (i = 0; i < MAX_BINS/2; i++){
	  	__imag__ (r->fft_time[i+(MAX_BINS/2)]) *= r->agc_gain;
		}
		r->agc_gain += agc_ramp;		
//		printf("by %g to %g ", agc_ramp, r->agc_gain);
	}
	else 
  	for (i = 0; i < MAX_BINS/2; i++)
	  	__imag__ (r->fft_time[i+(MAX_BINS/2)]) *= r->agc_gain;

//	printf("\n");
  r->agc_loop--;

	//printf("%d:s meter: %d %d %d \n", count++, (int)r->agc_gain, (int)r->signal_strength, r->agc_loop);
  return 100000000000 / r->agc_gain;  
}

void rx_process(int32_t *input_rx,  int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, int n_samples)
{
	int i, j = 0;
	double i_sample, q_sample;


	if (mute_count){
		memset(input_rx, 0, n_samples * sizeof(int32_t));
		mute_count--;
	}

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
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}
	else  
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
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
	agc2(r);
	
	//STEP 9: send the output back to where it needs to go
	int is_digital = 0;

	if (rx_list->output == 0)
		for (i= 0; i < MAX_BINS/2; i++){
			int32_t sample;
			sample = cimag(r->fft_time[i+(MAX_BINS/2)]);
			//keep transmit buffer empty
			output_speaker[i] = sample;
			output_tx[i] = 0;
		}

/*
	if (rx_tx_ramp){
		memset(output_speaker, 0, sizeof(int32_t) * MAX_BINS/2);
		printf("Rx muted %d\n", rx_tx_ramp);
		rx_tx_ramp--;
	}
*/
	//push the data to any potential modem 
	modem_rx(rx_list->mode, output_speaker, MAX_BINS/2);
}


void tx_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i;
	double i_sample, q_sample;

	struct rx *r = tx_list;


	if (mute_count && (r->mode == MODE_USB || r->mode == MODE_LSB)){
		memset(input_mic, 0, n_samples * sizeof(int32_t));
		mute_count--;
	}
	//first add the previous M samples
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	int m = 0;
	int j = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		if (r->mode == MODE_2TONE)
			i_sample = (1.0 * (vfo_read(&tone_a) 
										+ vfo_read(&tone_b))) / 50000000000.0;
		else if (r->mode == MODE_CW || r->mode == MODE_CWR || r->mode == MODE_FT8)
			i_sample = modem_next_sample(r->mode) / 3;
		else 
	  	i_sample = (1.0 * input_mic[j]) / 2000000000.0;

		//don't echo the voice modes
		if (r->mode == MODE_USB || r->mode == MODE_LSB || r->mode == MODE_AM 
			|| r->mode == MODE_NBFM)
			output_speaker[j] = 0;
		else
			output_speaker[j] = i_sample * sidetone;
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
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ fft_out[i] = 0;
			__imag__ fft_out[i] = 0;	
		}
	else
		// zero out the USB
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
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
	//spectrum_update();

	//convert back to time domain	
	fftw_execute(r->plan_rev);

	float scale = volume;

	if ((r->mode == MODE_USB || r->mode == MODE_LSB) && tx_compress > 10){
		double max = 0;
		for (i= 0; i < MAX_BINS/2; i++){
			double s = creal(r->fft_time[i+(MAX_BINS/2)]);
			if (max < s)
				max = s;
			s *= tx_compress/30;
			if (s > 35.0)
				s = 35.0;
			if (s < -35.0)
				s = -35.0;
			output_tx[i] = s * scale * tx_amp;
		}
		//printf("max %f\n", max);
	}
	else{ 
		for (i= 0; i < MAX_BINS/2; i++){
			double s = creal(r->fft_time[i+(MAX_BINS/2)]);
			output_tx[i] = s * scale * tx_amp;
		}
	}

	sdr_modulation_update(output_tx, MAX_BINS/2, tx_amp);	
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

	if (pf_record)
		wav_record(in_tx == 0 ? output_speaker : input_mic, n_samples);
}


void set_rx_filter(){
	if(rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR)
    filter_tune(rx_list->filter, 
      (1.0 * -rx_list->high_hz)/96000.0, 
      (1.0 * -rx_list->low_hz)/96000.0 , 
      5);
	else
    filter_tune(rx_list->filter, 
      (1.0 * rx_list->low_hz)/96000.0, 
      (1.0 * rx_list->high_hz)/96000.0 , 
      5);
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
 
	sound_mixer(audio_card, "Master", 10);
	sound_mixer(audio_card, "Output Mixer HiFi", 1);
	sound_mixer(audio_card, "Output Mixer Mic Sidetone", 0);

}

void setup_oscillators(){
  //initialize the SI5351

  delay(200);
  si5351bx_init();
  delay(200);
  si5351bx_setfreq(1, bfo_freq);

  delay(200);
  si5351bx_setfreq(1, bfo_freq);

  si5351_reset();
}

struct power_settings {
	int f_start;
	int f_stop;
	int	max_watts;
	int tx_scale;
	double scale;
};

struct power_settings band_power[] ={
	{ 3500000,  4000000, 40, 80, 0.005},
	{ 7000000,  7300000, 40,82, 0.006},
	{10000000, 10200000, 30, 81, 0.008},
	{14000000, 14300000, 30, 91, 0.022},
	{18000000, 18200000, 25, 93, 0.03},
	{21000000, 21450000, 20, 93, 0.05},
	{24800000, 25000000, 10, 94, 0.1},
	{28000000, 29700000,  10, 95, 0.1}  
};

/*
	 the PA gain varies across the band from 3.5 MHz to 30 MHz
 	here we adjust the drive levels to keep it up, almost level
*/
void set_tx_power_levels(){
 // printf("Setting tx_power to %d, gain to %d\n", tx_power_watts, tx_gain);
	int tx_power_gain = 0;

	//search for power in the approved bands
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		if (band_power[i].f_start <= freq_hdr && freq_hdr <= band_power[i].f_stop){
			if (tx_power_watts > band_power[i].max_watts)
				tx_power_watts = band_power[i].max_watts;
		
			//next we do a decimal coversion of the power reduction needed
			tx_amp = (1.0 * tx_power_watts * band_power[i].scale);  
		}	
	}
//	printf("tx_gain_compensation is set to %g for %d watts\n", tx_amp, tx_power_watts);
	//we keep the audio card output 'volume' constant'
	sound_mixer(audio_card, "Master", 95);
	sound_mixer(audio_card, "Capture", tx_gain);
}


void set_tx_power_levels_old(){
//  printf("Setting tx_power to %d, gain to %d\n", tx_power_watts, tx_gain);
	int tx_power_gain = 0;

	//search for power in the approved bands
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		if (band_power[i].f_start <= freq_hdr && freq_hdr <= band_power[i].f_stop){
			if (tx_power_watts > band_power[i].max_watts)
				tx_power_watts = band_power[i].max_watts;
		
			//next we do a decimal coversion of the power reduction needed
			int attenuation = 
			(20*log10((1.0*tx_power_watts)/(1.0*band_power[i].max_watts))); 
			tx_power_gain = band_power[i].tx_scale + attenuation; 
		}	
	}
	printf("tx_power_gain set to %d for %d watts\n", tx_power_gain, tx_power_watts);
	sound_mixer(audio_card, "Master", tx_power_gain);
	sound_mixer(audio_card, "Capture", tx_gain);
}

/* 
This is the one-time initialization code 
*/
void setup(){

	//setup the LPF and the gpio pins
	pinMode(TX_LINE, OUTPUT);
	pinMode(LPF_A, OUTPUT);
	pinMode(LPF_B, OUTPUT);
	pinMode(LPF_C, OUTPUT);
	pinMode(LPF_D, OUTPUT);
  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);
	digitalWrite(TX_LINE, LOW);

	fft_init();
	vfo_init_phase_table();
  setup_oscillators();

	modem_init();

	add_rx(7000000, MODE_LSB, -3000, -300);
	add_tx(7000000, MODE_LSB, -3000, -300);
	rx_list->tuned_bin = 512;
  tx_list->tuned_bin = 512;
	tx_init(7000000, MODE_LSB, -3000, -300);


	setup_audio_codec();
	sound_thread_start("plughw:0,0");

	sleep(1); //why? to allow the aloop to initialize?

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

}

void sdr_request(char *request, char *response){
	char cmd[100], value[1000];

//	printf("[%s]\n", request);

	char *p = strchr(request, '=');
	int n = p - request;
	if (!p)
		return;
	strncpy(cmd, request, n);
	cmd[n] = 0;
	strcpy(value, request+n+1);

	if (!strcmp(cmd, "stat:tx")){
		if (in_tx)
			strcpy(response, "ok on");
		else
			strcpy(response, "ok off");
	}
	else if (!strcmp(cmd, "r1:freq")){
		int d = atoi(value);
		set_rx1(d);
		//printf("Frequency set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strcmp(cmd, "r1:mode")){
		if (!strcmp(value, "LSB"))
			rx_list->mode = MODE_LSB;
		else if (!strcmp(value, "CW"))
			rx_list->mode = MODE_CW;
		else if (!strcmp(value, "CWR"))
			rx_list->mode = MODE_CWR;
		else if (!strcmp(value, "2TONE"))
			rx_list->mode = MODE_2TONE;
		else if (!strcmp(value, "FT8"))
			rx_list->mode = MODE_FT8;
		else if (!strcmp(value, "PSK31"))
			rx_list->mode = MODE_PSK31;
		else if (!strcmp(value, "RTTY"))
			rx_list->mode = MODE_RTTY;
		else
			rx_list->mode = MODE_USB;
		
    //set the tx mode to that of the rx1
    tx_list->mode = rx_list->mode;

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
			//puts("\n\n\ntx filter ");
			filter_tune(tx_list->filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
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
			filter_tune(tx_list->filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
		}
		
		//printf("mode set to %d\n", rx_list->mode);
		strcpy(response, "ok");
	}
	else if (!strcmp(cmd, "txmode")){
		puts("\n\n\n\n###### tx filter #######");
		if (!strcmp(value, "LSB") || !strcmp(value, "CWR"))
			filter_tune(tx_filter, (1.0*-3000)/96000.0, (1.0 * -300)/96000.0, 5);
		else
			filter_tune(tx_filter, (1.0*300)/96000.0, (1.0*3000)/96000.0, 5);
	}
	else if(!strcmp(cmd, "record")){
		if (!strcmp(value, "off")){
			fclose(pf_record);
			pf_record = NULL;
		}
		else
			pf_record = wav_start_writing(value);
	}
	else if (!strcmp(cmd, "tx")){
		if (!strcmp(value, "on")){
			in_tx = 1;
			mute_count = 20;
      fft_reset_m_bins();
			digitalWrite(TX_LINE, HIGH);
      delay(50);
			set_tx_power_levels();
			strcpy(response, "ok");
			spectrum_reset();
		//	rx_tx_ramp = 1;
		}
		else {
			in_tx = 0;
      fft_reset_m_bins();
			mute_count = MUTE_MAX;
			strcpy(response, "ok");
			digitalWrite(TX_LINE, LOW);
			sound_mixer(audio_card, "Master", rx_vol);
			sound_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
			//rx_tx_ramp = 10;
		}
	}
	else if (!strcmp(cmd, "tx_gain")){
		tx_gain = atoi(value);
		if(in_tx)
			set_tx_power_levels();
	}
	else if (!strcmp(cmd, "tx_power")){
    tx_power_watts = atoi(value);
		if(in_tx)
			set_tx_power_levels();	
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
	else if(!strcmp(cmd, "r1:high")){
    rx_list->high_hz = atoi(value);
    set_rx_filter();
  }
	else if(!strcmp(cmd, "r1:low")){
    rx_list->low_hz = atoi(value);
    set_rx_filter();
  }
  else if (!strcmp(cmd, "r1:agc")){
    if (!strcmp(value, "OFF"))
      rx_list->agc_speed = -1;
    else if (!strcmp(value, "SLOW"))
      rx_list->agc_speed = 100;
		else if (!strcmp(value, "MED"))
			rx_list->agc_speed = 33; 
    else if (!strcmp(value, "FAST"))
      rx_list->agc_speed = 10;
  }
	else if (!strcmp(cmd, "sidetone")){ //between 100 and 0
		float t_sidetone = atof(value);
		if (0 <= t_sidetone && t_sidetone <= 100)
			sidetone = atof(value) * 20000000;
	}
  else if (!strcmp(cmd, "mod")){
    if (!strcmp(value, "MIC"))
      tx_use_line = 0;
    else if (!strcmp(value, "LINE"))
      tx_use_line = 1;
  }
	else if (!strcmp(cmd, "tx_compress"))
		tx_compress = atoi(value); 
  /* else
		printf("*Error request[%s] not accepted\n", request); */
}

