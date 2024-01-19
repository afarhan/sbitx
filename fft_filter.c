#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <memory.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "sdr.h"

// Wisdom Defines for the FFTW and FFTWF libraries
// Options for WISDOM_MODE from least to most rigorous are FFTW_ESTIMATE, FFTW_MEASURE, FFTW_PATIENT, and FFTW_EXHAUSTIVE
// The FFTW_ESTIMATE mode seems to make completely incorrect Wisdom plan choices sometimes, and is not recommended.
// Wisdom plans found in an existing Wisdom file will negate the need for time consuming Wisdom plan calculations
// if the Wisdom plans in the file were generated at the same or more rigorous level.
#define WISDOM_MODE FFTW_MEASURE
#define PLANTIME -1		// spend no more than plantime seconds finding the best FFT algorithm. -1 turns the platime cap off.
char wisdom_file_f[] = "sbitx_wisdom_f.wis";

// Modified Bessel function of the 0th kind, used by the Kaiser window
const float i0(float const z){
  const float t = (z*z)/4;
  float sum = 1 + t;
  float term = t;
  for(int k=2; k<40; k++){
    term *= t/(k*k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}

// Modified Bessel function of first kind
const float i1(float const z){
  const float t = (z*z)/4;
  float term = 1;
  float sum = term;

  for(int k=1; k<40; k++){
    term *= t / (k*(k+1));
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return 0.5 * z * sum;
}

// Compute an entire Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiser(float * const window,unsigned int const M,float const beta){
  assert(window != NULL);
  if(window == NULL)
    return -1;
  // Precompute unchanging partial values
  float const numc = M_PI * beta;
  float const inv_denom = 1. / i0(numc); // Inverse of denominator
  float const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  for(int n = 0; n < M/2; n++){
    float const p = pc * n  - 1;
    window[M-1-n] = window[n] = i0(numc * sqrtf(1-p*p)) * inv_denom;
  }
  // If sequence length is odd, middle value is unity
  if(M & 1)
    window[(M-1)/2] = 1; // The -1 is actually unnecessary

  return 0;
}

const static float hann(int const n,int const M){
    return 0.5 - 0.5 * cos(2*M_PI*n/(M-1));
}

int make_hann_window(float *window, int max_count){
	//apply to the entire fft (MAX_BINS)
	for (int i = 0; i < max_count; i++)
		window[i] = hann(i, max_count);	 
}

// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N complex floats
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (cGenter of impulse response) is at M/2
// L and M refer to the decimated output
int window_filter(int const L,int const M,complex float * const response,float const beta){

	//total length of the convolving samples
  int const N = L + M - 1;

  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  complex float * const buffer = fftwf_alloc_complex(N);

  fftw_set_timelimit(PLANTIME);
  fftwf_set_timelimit(PLANTIME);
  int e = fftwf_import_wisdom_from_filename(wisdom_file_f);
  if (e == 0)
  {
    printf("Generating Wisdom File...\n");
  }
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
  fftwf_export_wisdom_to_filename(wisdom_file_f);

  // Convert to time domain
  memcpy(buffer,response,N*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);

#if 0 
  for(int n = 0; n < N; n++)
    printf("# time[%d] = %lg, %lg\n", n, crealf(buffer[n]), cimagf(buffer[n]));
#endif

#if 0 
  for(int m = 0; m < M; m++)
    printf("# kaiser[%d] = %g\n",m,kaiser_window[m]);
#endif  

  // Round trip through FFT/IFFT scales by N
  float const gain = 1.;

	//shift the buffer to make it causal
  for(int n = M - 1; n >= 0; n--)
    buffer[n] = buffer[ (n-M/2+N) % N];

#if 0
  printf("#Filter time impulse response, shifted\n");
  for(int n=0;n< N;n++)
    printf("# %d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif

  // apply window and gain
  for(int n = M - 1; n >= 0; n--)
    buffer[n] = buffer[n] * kaiser_window[n] * gain;
#if 0
  printf("#Filter time impulse response, windowed and gain adjusted\n");
  for(int n=0;n< N;n++)
    printf("# %d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
	
  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if 0 
  printf("# Filter time impulse response  zero padded\n");
  for(int n=0;n< N;n++)
    printf("# %d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);

#if 0 
  printf("#Filter Frequency response amplitude\n");
  for(int n=0;n<N;n++){
    printf("#%d %.1f db\n",n,power2dB(cnrmf(buffer[n])));
  }
  printf("\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));

#if 0 
  printf("#Filter windowed FIR frequency coefficients\n");
	for(int n=0;n<N;n++){
    printf("%d,%.17f,%.17f\n", n, crealf(buffer[n]), cimagf(buffer[n]));
  }
#endif

  fftwf_free(buffer);
  return 0;
}

struct filter *filter_new(int input_length, int impulse_length){

	struct filter *f = malloc(sizeof(struct filter));
	f->L = input_length;
	f->M = impulse_length;
  f->N = f->L + f->M - 1;
  f->fir_coeff = fftwf_alloc_complex(f->N);
	
	return f;
}

int filter_tune(struct filter *f, float const low,float const high,float const kaiser_beta){

  if(isnan(low) || isnan(high) || isnan(kaiser_beta))
    return -1;

  assert(fabs(low) <= 0.5);
  assert(fabs(high) <= 0.5);

  float gain = 1./((float)f->N);
	//printf("# Gain is %lf\n", gain);
	//printf("# filter elements %d\n", f->N);

  for(int n = 0; n < f->N; n++){
    float s;
		//the first half is +ve frequencies in frequency domain
    if(n <= f->N/2)
      s = (float)n / f->N;
    else	//the second half is -ve frequencies, inverted
      s = (float)(n-f->N) / f->N;

    if(s >= low && s <= high)
      f->fir_coeff[n] = gain;
    else
      f->fir_coeff[n] = 0;
//		printf("#1 %d  %g  %g %g before windowing: %g,%g\n", n, s, low, high, creal(f->fir_coeff[n]), cimag(f->fir_coeff[n]));
  }


  window_filter(f->L, f->M, f->fir_coeff, kaiser_beta);
  return 0;
}

void filter_print(struct filter *f){

  printf("#Filter windowed FIR frequency coefficients\n");
	for(int n=0;n<f->N;n++){
    printf("%d,%.17f,%.17f\n", n, crealf(f->fir_coeff[n]), cimagf(f->fir_coeff[n]));
  }
}

/*
int main(int argc, char **argv){
	float window[30];

	struct filter *f = filter_new(1024,1025);

	float low_cutoff = atof(argv[1])/22000.0;
	float high_cutoff = atof(argv[2])/22000.0;
	filter_tune(f, low_cutoff, high_cutoff, 5.0);
}
*/
