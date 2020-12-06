
int sound_thread_start(char *device);
void sound_process(int32_t *input_i, int32_t *input_q, int32_t *output_i, int32_t *output_q, int n_samples);
void	sound_thread_stop();
void sound_volume(char *card_name, char *element, int volume);
void sound_mixer(char *card_name, char *element, int make_on);
