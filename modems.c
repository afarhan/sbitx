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
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>

#include "sdr.h"
#include "sdr_ui.h"

typedef float float32_t;
/*
	This file implements modems for :
	1. PSK31
	2. RTTY
	3. FT8

	FT8 is not really a modem, we use a slightly modified version of
	the decode_ft8 program written by Karlis Goba at https://github.com/kgoba/ft8_lib.

	The PSK31 and RTTY are courtesy UHSDR code. 
*/

/*******************************************************
**********                  FT8                  *******
********************************************************/


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

		puts("Decoding FT8 on cmd line");
		//create a temporary file of the ft8 samples
		FILE *pf = fopen("/tmp/ftrx.raw", "w");
		fwrite(ft8_rx_buff, sizeof(ft8_rx_buff), 1, pf);
		fclose(pf);

		//let the next batch begin
		ft8_do_decode = 0;
		ft8_rx_buff_index = 0;

		
		//now launch the decoder
		pf = popen("/home/pi/ft8_lib/decode_ft8 /tmp/ftrx.raw", "r");
		while(fgets(buff, sizeof(buff), pf)) {
			write_log(buff);
			printf("FT8 decode : %s\n", buff);
		}
		puts("FT decoding ended");	
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


/*******************************************************
**********             SoftDDS library           *******
********** (To be replaced by vfo.c routines)    *******
********************************************************/


#define DDS_TBL_BITS        10
#define DDS_TBL_SIZE        (1 << DDS_TBL_BITS) // 10 = 1024

// this table represents 2*PI, i.e. a full sine wave
const int16_t DDS_TABLE[DDS_TBL_SIZE] =
{

    // 1024 points
    0,
    201, 402, 603, 804, 1005, 1206, 1406, 1607, 1808, 2009, 2209, 2410, 2610, 2811, 3011,
    3211, 3411, 3611, 3811, 4011, 4210, 4409, 4608, 4807, 5006, 5205, 5403, 5601, 5799, 5997, 6195,
    6392, 6589, 6786, 6982, 7179, 7375, 7571, 7766, 7961, 8156, 8351, 8545, 8739, 8932, 9126, 9319,
    9511, 9703, 9895, 10087, 10278, 10469, 10659, 10849, 11038, 11227, 11416, 11604, 11792, 11980, 12166, 12353,
    12539, 12724, 12909, 13094, 13278, 13462, 13645, 13827, 14009, 14191, 14372, 14552, 14732, 14911, 15090, 15268,
    15446, 15623, 15799, 15975, 16150, 16325, 16499, 16672, 16845, 17017, 17189, 17360, 17530, 17699, 17868, 18036,
    18204, 18371, 18537, 18702, 18867, 19031, 19194, 19357, 19519, 19680, 19840, 20000, 20159, 20317, 20474, 20631,
    20787, 20942, 21096, 21249, 21402, 21554, 21705, 21855, 22004, 22153, 22301, 22448, 22594, 22739, 22883, 23027,
    23169, 23311, 23452, 23592, 23731, 23869, 24006, 24143, 24278, 24413, 24546, 24679, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25707, 25831, 25954, 26077, 26198, 26318, 26437, 26556, 26673, 26789, 26905, 27019, 27132,
    27244, 27355, 27466, 27575, 27683, 27790, 27896, 28001, 28105, 28208, 28309, 28410, 28510, 28608, 28706, 28802,
    28897, 28992, 29085, 29177, 29268, 29358, 29446, 29534, 29621, 29706, 29790, 29873, 29955, 30036, 30116, 30195,
    30272, 30349, 30424, 30498, 30571, 30643, 30713, 30783, 30851, 30918, 30984, 31049, 31113, 31175, 31236, 31297,
    31356, 31413, 31470, 31525, 31580, 31633, 31684, 31735, 31785, 31833, 31880, 31926, 31970, 32014, 32056, 32097,
    32137, 32176, 32213, 32249, 32284, 32318, 32350, 32382, 32412, 32441, 32468, 32495, 32520, 32544, 32567, 32588,
    32609, 32628, 32646, 32662, 32678, 32692, 32705, 32717, 32727, 32736, 32744, 32751, 32757, 32761, 32764, 32766,
    32766, 32766, 32764, 32761, 32757, 32751, 32744, 32736, 32727, 32717, 32705, 32692, 32678, 32662, 32646, 32628,
    32609, 32588, 32567, 32544, 32520, 32495, 32468, 32441, 32412, 32382, 32350, 32318, 32284, 32249, 32213, 32176,
    32137, 32097, 32056, 32014, 31970, 31926, 31880, 31833, 31785, 31735, 31684, 31633, 31580, 31525, 31470, 31413,
    31356, 31297, 31236, 31175, 31113, 31049, 30984, 30918, 30851, 30783, 30713, 30643, 30571, 30498, 30424, 30349,
    30272, 30195, 30116, 30036, 29955, 29873, 29790, 29706, 29621, 29534, 29446, 29358, 29268, 29177, 29085, 28992,
    28897, 28802, 28706, 28608, 28510, 28410, 28309, 28208, 28105, 28001, 27896, 27790, 27683, 27575, 27466, 27355,
    27244, 27132, 27019, 26905, 26789, 26673, 26556, 26437, 26318, 26198, 26077, 25954, 25831, 25707, 25582, 25456,
    25329, 25201, 25072, 24942, 24811, 24679, 24546, 24413, 24278, 24143, 24006, 23869, 23731, 23592, 23452, 23311,
    23169, 23027, 22883, 22739, 22594, 22448, 22301, 22153, 22004, 21855, 21705, 21554, 21402, 21249, 21096, 20942,
    20787, 20631, 20474, 20317, 20159, 20000, 19840, 19680, 19519, 19357, 19194, 19031, 18867, 18702, 18537, 18371,
    18204, 18036, 17868, 17699, 17530, 17360, 17189, 17017, 16845, 16672, 16499, 16325, 16150, 15975, 15799, 15623,
    15446, 15268, 15090, 14911, 14732, 14552, 14372, 14191, 14009, 13827, 13645, 13462, 13278, 13094, 12909, 12724,
    12539, 12353, 12166, 11980, 11792, 11604, 11416, 11227, 11038, 10849, 10659, 10469, 10278, 10087, 9895, 9703,
    9511, 9319, 9126, 8932, 8739, 8545, 8351, 8156, 7961, 7766, 7571, 7375, 7179, 6982, 6786, 6589,
    6392, 6195, 5997, 5799, 5601, 5403, 5205, 5006, 4807, 4608, 4409, 4210, 4011, 3811, 3611, 3411,
    3211, 3011, 2811, 2610, 2410, 2209, 2009, 1808, 1607, 1406, 1206, 1005, 804, 603, 402, 201,
    0,
    -201, -402, -603, -804, -1005, -1206, -1406, -1607, -1808, -2009, -2209, -2410, -2610, -2811, -3011,
    -3211, -3411, -3611, -3811, -4011, -4210, -4409, -4608, -4807, -5006, -5205, -5403, -5601, -5799, -5997, -6195,
    -6392, -6589, -6786, -6982, -7179, -7375, -7571, -7766, -7961, -8156, -8351, -8545, -8739, -8932, -9126, -9319,
    -9511, -9703, -9895, -10087, -10278, -10469, -10659, -10849, -11038, -11227, -11416, -11604, -11792, -11980, -12166, -12353,
    -12539, -12724, -12909, -13094, -13278, -13462, -13645, -13827, -14009, -14191, -14372, -14552, -14732, -14911, -15090, -15268,
    -15446, -15623, -15799, -15975, -16150, -16325, -16499, -16672, -16845, -17017, -17189, -17360, -17530, -17699, -17868, -18036,
    -18204, -18371, -18537, -18702, -18867, -19031, -19194, -19357, -19519, -19680, -19840, -20000, -20159, -20317, -20474, -20631,
    -20787, -20942, -21096, -21249, -21402, -21554, -21705, -21855, -22004, -22153, -22301, -22448, -22594, -22739, -22883, -23027,
    -23169, -23311, -23452, -23592, -23731, -23869, -24006, -24143, -24278, -24413, -24546, -24679, -24811, -24942, -25072, -25201,
    -25329, -25456, -25582, -25707, -25831, -25954, -26077, -26198, -26318, -26437, -26556, -26673, -26789, -26905, -27019, -27132,
    -27244, -27355, -27466, -27575, -27683, -27790, -27896, -28001, -28105, -28208, -28309, -28410, -28510, -28608, -28706, -28802,
    -28897, -28992, -29085, -29177, -29268, -29358, -29446, -29534, -29621, -29706, -29790, -29873, -29955, -30036, -30116, -30195,
    -30272, -30349, -30424, -30498, -30571, -30643, -30713, -30783, -30851, -30918, -30984, -31049, -31113, -31175, -31236, -31297,
    -31356, -31413, -31470, -31525, -31580, -31633, -31684, -31735, -31785, -31833, -31880, -31926, -31970, -32014, -32056, -32097,
    -32137, -32176, -32213, -32249, -32284, -32318, -32350, -32382, -32412, -32441, -32468, -32495, -32520, -32544, -32567, -32588,
    -32609, -32628, -32646, -32662, -32678, -32692, -32705, -32717, -32727, -32736, -32744, -32751, -32757, -32761, -32764, -32766,
    -32766, -32766, -32764, -32761, -32757, -32751, -32744, -32736, -32727, -32717, -32705, -32692, -32678, -32662, -32646, -32628,
    -32609, -32588, -32567, -32544, -32520, -32495, -32468, -32441, -32412, -32382, -32350, -32318, -32284, -32249, -32213, -32176,
    -32137, -32097, -32056, -32014, -31970, -31926, -31880, -31833, -31785, -31735, -31684, -31633, -31580, -31525, -31470, -31413,
    -31356, -31297, -31236, -31175, -31113, -31049, -30984, -30918, -30851, -30783, -30713, -30643, -30571, -30498, -30424, -30349,
    -30272, -30195, -30116, -30036, -29955, -29873, -29790, -29706, -29621, -29534, -29446, -29358, -29268, -29177, -29085, -28992,
    -28897, -28802, -28706, -28608, -28510, -28410, -28309, -28208, -28105, -28001, -27896, -27790, -27683, -27575, -27466, -27355,
    -27244, -27132, -27019, -26905, -26789, -26673, -26556, -26437, -26318, -26198, -26077, -25954, -25831, -25707, -25582, -25456,
    -25329, -25201, -25072, -24942, -24811, -24679, -24546, -24413, -24278, -24143, -24006, -23869, -23731, -23592, -23452, -23311,
    -23169, -23027, -22883, -22739, -22594, -22448, -22301, -22153, -22004, -21855, -21705, -21554, -21402, -21249, -21096, -20942,
    -20787, -20631, -20474, -20317, -20159, -20000, -19840, -19680, -19519, -19357, -19194, -19031, -18867, -18702, -18537, -18371,
    -18204, -18036, -17868, -17699, -17530, -17360, -17189, -17017, -16845, -16672, -16499, -16325, -16150, -15975, -15799, -15623,
    -15446, -15268, -15090, -14911, -14732, -14552, -14372, -14191, -14009, -13827, -13645, -13462, -13278, -13094, -12909, -12724,
    -12539, -12353, -12166, -11980, -11792, -11604, -11416, -11227, -11038, -10849, -10659, -10469, -10278, -10087, -9895, -9703,
    -9511, -9319, -9126, -8932, -8739, -8545, -8351, -8156, -7961, -7766, -7571, -7375, -7179, -6982, -6786, -6589,
    -6392, -6195, -5997, -5799, -5601, -5403, -5205, -5006, -4807, -4608, -4409, -4210, -4011, -3811, -3611, -3411,
    -3211, -3011, -2811, -2610, -2410, -2209, -2009, -1808, -1607, -1406, -1206, -1005, -804, -603, -402, -201
};
#define SOFTDDS_ACC_SHIFT       (32-DDS_TBL_BITS)

// Soft DDS public structure
typedef struct
{
	// DDS accumulator
	uint32_t   acc;

	// DDS step - not working if part of the structure
	uint32_t   step;
} soft_dds_t;


/**
 * Execute a single step in the sinus generation
 */
uint32_t softdds_nextSampleIndex(soft_dds_t* dds);

/*
 * Get the index which represents a -90 degree shift compared to
 * k, i.e. get  k = sin(a) => cos(a)
 */
uint32_t softdds_phase_shift90(uint32_t k);

/**
 * Execute a single step in the sinus generation and return actual sample value
 */
int16_t softdds_nextSample(soft_dds_t* dds);


void softdds_setFreqDDS(soft_dds_t* dds, float32_t freq, uint32_t samp_rate, uint8_t smooth);
void softdds_genIQSingleTone(soft_dds_t* dds, float32_t *i_buff,float32_t *q_buff,uint16_t size);
void softdds_genIQTwoTone(soft_dds_t* ddsA, soft_dds_t* ddsB, float *i_buff,float *q_buff,ushort size);

void softdds_addSingleTone(soft_dds_t* dds_ptr, float32_t* buffer, const size_t blockSize, float32_t scaling);
void softdds_addSingleToneToTwobuffers(soft_dds_t* dds_ptr, float32_t* buffer1, float32_t* buffer2, const size_t blockSize, float32_t scaling);

void softdds_runIQ(float32_t *i_buff, float32_t *q_buff, uint16_t size);
void softdds_configRunIQ(float32_t freq[2],uint32_t samp_rate,uint8_t smooth);


/**
 * Execute a single step in the sinus generation
 */
uint32_t softdds_nextSampleIndex(soft_dds_t* dds)
{
    uint32_t retval = (dds->acc >> SOFTDDS_ACC_SHIFT)%DDS_TBL_SIZE;

	dds->acc += dds->step;

	// now scale down precision and  make sure that
	// index wraps around properly
	return retval;
}

/*
 * Get the index which represents a -90 degree shift compared to
 * k, i.e. get  k = sin(a) => cos(a)
 */
uint32_t softdds_phase_shift90(uint32_t k)
{
    // make sure that
    // index wraps around properly
    return (k+(3*DDS_TBL_SIZE/4))%DDS_TBL_SIZE;
}


/**
 * Execute a single step in the sinus generation and return actual sample value
 */
int16_t softdds_nextSample(soft_dds_t* dds)
{
	return DDS_TABLE[softdds_nextSampleIndex(dds)];
}

// Two Tone Dds
soft_dds_t dbldds[2];

uint32_t softdds_stepForSampleRate(float32_t freq, uint32_t samp_rate)
{
    uint64_t freq64_shifted = freq * DDS_TBL_SIZE;
    freq64_shifted <<= SOFTDDS_ACC_SHIFT;
    uint64_t step = freq64_shifted / samp_rate;
    return step;
}

/**
 * Initialize softdds for given frequency and sample rate
 */
void softdds_setFreqDDS(soft_dds_t* softdds_p, float32_t freq, uint32_t samp_rate,uint8_t smooth)
{
    // Reset accumulator, if need smooth tone
    // transition, do not reset it (e.g. wspr)
    if(smooth == false)
    {
        softdds_p->acc = 0;
    }
    // Calculate new step
    softdds_p->step = softdds_stepForSampleRate(freq,samp_rate);
}


/**
 * Initialize softdds for given frequency and sample rate
 */

void softdds_configRunIQ(float freq[2],uint32_t samp_rate,uint8_t smooth)
{
    softdds_setFreqDDS(&dbldds[0],freq[0],samp_rate,smooth);
    softdds_setFreqDDS(&dbldds[1],freq[1],samp_rate,smooth);
  }

void softdds_genIQSingleTone(soft_dds_t* dds, float32_t *i_buff,float32_t *q_buff,uint16_t size)
{
	for(uint16_t i = 0; i < size; i++)
	{
		// Calculate next sample
		uint32_t k    = softdds_nextSampleIndex(dds);

		// Load I value (sin)
		*i_buff = DDS_TABLE[k];

		// -90 degrees shift (cos)
		// Load Q value
		*q_buff = DDS_TABLE[softdds_phase_shift90(k)];

		// Next ptr
		i_buff++;
		q_buff++;
	}
}

/*
 * Generates the addition  of two sinus frequencies as IQ data stream
 * min/max value is +/-2^15-1
 * Frequencies need to be configured using softdds_setfreq_dbl
 */
void softdds_genIQTwoTone(soft_dds_t* ddsA, soft_dds_t* ddsB, float *i_buff,float *q_buff,ushort size)
{
    for(int i = 0; i < size; i++)
    {
        uint32_t k[2];
        // Calculate next sample
        k[0]    = softdds_nextSampleIndex(ddsA);
        k[1]    = softdds_nextSampleIndex(ddsB);

        // Load I value 0.5*(sin(a)+sin(b))
        *i_buff = ((int32_t)DDS_TABLE[k[0]] + (int32_t)DDS_TABLE[k[1]])/2;

        *q_buff = ((int32_t)DDS_TABLE[softdds_phase_shift90(k[0])] + (int32_t)DDS_TABLE[softdds_phase_shift90(k[1])])/2;

        // Next ptr
        i_buff++;
        q_buff++;
    }
}

/**
 * Overlays an audio stream with a beep signal
 * @param dds The previously initialized dds configuration
 * @param buffer audio buffer of blockSize (mono/single channel) samples
 * @param blockSize
 * @param scaling scale the resulting sine wave with this factor
 */

void softdds_addSingleTone(soft_dds_t* dds_ptr, float32_t* buffer, const size_t blockSize, float32_t scaling)
{
    for(int i=0; i < blockSize; i++)                            // transfer to DMA buffer and do conversion to INT
    {
        buffer[i] += (float32_t)softdds_nextSample(dds_ptr) * scaling; // load indexed sine wave value, adding it to audio, scaling the amplitude and putting it on "b" - speaker (ONLY)
    }
}

/**
 * Overlays an audio stream with a beep signal
 * @param dds The previously initialized dds configuration
 * @param buffer1 audio buffer of blockSize (mono/single channel) samples
 * @param buffer2 audio buffer of blockSize (mono/single channel) samples
 * @param blockSize
 * @param scaling scale the resulting sine wave with this factor
 */

void softdds_addSingleToneToTwobuffers(soft_dds_t* dds_ptr, float32_t* buffer1, float32_t* buffer2, const size_t blockSize, float32_t scaling)
{
    float32_t Tone;
    for(int i=0; i < blockSize; i++)                            // transfer to DMA buffer and do conversion to INT
    {
        Tone=(float32_t) softdds_nextSample(dds_ptr) * scaling; // load indexed sine wave value, adding it to audio, scaling the amplitude and putting it on "b" - speaker (ONLY)
        buffer1[i] += Tone;
        buffer2[i] += Tone;
    }
}

/*
 * Generates the sinus frequencies as IQ data stream
 * min/max value is +/-2^15-1
 * Frequency needs to be configured using softdds_setfreq
 */
void softdds_runIQ(float32_t *i_buff,float32_t *q_buff,uint16_t size)
{
    if (dbldds[1].step>0.0)
    {
        softdds_genIQTwoTone(&dbldds[0], &dbldds[1], i_buff, q_buff,size);
    }
    else
    {
    	softdds_genIQSingleTone(&dbldds[0],i_buff,q_buff,size);
    }
}


/*******************************************************
**********             RTTY routine              *******
********************************************************/

typedef float float32_t;

typedef enum {
    RTTY_STOP_1 = 0,
    RTTY_STOP_1_5,
    RTTY_STOP_2
} rtty_stop_t;


typedef struct
{
    float32_t speed;
    rtty_stop_t stopbits;
    uint16_t shift;
    float32_t samplerate;
} rtty_mode_config_t;



typedef enum {
    RTTY_SPEED_45,
    RTTY_SPEED_50,
    RTTY_SPEED_NUM
} rtty_speed_t;

typedef enum {
    RTTY_SHIFT_85,
    RTTY_SHIFT_170,
	RTTY_SHIFT_200,
    RTTY_SHIFT_425,
	RTTY_SHIFT_450,
	RTTY_SHIFT_850,
    RTTY_SHIFT_NUM
} rtty_shift_t;

typedef struct
{
    rtty_speed_t id;
    float32_t value;
    char* label;
} rtty_speed_item_t;

// TODO: Probably we should define just a few for the various value types and let
// the id be an uint32_t
typedef struct
{
    rtty_shift_t id;
    uint32_t value;
    char* label;
} rtty_shift_item_t;


extern const rtty_speed_item_t rtty_speeds[RTTY_SPEED_NUM];
extern const rtty_shift_item_t rtty_shifts[RTTY_SHIFT_NUM];
extern float32_t decayavg(float32_t average, float32_t input, int weight);

// TODO: maybe this should be placed in the ui or radio management part
typedef struct
{
    rtty_shift_t shift_idx;
    rtty_speed_t speed_idx;
    rtty_stop_t stopbits_idx;
    bool atc_disable; // should the automatic level control be turned off?
}  rtty_ctrl_t;

extern rtty_ctrl_t rtty_ctrl_config;
void Rtty_Modem_Init(uint32_t output_sample_rate);
void Rtty_Demodulator_ProcessSample(float32_t sample);
int16_t Rtty_Modulator_GenSample(void);
FILE *pf = NULL; //holds samples to read or to write

char tx_buff[256]; // tx buffer of what to send
char *tx_next = NULL;

// character tables borrowed from fldigi / rtty.cxx
static const char RTTYLetters[] = {
    '\0',   'E',    '\n',   'A',    ' ',    'S',    'I',    'U',
    '\r',   'D',    'R',    'J',    'N',    'F',    'C',    'K',
    'T',    'Z',    'L',    'W',    'H',    'Y',    'P',    'Q',
    'O',    'B',    'G',    ' ',    'M',    'X',    'V',    ' '
};

static const char RTTYSymbols[32] = {
    '\0',   '3',    '\n',   '-',    ' ',    '\a',   '8',    '7',
    '\r',   '$',    '4',    '\'',   ',',    '!',    ':',    '(',
    '5',    '"',    ')',    '2',    '#',    '6',    '0',    '1',
    '9',    '?',    '&',    ' ',    '.',    '/',    ';',    ' '
};


// bits 0-4 -> baudot, bit 5 1 == LETTER, 0 == NUMBER/FIGURE
const uint8_t Ascii2Baudot[128] =
{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0b001011, //	BEL	N
		0,
		0,
		0b000010, //	\n	NL
		0,
		0,
		0b001000, //	\r	NL
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0b100100, // 	 	NL
		0b001101, //	!   N
		0b010001, //	"   N
		0b010100, //	#   N
		0b001001, //    $   N
		0, 		  //	%
		0b011010, //	&   N
		0b000101, //	'	N
		0b001111, //	(	N
		0b010010, //	)	N
		0, 		  //	*
		0, 		  //	+	
		0b001100, //	,	N
		0b000011, //	-	N
		0b011100, //	.	N
		0b011101, //	/	N
		0b010110, //	0	N
		0b010111, //	1	N
		0b010011, //	2	N
		0b000001, //	3	N
		0b001010, //	4	N
		0b010000, //	5	N
		0b010101, //	6	N
		0b000111, //	7	N
		0b000110, //	8	N
		0b011000, //	9	N
		0b001110, //	:	N
		0b011110, //	;   N
		0, 		  //	<
		0,        //	=
		0,        //	>
		0b011001, //	?	N
		0,        //	@
		0b100011, //	A	L
		0b111001, //	B	L
		0b101110, //	C	L
		0b101001, //	D	L
		0b100001, //	E	L
		0b101101, //	F	L
		0b111010, //	G	L
		0b110100, //	H	L
		0b100110, //	I	L
		0b101011, //	J	L
		0b101111, //	K	L
		0b110010, //	L	L
		0b111100, //	M	L
		0b101100, //	N	L
		0b111000, //	O	L
		0b110110, //	P	L
		0b110111, //	Q	L
		0b101010, //	R	L
		0b100101, //	S	L
		0b110000, //	T	L
		0b100111, //	U	L
		0b111110, //	V	L
		0b110011, //	W	L
		0b111101, //	X	L
		0b110101, //	Y	L
		0b110001, //	Z	L
		0,
		0,
		0,
		0,
		0,
		0,
		0b100011, //	A	L
		0b111001, //	B	L
		0b101110, //	C	L
		0b101001, //	D	L
		0b100001, //	E	L
		0b101101, //	F	L
		0b111010, //	G	L
		0b110100, //	H	L
		0b100110, //	I	L
		0b101011, //	J	L
		0b101111, //	K	L
		0b110010, //	L	L
		0b111100, //	M	L
		0b101100, //	N	L
		0b111000, //	O	L
		0b110110, //	P	L
		0b110111, //	Q	L
		0b101010, //	R	L
		0b100101, //	S	L
		0b110000, //	T	L
		0b100111, //	U	L
		0b111110, //	V	L
		0b110011, //	W	L
		0b111101, //	X	L
		0b110101, //	Y	L
		0b110001, //	Z	L
		0,
		0,
		0,
		0,
		0,
};

#define RTTY_SYMBOL_CODE (0b11011)
#define RTTY_LETTER_CODE (0b11111)

// RTTY Experiment based on code from the DSP Tutorial at http://dp.nonoo.hu/projects/ham-dsp-tutorial/18-rtty-decoder-using-iir-filters/
// Used with permission from Norbert Varga, HA2NON under GPLv3 license

/*
 * Experimental Code
 */

const rtty_speed_item_t rtty_speeds[RTTY_SPEED_NUM] =
{
		{ .id =RTTY_SPEED_45, .value = 45.45, .label = "45" },
		{ .id =RTTY_SPEED_50, .value = 50, .label = "50"  },
};

const rtty_shift_item_t rtty_shifts[RTTY_SHIFT_NUM] =
{
		{ RTTY_SHIFT_85, 85, " 85" },
		{ RTTY_SHIFT_170, 170, "170" },
		{ RTTY_SHIFT_200, 200, "200" },
		{ RTTY_SHIFT_425, 425, "425" },
		{ RTTY_SHIFT_450, 450, "450" },
		{ RTTY_SHIFT_850, 850, "850" },
};

rtty_ctrl_t rtty_ctrl_config =
{
		.shift_idx = RTTY_SHIFT_170,
		.speed_idx = RTTY_SPEED_45,
		.stopbits_idx = RTTY_STOP_1_5
};



typedef struct
{
	float32_t gain;
	float32_t coeffs[4];
	uint16_t freq; // center freq
} rtty_bpf_config_t;

typedef struct
{
	float32_t gain;
	float32_t coeffs[2];
} rtty_lpf_config_t;

typedef struct
{
	float32_t xv[5];
	float32_t yv[5];
} rtty_bpf_data_t;

typedef struct
{
	float32_t xv[3];
	float32_t yv[3];
} rtty_lpf_data_t;



static float32_t RttyDecoder_bandPassFreq(float32_t sampleIn, const rtty_bpf_config_t* coeffs, rtty_bpf_data_t* data) {
	//shift the x data down the array
	data->xv[0] = data->xv[1]; 
	data->xv[1] = data->xv[2]; 
	data->xv[2] = data->xv[3]; 
	data->xv[3] = data->xv[4];

	//shif the y data down the array, placing sample at 4th position
	data->xv[4] = sampleIn / coeffs->gain; // gain at centre
	data->yv[0] = data->yv[1]; 
	data->yv[1] = data->yv[2]; 
	data->yv[2] = data->yv[3]; 
	data->yv[3] = data->yv[4];
	//calcualte the IIR
	data->yv[4] = (data->xv[0] + data->xv[4]) - 2 * data->xv[2]
							+ (coeffs->coeffs[0] * data->yv[0]) 
							+ (coeffs->coeffs[1] * data->yv[1])
							+ (coeffs->coeffs[2] * data->yv[2]) 
							+ (coeffs->coeffs[3] * data->yv[3]);
	return data->yv[4];
}

static float32_t RttyDecoder_lowPass(float32_t sampleIn, const rtty_lpf_config_t* coeffs, rtty_lpf_data_t* data) {
	data->xv[0] = data->xv[1]; data->xv[1] = data->xv[2];
	data->xv[2] = sampleIn / coeffs->gain; // gain at DC
	data->yv[0] = data->yv[1]; data->yv[1] = data->yv[2];
	data->yv[2] = (data->xv[0] + data->xv[2]) + 2 * data->xv[1]
								+ (coeffs->coeffs[0] * data->yv[0]) 
								+ (coeffs->coeffs[1] * data->yv[1]);
	return data->yv[2];
}

typedef enum {
	RTTY_RUN_STATE_WAIT_START = 0,
	RTTY_RUN_STATE_BIT,
} rtty_run_state_t;


typedef enum {
	RTTY_MODE_LETTERS = 0,
	RTTY_MODE_SYMBOLS
} rtty_charSetMode_t;




typedef struct {
	rtty_bpf_data_t bpfSpaceData; //state data for the filters
	rtty_bpf_data_t bpfMarkData;
	rtty_lpf_data_t lpfData;
	rtty_bpf_config_t *bpfSpaceConfig; //coefficients, gain, etc
	rtty_bpf_config_t *bpfMarkConfig;
	rtty_lpf_config_t *lpfConfig;

	uint16_t oneBitSampleCount;
	int32_t DPLLOldVal;
	int32_t DPLLBitPhase;

	uint8_t byteResult;
	uint16_t byteResultp;

	rtty_charSetMode_t charSetMode;

	rtty_run_state_t state;

	soft_dds_t tx_dds[2];

	const rtty_mode_config_t* config_p;

} rtty_decoder_data_t;

rtty_decoder_data_t rttyDecoderData;




#if 0 // 48Khz filters, not needed
// this is for 48ksps sample rate
// for filter designing, see http://www-users.cs.york.ac.uk/~fisher/mkfilter/
// order 2 Butterworth, freqs: 865-965 Hz
rtty_bpf_config_t rtty_bp_48khz_915 =
{
		.gain = 2.356080041e+04,  // gain at centre
		.coeffs = {-0.9816582826, 3.9166274264, -5.8882201843, 3.9530488323 },
		.freq = 915
};

// order 2 Butterworth, freqs: 1035-1135 Hz
rtty_bpf_config_t rtty_bp_48khz_1085 =
{
		.gain = 2.356080365e+04,
		.coeffs = {-0.9816582826, 3.9051693660, -5.8653953990, 3.9414842213 },
		.freq = 1085
};
#endif

// order 2 Butterworth, freq: 50 Hz
rtty_lpf_config_t rtty_lp_48khz_50 =
{
		.gain = 9.381008646e+04,
		.coeffs = {-0.9907866988, 1.9907440595 }
};

// this is for 12ksps sample rate
// for filter designing, see http://www-users.cs.york.ac.uk/~fisher/mkfilter/
// order 2 Butterworth, freqs: 865-965 Hz, centre: 915 Hz
static rtty_bpf_config_t rtty_bp_12khz_915 =
{
		.gain = 1.513364755e+03,
		.coeffs = { -0.9286270861, 3.3584472566, -4.9635817596, 3.4851652468 },
		.freq = 915
};

// order 2 Butterworth, freqs: 1315-1415 Hz, centre 1365Hz
static rtty_bpf_config_t rtty_bp_12khz_1365 =
{
		.gain = 1.513365019e+03,
		.coeffs = { -0.9286270861, 2.8583904591, -4.1263569881, 2.9662407442 },
		.freq = 1365
};
// order 2 Butterworth, freqs: 1035-1135 Hz, centre: 1085Hz
static rtty_bpf_config_t rtty_bp_12khz_1085 =
{
		.gain = 1.513364927e+03,
		.coeffs = { -0.9286270861, 3.1900687350, -4.6666321298, 3.3104336142 },
		.freq = 1085
};
// order 2 Butterworth, freqs: 1065-1165 Hz, centre: 1115Hz
// for 200Hz shift
static rtty_bpf_config_t rtty_bp_12khz_1115 =
{
		.gain = 1.513364944e+03,
		.coeffs = { -0.9286270861, 3.1576917276, -4.6112830458, 3.2768349860 },
		.freq = 1115
};

// for 85Hz shift --> 915 + 85Hz = space = 1000Hz
// 3dB bandwidth 50Hz
// order 2 Butterworth, freqs: 975-1025 Hz, centre: 1000Hz
static rtty_bpf_config_t rtty_bp_12khz_1000 =
{
		.gain = 5.944465260e+03,
		.coeffs = { -0.9636529842, 3.3693752166, -4.9084595657, 3.4323354886 },
		.freq = 1000
};

// for 425Hz shift --> 915 + 425Hz = space = 1340Hz
// 3dB bandwidth 100Hz
// order 2 Butterworth, freqs: 1290 - 1390 Hz, centre: 1340Hz
static rtty_bpf_config_t rtty_bp_12khz_1340 =
{
		.gain = 1.513365018e+03,
		.coeffs = { -0.9286270862, 2.8906128091, -4.1762457780, 2.9996788796 },
		.freq = 1340
};

// for 850Hz shift --> 915 + 850Hz = space = 1765Hz
// 3dB bandwidth 100Hz
// order 2 Butterworth, freqs: 1715 - 1815 Hz, centre: 1765Hz
static rtty_bpf_config_t rtty_bp_12khz_1765 =
{
		.gain = 1.513365057e+03,
		.coeffs = { -0.9286270862, 2.1190223173, -3.1352567157, 2.1989754113 },
		.freq = 1765
};


static rtty_lpf_config_t rtty_lp_12khz_50 =
{
		.gain = 5.944465310e+03,
		.coeffs = { -0.9636529842, 1.9629800894 }
};

static rtty_mode_config_t  rtty_mode_current_config;


void Rtty_Modem_Init(uint32_t output_sample_rate)
{

	// TODO: pass config as parameter and make it changeable via menu
	rtty_mode_current_config.samplerate = 12000;
	rtty_mode_current_config.shift = rtty_shifts[rtty_ctrl_config.shift_idx].value;
	rtty_mode_current_config.speed = rtty_speeds[rtty_ctrl_config.speed_idx].value;
	rtty_mode_current_config.stopbits = rtty_ctrl_config.stopbits_idx;

	rttyDecoderData.config_p = &rtty_mode_current_config;

	// common config to all supported modes
	rttyDecoderData.oneBitSampleCount = (uint16_t)roundf(rttyDecoderData.config_p->samplerate/rttyDecoderData.config_p->speed);
	rttyDecoderData.charSetMode = RTTY_MODE_LETTERS;
	rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;

	rttyDecoderData.bpfMarkConfig = &rtty_bp_12khz_915; // this is mark, or '1'
	rttyDecoderData.lpfConfig = &rtty_lp_12khz_50;

	// now we handled the specifics
	switch (rttyDecoderData.config_p->shift)
	{
	case 85:
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1000;
		break;
	case 200:
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1115;
		break;
	case 425:
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1340;
		break;
	case 450:
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1365; // this is space or '0'
		break;
	case 850:
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1765; // this is space or '0'
		break;
	case 170:
	default:
		// all unsupported shifts are mapped to 170
		rttyDecoderData.bpfSpaceConfig = &rtty_bp_12khz_1085; // this is space or '0'
	}

	// configure DDS for transmission
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[0], rttyDecoderData.bpfSpaceConfig->freq, output_sample_rate, 0);
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[1], rttyDecoderData.bpfMarkConfig->freq, output_sample_rate, 0);

}

float32_t decayavg(float32_t average, float32_t input, int weight)
{ // adapted from https://github.com/ukhas/dl-fldigi/blob/master/src/include/misc.h
	float32_t retval;
	if (weight <= 1)
	{
		retval = input;
	}
	else
	{
		retval = ( ( input - average ) / (float32_t)weight ) + average ;
	}
	return retval;
}


// this function returns the bit value of the current sample
static int RttyDecoder_demodulator(float32_t sample)
{

	float32_t space_mag = RttyDecoder_bandPassFreq(sample, rttyDecoderData.bpfSpaceConfig, &rttyDecoderData.bpfSpaceData);
	float32_t mark_mag = RttyDecoder_bandPassFreq(sample, rttyDecoderData.bpfMarkConfig, &rttyDecoderData.bpfMarkData);

	float32_t v1 = 0.0;
	// calculating the RMS of the two lines (squaring them)
	space_mag *= space_mag;
	mark_mag *= mark_mag;

    if(rtty_ctrl_config.atc_disable == false)
	{   // RTTY decoding with ATC = automatic threshold correction
		// FIXME: space & mark seem to be swapped in the following code
		// dirty fix
		float32_t helper = space_mag;
		space_mag = mark_mag;
		mark_mag = helper;
		static float32_t mark_env = 0.0;
		static float32_t space_env = 0.0;
		static float32_t mark_noise = 0.0;
		static float32_t space_noise = 0.0;
		// experiment to implement an ATC (Automatic threshold correction), DD4WH, 2017_08_24
		// everything taken from FlDigi, licensed by GNU GPLv2 or later
		// https://github.com/ukhas/dl-fldigi/blob/master/src/cw_rtty/rtty.cxx
		// calculate envelope of the mark and space signals
		// uses fast attack and slow decay
		mark_env = decayavg (mark_env, mark_mag,
				(mark_mag > mark_env) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 16);
		space_env = decayavg (space_env, space_mag,
				(space_mag > space_env) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 16);
		// calculate the noise on the mark and space signals
		mark_noise = decayavg (mark_noise, mark_mag,
				(mark_mag < mark_noise) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 48);
		space_noise = decayavg (space_noise, space_mag,
				(space_mag < space_noise) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 48);
		// the noise floor is the lower signal of space and mark noise
		float32_t noise_floor = (space_noise < mark_noise) ? space_noise : mark_noise;

		// Linear ATC, section 3 of www.w7ay.net/site/Technical/ATC
		//		v1 = space_mag - mark_mag - 0.5 * (space_env - mark_env);

		// Compensating for the noise floor by using clipping
		float32_t mclipped = 0.0, sclipped = 0.0;
		mclipped = mark_mag > mark_env ? mark_env : mark_mag;
		sclipped = space_mag > space_env ? space_env : space_mag;
		if (mclipped < noise_floor)
		{
			mclipped = noise_floor;
		}
		if (sclipped < noise_floor)
		{
			sclipped = noise_floor;
		}

		// we could add options for mark-only or space-only decoding
		// however, the current implementation with ATC already works quite well with mark-only/space-only
		/*					switch (progdefaults.rtty_cwi) {
						case 1 : // mark only decode
							space_env = sclipped = noise_floor;
							break;
						case 2: // space only decode
							mark_env = mclipped = noise_floor;
						default : ;
		}
		 */

		// Optimal ATC (Section 6 of of www.w7ay.net/site/Technical/ATC)
		v1  = (mclipped - noise_floor) * (mark_env - noise_floor) -
				(sclipped - noise_floor) * (space_env - noise_floor) -
				0.25 *  ((mark_env - noise_floor) * (mark_env - noise_floor) -
						(space_env - noise_floor) * (space_env - noise_floor));

		v1 = RttyDecoder_lowPass(v1, rttyDecoderData.lpfConfig, &rttyDecoderData.lpfData);

	}
	else
	{   // RTTY without ATC, which works very well too!
		// inverting line 1
		mark_mag *= -1;

		// summing the two lines
		v1 = mark_mag + space_mag;

		// lowpass filtering the summed line
		v1 = RttyDecoder_lowPass(v1, rttyDecoderData.lpfConfig, &rttyDecoderData.lpfData);
	}

	return (v1 > 0)?0:1;
}

// this function returns true once at the half of a bit with the bit's value
static bool RttyDecoder_getBitDPLL(float32_t sample, bool* val_p) {
	static bool phaseChanged = false;
	bool retval = false;


	if (rttyDecoderData.DPLLBitPhase < rttyDecoderData.oneBitSampleCount)
	{
		*val_p = RttyDecoder_demodulator(sample);

		if (!phaseChanged && *val_p != rttyDecoderData.DPLLOldVal) {
			if (rttyDecoderData.DPLLBitPhase < rttyDecoderData.oneBitSampleCount/2)
			{
				// rttyDecoderData.DPLLBitPhase += rttyDecoderData.oneBitSampleCount/8; // early
				rttyDecoderData.DPLLBitPhase += rttyDecoderData.oneBitSampleCount/32; // early
			}
			else
			{
				//rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount/8; // late
				rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount/32; // late
			}
			phaseChanged = true;
		}
		rttyDecoderData.DPLLOldVal = *val_p;
		rttyDecoderData.DPLLBitPhase++;
	}

	if (rttyDecoderData.DPLLBitPhase >= rttyDecoderData.oneBitSampleCount)
	{
		rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount;
		retval = true;
	}

	return retval;
}

// this function returns only true when the start bit is successfully received
static bool RttyDecoder_waitForStartBit(float32_t sample) {
	bool retval = false;
	int bitResult;
	static int16_t wait_for_start_state = 0;
	static int16_t wait_for_half = 0;

	bitResult = RttyDecoder_demodulator(sample);
	switch (wait_for_start_state)
	{
	case 0:
		// waiting for a falling edge
		if (bitResult != 0)
		{
			wait_for_start_state++;
		}
		break;
	case 1:
		if (bitResult != 1)
		{
			wait_for_start_state++;
		}
		break;
	case 2:
		wait_for_half = rttyDecoderData.oneBitSampleCount/2;
		wait_for_start_state ++;
        /* fall through */ // this is for the compiler, the following comment is for Eclipse
		/* no break */
	case 3:
		wait_for_half--;
		if (wait_for_half == 0)
		{
			retval = (bitResult == 0);
			wait_for_start_state = 0;
		}
		break;
	}
	return retval;
}


void Rtty_Demodulator_ProcessSample(float32_t sample)
{

	switch(rttyDecoderData.state)
	{
	case RTTY_RUN_STATE_WAIT_START: // not synchronized, need to wait for start bit
		if (RttyDecoder_waitForStartBit(sample))
		{
			rttyDecoderData.state = RTTY_RUN_STATE_BIT;
			rttyDecoderData.byteResultp = 1;
			rttyDecoderData.byteResult = 0;
		}
		break;
	case RTTY_RUN_STATE_BIT:
		// reading 7 more bits
		if (rttyDecoderData.byteResultp < 8)
		{
			bool bitResult = false;
			if (RttyDecoder_getBitDPLL(sample, &bitResult))
			{
				switch (rttyDecoderData.byteResultp)
				{
				case 6: // stop bit 1

				case 7: // stop bit 2
				if (bitResult == false)
				{
					// not in sync
					rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;
				}
				if (rttyDecoderData.config_p->stopbits != RTTY_STOP_2 && rttyDecoderData.byteResultp == 6)
				{
					// we pretend to be at the 7th bit after receiving the first stop bit if we have less than 2 stop bits
					// this omits check for 1.5 bit condition but we should be more or less safe here, may cause
					// a little more unaligned receive but without that shortcut we simply cannot receive these configurations
					// so it is worth it
					rttyDecoderData.byteResultp = 7;
				}

				break;
				default:
					// System.out.print(bitResult);
					rttyDecoderData.byteResult |= (bitResult?1:0) << (rttyDecoderData.byteResultp-1);
				}
				rttyDecoderData.byteResultp++;
			}
		}
		if (rttyDecoderData.byteResultp == 8 && rttyDecoderData.state == RTTY_RUN_STATE_BIT)
		{
			char charResult;

			switch (rttyDecoderData.byteResult) {
			case RTTY_LETTER_CODE:
				rttyDecoderData.charSetMode = RTTY_MODE_LETTERS;
				// System.out.println(" ^L^");
				break;
			case RTTY_SYMBOL_CODE:
				rttyDecoderData.charSetMode = RTTY_MODE_SYMBOLS;
				// System.out.println(" ^F^");
				break;
			default:
				switch (rttyDecoderData.charSetMode)
				{
				case RTTY_MODE_SYMBOLS:
					charResult = RTTYSymbols[rttyDecoderData.byteResult];
					break;
                case RTTY_MODE_LETTERS:
                default:
                    charResult = RTTYLetters[rttyDecoderData.byteResult];
                    break;
				}
				//UiDriver_TextMsgPutChar(charResult);
				char output[2];
				output[0] = charResult;
				output[1] = 0;
				write_log(output);
				printf(output);
				break;
			}
			rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;
		}
	}
}




/*

	TX routines

*/


typedef enum
{
	MSK_IDLE = 0,
	MSK_WAIT_FOR_NEG,
	MSK_WAIT_FOR_PLUS,
	MSK_WAIT_FOR_PLUSGRAD, // wait for the values growing towards max
	MSK_WAIT_FOR_MAX, // wait for the value after max, needs to be done one the growing part of the curve
} msk_state_t;

typedef struct
{
	uint8_t char_bit_idx;
	uint16_t char_bits;
	uint32_t char_bit_samples;
	uint16_t last_bit;
	int16_t last_value;
	uint8_t current_bit;
	msk_state_t msk_state;
	rtty_charSetMode_t char_mode;

} rtty_tx_encoder_state_t;

rtty_tx_encoder_state_t  rtty_tx =
{
		.last_bit = 1,
		.last_value = INT16_MIN,
		.char_mode = RTTY_MODE_LETTERS,

};

#define USE_RTTY_MSK
#define RTTY_CODE_MODE_MASK (0b100000)
#define RTTY_CODE_MODE_LETTER (RTTY_CODE_MODE_MASK)
#define RTTY_CODE_MODE_SYMBOL (0)

static void Rtty_BaudotAdd(uint8_t bits)
{
	uint8_t bitCount = rttyDecoderData.config_p->stopbits == RTTY_STOP_1?7:8;

	bits <<= 1; // add stop bit

	if (bitCount == 7)
	{
		bits |= 0b01000000;
	}
	else
	{
		// for 1.5 and 2 we use 2 bits for now
		bits |= 0b11000000;
	}

	rtty_tx.char_bits |= bits << rtty_tx.char_bit_idx;
	// we add more bits towards the MSB if we already have bits

	rtty_tx.char_bit_idx += bitCount;
	// now remember how many bits we added
}

void Rtty_Modulator_Code2Bits(uint8_t baudot_info)
{
	rtty_tx.char_bits = 0;
	rtty_tx.char_bit_idx = 0;

	if ((baudot_info & RTTY_CODE_MODE_MASK) == RTTY_CODE_MODE_LETTER)
	{
		if(rtty_tx.char_mode != RTTY_MODE_LETTERS)
		{
			rtty_tx.char_mode = RTTY_MODE_LETTERS;
			Rtty_BaudotAdd(RTTY_LETTER_CODE);
		}
	}
	else
	{
		if(rtty_tx.char_mode != RTTY_MODE_SYMBOLS)
		{
			rtty_tx.char_mode = RTTY_MODE_SYMBOLS;
			Rtty_BaudotAdd(RTTY_SYMBOL_CODE);
		}
	}
	Rtty_BaudotAdd(baudot_info & ~RTTY_CODE_MODE_MASK);
}

// MUST BE CALLED BEFORE WE START RTTY TX, i.e. in RX Mode !!!
void Rtty_Modulator_StartTX()
{
	rtty_tx.last_bit = 1;
	rtty_tx.last_value = INT16_MIN;
	rtty_tx.char_mode = RTTY_MODE_LETTERS;
	Rtty_Modulator_Code2Bits(RTTY_LETTER_CODE);
}

const char rtty_test_string[] = " --- UHSDR FIRMWARE RTTY TX TEST DE DB4PLE --- ";

int16_t Rtty_Modulator_GenSample()
{
	if (rtty_tx.char_bit_samples == 0)
	{
		//this is because the sampling rate on rx is 12000 vs 48000 on transmit?
		rtty_tx.char_bit_samples = rttyDecoderData.oneBitSampleCount * 4;

		rtty_tx.char_bits >>= 1;
		if (rtty_tx.char_bit_idx == 0)
		{
			// load the character and add the stop bits;
			bool bitsFilled = false;


			// ESE: change this to read from the keyboard buffer
			//while ( bitsFilled == false
			//        &&  DigiModes_TxBufferHasDataFor(RTTY))
			while ( bitsFilled == false && *tx_next)
			{

			    uint8_t current_ascii;

			    // as the character might have been removed from the buffer,
			    // we do a final check when removing the character
			    //if (DigiModes_TxBufferRemove( &current_ascii, RTTY ))
					if (*tx_next)
			    {
							current_ascii = *tx_next++;
			        if (current_ascii == 0x04 ) //EOT
			        {
									puts("TX off!!!\n");
									fclose(pf);
									//exit(0);
			            //RadioManagement_Request_TxOff();
			        }
			        else
			        {
			            uint8_t current_baudot = Ascii2Baudot[current_ascii & 0x7f];
			            if (current_baudot > 0)
			            { // we have valid baudot code
			                Rtty_Modulator_Code2Bits(current_baudot);
			                bitsFilled = true;
			            }
			        }
			    }
			}

			if (bitsFilled == false)
			{
				// IDLE
				Rtty_Modulator_Code2Bits(RTTY_LETTER_CODE | RTTY_CODE_MODE_LETTER);
				// we ensure that we will switch back to letter mode by making the switch symbol a letter
#if 0
				for (uint8_t idx = 0; idx < sizeof(rtty_test_string); idx++)
				{
					DigiModes_TxBufferPutChar( rtty_test_string[idx] , CW );
				}
#endif
			}
		}
		rtty_tx.char_bit_idx--;

		rtty_tx.current_bit = rtty_tx.char_bits&1;
#ifdef USE_RTTY_MSK
		if (rtty_tx.last_bit != rtty_tx.current_bit)
		{
			if (rtty_tx.last_bit == 1)
			{
				rtty_tx.msk_state = MSK_WAIT_FOR_NEG;
				// WAIT_FOR_NEG
				// WAIT_FOR_ZERO or plus
				rtty_tx.current_bit = rtty_tx.last_bit;
			}
			else
			{
				rtty_tx.msk_state = MSK_WAIT_FOR_PLUSGRAD;
				rtty_tx.current_bit = rtty_tx.last_bit;
				rtty_tx.last_value = INT16_MIN;
				// WAIT_FOR_MAX
			}
		}
#else
		rtty_tx.msk_state = MSK_IDLE;
#endif
	}

	rtty_tx.char_bit_samples--;

	int16_t current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);

	switch(rtty_tx.msk_state)
	{
	case MSK_WAIT_FOR_NEG:
		if (current_value < 0)
		{
			rtty_tx.msk_state = MSK_WAIT_FOR_PLUS;
		}
		break;
	case MSK_WAIT_FOR_PLUS:
		if (current_value >= 0)
		{
			rtty_tx.msk_state = MSK_IDLE;
			rtty_tx.current_bit = rtty_tx.char_bits&1;
			rttyDecoderData.tx_dds[rtty_tx.current_bit].acc = rttyDecoderData.tx_dds[rtty_tx.last_bit].acc;
			rtty_tx.last_bit = rtty_tx.current_bit;
			current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);
		}
		break;
	case MSK_WAIT_FOR_PLUSGRAD:
		if (current_value > rtty_tx.last_value)
		{
			rtty_tx.msk_state = MSK_WAIT_FOR_MAX;
		}
		break;
	case MSK_WAIT_FOR_MAX:
		if (current_value >= rtty_tx.last_value)
		{
			rtty_tx.last_value = current_value;
		}
		else if (current_value < rtty_tx.last_value && rtty_tx.last_value > 0)
		{
			rtty_tx.msk_state = MSK_IDLE;
			rtty_tx.current_bit = rtty_tx.char_bits&1;
			rttyDecoderData.tx_dds[rtty_tx.current_bit].acc = rttyDecoderData.tx_dds[rtty_tx.last_bit].acc;
			rtty_tx.last_bit = rtty_tx.current_bit;
			current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);
		}
		break;
	default:
		break;
	}

	rtty_tx.last_value = current_value;

	return current_value;
}

/*
int main(int argc, char *argv[]){
	int tx = 0;

	printf("Size of float32_t is %d\n", (int)sizeof(float32_t));

	strcpy(tx_buff, "LEND ME YOUR EARS, I COME TO BURY CAESER, NOT TO PRAISE HIM. THE EVIL THAT MEN DO LIVES AFTER THEM\004");
	tx_next = tx_buff; 
	Rtty_Modem_Init(48000);

	if (tx){
		Rtty_Modulator_StartTX();
		FILE *pf = fopen("grtty.raw", "w");

		while(1){
			int16_t sample = Rtty_Modulator_GenSample();
			fwrite(&sample, 2, 1, pf);
		}
	} else {
		FILE *pf = fopen("rtty.raw", "r");
		int decimation_factor = 4;
		int decimation_count = 0;
		float32_t sample_big = 0;	
		int16_t sample;
		int count = 0;
		while(!feof(pf)){
			
			fread(&sample, 2, 1, pf);
			sample_big += sample;
			decimation_count++;
			if (decimation_count >= decimation_factor){
				sample_big /= 132000.0;
				//printf("%d = %g \n", count++, sample_big);
				Rtty_Demodulator_ProcessSample(sample_big);
				decimation_count = 0;
				sample_big = 0.0;
			}
		}
	}
}

*/

/*******************************************************
**********             PSK31 routines            *******
********************************************************/

/*
This demodulator assumes that the narrow filtering has already happened
at the receiver end.
*/

// table courtesy of fldigi pskvaricode.cxx
static const uint16_t psk_varicode[] = {
    0b1010101011,       /*   0 - <NUL>  */
    0b1011011011,       /*   1 - <SOH>  */
    0b1011101101,       /*   2 - <STX>  */
    0b1101110111,       /*   3 - <ETX>  */
    0b1011101011,       /*   4 - <EOT>  */
    0b1101011111,       /*   5 - <ENQ>  */
    0b1011101111,       /*   6 - <ACK>  */
    0b1011111101,       /*   7 - <BEL>  */
    0b1011111111,       /*   8 - <BS>   */
    0b11101111,         /*   9 - <TAB>  */
    0b11101,            /*  10 - <LF>   */
    0b1101101111,       /*  11 - <VT>   */
    0b1011011101,       /*  12 - <FF>   */
    0b11111,            /*  13 - <CR>   */
    0b1101110101,       /*  14 - <SO>   */
    0b1110101011,       /*  15 - <SI>   */
    0b1011110111,       /*  16 - <DLE>  */
    0b1011110101,       /*  17 - <DC1>  */
    0b1110101101,       /*  18 - <DC2>  */
    0b1110101111,       /*  19 - <DC3>  */
    0b1101011011,       /*  20 - <DC4>  */
    0b1101101011,       /*  21 - <NAK>  */
    0b1101101101,       /*  22 - <SYN>  */
    0b1101010111,       /*  23 - <ETB>  */
    0b1101111011,       /*  24 - <CAN>  */
    0b1101111101,       /*  25 - <EM>   */
    0b1110110111,       /*  26 - <SUB>  */
    0b1101010101,       /*  27 - <ESC>  */
    0b1101011101,       /*  28 - <FS>   */
    0b1110111011,       /*  29 - <GS>   */
    0b1011111011,       /*  30 - <RS>   */
    0b1101111111,       /*  31 - <US>   */
    0b1,                /*  32 - <SPC>  */
    0b111111111,        /*  33 - !  */
    0b101011111,        /*  34 - '"'    */
    0b111110101,        /*  35 - #  */
    0b111011011,        /*  36 - $  */
    0b1011010101,       /*  37 - %  */
    0b1010111011,       /*  38 - &  */
    0b101111111,        /*  39 - '  */
    0b11111011,         /*  40 - (  */
    0b11110111,         /*  41 - )  */
    0b101101111,        /*  42 - *  */
    0b111011111,        /*  43 - +  */
    0b1110101,          /*  44 - ,  */
    0b110101,           /*  45 - -  */
    0b1010111,          /*  46 - .  */
    0b110101111,        /*  47 - /  */
    0b10110111,         /*  48 - 0  */
    0b10111101,         /*  49 - 1  */
    0b11101101,         /*  50 - 2  */
    0b11111111,         /*  51 - 3  */
    0b101110111,        /*  52 - 4  */
    0b101011011,        /*  53 - 5  */
    0b101101011,        /*  54 - 6  */
    0b110101101,        /*  55 - 7  */
    0b110101011,        /*  56 - 8  */
    0b110110111,        /*  57 - 9  */
    0b11110101,         /*  58 - :  */
    0b110111101,        /*  59 - ;  */
    0b111101101,        /*  60 - <  */
    0b1010101,          /*  61 - =  */
    0b111010111,        /*  62 - >  */
    0b1010101111,       /*  63 - ?  */
    0b1010111101,       /*  64 - @  */
    0b1111101,          /*  65 - A  */
    0b11101011,         /*  66 - B  */
    0b10101101,         /*  67 - C  */
    0b10110101,         /*  68 - D  */
    0b1110111,          /*  69 - E  */
    0b11011011,         /*  70 - F  */
    0b11111101,         /*  71 - G  */
    0b101010101,        /*  72 - H  */
    0b1111111,          /*  73 - I  */
    0b111111101,        /*  74 - J  */
    0b101111101,        /*  75 - K  */
    0b11010111,         /*  76 - L  */
    0b10111011,         /*  77 - M  */
    0b11011101,         /*  78 - N  */
    0b10101011,         /*  79 - O  */
    0b11010101,         /*  80 - P  */
    0b111011101,        /*  81 - Q  */
    0b10101111,         /*  82 - R  */
    0b1101111,          /*  83 - S  */
    0b1101101,          /*  84 - T  */
    0b101010111,        /*  85 - U  */
    0b110110101,        /*  86 - V  */
    0b101011101,        /*  87 - W  */
    0b101110101,        /*  88 - X  */
    0b101111011,        /*  89 - Y  */
    0b1010101101,       /*  90 - Z  */
    0b111110111,        /*  91 - [  */
    0b111101111,        /*  92 - \  */
    0b111111011,        /*  93 - ]  */
    0b1010111111,       /*  94 - ^  */
    0b101101101,        /*  95 - _  */
    0b1011011111,       /*  96 - `  */
    0b1011,             /*  97 - a  */
    0b1011111,          /*  98 - b  */
    0b101111,           /*  99 - c  */
    0b101101,           /* 100 - d  */
    0b11,               /* 101 - e  */
    0b111101,           /* 102 - f  */
    0b1011011,          /* 103 - g  */
    0b101011,           /* 104 - h  */
    0b1101,             /* 105 - i  */
    0b111101011,        /* 106 - j  */
    0b10111111,         /* 107 - k  */
    0b11011,            /* 108 - l  */
    0b111011,           /* 109 - m  */
    0b1111,             /* 110 - n  */
    0b111,              /* 111 - o  */
    0b111111,           /* 112 - p  */
    0b110111111,        /* 113 - q  */
    0b10101,            /* 114 - r  */
    0b10111,            /* 115 - s  */
    0b101,              /* 116 - t  */
    0b110111,           /* 117 - u  */
    0b1111011,          /* 118 - v  */
    0b1101011,          /* 119 - w  */
    0b11011111,         /* 120 - x  */
    0b1011101,          /* 121 - y  */
    0b111010101,        /* 122 - z  */
    0b1010110111,       /* 123 - {  */
    0b110111011,        /* 124 - |  */
    0b1010110101,       /* 125 - }  */
    0b1011010111,       /* 126 - ~  */
    0b1110110101,       /* 127 - <DEL>  */
    0b1110111101,       /* 128 -    */
    0b1110111111,       /* 129 -    */
    0b1111010101,       /* 130 -    */
    0b1111010111,       /* 131 -    */
    0b1111011011,       /* 132 -    */
    0b1111011101,       /* 133 -    */
    0b1111011111,       /* 134 -    */
    0b1111101011,       /* 135 -    */
    0b1111101101,       /* 136 -    */
    0b1111101111,       /* 137 -    */
    0b1111110101,       /* 138 -    */
    0b1111110111,       /* 139 -    */
    0b1111111011,       /* 140 -    */
    0b1111111101,       /* 141 -    */
    0b1111111111,       /* 142 -    */
    0b10101010101,      /* 143 -    */
    0b10101010111,      /* 144 -    */
    0b10101011011,      /* 145 -    */
    0b10101011101,      /* 146 -    */
    0b10101011111,      /* 147 -    */
    0b10101101011,      /* 148 -    */
    0b10101101101,      /* 149 -    */
    0b10101101111,      /* 150 -    */
    0b10101110101,      /* 151 -    */
    0b10101110111,      /* 152 -    */
    0b10101111011,      /* 153 -    */
    0b10101111101,      /* 154 -    */
    0b10101111111,      /* 155 -    */
    0b10110101011,      /* 156 -    */
    0b10110101101,      /* 157 -    */
    0b10110101111,      /* 158 -    */
    0b10110110101,      /* 159 -    */
    0b10110110111,      /* 160 -    */
    0b10110111011,      /* 161 -    */
    0b10110111101,      /* 162 -    */
    0b10110111111,      /* 163 -    */
    0b10111010101,      /* 164 -    */
    0b10111010111,      /* 165 -    */
    0b10111011011,      /* 166 -    */
    0b10111011101,      /* 167 -    */
    0b10111011111,      /* 168 -    */
    0b10111101011,      /* 169 -    */
    0b10111101101,      /* 170 -    */
    0b10111101111,      /* 171 -    */
    0b10111110101,      /* 172 -    */
    0b10111110111,      /* 173 -    */
    0b10111111011,      /* 174 -    */
    0b10111111101,      /* 175 -    */
    0b10111111111,      /* 176 -    */
    0b11010101011,      /* 177 -    */
    0b11010101101,      /* 178 -    */
    0b11010101111,      /* 179 -    */
    0b11010110101,      /* 180 -    */
    0b11010110111,      /* 181 -    */
    0b11010111011,      /* 182 -    */
    0b11010111101,      /* 183 -    */
    0b11010111111,      /* 184 -    */
    0b11011010101,      /* 185 -    */
    0b11011010111,      /* 186 -    */
    0b11011011011,      /* 187 -    */
    0b11011011101,      /* 188 -    */
    0b11011011111,      /* 189 -    */
    0b11011101011,      /* 190 -    */
    0b11011101101,      /* 191 -    */
    0b11011101111,      /* 192 -    */
    0b11011110101,      /* 193 -    */
    0b11011110111,      /* 194 -    */
    0b11011111011,      /* 195 -    */
    0b11011111101,      /* 196 -    */
    0b11011111111,      /* 197 -    */
    0b11101010101,      /* 198 -    */
    0b11101010111,      /* 199 -    */
    0b11101011011,      /* 200 -    */
    0b11101011101,      /* 201 -    */
    0b11101011111,      /* 202 -    */
    0b11101101011,      /* 203 -    */
    0b11101101101,      /* 204 -    */
    0b11101101111,      /* 205 -    */
    0b11101110101,      /* 206 -    */
    0b11101110111,      /* 207 -    */
    0b11101111011,      /* 208 -    */
    0b11101111101,      /* 209 -    */
    0b11101111111,      /* 210 -    */
    0b11110101011,      /* 211 -    */
    0b11110101101,      /* 212 -    */
    0b11110101111,      /* 213 -    */
    0b11110110101,      /* 214 -    */
    0b11110110111,      /* 215 -    */
    0b11110111011,      /* 216 -    */
    0b11110111101,      /* 217 -    */
    0b11110111111,      /* 218 -    */
    0b11111010101,      /* 219 -    */
    0b11111010111,      /* 220 -    */
    0b11111011011,      /* 221 -    */
    0b11111011101,      /* 222 -    */
    0b11111011111,      /* 223 -    */
    0b11111101011,      /* 224 -    */
    0b11111101101,      /* 225 -    */
    0b11111101111,      /* 226 -    */
    0b11111110101,      /* 227 -    */
    0b11111110111,      /* 228 -    */
    0b11111111011,      /* 229 -    */
    0b11111111101,      /* 230 -    */
    0b11111111111,      /* 231 -    */
    0b101010101011,     /* 232 -    */
    0b101010101101,     /* 233 -    */
    0b101010101111,     /* 234 -    */
    0b101010110101,     /* 235 -    */
    0b101010110111,     /* 236 -    */
    0b101010111011,     /* 237 -    */
    0b101010111101,     /* 238 -    */
    0b101010111111,     /* 239 -    */
    0b101011010101,     /* 240 -    */
    0b101011010111,     /* 241 -    */
    0b101011011011,     /* 242 -    */
    0b101011011101,     /* 243 -    */
    0b101011011111,     /* 244 -    */
    0b101011101011,     /* 245 -    */
    0b101011101101,     /* 246 -    */
    0b101011101111,     /* 247 -    */
    0b101011110101,     /* 248 -    */
    0b101011110111,     /* 249 -    */
    0b101011111011,     /* 250 -    */
    0b101011111101,     /* 251 -    */
    0b101011111111,     /* 252 -    */
    0b101101010101,     /* 253 -    */
    0b101101010111,     /* 254 -    */
    0b101101011011,     /* 255 -    */
};

struct psk31_decoder {

	// this is a trivial IIR low pass filter
	// it calculates the lowpass response as 
	//		output = input *k - previous_output * (k-1)

	float lpf_output;
	float iir_k;

	// we run a clock of enough samples to exactly over the time it takes
	// to receive one symbol (0 or 1 in our case). 
	// the clock counts the number of samples caculated as sampling_rate/31.25Hz
	// to keep the clock synchronized with the received signal, we reset the 
	// clock each time a transition of the phase is detected

	int	bit_clk_max;
	int	bit_clk;

	// as the symbols are received we look for a pattern of two consecutive
	// zeros are the start of a new varicode letter. Hence, we store 
	// the last symbol to compare it with the currently detected symbol 

	int last_symbol;
	int	varicode;

	// these rates are usually fixed in an SDR
	int sampling_rate;
	int symbol_period_ms; //in milliseconds

	// we run a circular buffer that has exaclty as many samples as 
	// will fit into 32 msec, the symbol period of PSK31. 
	// the last sample, delayed 32 msec is pointed to by delay_index. 
	// the next sample is written at delay_index+1, 
	int delay_length;
	float *delay_line;
	int delay_index;
};


struct psk31_decoder psk_demodulator;

void psk31_init(struct psk31_decoder *p, int sampling_rate){

	p->lpf_output = 0;
	p->iir_k = 0.02;
	p->bit_clk = 0;

	p->last_symbol = 1;
	p->varicode = 0;

	// these rates are usually fixed in an SDR
	p->sampling_rate = 12000;
	p->symbol_period_ms = 32; //in milliseconds

	// we run a circular buffer that has exaclty as many samples as 
	// will fit into 32 msec, the symbol period of PSK31. 
	// the last sample, delayed 32 msec is pointed to by delay_index. 
	// the next sample is written at delay_index+1, 
	p->delay_index = 0;

	p->delay_length = (p->symbol_period_ms * sampling_rate) / 1000;
	p->delay_line = malloc(sizeof(float) * p->delay_length);

	p->bit_clk_max = p->delay_length;
	p->last_symbol = 1;
	p->varicode = 0;
	p->iir_k = 0.01;
	//printf("delay length = %d\n", p->delay_length);
	for (int i = 0; i < p->delay_length; i++)
		p->delay_line[i] = 0;

	//printf("bit clock max = %d\n", p->bit_clk_max);
	p->delay_index = 0;
}


float_t psk31_process_sample(struct psk31_decoder *p, int32_t sample){

	//scale the integer into float and multiply with a sample that came
	//exactly 32 msec ago

	float sample_f  =  (sample *1.0)/  1000000000.0;
	float output  = p->delay_line[p->delay_index]	* (sample_f) ;

	//overwrite the oldeest sample in the delay line and move 
	//to the next oldest that will be read and overwritten in the next 
	//call to this function

	p->delay_line[p->delay_index] = sample_f;
	p->delay_index++;
	if (p->delay_index >= p->delay_length)
		p->delay_index = 0;

	// low pass the output to remove the AC component and see the
	// dc shift of the multiplier

	float lpf_new =  (output * p->iir_k) + (p->lpf_output * (1-p->iir_k));

	//advance the bit clock
	p->bit_clk++;

	//look for a zero-crossing, going from -ve to +ve 
	//only after half the bit clk period

	if (p->bit_clk > p->bit_clk_max /2 && lpf_new > 0 
				&& p->lpf_output < 0 && p->bit_clk >= p->bit_clk_max)
		p->bit_clk = 0;

	//update the LPF state	
	p->lpf_output = lpf_new;

	//wrap the bit clock around after 32 msec
	if (p->bit_clk >= p->bit_clk_max)
		p->bit_clk = 0; 	

	//at half bitclk, see if it is a zero or a 1
	//this is done only once every 32 msec

	int symbol;

	if (p->bit_clk == p->bit_clk_max/2){
		if ( p->lpf_output > 0)
			symbol = 0;
		else
			symbol = 1;

		// have we detected two consequetive zeros as the boundary of a varicode?
		if (symbol == 0 && p->last_symbol == 0){
			if (p->varicode || p->varicode < 255){
				// the varicode already has one zero from the boundary, chop it off
				p->varicode = p->varicode >> 1;
				//search for a matching varicode and emit its ascii
				for (int i = 0; i < 256; i++)
					if (p->varicode == psk_varicode[i])
						printf("%c\n", i);
			}
			//putchar('\n');
			//reset the varicdoe
			p->varicode = 0;
		}
		else if (symbol == 0){
			//shift a zero into the varicode
			//putchar('0');
			p->varicode = p->varicode << 1;
		}
		else {
			//shift a 1 into the varicode
			//putchar('1');
			p->varicode = (p->varicode << 1) + 1;
		}
		p->last_symbol = symbol;
	}
	return output;
}

/*******************************************************
**********      Modem dispatch routines          *******
********************************************************/

void modem_rx(int mode, int32_t *samples, int count){
	int i;
	int32_t *s;
	FILE *pf;

	s = samples;
	switch(mode){
	case MODE_FT8:
		ft8_rx(samples, count);
		break;
	case MODE_PSK31:
		//pf = fopen("psk31-sbitx.raw", "a");
		//trivially decimate by a factor of 8,
		//the modems from UHSDR need 12000 samples/sec, not 96K
		s = samples;
		for (i = 0; i < count; i += 8){ 
			int32_t sample = *s;
			psk31_process_sample(&psk_demodulator, *s);
			//fwrite(&sample, sizeof(int32_t), 1, pf);	
			s += 8;
		}
		//fclose(pf);
		break;
	case MODE_RTTY:
		pf = fopen("rtty12k.raw", "a");
		for (i = 0; i < count; i += 8) {
			s += 8;
			float fs = *s / 1000000000.0;
			int32_t si = *s;
			Rtty_Demodulator_ProcessSample(fs);
			fwrite(&si, sizeof(int32_t), 1, pf);	
		}
		fclose(pf);
		break;
	}
}

void modem_init(){
	// init the ft8
	ft8_rx_buff_index = 0;
	pthread_create( &ft8_thread, NULL, ft8_thread_function, (void*)NULL);
	psk31_init(&psk_demodulator, 12000);
	Rtty_Modem_Init(96000);
}

int modem_center_freq(int mode){
	switch(mode){
		case MODE_FT8:
			return 3000;
		case MODE_RTTY:
			return 915;
		case MODE_PSK31:
			return 500; 
		default:
			return 0;
	}
}
