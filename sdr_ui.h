void setup();
void loop();
void display();
void redraw();
void key_pressed(char c);
int set_field(char *id, char *value);
int get_field_value(char *id, char *value);
int get_field_value_by_label(char *label, char *value);
extern int spectrum_plot[];
void remote_execute(char *command);
int remote_update_field(int i, char *text);
void web_get_spectrum(char *buff);
int web_get_console(char *buff, int max);
int remote_audio_output(int16_t *samples);
int is_in_tx();
void abort_tx();
extern int display_freq;

#define FONT_FIELD_LABEL 0
#define FONT_FIELD_VALUE 1
#define FONT_LARGE_FIELD 2
#define FONT_LARGE_VALUE 3
#define FONT_SMALL 4
#define FONT_LOG 5
#define FONT_FT8_RX 6
#define FONT_FT8_TX 7
#define FONT_SMALL_FIELD_VALUE 8
#define FONT_CW_RX 9 
#define FONT_CW_TX 10 
#define FONT_FLDIGI_RX 11
#define FONT_FLDIGI_TX 12
#define FONT_TELNET 13

void write_console(int style, char *text);
int macro_load(char *filename, char *output);
int macro_exec(int key, char *dest);
void macro_label(int fn_key, char *label);
void macro_list(char *output);
void macro_get_keys(char *output);
void update_log_ed();
void write_call_log();
time_t time_sbitx();

#define VER_STR "sbitx v2.0"
