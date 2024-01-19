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
#include <linux/limits.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"
#include "i2cbb.h"
#include "si5351.h"
#include "ini.h"

char audio_card[32];
static int tx_shift = 512;

FILE *pf_debug = NULL;

//this is for processing FT8 decodes 
//unsigned int	wallclock = 0;

#define TX_LINE 4
#define TX_POWER 27
#define BAND_SELECT 5
#define LPF_A 5
#define LPF_B 6
#define LPF_C 10
#define LPF_D 11

#define SBITX_DE (0)
#define SBITX_V2 (1)

int sbitx_version = SBITX_V2;
int fwdpower, vswr;
float fft_bins[MAX_BINS]; // spectrum ampltiudes  
int spectrum_plot[MAX_BINS];
fftw_complex *fft_spectrum;
fftw_plan plan_spectrum;
float spectrum_window[MAX_BINS];
void set_rx1(int frequency);
void tr_switch(int tx_on);

fftw_complex *fft_out;		// holds the incoming samples in freq domain (for rx as well as tx)
fftw_complex *fft_in;			// holds the incoming samples in time domain (for rx as well as tx) 
fftw_complex *fft_m;			// holds previous samples for overlap and discard convolution 
fftw_plan plan_fwd, plan_tx;
int bfo_freq = 40035000;
int freq_hdr = -1;

static double volume 	= 100.0;
static int tx_drive = 40;
static int rx_gain = 100;
static int rx_vol = 100;
static int tx_gain = 100;
static int tx_compress = 0;
static double spectrum_speed = 0.3;
static int in_tx = 0;
static int rx_tx_ramp = 0;
static int sidetone = 2000000000;
struct vfo tone_a, tone_b; //these are audio tone generators
static int tx_use_line = 0;
struct rx *rx_list = NULL;
struct rx *tx_list = NULL;
struct filter *tx_filter;	//convolution filter
static double tx_amp = 0.0;
static double alc_level = 1.0;
static int tr_relay = 0;
static int rx_pitch = 700; //used only to offset the lo for CW,CWR
static int bridge_compensation = 100;
static double voice_clip_level = 0.022;
static int in_calibration = 1; // this turns off alc, clipping et al

#define MUTE_MAX 6 
static int mute_count = 50;

FILE *pf_record;
int16_t record_buffer[1024];
int32_t modulation_buff[MAX_BINS];


/* the power gain of the tx varies widely from 
band to band. these data structures help in flattening 
the gain */

struct power_settings {
	int f_start;
	int f_stop;
	int	max_watts;
	double scale;
};

struct power_settings band_power[] ={
	{ 3500000,  4000000, 37, 0.002},
	{ 7000000,  7300009, 40, 0.0015},
	{10000000, 10200000, 35, 0.0019},
	{14000000, 14300000, 35, 0.0025},
	{18000000, 18200000, 20, 0.0023},
	{21000000, 21450000, 20, 0.003},
	{24800000, 25000000, 20, 0.0034},
	{28000000, 29700000, 20, 0.0037}  
};

#define CMD_TX (2)
#define CMD_RX (3)
#define TUNING_SHIFT (0)
#define MDS_LEVEL (-135)

struct Queue qremote;

void radio_tune_to(u_int32_t f){
	if (rx_list->mode == MODE_CW)
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT - rx_pitch);
	else if (rx_list->mode == MODE_CWR)
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT + rx_pitch);
	else
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT);

//  printf("Setting radio rx_pitch %d\n", rx_pitch);
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
	memset(tx_list->fft_time, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(tx_list->fft_freq, 0, sizeof(fftw_complex) * MAX_BINS);
/*	for (int i= 0; i < MAX_BINS/2; i++){
		__real__ fft_m[i]  = 0.0;
		__imag__ fft_m[i]  = 0.0;
	}
*/
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
	//we are only using the lower half of the bins, 
	//so this copies twice as many bins, 
	//it can be optimized. leaving it here just in case 
	//someone wants to try I Q channels 
	//in hardware

	// this has been hand optimized to lower
	//the inordinate cpu usage
	for (int i = 1269; i < 1803; i++){

		fft_bins[i] = ((1.0 - spectrum_speed) * fft_bins[i]) + 
			(spectrum_speed * cabs(fft_spectrum[i]));

		int y = power2dB(cnrmf(fft_bins[i])); 
		spectrum_plot[i] = y;
	}
}

int remote_audio_output(int16_t *samples){
	int length = q_length(&qremote);
	for (int i = 0; i < length; i++){
		samples[i] = q_read(&qremote) / 32786;
	}
	return length;
}

static int prev_lpf = -1;
void set_lpf_40mhz(int frequency){
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
	if (frequency == freq_hdr)
		return;
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
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}
	else {
		r->agc_speed = 10;
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

void my_fftw_execute(fftw_plan f){
	fftw_execute(f);
}


//TODO : optimize the memory copy and moves to use the memcpy
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
	my_fftw_execute(plan_fwd);

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
	my_fftw_execute(plan_spectrum);

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
	my_fftw_execute(r->plan_rev);

	//STEP 8 : AGC
	agc2(r);
	
	//STEP 9: send the output back to where it needs to go
	int is_digital = 0;

	if (rx_list->output == 0){
		for (i= 0; i < MAX_BINS/2; i++){
			int32_t sample;
			sample = cimag(r->fft_time[i+(MAX_BINS/2)]);
			//keep transmit buffer empty
			output_speaker[i] = sample;
			output_tx[i] = 0;
		}

		//push the samples to the remote audio queue, decimated to 16000 samples/sec
		for (i = 0; i < MAX_BINS/2; i += 6)
			q_write(&qremote, output_speaker[i]);

	}

	if (mute_count){
		memset(output_speaker, 0, MAX_BINS/2 * sizeof(int32_t));
		mute_count--;
	}

	//push the data to any potential modem 
	modem_rx(rx_list->mode, output_speaker, MAX_BINS/2);
}

void read_power(){
	uint8_t response[4];
	int16_t vfwd, vref;
	char buff[20];

	if (!in_tx)
		return;
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return;

	vfwd = vref = 0;

	memcpy(&vfwd, response, 2);
	memcpy(&vref, response+2, 2);
//	printf("%d:%d\n", vfwd, vref);
	if (vref >= vfwd)
		vswr = 100;
	else
		vswr = (10*(vfwd + vref))/(vfwd-vref);

	//here '400' is the scaling factor as our ref power output is 40 watts
	//this calculates the power as 1/10th of a watt, 400 = 40 watts
	int fwdvoltage =  (vfwd * 40)/bridge_compensation;
	fwdpower = (fwdvoltage * fwdvoltage)/400;

	int rf_v_p2p = (fwdvoltage * 126)/400;
//	printf("rf volts: %d, alc %g, %d watts ", rf_v_p2p, alc_level, fwdpower/10);	
	if (rf_v_p2p > 135 && !in_calibration){
		alc_level *= 135.0 / (1.0 * rf_v_p2p);
		printf("ALC tripped, to %d percent\n", (int)(100 * alc_level));
	}
/*	else if (alc_level < 0.95){
		printf("alc releasing to ");
		alc_level *= 1.02;
	}
*/
//	printf("alc: %g\n", alc_level);
}

static int tx_process_restart = 0;

void tx_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i;
	double i_sample, q_sample;

	struct rx *r = tx_list;

	//fix the burst at the start of transmission
	if (tx_process_restart){
    fft_reset_m_bins();
		tx_process_restart = 0;
	} 

	if (mute_count && (r->mode == MODE_USB || r->mode == MODE_LSB)){
		memset(input_mic, 0, n_samples * sizeof(int32_t));
		mute_count--;
	}
	//first add the previous M samples
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	int m = 0;
	int j = 0;

	//double max = -10.0, min = 10.0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		if (r->mode == MODE_2TONE)
			i_sample = (1.0 * (vfo_read(&tone_a) 
										+ vfo_read(&tone_b))) / 50000000000.0;
		else if (r->mode == MODE_CALIBRATE)
			i_sample = (1.0 * (vfo_read(&tone_a))) / 25000000000.0;
		else if (r->mode == MODE_CW || r->mode == MODE_CWR || r->mode == MODE_FT8)
			i_sample = modem_next_sample(r->mode) / 3;
		else {
	  	i_sample = (1.0 * input_mic[j]) / 2000000000.0;
		}
		//clip the overdrive to prevent damage up the processing chain, PA
		if (r->mode == MODE_USB || r->mode == MODE_LSB){
			if (i_sample < (-1.0 * voice_clip_level))
				i_sample = -1.0 * voice_clip_level;
			else if (i_sample > voice_clip_level)
				i_sample = voice_clip_level;
		}
/*
		//to measure the voice peaks, used to measure voice_clip_level 
		if (max < i_sample)
			max = i_sample;
		if (min > i_sample)
			min = i_sample;
*/
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

	//push the samples to the remote audio queue, decimated to 16000 samples/sec
	for (i = 0; i < MAX_BINS/2; i += 6)
		q_write(&qremote, output_speaker[i]);

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
	int min = 10000000;
	int max = -10000000;
	float scale = volume;
	for (i= 0; i < MAX_BINS/2; i++){
		double s = creal(r->fft_time[i+(MAX_BINS/2)]);
		output_tx[i] = s * scale * tx_amp * alc_level;
		if (min > output_tx[i])
			min = output_tx[i];
		if (max < output_tx[i])
			max = output_tx[i];	
			//output_tx[i] = 0;
	}
//	printf("min %d, max %d\n", min, max);

	read_power();
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

void signal_handler(int signum){
	digitalWrite(TX_LINE, LOW);
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


static int hw_init_index = 0;
static int hw_settings_handler(void* user, const char* section, 
            const char* name, const char* value)
{
  char cmd[1000];
  char new_value[200];
		

	if (!strcmp(name, "f_start"))
		band_power[hw_init_index].f_start = atoi(value);
	if (!strcmp(name, "f_stop"))
		band_power[hw_init_index].f_stop = atoi(value);
	if (!strcmp(name, "scale"))
		band_power[hw_init_index++].scale = atof(value);

	if (!strcmp(name, "bfo_freq"))
		bfo_freq = atoi(value);
}

static void read_hw_ini(){
	hw_init_index = 0;
	char directory[PATH_MAX];
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/hw_settings.ini");
  if (ini_parse(directory, hw_settings_handler, NULL)<0){
    printf("Unable to load ~/sbitx/data/hw_settings.ini\nLoading default_hw_settings.ini instead\n");
		strcpy(directory, path);
		strcat(directory, "/sbitx/data/default_hw_settings.ini");
  	ini_parse(directory, hw_settings_handler, NULL);
  }
}

/*
	 the PA gain varies across the band from 3.5 MHz to 30 MHz
 	here we adjust the drive levels to keep it up, almost level
*/
void set_tx_power_levels(){
 // printf("Setting tx_power to %d, gain to %d\n", tx_power_watts, tx_gain);
	//int tx_power_gain = 0;

	//search for power in the approved bands
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		if (band_power[i].f_start <= freq_hdr && freq_hdr <= band_power[i].f_stop){
		
			//next we do a decimal coversion of the power reduction needed
			tx_amp = (1.0 * tx_drive * band_power[i].scale);  
		}	
	}
//	printf("tx_amp is set to %g for %d drive\n", tx_amp, tx_drive);
	//we keep the audio card output 'volume' constant'
	sound_mixer(audio_card, "Master", 95);
	sound_mixer(audio_card, "Capture", tx_gain);
	alc_level = 1.0;
}

/* calibrate power on all bands */

void calibrate_band_power(struct power_settings *b){
	
	set_rx1(b->f_start + 35000);
	printf("*calibrating for %d\n", freq_hdr);
	tx_list->mode = MODE_CALIBRATE;
	tx_drive = 100;

	int i, j;

//	double scale_delta = b->scale / 25;
	double scaling_factor = 0.0001;
	b->scale = scaling_factor;
 	set_tx_power_levels();
	delay(50);

	tr_switch(1);
	delay(100);

	for (i = 0; i < 200 && b->scale < 0.015; i++){
		scaling_factor *= 1.1;
		b->scale = scaling_factor;
 		set_tx_power_levels();
		delay(50); //let the new power levels take hold		

		int avg = 0;
		//take many readings to get a peak
		for (j = 0; j < 10; j++){
			delay(20);
			avg += fwdpower /10; //fwdpower in 1/10th of a watt
//			printf("  avg %d, fwd %d scale %g\n", avg, fwdpower, b->scale);
		}
		avg /= 10;
		printf("*%d, f %d : avg %d, max = %d\n", i, b->f_start, avg, b->max_watts);
		if (avg >= b->max_watts)
				break;
	}
	tr_switch(0);
	printf("*tx scale for %d is set to %g\n", b->f_start, b->scale);
	delay(100);	
}

static void save_hw_settings(){
	static int last_save_at = 0;
	char file_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	char *path = getenv("HOME");
	strcpy(file_path, path);
	strcat(file_path, "/sbitx/data/hw_settings.ini");

	FILE *f = fopen(file_path, "w");
	if (!f){
		printf("Unable to save %s : %s\n", file_path, strerror(errno));
		return;
	}

	fprintf(f, "bfo_freq=%d\n\n", bfo_freq);
	//now save the band stack
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		fprintf(f, "[tx_band]\nf_start=%d\nf_stop=%d\nscale=%g\n\n", 
			band_power[i].f_start, band_power[i].f_stop, band_power[i].scale);
	}

	fclose(f);
}

pthread_t calibration_thread;

void *calibration_thread_function(void *server){

	int old_freq = freq_hdr;
	int old_mode = tx_list->mode;
	int	old_tx_drive = tx_drive;

	in_calibration = 1;
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		calibrate_band_power(band_power + i);
	}
	in_calibration = 0;

	set_rx1(old_freq);
	tx_list->mode = old_mode;
	tx_drive = old_tx_drive;
	save_hw_settings();
	printf("*Finished band power calibrations\n");
}

void tx_cal(){
	printf("*Starting tx calibration, with dummy load connected\n");
 	pthread_create( &calibration_thread, NULL, calibration_thread_function, 
		(void*)NULL);
}

void tr_switch_de(int tx_on){
		if (tx_on){
			//mute it all and hang on for a millisecond
			sound_mixer(audio_card, "Master", 0);
			sound_mixer(audio_card, "Capture", 0);
			delay(1);

			//now switch of the signal back
			//now ramp up after 5 msecs
			delay(2);
			digitalWrite(TX_LINE, HIGH);
			mute_count = 20;
			tx_process_restart = 1;
			//give time for the reed relay to switch
      delay(2);
			set_tx_power_levels();
			in_tx = 1;
			//finally ramp up the power 
			if (tr_relay){
				set_lpf_40mhz(freq_hdr);
				delay(10); //debounce the lpf relays
			}
			digitalWrite(TX_POWER, HIGH);
			spectrum_reset();
		}
		else {
			in_tx = 0;
			//mute it all and hang on
			sound_mixer(audio_card, "Master", 0);
			sound_mixer(audio_card, "Capture", 0);
			delay(1);
      fft_reset_m_bins();
			mute_count = MUTE_MAX;

			//power down the PA chain to null any gain
			digitalWrite(TX_POWER, LOW);
			delay(2);

			if (tr_relay){
  			digitalWrite(LPF_A, LOW);
  			digitalWrite(LPF_B, LOW);
 	 			digitalWrite(LPF_C, LOW);
  			digitalWrite(LPF_D, LOW);
			}
			delay(10);

			//drive the tx line low, switching the signal path 
			digitalWrite(TX_LINE, LOW);
			delay(5); 
			//audio codec is back on
			sound_mixer(audio_card, "Master", rx_vol);
			sound_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
			//rx_tx_ramp = 10;
		}
}

//v2 t/r switch uses the lpfs to cut the feedback during t/r transitions
void tr_switch_v2(int tx_on){
		if (tx_on){

			//first turn off the LPFs, so PA doesnt connect 
  		digitalWrite(LPF_A, LOW);
  		digitalWrite(LPF_B, LOW);
 	 		digitalWrite(LPF_C, LOW);
  		digitalWrite(LPF_D, LOW);

			//mute it all and hang on for a millisecond
			sound_mixer(audio_card, "Master", 0);
			sound_mixer(audio_card, "Capture", 0);
			delay(1);

			//now switch of the signal back
			//now ramp up after 5 msecs
			delay(2);
			mute_count = 20;
			tx_process_restart = 1;
			digitalWrite(TX_LINE, HIGH);
      delay(20);
			set_tx_power_levels();
			in_tx = 1;
			prev_lpf = -1; //force this
			set_lpf_40mhz(freq_hdr);
			delay(10);
			spectrum_reset();
		}
		else {
			in_tx = 0;
			//mute it all and hang on
			sound_mixer(audio_card, "Master", 0);
			sound_mixer(audio_card, "Capture", 0);
			delay(1);
      fft_reset_m_bins();
			mute_count = MUTE_MAX;

  		digitalWrite(LPF_A, LOW);
  		digitalWrite(LPF_B, LOW);
 	 		digitalWrite(LPF_C, LOW);
  		digitalWrite(LPF_D, LOW);
			prev_lpf = -1; //force the lpf to be re-energized
			delay(10);
			//power down the PA chain to null any gain
			digitalWrite(TX_LINE, LOW);
			delay(5); 
			//audio codec is back on
			sound_mixer(audio_card, "Master", rx_vol);
			sound_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
			prev_lpf = -1;
			set_lpf_40mhz(freq_hdr);
			//rx_tx_ramp = 10;
		}
}

void tr_switch(int tx_on){
	if (sbitx_version == SBITX_DE)
		tr_switch_de(tx_on);
	else
		tr_switch_v2(tx_on);
}

/* 
This is the one-time initialization code 
*/
void setup(){

	read_hw_ini();

	//setup the LPF and the gpio pins
	pinMode(TX_LINE, OUTPUT);
	pinMode(TX_POWER, OUTPUT);
	pinMode(LPF_A, OUTPUT);
	pinMode(LPF_B, OUTPUT);
	pinMode(LPF_C, OUTPUT);
	pinMode(LPF_D, OUTPUT);
  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);
	digitalWrite(TX_LINE, LOW);
	digitalWrite(TX_POWER, LOW);

	fft_init();
	vfo_init_phase_table();
  setup_oscillators();
	q_init(&qremote, 8000);

	modem_init();

	add_rx(7000000, MODE_LSB, -3000, -300);
	add_tx(7000000, MODE_LSB, -3000, -300);
	rx_list->tuned_bin = 512;
  tx_list->tuned_bin = 512;
	tx_init(7000000, MODE_LSB, -3000, -150);

	//detect the version of sbitx
	uint8_t response[4];
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		sbitx_version = SBITX_DE;
	else
		sbitx_version = SBITX_V2;

	setup_audio_codec();
	sound_thread_start("plughw:0,0");

	sleep(1); //why? to allow the aloop to initialize?

	vfo_start(&tone_a, 700, 0);
	vfo_start(&tone_b, 1900, 0);

	delay(2000);	

}

void sdr_request(char *request, char *response){
	char cmd[100], value[1000];

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
	
		//we need to nudge the oscillator to adjust 
		//to cw offset. setting it to the already tuned freq
		//doesnt recalculte the offsets

		int f = freq_hdr;
		set_rx1(f-10);
		set_rx1(f);
	
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
		if (!strcmp(value, "on"))
			tr_switch_v2(1);
		else
			tr_switch_v2(0);
		strcpy(response, "ok");
	}
	else if (!strcmp(cmd, "rx_pitch")){
		rx_pitch = atoi(value);
	}
	else if (!strcmp(cmd, "tx_gain")){
		tx_gain = atoi(value);
		if(in_tx)
			set_tx_power_levels();
	}
	else if (!strcmp(cmd, "tx_power")){
    tx_drive = atoi(value);
		if(in_tx)
			set_tx_power_levels();	
	}
	else if (!strcmp(cmd, "bridge")){
    bridge_compensation = atoi(value);
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
	else if (!strcmp(cmd, "txcal"))
		tx_cal();
	else if (!strcmp(cmd, "tx_compress"))
		tx_compress = atoi(value); 
  /* else
		printf("*Error request[%s] not accepted\n", request); */
}


