#include <math.h>
#include <stdio.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include "sdr.h"

//we define one more than needed to cover the boundary of quadrature
#define MAX_PHASE_COUNT (16385)
static int	phase_table[MAX_PHASE_COUNT];
int sampling_freq = 96000; 

void vfo_init_phase_table(){
	for (int i = 0; i < MAX_PHASE_COUNT; i++){
		double d = (M_PI/2) * ((double)i)/((double)MAX_PHASE_COUNT);
		phase_table[i] = (int)(sin(d) * 1073741824.0);
	}
}

void vfo_start(struct vfo *v, int frequency_hz, int start_phase){
	v->phase_increment = (frequency_hz * 65536) / sampling_freq;
	v->phase = start_phase;
}
 
int vfo_read(struct vfo *v){
	int i = 0;
	if (v->phase < 16384)
		i = phase_table[v->phase];
	else if (v->phase < 32768)
		i = phase_table[32767 - v->phase];
	else if (v->phase < 49152)
		i =  -phase_table[v->phase - 32768];
	else
		i = -phase_table[65535- v->phase];  

	//increment the phase and modulo-65536 
	v->phase += v->phase_increment; 
	v->phase &= 0xffff;

	return i;
}

/*
void main(int argc, char **argv){
	struct vfo v;
	
	vfo_init_phase_table();

	vfo_start(&v, 1000, 0);

	for (int i = 0; i < 100; i++)
		printf("%d ", vfo_read(&v));

}
*/
