void setup();
void loop();
void display();
void redraw();
void key_pressed(char c);
int set_field(char *id, char *value);
extern int display_freq;

#define FONT_FIELD_LABEL 0
#define FONT_FIELD_VALUE 1
#define FONT_LARGE_FIELD 2
#define FONT_LARGE_VALUE 3
#define FONT_SMALL 4
#define FONT_LOG 5
#define FONT_LOG_RX 6
#define FONT_LOG_TX 7

void write_log(int style, char *text);
int macro_load(char *filename);
int macro_exec(int key, char *dest);
void macro_label(int fn_key, char *label);
void update_log_ed();
void write_call_log();

