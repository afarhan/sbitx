#include <stdio.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <complex.h>
#include <fftw3.h>
#include <time.h>
#include "sound.h"
#include "sdr.h"

/* follows the tutorial at http://alsamodular.sourceforge.net/alsa_programming_howto.html
Next thing to try is http://www.saunalahti.fi/~s7l/blog/2005/08/21/Full%20Duplex%20ALSA

	We are using 4 bytes per sample, 
	each frame is consists of two channels of audio, hene 8 bytes 
  We are shooting for 2048 samples per period. that is 8K
  At two periods in the buffer, the buffer has to be 16K


	To simply the work, we are picking up some settings for the Wolfson codec
	as it connects to a raspberry pi. These values are interdependent
	and they will work out of the box. It takes the guess work out of
	configuring the Raspberry Pi with Wolfson codec.
*/


/*
	MIXER api

	https://alsa.opensrc.org/HowTo_access_a_mixer_control

https://android.googlesource.com/platform/hardware/qcom/audio/+/jb-mr1-dev/alsa_sound/ALSAMixer.cpp

https://github.com/bear24rw/alsa-utils/blob/master/amixer/amixer.c

There are six kinds of controls:
	playback volume 
	playback switch
	playback enumeration
	capture volume
	capture switch
	capture enumeration

examples of using amixer to mute and unmute:
amixer -c 1  set 'Output Mixer Mic Sidetone' unmute
amixer -c 1  set 'Output Mixer Mic Sidetone' mute


examples of using sound_mixer function:
'Mic' 0/1 = mute/unmute the mic
'Line' 0/1= mute/unmute the line in
'Master' 0-100 controls the earphone volume only, line out remains unaffected
'Input Mux' 1/0 take the input either from the Mic or Line In


*/

struct vfo tone_c; //these are audio tone generators

void sound_volume(char *card_name, char *element, int volume)
{
    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
		char *card;

		card = card_name;
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, element);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);

    snd_mixer_close(handle);
}

void sound_mixer(char *card_name, char *element, int make_on)
{
    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    char *card = card_name;

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, element);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

/*		if (elem)
			puts("Element found.");	
	*/
    //find out if the his element is capture side or plaback
    if(snd_mixer_selem_has_capture_switch(elem)){
			//puts("this is a capture switch.");  
	  	snd_mixer_selem_set_capture_switch_all(elem, make_on);
		}
    else if (snd_mixer_selem_has_playback_switch(elem)){
		//	puts("this is a playback switch.");
			snd_mixer_selem_set_playback_switch_all(elem, make_on);
		}
    else if (snd_mixer_selem_has_playback_volume(elem)){
			//puts("this is  playback volume");
			long volume = make_on;
    	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    	snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);
    }	
    else if (snd_mixer_selem_has_capture_volume(elem)){
		//	puts("this is a capture volume");
			long volume = make_on;
    	snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
    	snd_mixer_selem_set_capture_volume_all(elem, volume * max / 100);
    }
		else if (snd_mixer_selem_is_enumerated(elem)){
			puts("TBD: this is an enumerated capture element");
			snd_mixer_selem_set_enum_item(elem, 0, make_on);
		}
    snd_mixer_close(handle);
}

int rate = 96000; /* Sample rate */
static snd_pcm_uframes_t buff_size = 8192; /* Periodsize (bytes) */
static int n_periods_per_buffer = 2;       /* Number of periods */

static snd_pcm_t *pcm_play_handle=0;   	//handle for the pcm device
static snd_pcm_t *pcm_capture_handle=0;   	//handle for the pcm device
static snd_pcm_t *loopback_play_handle=0;   	//handle for the pcm device
static snd_pcm_t *loopback_capture_handle=0;   	//handle for the pcm device

static snd_pcm_stream_t play_stream = SND_PCM_STREAM_PLAYBACK;	//playback stream
static snd_pcm_stream_t capture_stream = SND_PCM_STREAM_CAPTURE;	//playback stream

static char	*pcm_play_name, *pcm_capture_name;
static snd_pcm_hw_params_t *hwparams;
static int exact_rate;   /* Sample rate returned by */

static int	sound_thread_continue = 0;
pthread_t sound_thread;

int use_virtual_cable = 0;

/* this function should be called just once in the application process.
Calling it frequently will result in more allocation of hw_params memory blocks
without releasing them.
The list of PCM devices available on any platform can be found by running
	aplay -L 
We have to pass the id of one of those devices to this function.
The sequence of the alsa functions must be maintained for this to work consistently

It returns a -1 if the device didn't open. The error message is on stderr.

IMPORTANT:
The sound is playback is carried on in a non-blocking way  

*/

int sound_start_play(char *device){
	//found out the correct device through aplay -L (for pcm devices)

	//pcm_play_name = strdup("plughw:1,0");	//pcm_play_name on the heap, free() it
	snd_pcm_hw_params_alloca(&hwparams);	//more alloc

	//puts a playback handle into the pointer to the pointer
	int e = snd_pcm_open(&pcm_play_handle, device, play_stream, SND_PCM_NONBLOCK);
	
	if (e < 0) {
		fprintf(stderr, "Error opening PCM playback device %s: %s\n", device, snd_strerror(e));
		return -1;
	}

	//fills up the hwparams with values, hwparams was allocated above
	e = snd_pcm_hw_params_any(pcm_play_handle, hwparams);

	if (e < 0) {
		fprintf(stderr, "*Error getting hw playback params (%d)\n", e);
		return(-1);
	}

	// set the pcm access to interleaved
	e = snd_pcm_hw_params_set_access(pcm_play_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (e < 0) {
		fprintf(stderr, "*Error setting playback access.\n");
		return(-1);
	}

  /* Set sample format */
	e = snd_pcm_hw_params_set_format(pcm_play_handle, hwparams, SND_PCM_FORMAT_S32_LE);
	if (e < 0) {
		fprintf(stderr, "*Error setting plyaback format.\n");
		return(-1);
	}


	/* Set sample rate. If the exact rate is not supported */
	/* by the hardware, use nearest possible rate.         */ 
	exact_rate = rate;
	e = snd_pcm_hw_params_set_rate_near(pcm_play_handle, hwparams, &exact_rate, 0);
	if ( e< 0) {
		fprintf(stderr, "Error setting playback rate.\n");
		return(-1);
	}
	if (rate != exact_rate)
		fprintf(stderr, "*The playback rate %d changed to %d Hz\n", rate, exact_rate);
/*	else
		fprintf(stderr, "Playback sampling rate is set to %d\n", exact_rate);
*/

	/* Set number of channels */
	if ((e = snd_pcm_hw_params_set_channels(pcm_play_handle, hwparams, 2)) < 0) {
		fprintf(stderr, "*Error setting playback channels.\n");
		return(-1);
	}


	// frame = bytes_per_sample x n_channel
	// period = frames transfered at a time (160 for voip, etc.)
	// we use two periods per buffer.
	if ((e = snd_pcm_hw_params_set_periods(pcm_play_handle, hwparams, n_periods_per_buffer, 0)) < 0) {
		fprintf(stderr, "*Error setting playback periods.\n");
		return(-1);
	}


	// the buffer size is each periodsize x n_periods
	snd_pcm_uframes_t  n_frames= (buff_size  * n_periods_per_buffer)/8;
	printf("trying for buffer size of %ld\n", n_frames);
	e = snd_pcm_hw_params_set_buffer_size_near(pcm_play_handle, hwparams, &n_frames);
	if (e < 0) {
		    fprintf(stderr, "*Error setting playback buffersize.\n");
		    return(-1);
	}

	if (snd_pcm_hw_params(pcm_play_handle, hwparams) < 0) {
		fprintf(stderr, "*Error setting playback HW params.\n");
		return(-1);
	}
//	puts("All hw params set to play sound");

	return 0;
}


int sound_start_loopback_capture(char *device){
	snd_pcm_hw_params_alloca(&hwparams);

	int e = snd_pcm_open(&loopback_capture_handle, device, capture_stream, 0);
	
	if (e < 0) {
		fprintf(stderr, "Error open loop capture device %s: %s\n", pcm_capture_name, snd_strerror(e));
		return -1;
	}

	e = snd_pcm_hw_params_any(loopback_capture_handle, hwparams);

	if (e < 0) {
		fprintf(stderr, "*Error setting capture access (%d)\n", e);
		return(-1);
	}

	e = snd_pcm_hw_params_set_access(loopback_capture_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (e < 0) {
		fprintf(stderr, "*Error setting capture access.\n");
		return(-1);
	}

  /* Set sample format */
	e = snd_pcm_hw_params_set_format(loopback_capture_handle, hwparams, SND_PCM_FORMAT_S32_LE);
	if (e < 0) {
		fprintf(stderr, "*Error setting capture format.\n");
		return(-1);
	}

	/* Set sample rate. If the exact rate is not supported */
	/* by the hardware, use nearest possible rate.         */ 
	exact_rate = rate;
	e = snd_pcm_hw_params_set_rate_near(loopback_capture_handle, hwparams, &exact_rate, 0);
	if ( e< 0) {
		fprintf(stderr, "*Error setting capture rate.\n");
		return(-1);
	}

	if (rate != exact_rate)
		fprintf(stderr, "#The capture rate %d changed to %d Hz\n", rate, exact_rate);

	/* Set number of channels */
	if ((e = snd_pcm_hw_params_set_channels(loopback_capture_handle, hwparams, 2)) < 0) {
		fprintf(stderr, "*Error setting capture channels.\n");
		return(-1);
	}

	/* Set number of periods. Periods used to be called fragments. */ 
	if ((e = snd_pcm_hw_params_set_periods(loopback_capture_handle, hwparams, 16, 0)) < 0) {
		fprintf(stderr, "*Error setting capture periods.\n");
		return(-1);
	}


	// the buffer size is each periodsize x n_periods
	snd_pcm_uframes_t  n_frames= (buff_size  * n_periods_per_buffer)/ 8;
	//printf("trying for buffer size of %ld\n", n_frames);
	e = snd_pcm_hw_params_set_buffer_size_near(loopback_play_handle, hwparams, &n_frames);
	if (e < 0) {
		    fprintf(stderr, "*Error setting capture buffersize.\n");
		    return(-1);
	}

	if (snd_pcm_hw_params(loopback_capture_handle, hwparams) < 0) {
		fprintf(stderr, "*Error setting capture HW params.\n");
		return(-1);
	}

	return 0;
}

/*
The capture is opened in a blocking mode, the read function will block until 
there are enough samples to return a block.
This ensures that the blocks are returned in perfect timing with the codec's clock
Once you process these captured samples and send them to the playback device, you
just wait for the next block to arrive 
*/

int sound_start_capture(char *device){
	snd_pcm_hw_params_alloca(&hwparams);

	int e = snd_pcm_open(&pcm_capture_handle, device,  	capture_stream, 0);
	
	if (e < 0) {
		fprintf(stderr, "Error opening PCM capture device %s: %s\n", pcm_capture_name, snd_strerror(e));
		return -1;
	}

	e = snd_pcm_hw_params_any(pcm_capture_handle, hwparams);

	if (e < 0) {
		fprintf(stderr, "*Error setting capture access (%d)\n", e);
		return(-1);
	}

	e = snd_pcm_hw_params_set_access(pcm_capture_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (e < 0) {
		fprintf(stderr, "*Error setting capture access.\n");
		return(-1);
	}

  /* Set sample format */
	e = snd_pcm_hw_params_set_format(pcm_capture_handle, hwparams, SND_PCM_FORMAT_S32_LE);
	if (e < 0) {
		fprintf(stderr, "*Error setting capture format.\n");
		return(-1);
	}


	/* Set sample rate. If the exact rate is not supported */
	/* by the hardware, use nearest possible rate.         */ 
	exact_rate = rate;
	e = snd_pcm_hw_params_set_rate_near(pcm_capture_handle, hwparams, &exact_rate, 0);
	if ( e< 0) {
		fprintf(stderr, "*Error setting capture rate.\n");
		return(-1);
	}

	if (rate != exact_rate)
		printf("#The capture rate %d changed to %d Hz\n", rate, exact_rate);


	/* Set number of channels */
	if ((e = snd_pcm_hw_params_set_channels(pcm_capture_handle, hwparams, 2)) < 0) {
		fprintf(stderr, "*Error setting capture channels.\n");
		return(-1);
	}


	/* Set number of periods. Periods used to be called fragments. */ 
	if ((e = snd_pcm_hw_params_set_periods(pcm_capture_handle, hwparams, n_periods_per_buffer, 0)) < 0) {
		fprintf(stderr, "*Error setting capture periods.\n");
		return(-1);
	}


	// the buffer size is each periodsize x n_periods
	snd_pcm_uframes_t  n_frames= (buff_size  * n_periods_per_buffer)/ 8;
	//printf("trying for buffer size of %ld\n", n_frames);
	e = snd_pcm_hw_params_set_buffer_size_near(pcm_play_handle, hwparams, &n_frames);
	if (e < 0) {
		    fprintf(stderr, "*Error setting capture buffersize.\n");
		    return(-1);
	}

	if (snd_pcm_hw_params(pcm_capture_handle, hwparams) < 0) {
		fprintf(stderr, "*Error setting capture HW params.\n");
		return(-1);
	}

	return 0;
}

int sound_start_loopback_play(char *device){
	//found out the correct device through aplay -L (for pcm devices)

	snd_pcm_hw_params_alloca(&hwparams);	//more alloc

	int e = snd_pcm_open(&loopback_play_handle, device, play_stream, SND_PCM_NONBLOCK);
	
	if (e < 0) {
		fprintf(stderr, "Error opening loopback playback device %s: %s\n", device, snd_strerror(e));
		return -1;
	}

	e = snd_pcm_hw_params_any(loopback_play_handle, hwparams);

	if (e < 0) {
		fprintf(stderr, "*Error getting loopback playback params (%d)\n", e);
		return(-1);
	}

	e = snd_pcm_hw_params_set_access(loopback_play_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (e < 0) {
		fprintf(stderr, "*Error setting loopback access.\n");
		return(-1);
	}

  /* Set sample format */
	e = snd_pcm_hw_params_set_format(loopback_play_handle, hwparams, SND_PCM_FORMAT_S32_LE);
	if (e < 0) {
		fprintf(stderr, "*Error setting loopback format.\n");
		return(-1);
	}


	/* Set sample rate. If the exact rate is not supported */
	/* by the hardware, use nearest possible rate.         */ 
	exact_rate = rate;
	e = snd_pcm_hw_params_set_rate_near(loopback_play_handle, hwparams, &exact_rate, 0);
	if ( e< 0) {
		fprintf(stderr, "Error setting playback rate.\n");
		return(-1);
	}
	if (rate != exact_rate)
		fprintf(stderr, "*The loopback playback rate %d changed to %d Hz\n", rate, exact_rate);
/*	else
		fprintf(stderr, "Playback sampling rate is set to %d\n", exact_rate);
*/

	/* Set number of channels */
	if ((e = snd_pcm_hw_params_set_channels(loopback_play_handle, hwparams, 2)) < 0) {
		fprintf(stderr, "*Error setting playback channels.\n");
		return(-1);
	}


	// frame = bytes_per_sample x n_channel
	// period = frames transfered at a time (160 for voip, etc.)
	// we use two periods per buffer.
	if ((e = snd_pcm_hw_params_set_periods(loopback_play_handle, hwparams, 8, 0)) < 0) {
		fprintf(stderr, "*Error setting playback periods.\n");
		return(-1);
	}


	// the buffer size is each periodsize x n_periods
	snd_pcm_uframes_t  n_frames= (buff_size  * n_periods_per_buffer)/8;
	printf("trying for buffer size of %ld\n", n_frames);
	e = snd_pcm_hw_params_set_buffer_size_near(loopback_play_handle, hwparams, &n_frames);
	if (e < 0) {
		    fprintf(stderr, "*Error setting loopback playback buffersize.\n");
		    return(-1);
	}

	if (snd_pcm_hw_params(loopback_play_handle, hwparams) < 0) {
		fprintf(stderr, "*Error setting loopback playback HW params.\n");
		return(-1);
	}
	puts("All hw params set to loop sound");

	return 0;
}

void sound_process2(int32_t *input_i, int32_t *input_q, int32_t *output_i, int32_t *output_q, int n_samples){
 
	for (int i= 0; i < n_samples; i++){
		output_i[i] = input_i[i];
		output_q[i] = 0;
	}	
}

static int count = 0;
static struct timespec gettime_now;
static long int last_time = 0;
static long int last_sec = 0;
static int nframes = 0;
int sound_loop(){
	int32_t		*line_in, *data_in, *data_out, *input_i, *output_i, *input_q, *output_q;
  int pcmreturn, i, j, loopreturn;
  short s1, s2;
  int frames;

	//we allocate enough for two channels of in32_t sized samples	
  data_in = (int32_t *)malloc(buff_size * 2);
  line_in = (int32_t *)malloc(buff_size * 2);
  data_out = (int32_t *)malloc(buff_size * 2);
  input_i = (int32_t *)malloc(buff_size * 2);
  output_i = (int32_t *)malloc(buff_size * 2);
  input_q = (int32_t *)malloc(buff_size * 2);
  output_q = (int32_t *)malloc(buff_size * 2);

  frames = buff_size / 8;
	
  snd_pcm_prepare(pcm_play_handle);
  snd_pcm_writei(pcm_play_handle, data_out, frames);
  snd_pcm_writei(pcm_play_handle, data_out, frames);
 
  while(sound_thread_continue) {
    printf("[%d ", count++);
		//restart the pcm capture if there is an error reading the samples
		//this is opened as a blocking device, hence we derive accurate timing 
    if (use_virtual_cable)
  	  while((pcmreturn = snd_pcm_readi(loopback_capture_handle, data_in, frames)) < 0){ 
        if (pcmreturn == -EBADFD)
          printf("EBADFD ");
        else if (pcmreturn == -EPIPE)
          printf("EPIPE ");
        else if (pcmreturn == -ESTRPIPE)
          printf("ESTRPIPE ");
    //   else  
    //      printf("E %d ", pcmreturn);
        putchar('a');
        snd_pcm_prepare(loopback_capture_handle);
      }  
    else 
  	  while ((pcmreturn = snd_pcm_readi(pcm_capture_handle, 
								data_in,
								frames)) < 0) {
        snd_pcm_prepare(pcm_capture_handle);
  	  }

    putchar('b');
		//there are two samples of 32-bit in each frame
		i = 0; 
		j = 0;	
		while (i < pcmreturn){
			input_i[i] = data_in[j++];
			input_q[i] = data_in[j++];
			i++;
		}
    if (use_virtual_cable)
		  sound_process2(input_i, input_q, output_i, output_q, pcmreturn);
    else
		  sound_process(input_i, input_q, output_i, output_q, pcmreturn);
    putchar('2');
	//	sound_process2(input_i, input_q, output_i, output_q, pcmreturn);

		i = 0; 
		j = 0;	
		while (i < pcmreturn){
			data_out[j++] = output_i[i];
			data_out[j++] = output_q[i++];
		}
		putchar('c');
    while ((pcmreturn = snd_pcm_writei(pcm_play_handle, 
			data_out, frames)) < 0) {
       snd_pcm_prepare(pcm_play_handle);
      putchar('d');
    }
    while((pcmreturn = snd_pcm_writei(loopback_play_handle, 
			data_out, frames)) < 0){
        putchar('e');
        /* if (pcmreturn == -EPIPE)
          printf(" EPIPE ");
        else */ if (pcmreturn == -EBADFD)
          printf(" EBADFD ");
        else if (pcmreturn == -ESTRPIPE)
          printf(" ESTRPIPE ");
       snd_pcm_prepare(loopback_play_handle);
			//puts("preparing loopback");
    } 
    printf("]\n");
	}
  printf("********Ending sound thread\n");
}

//check that we haven't free()-ed up the hwparams block
//don't call this function at all until that is fixed
//you don't have to call it anyway
void sound_stop(){
	snd_pcm_drop(pcm_play_handle);
	snd_pcm_drain(pcm_play_handle);

	snd_pcm_drop(pcm_capture_handle);
	snd_pcm_drain(pcm_capture_handle);
}


/*
We process the sound in a background thread.
It will call the user-supplied function sound_process()  
*/
void *sound_thread_function(void *ptr){
	char *device = (char *)ptr;
	struct sched_param sch;

	//switch to maximum priority
	sch.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_setschedparam(sound_thread, SCHED_FIFO, &sch);

  printf("opening %s sound card\n", device);	
	if (sound_start_play(device)){
		fprintf(stderr, "*Error opening play device");
		return NULL;
	}

  printf("opening hw:4,0 sound card\n");	
	if(sound_start_loopback_play("hw:4,0")){
		fprintf(stderr, "*Error opening loopback play device");
		return NULL;
	}

	if (sound_start_capture(device)){
		fprintf(stderr, "*Error opening capture device");
		return NULL;
	}

	if (sound_start_loopback_capture("plughw:1,0")){
		fprintf(stderr, "*Error opening loopback capture device");
		return NULL;
	}
	sound_thread_continue = 1;
  vfo_start(&tone_c, 700, 0);
	sound_loop();
	sound_stop();
}


int sound_thread_start(char *device){
	pthread_create( &sound_thread, NULL, sound_thread_function, (void*)device);
}

void sound_thread_stop(){
	sound_thread_continue = 0;
}

void sound_use_loop_input(int loop){
  if (loop)
    use_virtual_cable = 1;
  else
    use_virtual_cable = 0;
}

//demo, uncomment it to test it out
/*
void sound_process(int32_t *input_i, int32_t *input_q, int32_t *output_i, int32_t *output_q, int n_samples){
 
	for (int i= 0; i < n_samples; i++){
		output_i[i] = input_i[i];
		output_q[i] = input_q[i];
	}	
}

void main(int argc, char **argv){
	sound_thread_start("plughw:0,0");
	sleep(10);
	sound_thread_stop();
	sleep(10);
}
*/
