/*
1. The sound system is kickstarted by calling sound_thread_start() with the 
device id (as a string).

2. The sound system is run in separate thread and it keeps calling sound_process()
WARNING: sound_process() is being called from a different thread. It should
return quickly before the next set of audio data is due.

3. The left channel is used for rx and the right channel is used for tx.
The left channel takes its input (between 0 and 48 KHz( from the rx, 
demodulates it and writes out to the speaker/audio output.

4. The right channel gets audio data from mic, modulates it as a signal between
0 and 48 KHz and sends it out to right channel output.

5. A number of settings for the sound card like gain, etc can be set by calling
sound_mixer(). search for this function to know how to work this.

*/
void sound_thread_start(char *device);
void sound_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples);
void	sound_thread_stop();
void sound_volume(char *card_name, char *element, int volume);
void sound_mixer(char *card_name, char *element, int make_on);
void sound_input(int loop);
