/*
The initial sync between the gui values, the core radio values, settings, et al are manually set.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <math.h>
#include <fcntl.h>
#include <complex.h>
#include <fftw3.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <cairo.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include "sdr.h"
#include "sound.h"
#include "sdr_ui.h"
#include "ini.h"
#include "hamlib.h"
#include "remote.h"
#include "remote.h"
#include "wsjtx.h"

/* Front Panel controls */
char pins[15] = {0, 2, 3, 6, 7, 
								10, 11, 12, 13, 14, 
								21, 22, 23, 25, 27};

#define ENC1_A (13)
#define ENC1_B (12)
#define ENC1_SW (14)

#define ENC2_A (0)
#define ENC2_B (2)
#define ENC2_SW (3)

#define SW5 (22)
#define PTT (7)
#define DASH (21)

#define ENC_FAST 1
#define ENC_SLOW 5

//time sync, when the NTP time is not synced, this tracks the number of seconds 
//between the system cloc and the actual time set by \utc command
static unsigned long time_delta = 0;

//mouse/touch screen state
static int mouse_down = 0;
static int last_mouse_x = -1;
static int last_mouse_y = -1;

//encoder state
struct encoder {
	int pin_a,  pin_b;
	int speed;
	int prev_state;
	int history;
};
void tuning_isr(void);
void cw_paddle_isr(void);
volatile int cw_paddle_isr_key_memory = 0;

#define COLOR_SELECTED_TEXT 0
#define COLOR_TEXT 1
#define COLOR_TEXT_MUTED 2
#define COLOR_SELECTED_BOX 3 
#define COLOR_BACKGROUND 4
#define COLOR_FREQ 5
#define COLOR_LABEL 6
#define SPECTRUM_BACKGROUND 7
#define SPECTRUM_GRID 8
#define SPECTRUM_PLOT 9
#define SPECTRUM_NEEDLE 10
#define COLOR_CONTROL_BOX 11
#define SPECTRUM_BANDWIDTH 12
#define SPECTRUM_PITCH 13
#define SELECTED_LINE 14

float palette[][3] = {
	{1,1,1}, 		// COLOR_SELECTED_TEXT
	{0,1,1},		// COLOR_TEXT
	{0.5,0.5,0.5}, //COLOR_TEXT_MUTED
	{1,1,1},		// COLOR_SELECTED_BOX
	{0,0,0},		// COLOR_BACKGROUND
	{1,1,0},		//COLOR_FREQ
	{1,0,1},		//COLOR_LABEL
	//spectrum
	{0,0,0},	//SPECTRUM_BACKGROUND
	{0.1, 0.1, 0.1}, //SPECTRUM_GRID
	{1,1,0},	//SPECTRUM_PLOT
	{0.2,0.2,0.2}, 	//SPECTRUM_NEEDLE
	{0.5,0.5,0.5}, //COLOR_CONTROL_BOX
	{0.2, 0.2, 0.2}, //SPECTRUM_BANDWIDTH
	{1,0,0},	//SPECTRUM_PITCH
	{0.1, 0.1, 0.2} //SELECTED_LINE
};

char *ui_font = "Sans";
int field_font_size = 12;
// we just use a look-up table to define the fonts used
// the struct field indexes into this table
struct font_style {
	int index;
	double r, g, b;
	char name[32];
	int height;
	int weight;
	int type;
};

guint key_modifier = 0;

struct font_style font_table[] = {
	{FONT_FIELD_LABEL, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FIELD_VALUE, 1, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_FIELD, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_VALUE, 1, 1, 1, "Arial", 24, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL, 0, 1, 1, "Mono", 10, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LOG, 1, 1, 1, "Mono", 12, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LOG_RX, 0, 1, 0, "Mono", 12, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LOG_TX, 1, 0.6, 0, "Mono", 12, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL_FIELD_VALUE, 1, 1, 1, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
};

struct encoder enc_a, enc_b;

#define MAX_FIELD_LENGTH 128

#define FIELD_NUMBER 0
#define FIELD_BUTTON 1
#define FIELD_TOGGLE 2
#define FIELD_SELECTION 3
#define FIELD_TEXT 4
#define FIELD_STATIC 5
#define FIELD_CONSOLE 6

// The console is a series of lines
#define MAX_CONSOLE_BUFFER 10000
#define MAX_LINE_LENGTH 128
#define MAX_CONSOLE_LINES 500
static int 	console_cols = 50;

//we use just one text list in our user interface

struct console_line {
	char text[MAX_LINE_LENGTH];
	int style;
};
static int console_style = FONT_LOG;
static struct console_line console_stream[MAX_CONSOLE_LINES];
int console_current_line = 0;
int	console_selected_line = -1;


// event ids, some of them are mapped from gtk itself
#define FIELD_DRAW 0
#define FIELD_UPDATE 1 
#define FIELD_EDIT 2
#define MIN_KEY_UP 0xFF52
#define MIN_KEY_DOWN	0xFF54
#define MIN_KEY_LEFT 0xFF51
#define MIN_KEY_RIGHT 0xFF53
#define MIN_KEY_ENTER 0xFF0D
#define MIN_KEY_ESC	0xFF1B
#define MIN_KEY_BACKSPACE 0xFF08
#define MIN_KEY_TAB 0xFF09
#define MIN_KEY_CONTROL 0xFFE3
#define MIN_KEY_F1 0xFFBE
#define MIN_KEY_F2 0xFFBF
#define MIN_KEY_F3 0xFFC0
#define MIN_KEY_F4 0xFFC1
#define MIN_KEY_F5 0xFFC2
#define MIN_KEY_F6 0xFFC3
#define MIN_KEY_F7 0xFFC4
#define MIN_KEY_F8 0xFFC5
#define MIN_KEY_F9 0xFFC6
#define MIN_KEY_F9 0xFFC6
#define MIN_KEY_F10 0xFFC7
#define MIN_KEY_F11 0xFFC8
#define MIN_KEY_F12 0xFFC9
#define COMMAND_ESCAPE '\\'

void set_ui(int id);

/* 	the field in focus will be exited when you hit an escape
		the field in focus will be changeable until it loses focus
		hover will always be on the field in focus.
		if the focus is -1,then hover works
*/

/*
	Warning: The field selection is used for TOGGLE and SELECTION fields
	each selection by the '/' should be unique. otherwise, the simple logic will
	get confused 
*/


//the main app window
GtkWidget *window;
GtkWidget *display_area = NULL;

// these are callbacks called by the operating system
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, 
	gpointer user_data); 
static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_mouse_press (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_mouse_move (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_mouse_release (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_scroll (GtkWidget *widget, GdkEventScroll *event, 
	gpointer data); 
static gboolean on_window_state (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, 
	gpointer user_data);
gboolean ui_tick(gpointer gook);

static int measure_text(cairo_t *gfx, char *text, int font_entry){
	cairo_text_extents_t ext;
	struct font_style *s = font_table + font_entry;
	
	cairo_select_font_face(gfx, s->name, s->type, s->weight);
	cairo_set_font_size(gfx, s->height);
	cairo_move_to(gfx, 0, 0);
	cairo_text_extents(gfx, text, &ext);
	return (int) ext.x_advance;
}

static void draw_text(cairo_t *gfx, int x, int y, char *text, int font_entry){
	struct font_style *s  = font_table + font_entry;
  cairo_set_source_rgb( gfx, s->r, s->g, s->b);
	cairo_select_font_face(gfx, s->name, s->type, s->weight);
	cairo_set_font_size(gfx, s->height);
	cairo_move_to(gfx, x, y + s->height);
	cairo_show_text(gfx, text);
	//printf("drawing '%s' with font %s / %d\n", text, s->name, s->height);
}

static void fill_rect(cairo_t *gfx, int x, int y, int w, int h, int color){
  cairo_set_source_rgb( gfx, palette[color][0], palette[color][1], palette[color][2]);
	cairo_rectangle(gfx, x, y, w, h);
  cairo_fill(gfx);
}

static void rect(cairo_t *gfx, int x, int y, int w, int h, 
	int color, int thickness){

  cairo_set_source_rgb( gfx, 
		palette[color][0], 
		palette[color][1], 
		palette[color][2]);

	cairo_set_line_width(gfx, thickness);
	cairo_rectangle(gfx, x, y, w, h);
  cairo_stroke(gfx);
}


/****************************************************************************
	Using the above hooks and primitives, we build user interface controls,
	All of them are defined by the struct field
****************************************************************************/


struct field {
	char	*cmd;
	int		(*fn)(struct field *f, cairo_t *gfx, int event, int param_a, int param_b, int param_c);
	int		x, y, width, height;
	char	label[30];
	int 	label_width;
	char	value[MAX_FIELD_LENGTH];
	char	value_type; //NUMBER, SELECTION, TEXT, TOGGLE, BUTTON
	int 	font_index; //refers to font_style table
	char  selection[1000];
	long int	 	min, max;
  int step;
};

#define STACK_DEPTH 4

struct band {
	char name[10];
	int	start;
	int	stop;
	int	power;
	int	max;
	int index;
	int	freq[STACK_DEPTH];
	int mode[STACK_DEPTH];
};

struct cmd {
	char *cmd;
	int (*fn)(char *args[]);
};


static unsigned long focus_since = 0;
static struct field *f_focus = NULL;
static struct field *f_hover = NULL;
//variables to power up and down the tx
static int in_tx = 0;
static int key_down = 0;
static unsigned long tx_start_time = 0;

static int *tx_mod_buff = NULL;
static int tx_mod_index = 0;
static int tx_mod_max = 0;

char*mode_name[MAX_MODES] = {
	"USB", "LSB", "CW", "CWR", "NBFM", "AM", "FT8", "PSK31", "RTTY", 
	"DIGITAL", "2TONE" 
};

static int serial_fd = -1;
static int xit = 512; 
static long int tuning_step = 1000;
static int tx_mode = MODE_USB;


#define BAND80M	0
#define BAND40M	1
#define BAND30M 2	
#define BAND20M 3	
#define BAND17M 4	
#define BAND15M 5
#define BAND12M 6 
#define BAND10M 7 

struct band band_stack[] = {
	{"80m", 3500000, 4000000, 40, 40, 0, 
		{3500000,3574000,3600000,3700000},{MODE_CW, MODE_USB, MODE_CW,MODE_LSB}},
	{"40m", 7000000,7300000, 40, 40, 0,
		{7000000,7040000,7074000,7150000},{MODE_CW, MODE_CW, MODE_USB, MODE_LSB}},
	{"30m", 10100000, 10150000, 30, 30, 0,
		{10100000, 10100000, 10136000, 10150000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"20m", 14000000, 14400000, 30,30, 0,
		{14010000, 14040000, 14074000, 14200000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"17m", 18068000, 18168000, 25,25, 0,
		{18068000, 18100000, 18110000, 18160000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"15m", 21000000, 21500000, 20,20, 0,
		{21010000, 21040000, 21074000, 21250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"12m", 24890000, 24990000, 10, 10, 0,
		{24890000, 24910000, 24950000, 24990000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"10m", 28000000, 29700000, 6, 6, 0,
		{28000000, 28040000, 28074000, 28250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
};


#define VFO_A 0 
#define VFO_B 1 
int	vfo_a_freq = 7000000;
int	vfo_b_freq = 14000000;
char vfo_a_mode[10];
char vfo_b_mode[10];

//usefull data for macros, logging, etc
char mycallsign[12];
char mygrid[12];
char current_macro[32];
char contact_callsign[12];
char contact_grid[10];
char sent_rst[10];
char received_rst[10];
char sent_exchange[10];
char received_exchange[10];
int	contest_serial = 0;

int	tx_id = 0;

//recording duration in seconds
time_t record_start = 0;
int	data_delay = 700;
int cw_input_method = CW_KBD;
int	cw_delay = 1000;
int	cw_tx_pitch = 700;
int sidetone = 25;

#define MAX_RIT 25000
//how much to shift on rit
int	rit_delta = 0;

static int redraw_flag = 1; 
int screen_width, screen_height;
int spectrum_span = 48000;


void do_cmd(char *cmd);
void cmd_exec(char *cmd);


int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_waterfall(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_tuning(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_text(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_pitch(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_mouse_move(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_record(struct field *f, cairo_t *gfx, int event, int a, int b, int c);

struct field *active_layout = NULL;
char settings_updated = 0;
#define LAYOUT_KBD 0
#define LAYOUT_MACROS 1
int current_layout = LAYOUT_KBD;

// the cmd fields that have '#' are not to be sent to the sdr
struct field main_controls[] = {
	{ "r1:freq", do_tuning, 600, 0, 150, 49, "", 5, "14000000", FIELD_NUMBER, FONT_LARGE_VALUE, 
		"", 500000, 30000000, 100},

	// Main RX
	{ "r1:volume", NULL, 750, 330, 50, 50, "AUDIO", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1},
	{ "r1:mode", NULL, 500, 330, 50, 50, "MODE", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"USB/LSB/CW/CWR/FT8/PSK31/RTTY/DIGITAL/2TONE", 0,0, 0},
	{ "r1:low", NULL, 550, 330, 50, 50, "LOW", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0,4000, 50},
	{ "r1:high", NULL, 600, 330, 50, 50, "HIGH", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 300, 10000, 50},

	{ "r1:agc", NULL, 650, 330, 50, 50, "AGC", 40, "SLOW", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"OFF/SLOW/MED/FAST", 0, 1024, 1},
	{ "r1:gain", NULL, 700, 330, 50, 50, "IF", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1},

	//tx 
	{ "tx_power", NULL, 550, 430, 50, 50, "WATTS", 40, "40", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1},
	{ "tx_gain", NULL, 550, 380, 50, 50, "MIC", 40, "50", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1},

	{ "#split", NULL, 750, 380, 50, 50, "SPLIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0},
	{ "tx_compress", NULL, 600, 380, 50, 50, "COMP", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"ON/OFF", 0,100,10},
	{"#rit", NULL, 550, 0, 50, 50, "RIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0},
	{ "#tx_wpm", NULL, 650, 380, 50, 50, "WPM", 40, "12", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 1, 50, 1},
	{ "#rx_pitch", do_pitch, 700, 380, 50, 50, "PITCH", 40, "600", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 100, 3000, 10},
	
	{ "#tx", NULL, 600, 430, 50, 50, "TX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0},

	{ "#rx", NULL, 650, 430, 50, 50, "RX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0},
	
	{ "#record", do_record, 700, 430, 50, 50, "REC", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0, 0},

	// top row
	{"#step", NULL, 400, 0 ,50, 50, "STEP", 1, "10Hz", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"1MHz/100KHz/10KHz/1KHz/100Hz/10Hz", 0,0,0},
	{"#vfo", NULL, 450, 0 ,50, 50, "VFO", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"A/B", 0,0,0},
	{"#span", NULL, 500, 0 ,50, 50, "SPAN", 1, "25KHz", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"25KHz/10KHz/6KHz/2.5KHz", 0,0,0},

	{"spectrum", do_spectrum, 400, 80, 400, 100, "Spectrum ", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0},  
	{"#status", do_status, 400, 51, 400, 29, "00:00:00", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"status", 0,0,0},  
	{"waterfall", do_waterfall, 400, 181, 400, 148, "Waterfall ", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0},
	{"#console", do_console, 0, 0 , 400, 320, "console", 70, "console box", FIELD_CONSOLE, FONT_LOG, 
		"nothing valuable", 0,0,0},
	{"#log_ed", NULL, 0, 320, 400, 20, "", 70, "", FIELD_STATIC, FONT_LOG, 
		"nothing valuable", 0,128,0},
	{"#text_in", do_text, 0, 340, 398, 20, "text", 70, "text box", FIELD_TEXT, FONT_LOG, 
		"nothing valuable", 0,128,0},



	{"#close", NULL, 750, 430 ,50, 50, "CLOSE", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#off", NULL, 750, 0 ,50, 50, "OFF", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
  
  // other settings - currently off screen
  { "reverse_scrolling", NULL, 1000, 1000, 50, 50, "RS", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0},
  { "tuning_acceleration", NULL, 1000, 1000, 50, 50, "TA", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0},
  { "tuning_accel_thresh1", NULL, 1000, 1000, 50, 50, "TAT1", 40, "10000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100},
  { "tuning_accel_thresh2", NULL, 1000, 1000, 50, 50, "TAT2", 40, "500", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100},
  { "mouse_pointer", NULL, 1000, 1000, 50, 50, "MP", 40, "LEFT", FIELD_SELECTION, FONT_FIELD_VALUE,
    "BLANK/LEFT/RIGHT/CROSSHAIR", 0,0,0},

	/* band stack registers */
	{"#10m", NULL, 400, 330, 50, 50, "10M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#12m", NULL, 450, 330, 50, 50, "12M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#15m", NULL, 400, 380, 50, 50, "15M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#17m", NULL, 450, 380, 50, 50, "17M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#20m", NULL, 500, 380, 50, 50, "20M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#30m", NULL, 400, 430, 50, 50, "30M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#40m", NULL, 450, 430, 50, 50, "40M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},
	{"#80m", NULL, 500, 430, 50, 50, "80M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0},

	//soft keyboard
	{"#kbd_q", do_kbd, 0, 360 ,40, 30, "#", 1, "q", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_w", do_kbd, 40, 360, 40, 30, "1", 1, "w", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_e", do_kbd, 80, 360, 40, 30, "2", 1, "e", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_r", do_kbd, 120, 360, 40, 30, "3", 1, "r", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_t", do_kbd, 160, 360, 40, 30, "(", 1, "t", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_y", do_kbd, 200, 360, 40, 30, ")", 1, "y", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_u", do_kbd, 240, 360, 40, 30, "_", 1, "u", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_i", do_kbd, 280, 360, 40, 30, "-", 1, "i", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_o", do_kbd, 320, 360, 40, 30, "+", 1, "o", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#kbd_p", do_kbd, 360, 360, 40, 30, "@", 1, "p", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#kbd_a", do_kbd, 0, 390 ,40, 30, "*", 1, "a", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_s", do_kbd, 40, 390, 40, 30, "4", 1, "s", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_d", do_kbd, 80, 390, 40, 30, "5", 1, "d", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_f", do_kbd, 120, 390, 40, 30, "6", 1, "f", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_g", do_kbd, 160, 390, 40, 30, "/", 1, "g", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_h", do_kbd, 200, 390, 40, 30, ":", 1, "h", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_j", do_kbd, 240, 390, 40, 30, ";", 1, "j", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_k", do_kbd, 280, 390, 40, 30, "'", 1, "k", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_l", do_kbd, 320, 390, 40, 30, "\"", 1, "l", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_bs", do_kbd, 360, 390, 40, 30, "", 1, "DEL", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0},
 
	{"#kbd_alt", do_kbd, 0, 420 ,40, 30, "", 1, "Alt", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_z", do_kbd, 40, 420, 40, 30, "7", 1, "z", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_x", do_kbd, 80, 420, 40, 30, "8", 1, "x", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_c", do_kbd, 120, 420, 40, 30, "9", 1, "c", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_v", do_kbd, 160, 420, 40, 30, "?", 1, "v", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_b", do_kbd, 200, 420, 40, 30, "!", 1, "b", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_n", do_kbd, 240, 420, 40, 30, ",", 1, "n", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_m", do_kbd, 280, 420, 40, 30, ".", 1, "m", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_enter", do_kbd, 320, 420, 80, 30, "", 1, "Enter", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#kbd_cmd", do_kbd, 0, 450, 80, 30, "", 1, "\\cmd", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_0", do_kbd, 80, 450, 40, 30, "", 1, "0", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_ ", do_kbd, 120, 450, 120, 30, "", 1, " SPACE ", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_.", do_kbd, 240, 450, 40, 30, "\"", 1, ".", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_?", do_kbd, 280, 450, 40, 30, "?", 1, "?", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 
	{"#kbd_macro", do_kbd, 320, 450, 80, 30, "", 1, "Macro", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 


	//macros keyboard

	//row 1
	{"#mf1", do_macro, 0, 1360, 65, 40, "F1", 1, "CQ", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf2", do_macro, 65, 1360, 65, 40, "F2", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf3", do_macro, 130, 1360, 65, 40, "F3", 1, "Reply", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf4", do_macro, 195, 1360, 65, 40, "F4", 1, "RRR", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf5", do_macro, 260, 1360, 70, 40, "F5", 1, "73", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf6", do_macro, 330, 1360, 70, 40, "F6", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	//row 2

	{"#mf7", do_macro, 0, 1400, 65, 40, "F7", 1, "Exch", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf8", do_macro, 65, 1400, 65, 40, "F8", 1, "Tu", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf9", do_macro, 130, 1400, 65, 40, "F9", 1, "Rpt", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf10", do_macro, 195, 1400, 65, 40, "F10", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf11", do_macro, 260, 1400, 70, 40, "F11", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mf12", do_macro, 330, 1400, 70, 40, "F12", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	//row 3

	{"#mfqrz", do_macro, 0, 1440, 65, 40, "QRZ", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mfwipe", do_macro, 65, 1440, 65, 40, "Wipe", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mflog", do_macro, 130, 1440, 65, 40, "Log it", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mfedit", do_macro, 195, 1440, 65, 40, "Edit", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mfspot"	, do_macro, 260, 1440, 70, 40, "Spot", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	{"#mfkbd", do_macro, 330, 1440, 70, 40, "Kbd", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0}, 

	//the last control has empty cmd field 
	{"", NULL, 0, 0 ,0, 0, "#", 1, "Q", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
};


int spectrum_display_start_freq_adjustment = 0;  // spectrum display variable start freq
int spectrum_display_filter_low_position;
int spectrum_display_filter_high_position;
int spectrum_display_pitch_position;

struct field *get_field(char *cmd);
void update_field(struct field *f);
void tx_on();
void tx_off();

//#define MAX_CONSOLE_LINES 1000
//char *console_lines[MAX_CONSOLE_LINES];
int last_log = 0;

struct field *get_field(char *cmd){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcmp(active_layout[i].cmd, cmd))
			return active_layout + i;
	return NULL;
}

struct field *get_field_by_label(char *label){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcasecmp(active_layout[i].label, label))
			return active_layout + i;
	return NULL;
}

//set the field directly to a particuarl value, programmatically
int set_field(char *id, char *value){
	struct field *f = get_field(id);
	int v;

	if (!f){
		printf("*Error: field[%s] not found. Check for typo?\n", id);
		return 1;
	}

	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(value);
		if (v < f->min)
			v = f->min;
		if (v > f->max)
			v = f->max;
		sprintf(f->value, "%d",  v);
	}
	else if (f->value_type == FIELD_SELECTION || f->value_type == FIELD_TOGGLE){
		// toggle and selection are the same type: toggle has just two values instead of many more
		char *p, *prev, *next, b[100];
		//search the current text in the selection
		prev = NULL;
		strcpy(b, f->selection);
		p = strtok(b, "/");
		while(p){
			if (!strcmp(value, p))
				break;
			else
				prev = p;
			p = strtok(NULL, "/");
		}	
		//set to the first option
		if (p == NULL){
			if (prev)
				strcpy(f->value, prev);
			printf("*Error: setting field[%s] to [%s] not permitted\n", f->cmd, value);
			return 1;
		}
		else
			strcpy(f->value, value);
	}
	else if (f->value_type == FIELD_BUTTON){
		strcpy(f->value, value);	
		return 1;
	}
	else if (f->value_type == FIELD_TEXT){
		if (strlen(value) > f->max || strlen(value) < f->min){
			printf("*Error: field[%s] can't be set to [%s], improper size.\n", f->cmd, value);
			return 1;
		}
		else
			strcpy(f->value, value);
	}

	//send a command to the receiver
	char buff[200];
	sprintf(buff, "%s=%s", f->cmd, f->value);
	do_cmd(buff);
	update_field(f);
	redraw_flag++;
	return 0;
}

// log is a special field that essentially is a like text
// on a terminal

void console_init(){
	for (int i =0;  i < MAX_CONSOLE_LINES; i++){
		console_stream[i].text[0] = 0;
		console_stream[i].style = console_style;
	}
}

int console_init_next_line(){
	console_current_line++;
	if (console_current_line == MAX_CONSOLE_LINES)
		console_current_line = console_style;
	console_stream[console_current_line].text[0] = 0;	
	console_stream[console_current_line].style = console_style;
	return console_current_line;
}

void write_console(int style, char *text){
	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/display_log.txt");
	FILE *pf = fopen(directory, "a");

	//move to a new line if the style has changed
	if (style != console_style){
		console_style = style;
		if (strlen(console_stream[console_current_line].text)> 0)
			console_init_next_line();	
		console_stream[console_current_line].style = style;
		switch(style){
			case_FONT_LOG_RX:
				fputs("#RX ################################\n", pf);
				break;
			case FONT_LOG_TX:
				fputs("#TX ################################\n", pf);
				break;
			default:
				fputs("#INFO ##############################\n", pf);
				break;
		}
	}

	if (strlen(text) == 0)
		return;

	//write to the scroll
	fwrite(text, strlen(text), 1, pf);
	fclose(pf);

	remote_write(text);

	while(*text){
		char c = *text;
		if (c == '\n')
			console_init_next_line();
		else if (c < 128 && c >= ' '){
			char *p = console_stream[console_current_line].text;
			int len = strlen(p);
			if(len >= console_cols - 1){
				//start a fresh line
				console_init_next_line();
				p = console_stream[console_current_line].text;
				len = 0;
			}
		
			//printf("Adding %c to %d\n", (int)c, console_current_line);	
			p[len++] = c & 0x7f;
			p[len] = 0;
		}
		text++;	
	}
	redraw_flag++;
}

void draw_log(cairo_t *gfx, struct field *f){
	char this_line[1000];
	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;

	//estimate!
	int char_width = 1+measure_text(gfx, "01234567890123456789", f->font_index)/20;
	console_cols = f->width / char_width;
	int y = f->y + 2; 
	int j = 0;

	int start_line = console_current_line - n_lines;
	if (start_line < 0)
		start_line += MAX_CONSOLE_LINES;

 	for (int i = 0; i <= n_lines; i++){
		struct console_line *l = console_stream + start_line;
		if (start_line == console_selected_line)
			fill_rect(gfx, f->x, y+1, f->width, font_table[l->style].height+1, SELECTED_LINE);
		draw_text(gfx, f->x, y, l->text, l->style);
		start_line++;
		y += line_height;
		if(start_line >= MAX_CONSOLE_LINES)
			start_line = 0;
	}
}

void draw_field(GtkWidget *widget, cairo_t *gfx, struct field *f){
	struct font_style *s = font_table + 0;

	//if there is a handling function, use that else
	//skip down to the default behaviour of the controls
	if (f->fn){
		if(f->fn(f, gfx, FIELD_DRAW, -1, -1, 0))
			return;
	}

	fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_SELECTED_BOX, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
	else if (f->value_type != FIELD_STATIC)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	int width, offset, text_length, line_start, y;	
	char this_line[MAX_FIELD_LENGTH];
	int text_line_width = 0;

	switch(f->value_type){
		case FIELD_TEXT:
			text_length = strlen(f->value);
			line_start = 0;
			y = f->y + 2;
			text_line_width = 0;
			while(text_length > 0){
				if (text_length > console_cols){
					strncpy(this_line, f->value + line_start, console_cols);
					this_line[console_cols] = 0;
				}
				else
					strcpy(this_line, f->value + line_start);		
				draw_text(gfx, f->x + 2, y, this_line, f->font_index);
				text_line_width= measure_text(gfx, this_line, f->font_index);
				y += 14;
				line_start += console_cols;
				text_length -= console_cols;
			}
			//draw the text cursor, if there is no text, the text baseline is zero
			if (strlen(f->value))
				y -= 14;
			fill_rect(gfx, f->x + text_line_width + 5, y+3, 9, 10, f->font_index);
		break;
		case FIELD_SELECTION:
		case FIELD_NUMBER:
		case FIELD_TOGGLE:
			width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
			offset = f->width/2 - width/2;
			draw_text(gfx, f->x + offset, f->y+5 ,  f->label, FONT_FIELD_LABEL);
			width = measure_text(gfx, f->value, f->font_index);
      if (width >= f->width){ // automatic button font downsizing
        width = measure_text(gfx, f->value, FONT_SMALL_FIELD_VALUE);
        offset = f->width/2 - width/2;
        if (!strlen(f->label))
          draw_text(gfx, f->x + offset , f->y+6, f->value, FONT_SMALL_FIELD_VALUE);
        else
          draw_text(gfx, f->x+offset , f->y+25 , f->value , FONT_SMALL_FIELD_VALUE);
      } else {
        offset = f->width/2 - width/2;
        if (!strlen(f->label))
          draw_text(gfx, f->x + offset , f->y+6, f->value, f->font_index);
        else
          draw_text(gfx, f->x+offset , f->y+25 , f->value , f->font_index);
      }
      break;
		case FIELD_BUTTON:
			width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
			offset = f->width/2 - width/2;
			if (strlen(f->value) == 0)
				draw_text(gfx, f->x + offset, f->y+13 , f->label , FONT_FIELD_LABEL);
			else if (f->height <= 30){
				if (strlen(f->label)){
					draw_text(gfx, f->x+5, f->y ,  f->label, FONT_FIELD_LABEL);
					draw_text(gfx, f->x+18 , f->y+8 , f->value , f->font_index);
				}
				else 
					draw_text(gfx, f->x+10 , f->y+5 , f->value , f->font_index);
			}
			else {
				if (strlen(f->label)){
					draw_text(gfx, f->x + offset, f->y+5 ,  f->label, FONT_FIELD_LABEL);
					draw_text(gfx, f->x+offset , f->y+f->height - 20 , f->value , f->font_index);
				}
				else
					draw_text(gfx, f->x+offset , f->y+15 , f->value , f->font_index);
			}	
			break;
		case FIELD_STATIC:
			draw_text(gfx, f->x, f->y, f->label, FONT_FIELD_LABEL);
			break;
		case FIELD_CONSOLE:
			//draw_log(gfx, f);
			break;
	}
}


static int mode_id(char *mode_str){
	if (!strcmp(mode_str, "CW"))
		return MODE_CW;
	else if (!strcmp(mode_str, "CWR"))
		return MODE_CWR;
	else if (!strcmp(mode_str, "USB"))
		return MODE_USB;
	else if (!strcmp(mode_str,  "LSB"))
		return MODE_LSB;
	else if (!strcmp(mode_str,  "FT8"))
		return MODE_FT8;
	else if (!strcmp(mode_str,  "PSK31"))
		return MODE_PSK31;
	else if (!strcmp(mode_str,  "RTTY"))
		return MODE_RTTY;
	else if (!strcmp(mode_str, "NBFM"))
		return MODE_NBFM;
	else if (!strcmp(mode_str, "AM"))
		return MODE_AM;
	else if (!strcmp(mode_str, "2TONE"))
		return MODE_2TONE;
	else if (!strcmp(mode_str, "DIGITAL"))
		return MODE_DIGITAL;
	return -1;
}

static void save_user_settings(int forced){
	static unsigned long last_save_at = 0;
	char file_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	//attempt to save settings only if it has been 30 seconds since the 
	//last time the settings were saved
	unsigned long now = millis();
	if ((now < last_save_at + 30000 ||  !settings_updated) && forced == 0)
		return;

	char *path = getenv("HOME");
	strcpy(file_path, path);
	strcat(file_path, "/sbitx/data/user_settings.ini");

	//copy the current freq settings to the currently selected vfo
	struct field *f_freq = get_field("r1:freq");
	struct field *f_vfo  = get_field("#vfo");

	FILE *f = fopen(file_path, "w");
	if (!f){
		printf("Unable to save %s : %s\n", file_path, strerror(errno));
		return;
	}

	//save other stuff
	fprintf(f, "vfo_a_freq=%d\n", vfo_a_freq);
	fprintf(f, "vfo_b_freq=%d\n", vfo_b_freq);
	fprintf(f, "callsign=%s\n", mycallsign);
	fprintf(f, "grid=%s\n", mygrid);
	fprintf(f, "cw_delay=%d\n", cw_delay);
	fprintf(f, "data_delay=%d\n", data_delay);
	fprintf(f, "cw_input_method=%d\n", cw_input_method);
	fprintf(f, "current_macro=%s\n", current_macro);
	fprintf(f, "sidetone=%d\n", sidetone);
	fprintf(f, "contest_serial=%d\n", contest_serial);
	fprintf(f, "sent_exchange=%s\n", sent_exchange);

  // save the field values
	for (int i= 0; i < active_layout[i].cmd[0] > 0; i++)
		fprintf(f, "%s=%s\n", active_layout[i].cmd, active_layout[i].value);

	//now save the band stack
	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		fprintf(f, "\n[%s]\n", band_stack[i].name);
		fprintf(f, "power=%d\n", band_stack[i].power);
		for (int j = 0; j < STACK_DEPTH; j++)
			fprintf(f, "freq%d=%d\nmode%d=%d\n", j, band_stack[i].freq[j], j, band_stack[i].mode[j]);
	}


	fclose(f);
	settings_updated = 0;
}

static int user_settings_handler(void* user, const char* section, 
            const char* name, const char* value)
{
    char cmd[1000];
    char new_value[200];

		//printf("[%s] setting %s = %s\n", section, name, value);
    strcpy(new_value, value);
    if (!strcmp(section, "r1")){
      sprintf(cmd, "%s:%s", section, name);
      set_field(cmd, new_value);
    }
    else if (!strcmp(section, "tx")){
      strcpy(cmd, name);
      set_field(cmd, new_value);
    }
		else if (!strcmp(name, "vfo_a_freq"))
			vfo_a_freq = atoi(value);
		else if (!strcmp(name, "vfo_b_freq"))
			vfo_b_freq = atoi(value);
		else if (!strcmp(name, "vfo_a_mode"))
			strcpy(vfo_a_mode, value);
		else if (!strcmp(name, "vfo_b_mode"))
			strcpy(vfo_b_mode, value);
		else if (!strcmp(name, "callsign"))
			strcpy(mycallsign, value);
		else if (!strcmp(name, "grid"))
			strcpy(mygrid, value);
		//cw 
		else if (!strcmp(name, "cw_delay"))
			cw_delay = atoi(value);
		else if (!strcmp(name, "cw_input_method"))
			cw_input_method = atoi(value);
		else if(!strcmp(name, "cw_tx_pitch"))
			cw_tx_pitch = atoi(value);
		else if (!strcmp(name, "sidetone")){
			sidetone = atoi(value);
			strcpy(cmd, name);
			char request[100], response[100];
			sprintf(request, "sidetone=%d",sidetone);  
			sdr_request(request, response);
			sprintf(request, "sidetone is set to %d Hz\n", sidetone);
			write_console(FONT_LOG, request);
		}
		//contesting
		else if (!strcmp(name, "sent_exchange"))
			strcpy(sent_exchange, value);
		else if (!strcmp(name, "contest_serial"))
			contest_serial = atoi(value);
		//data
		else if (!strcmp(name, "data_delay"))
			data_delay = atoi(value);
		else if (!strcmp(name, "current_macro"))
			strcpy(current_macro, value);
    // if it is an empty section
    else if (strlen(section) == 0){
      sprintf(cmd, "%s", name);
      set_field(cmd, new_value); 
    }

		//band stacks
		int band = -1;
		if (!strcmp(section, "80m"))
			band = 0;
		else if (!strcmp(section, "40m"))
			band = 1;
		else if (!strcmp(section, "30m"))
			band = 2;
		else if (!strcmp(section, "20m"))
			band = 3;
		else if (!strcmp(section, "17m"))
			band = 4;
		else if (!strcmp(section, "15m"))
			band = 5;
		else if (!strcmp(section, "13m"))	
			band = 6;
		else if (!strcmp(section, "10m"))
			band = 7;	

		if (band >= 0 && !strcmp(name, "power"))
			band_stack[band].power = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq0"))
			band_stack[band].freq[0] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq1"))
			band_stack[band].freq[1] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq2"))
			band_stack[band].freq[2] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq3"))
			band_stack[band].freq[3] = atoi(value);
		else if (band >= 0 && !strcmp(name, "mode0"))
			band_stack[band].mode[0] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode1"))
			band_stack[band].mode[1] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode2"))
			band_stack[band].mode[2] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode3"))
			band_stack[band].mode[3] = atoi(value);	

    return 1;
}
/* rendering of the fields */

// mod disiplay holds the tx modulation time domain envelope
// even values are the maximum and the even values are minimum

#define MOD_MAX 800
int mod_display[MOD_MAX];
int mod_display_index = 0;

void sdr_modulation_update(int32_t *samples, int count, double scale_up){
	double min=0, max=0;

	for (int i = 0; i < count; i++){
		if (i % 48 == 0){
			if (mod_display_index >= MOD_MAX)
				mod_display_index = 0;
			mod_display[mod_display_index++] = (min / 40000000.0) / scale_up;
			mod_display[mod_display_index++] = (max / 40000000.0) / scale_up;
			min = 0x7fffffff;
			max = -0x7fffffff;
		}
		if (*samples < min)
			min = *samples;
		if (*samples > max)
			max = *samples;
		samples++;
	}
}

void draw_modulation(struct field *f, cairo_t *gfx){

	int y, sub_division, i, grid_height;
	long	freq, freq_div;
	char	freq_text[20];

//	f = get_field("spectrum");
	sub_division = f->width / 10;
	grid_height = f->height - 10;

	// clear the spectrum	
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_stroke(gfx);
	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	//draw the horizontal grid
	for (i =  0; i <= grid_height; i += grid_height/10){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	//draw the vertical grid
	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + grid_height); 
	}
	cairo_stroke(gfx);

	//start the plot
	cairo_set_source_rgb(gfx, palette[SPECTRUM_PLOT][0], 
		palette[SPECTRUM_PLOT][1], palette[SPECTRUM_PLOT][2]);
	cairo_move_to(gfx, f->x + f->width, f->y + grid_height);


	int n_env_samples = sizeof(mod_display)/sizeof(int32_t);		
	int h_center = f->y + grid_height / 2;
	for (i = 0; i < f->width; i++){
		int index = (i * n_env_samples)/f->width;
		int min = mod_display[index++];
		int max = mod_display[index++]; 
		cairo_move_to(gfx, f->x + i ,  min + h_center);
		cairo_line_to(gfx, f->x + i,   max + h_center + 1);
	}
	cairo_stroke(gfx);
}

static int waterfall_offset = 30;
static int  *wf;
GdkPixbuf *waterfall_pixbuf;
guint8 *waterfall_map;

void init_waterfall(){
	struct field *f = get_field("waterfall");

	//this will store the db values of waterfall
	wf = malloc((MAX_BINS/2) * f->height * sizeof(int));
	if (!wf){
		puts("*Error: malloc failed on waterfall buffer");
		exit(0);
	}
	memset(wf, 0, (MAX_BINS/2) * f->height * sizeof(int));

	//this will store the bitmap pixles, 3 bytes per pixel
	waterfall_map = malloc(f->width * f->height * 3);
	for (int i = 0; i < f->width; i++)
		for (int j = 0; j < f->height; j++){
			int row = j * f->width * 3;
			int	index = row + i * 3;
			waterfall_map[index++] = 0;
			waterfall_map[index++] = i % 256;
			waterfall_map[index++] = j % 256; 
	}
	waterfall_pixbuf = gdk_pixbuf_new_from_data(waterfall_map,
		GDK_COLORSPACE_RGB, FALSE, 8, f->width, f->height, f->width*3, NULL,NULL);
		// format,         alpha?, bit,  widht,    height, rowstride, destryfn, data

//	printf("%ld return from pixbuff", (int)waterfall_pixbuf);	
}


void draw_waterfall(struct field *f, cairo_t *gfx){

	memmove(waterfall_map + f->width * 3, waterfall_map, 
		f->width * (f->height - 1) * 3);

	int index = 0;
	
	for (int i = 0; i < f->width; i++){
			int v = wf[i] * 2;
			if (v > 100)		//we limit ourselves to 100 db range
				v = 100;

			if (v < 20){									// r = 0, g= 0, increase blue
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 0;
				waterfall_map[index++] = v * 12; 
			}
			else if (v < 40){							// r = 0, increase g, blue is max
				waterfall_map[index++] = 0;
				waterfall_map[index++] = (v - 20) * 12;
				waterfall_map[index++] = 255; 
			}
			else if (v < 60){							// r = 0, g= max, decrease b
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 255; 
				waterfall_map[index++] = (60-v)*12; 
			}
			else if (v < 80){						 	// increase r, g = max, b = 0
				waterfall_map[index++] = (v-60) * 12;
				waterfall_map[index++] = 255;
				waterfall_map[index++] = 0; 
			}else {												// r = max, decrease g, b = 0
				waterfall_map[index++] = 255;
				waterfall_map[index++] = (100-v) * 12;
				waterfall_map[index++] = 0; 
			}
	}

	gdk_cairo_set_source_pixbuf(gfx, waterfall_pixbuf, f->x, f->y);		
	cairo_paint(gfx);
	cairo_fill(gfx);
}

void draw_spectrum_grid(struct field *f_spectrum, cairo_t *gfx){
	int sub_division, grid_height;
	struct field *f = f_spectrum;

	sub_division = f->width / 10;
	grid_height = f->height - 10;

	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], 
		palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);


	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], 
		palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	//draw the horizontal grid
	int i;
	for (i =  0; i <= grid_height; i += grid_height/10){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	//draw the vertical grid
	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + grid_height); 
	}
	cairo_stroke(gfx);
}

void draw_spectrum(struct field *f_spectrum, cairo_t *gfx){
	int y, sub_division, i, grid_height, bw_high, bw_low, pitch;
	float span;
	struct field *f;
	long	freq, freq_div;
	char	freq_text[20];

  static float last_span;

	if (in_tx){
		draw_modulation(f_spectrum, gfx);
		return;
	}

	pitch = atoi(get_field("#rx_pitch")->value);
	struct field *mode_f = get_field("r1:mode");
  if(!strcmp(mode_f->value, "USB") || !strcmp(mode_f->value, "LSB")){ // for LSB and USB draw pitch line at center
    pitch = 0;
  }

	span = atof(get_field("#span")->value);

  if (span != last_span){
    spectrum_display_start_freq_adjustment = 0;
     last_span = span;
  }
	freq = atol(get_field("r1:freq")->value);
  freq = freq + spectrum_display_start_freq_adjustment; 

	bw_high = atoi(get_field("r1:high")->value);
	bw_low = atoi(get_field("r1:low")->value);
	grid_height = f_spectrum->height - 10;
	sub_division = f_spectrum->width / 10;

	// the step is in khz, we multiply by 1000 and div 10(divisions) = 100 
	freq_div = span * 100;  

	//calculate the position of bandwidth strip
	int filter_start, filter_width;

	if(!strcmp(mode_f->value, "CWR") || !strcmp(mode_f->value, "LSB")){
	 	filter_start = f_spectrum->x + (f_spectrum->width/2) - 
			((f_spectrum->width * (bw_high + spectrum_display_start_freq_adjustment))/(span * 1000)); 
		if (filter_start < f_spectrum->x){
	 	  filter_width = ((f_spectrum->width * (bw_high -bw_low))/(span * 1000)) - (f_spectrum->x - filter_start); 
			filter_start = f_spectrum->x;
    } else {
	 	  filter_width = (f_spectrum->width * (bw_high -bw_low))/(span * 1000); 
    }
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) -
			((f_spectrum->width * (pitch + spectrum_display_start_freq_adjustment))/(span * 1000));
    spectrum_display_filter_high_position = filter_start;
    spectrum_display_filter_low_position = filter_start + filter_width;
	}
	else {
		filter_start = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * (bw_low - spectrum_display_start_freq_adjustment))/(span * 1000)); 
		if (filter_start < f_spectrum->x)
			filter_start = f_spectrum->x;
		filter_width = (f_spectrum->width * (bw_high-bw_low))/(span * 1000); 
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * (pitch - spectrum_display_start_freq_adjustment))/(span * 1000));
    spectrum_display_filter_low_position = filter_start;
    spectrum_display_filter_high_position = filter_start + filter_width;
	}
	// clear the spectrum	
	f = f_spectrum;
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_stroke(gfx);
	fill_rect(gfx, filter_start,f->y,filter_width,grid_height,SPECTRUM_BANDWIDTH);  
	cairo_stroke(gfx);

	draw_spectrum_grid(f_spectrum, gfx);
	f = f_spectrum;

	//draw the frequency readout at the bottom
	cairo_set_source_rgb(gfx, palette[COLOR_TEXT_MUTED][0], 
			palette[COLOR_TEXT_MUTED][1], palette[COLOR_TEXT_MUTED][2]);
	long f_start = freq - (4 * freq_div); 
	for (i = f->width/10; i < f->width; i += f->width/10){
    if ((span == 25) || (span == 10)){
		  sprintf(freq_text, "%ld", f_start/1000);
    } else {
      float f_start_temp = (((float)f_start/1000000.0) - ((int)(f_start/1000000))) *1000;
		  sprintf(freq_text, "%5.1f", f_start_temp);
    }
		int off = measure_text(gfx, freq_text, FONT_SMALL)/2;
		draw_text(gfx, f->x + i - off , f->y+grid_height , freq_text, FONT_SMALL);
		f_start += freq_div;
	}

	//we only plot the second half of the bins (on the lower sideband
	int last_y = 100;

	int n_bins = (int)((1.0 * spectrum_span) / 46.875);
	//the center frequency is at the center of the lower sideband,
	//i.e, three-fourth way up the bins.
	int starting_bin = ((3 * MAX_BINS)/4 - n_bins/2) - (int)((float)spectrum_display_start_freq_adjustment / 46.875);
	//int starting_bin = (3 * MAX_BINS)/4 - n_bins/2;
	int ending_bin = starting_bin + n_bins; 

	float x_step = (1.0 * f->width )/n_bins;

	//start the plot
	cairo_set_source_rgb(gfx, palette[SPECTRUM_PLOT][0], 
		palette[SPECTRUM_PLOT][1], palette[SPECTRUM_PLOT][2]);
	cairo_move_to(gfx, f->x + f->width, f->y + grid_height);

	float x = 0;
	int j = 0;
	for (i = starting_bin; i <= ending_bin; i++){
		int y;

		// the center fft bin is at zero, from MAX_BINS/2 onwards,
		// the bins are at lowest frequency (-ve frequency)
		int offset = i;
		offset = (offset - MAX_BINS/2);
		//y axis is the power  in db of each bin, scaled to 80 db
		y = ((power2dB(cnrmf(fft_bins[i])) + waterfall_offset) * f->height)/80; 
		// limit y inside the spectrum display box
		if ( y <  0)
			y = 0;
		if (y > f->height)
			y = f->height - 1;
		//the plot should be increase upwards
		cairo_line_to(gfx, f->x + f->width - (int)x, f->y + grid_height - y);

		//fill the waterfall
		for (int k = 0; k <= 1 + (int)x_step; k++)
			wf[k + f->width - (int)x] = (y * 100)/grid_height;
		x += x_step;
	}
	cairo_stroke(gfx);
 
  if (pitch >= f_spectrum->x){
    cairo_set_source_rgb(gfx, palette[SPECTRUM_PITCH][0],palette[SPECTRUM_PITCH][1], palette[SPECTRUM_PITCH][2]);
	  cairo_move_to(gfx, pitch, f->y);
	  cairo_line_to(gfx, pitch, f->y + grid_height); 
   	cairo_stroke(gfx);
    spectrum_display_pitch_position = pitch;
  } else {
    spectrum_display_pitch_position = 0;
  }

	//draw the needle
	for (struct rx *r = rx_list; r; r = r->next){
		int needle_x  = (f->width*(MAX_BINS/2 - r->tuned_bin))/(MAX_BINS/2);
		fill_rect(gfx, f->x + needle_x, f->y, 1, grid_height,  SPECTRUM_NEEDLE);
	}

	draw_waterfall(get_field("waterfall"), gfx);
}

int waterfall_fn(struct field *f, cairo_t *gfx, int event, int a, int b){
		if(f->fn(f, gfx, FIELD_DRAW, -1, -1, 0))
	switch(FIELD_DRAW){
		case FIELD_DRAW:
			draw_waterfall(f, gfx);
			break;
	}
}

char* freq_with_separators(char* freq_str){

  int freq = atoi(freq_str);
  int f_mhz, f_khz, f_hz;
  char temp_string[11];
  static char return_string[11];

  f_mhz = freq / 1000000;
  f_khz = (freq - (f_mhz*1000000)) / 1000;
  f_hz = freq - (f_mhz*1000000) - (f_khz*1000);

  sprintf(temp_string,"%d",f_mhz);
  strcpy(return_string,temp_string);
  strcat(return_string,".");
  if (f_khz < 100){
    strcat(return_string,"0");
  }
  if (f_khz < 10){
    strcat(return_string,"0");
  }
  sprintf(temp_string,"%d",f_khz);
  strcat(return_string,temp_string);
  strcat(return_string,".");
  if (f_hz < 100){
    strcat(return_string,"0");
  }
  if (f_hz < 10){
    strcat(return_string,"0");
  }
  sprintf(temp_string,"%d",f_hz);
  strcat(return_string,temp_string);
  return return_string;
}

void draw_dial(struct field *f, cairo_t *gfx){
	struct font_style *s = font_table + 0;
	struct field *rit = get_field("#rit");
	struct field *split = get_field("#split");
	struct field *vfo = get_field("#vfo");
	char buff[20];

  char temp_str[20];

	fill_rect(gfx, f->x+1, f->y+1, f->width-2,f->height-2, COLOR_BACKGROUND);

	//update the vfos
	if (vfo->value[0] == 'A')
		vfo_a_freq = atoi(f->value);
	else
		vfo_b_freq = atoi(f->value);

	int width, offset;	
	
	width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
	offset = f->width/2 - width/2;
	draw_text(gfx, f->x + offset, f->y+5 ,  f->label, FONT_FIELD_LABEL);
	width = measure_text(gfx, f->value, f->font_index);
	offset = f->width/2 - width/2;
  if (!strcmp(rit->value, "ON")){
    if (!in_tx){
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(temp_str, "%d", (atoi(f->value) + rit_delta));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    }
    else {
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
      sprintf(temp_str, "%d", (atoi(f->value) + rit_delta));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(split->value, "ON")){
    if (!in_tx){
      sprintf(temp_str, "%d", vfo_b_freq);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "RX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    }
    else {
      sprintf(temp_str, "%d", vfo_b_freq);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
      sprintf(buff, "RX:%d", atoi(f->value) + rit_delta);
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(vfo->value, "A")){
    if (!in_tx){
      sprintf(temp_str, "%d", vfo_b_freq);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+15 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "A:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    } else {
      sprintf(temp_str, "%d", vfo_b_freq);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    }
  }
  else{
    if (!in_tx){
      sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "B:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    }else {
      sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+6 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+25 , buff , FONT_LARGE_VALUE);
    }
  }
}


void redraw_main_screen(GtkWidget *widget, cairo_t *gfx){
	double dx1, dy1, dx2, dy2;
	int x1, y1, x2, y2;

	cairo_clip_extents(gfx, &dx1, &dy1, &dx2, &dy2);
	x1 = (int)dx1;
	y1 = (int)dy1;
	x2 = (int)dx2;
	y2 = (int)dy2;

	fill_rect(gfx, x1, y1, x2-x1, y2-y1, COLOR_BACKGROUND);
	for (int i = 0; active_layout[i].cmd[0] > 0; i++){
		double cx1, cx2, cy1, cy2;
		struct field *f = active_layout + i;
		cx1 = f->x;
		cx2 = cx1 + f->width;
		cy1 = f->y;
		cy2 = cy1 + f->height;
		if (cairo_in_clip(gfx, cx1, cy1) || cairo_in_clip(gfx, cx2, cy2)){
			draw_field(widget, gfx, active_layout + i);
		}
	}
}

/* gtk specific routines */
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, gpointer user_data ) {
	redraw_main_screen(widget, cr);	
  return FALSE;
}

static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
	//printf("size changed to %d x %d\n", event->width, event->height);
	screen_width = event->width;
	screen_height = event->height;
}

void update_field(struct field *f){
	GdkRectangle r;
	r.x = f->x - 1;
	r.y = f->y - 1;
	r.width = f->width+2;
	r.height = f->height+2;
	//the update_field could be triggered from the sdr's waterfall update
	//which starts before the display_area is called 
	if (display_area){
		gtk_widget_queue_draw_area(display_area, r.x, r.y, r.width, r.height);
	}
} 

static void hover_field(struct field *f){
	struct field *prev_hover = f_hover;
	if (f){
		//set f_hover to none to remove the outline
		f_hover = NULL;
		update_field(prev_hover);
	}
	f_hover = f;
	update_field(f);
}


// respond to a UI request to change the field value
static void edit_field(struct field *f, int action){
	int v;

	if (f == f_focus)
		focus_since = millis();

	if (f->fn){
		if (f->fn(f, NULL, FIELD_EDIT, action, 0, 0))
			return;
	}
	
	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(f->value);
		if (action == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (action == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d",  v);
	}
	else if (f->value_type == FIELD_SELECTION){
		char *p, *prev, *next, b[100], *first, *last;
    // get the first and last selections
    strcpy(b, f->selection);
    p = strtok(b, "/");
    first = p;
    while(p){
      last = p;
      p = strtok(NULL, "/");
    }
		//search the current text in the selection
		prev = NULL;
		strcpy(b, f->selection);
		p = strtok(b, "/");
		while(p){
			if (!strcmp(p, f->value))
				break;
			else
				prev = p;
			p = strtok(NULL, "/");
		}	
		//set to the first option
		if (p == NULL){
			if (prev)
				strcpy(f->value, prev);
		}
		else if (action == MIN_KEY_DOWN){
			prev = p;
			p = strtok(NULL,"/");
			if (p)
				strcpy(f->value, p);
			else
        strcpy(f->value, first); // roll over
				//return;
				//strcpy(f->value, prev); 
		}
		else if (action == MIN_KEY_UP){
			if (prev)
				strcpy(f->value, prev);
			else
        strcpy(f->value, last); // roll over
				//return;
		}
	}
	else if (f->value_type == FIELD_TOGGLE){
		char *p, *prev, *next, b[100];
		//search the current text in the selection
		prev = NULL;
		strcpy(b, f->selection);
		p = strtok(b, "/");
		while(p){
			if (strcmp(p, f->value))
				break;
			p = strtok(NULL, "/");
		}	
		strcpy(f->value, p);
	}
	else if (f->value_type == FIELD_BUTTON){
		NULL; // ah, do nothing!
	}

	//send a command to the receiver
	char buff[200];
	sprintf(buff, "%s=%s", f->cmd, f->value);
	do_cmd(buff);
	update_field(f);
	settings_updated++;
}

static void focus_field(struct field *f){
	struct field *prev_hover = f_hover;
	struct field *prev_focus = f_focus;
	
	f_focus = NULL;
	if (prev_hover)
		update_field(prev_hover);
	if (prev_focus)
		update_field(prev_focus);
	if (f){
		f_focus = f_hover = f;
		focus_since = millis();
	}
	update_field(f_hover);

	//is it a toggle field?
	if (f_focus->value_type == FIELD_TOGGLE)
		edit_field(f_focus, MIN_KEY_DOWN);	

  //is it a selection field?
  if (f_focus->value_type == FIELD_SELECTION) 
    edit_field(f_focus, MIN_KEY_UP);

	//if the button has been pressed, do the needful
	if (f_focus->value_type == FIELD_TOGGLE || 
			f_focus->value_type == FIELD_BUTTON)
		do_cmd(f->cmd);
}

time_t time_sbitx(){
	if (time_delta)
		return  (millis()/1000l) + time_delta;
	else
		return time(NULL);
}


// setting the frequency is complicated by having to take care of the
// rit/split and power levels associated with each frequency
void set_operating_freq(int dial_freq, char *response){
	struct field *rit = get_field("#rit");
	struct field *split = get_field("#split");
	char freq_request[30];
 
	if (!strcmp(rit->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%d", dial_freq + rit_delta); 		
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq); 		
	}
	else if (!strcmp(split->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%d", vfo_b_freq);
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq);
	}
	else
			sprintf(freq_request, "r1:freq=%d", dial_freq);

	//set the power levels
	long operating_freq = atoi(freq_request + strlen("r1:freq="));
	//find the best match for this frequency in the bands table
	int max_power = 0;
	int power = 0;
	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		if (band_stack[i].start <= operating_freq && 
				operating_freq <= band_stack[i].stop){
			max_power = band_stack[i].max;
			power = band_stack[i].power;
		}
	} 
	// now, we set this up in the gui
	struct field *f_power = get_field("tx_power");
	f_power->max = max_power;
	sprintf(f_power->value, "%d", power);
	edit_field(f_power, 0);

	//get back to setting the frequency
	sdr_request(freq_request, response);
}

void clear_tx_text_buffer(){
	set_field("#text_in", "");
}

void band_stack_update_power(int power){
	struct field *f = get_field("r1:freq");
	long freq = atol(f->value);

	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		if (band_stack[i].start <= freq && 
				freq <= band_stack[i].stop){
				if (power < band_stack[i].max)
					band_stack[i].power = power;
				else
					band_stack[i].power = band_stack[i].max;
		}
	} 
}

int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	struct field *f_freq, *f_span, *f_pitch;
	int span, pitch, new_value;
  long freq;
	char buff[100];
  int mode = mode_id(get_field("r1:mode")->value);
  int multiplier = 1;

  if ((mode == MODE_LSB) || (mode == MODE_CWR)){
    multiplier = -1;
  }

	switch(event){
		case FIELD_DRAW:
			draw_spectrum(f, gfx);
			return 1;
		break;
		case GDK_MOTION_NOTIFY:
	    f_freq = get_field("r1:freq");
		  freq = atoi(f_freq->value);
		  f_span = get_field("#span");
		  span = atof(f_span->value) * 1000;
		  //a has the x position of the mouse and b has the y position
      // move the spectrum display if we're in the frequency below the grid
      if (b > (f->y + f->height - 10)){
        spectrum_display_start_freq_adjustment -= ((a - last_mouse_x) * (span/f->width));
      // are we in the pitch dragging area?
      } else if ((spectrum_display_pitch_position > 0) && (a > (spectrum_display_pitch_position - 10)) &&
        (a < (spectrum_display_pitch_position + 10)) && (b > (f->y + f->height - 10 - (f->height/5))) &&
        (b < (f->y + f->height - 10)) && (mode != MODE_LSB) && (mode != MODE_USB)){
        new_value = atoi(get_field("#rx_pitch")->value) + (multiplier * ((a - last_mouse_x) * (span/f->width)));
        sprintf(buff, "%d", new_value);
        set_field("#rx_pitch", buff);
      // are we in the LOW filter drag area?
      } else if ((a > (spectrum_display_filter_low_position - 30)) && (a < (spectrum_display_filter_low_position + 30)) && (b < (f->y+(f->height/5)))){
        new_value = atoi(get_field("r1:low")->value) + (multiplier * ((a - last_mouse_x) * (span/f->width)));
        sprintf(buff, "%d", new_value);
        set_field("r1:low", buff); 
      // are we in the HIGH filter drag area?
      } else if ((a > (spectrum_display_filter_high_position - 30)) && (a < (spectrum_display_filter_high_position + 30)) && (b < (f->y+(f->height/5)))){
          new_value = atoi(get_field("r1:high")->value) + (multiplier * ((a - last_mouse_x) * (span/f->width)));
          sprintf(buff, "%d", new_value);
          set_field("r1:high", buff);
      // we're in the grid and not the first division at the top, drag QSY
      } else if (b > (f->y+(f->height/5))){
        freq -= ((a - last_mouse_x) * (span/f->width));
        sprintf(buff, "%ld", freq);
        set_field("r1:freq", buff);
      }   
		  return 1;
		break;
    case GDK_BUTTON_PRESS: 
      if (c == GDK_BUTTON_SECONDARY){ // right click QSY
        f_freq = get_field("r1:freq");
        freq = atoi(f_freq->value);
        f_span = get_field("#span");
        span = atof(f_span->value) * 1000;
        f_pitch = get_field("#rx_pitch");
        pitch = atoi(f_pitch->value);
        if (mode == MODE_CW){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) - pitch + spectrum_display_start_freq_adjustment;
        } else if (mode == MODE_CWR){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) + pitch + spectrum_display_start_freq_adjustment;
        } else if (mode == MODE_LSB){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) + spectrum_display_start_freq_adjustment;
        } else { // other modes may need to be optimized - k3ng 2022-09-02
          freq += (((float)(a - f->x) / (float)f->width) - 0.5) * (float)span + spectrum_display_start_freq_adjustment;
        }
        sprintf(buff, "%ld", freq);
        set_field("r1:freq", buff);
        return 1;
      }
    break;
	}
	return 0;	
}

int do_waterfall(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	switch(event){
		case FIELD_DRAW:
			draw_waterfall(f, gfx);
			return 1;
/*
		case GDK_MOUSE_MOVE:{
			struct field *f_freq = get_field("r1:freq");
			long freq = atoi(f_freq->value);
			struct field *f_span = get_field("#span");
			int span = atoi(f_focus->value);
			freq -= ((x - last_mouse_x) *tuning_step)/4;	//slow this down a bit
			sprintf(buff, "%ld", freq);
			set_field("r1:freq", buff);
			}
			return 1;
		break;
*/
	}
	return 0;	
}

void call_wipe(){
	contact_callsign[0] = 0;
	contact_grid[0] = 0;
	received_rst[0] = 0;
	sent_rst[0] = 0;
	set_field("#log_ed", "");
	update_log_ed();
	redraw_flag++;
}

void update_log_ed(){
	struct field *f = get_field("#log_ed");
	char *log_info = f->label;

	strcpy(log_info, "Log:");
	if (strlen(contact_callsign))
		strcat(log_info, contact_callsign);
	else {
		redraw_flag++;
		return;
	}

	if (strlen(contact_grid)){
		strcat(log_info, " Grid:");
		strcat(log_info, contact_grid);
	}

	strcat(log_info, " Sent:");
	if (strlen(sent_rst)){	
				strcat(log_info, sent_rst);
		if (strlen(sent_exchange)){
			strcat(log_info, " ");
			strcat(log_info, sent_exchange);
		}
	}
	else	
		strcat(log_info, "-");


	strcat(log_info, " My:");
	if (strlen(received_rst)){
		strcat(log_info, received_rst);
	if (strlen(received_exchange)){
		strcat(log_info, " ");
		strcat(log_info, received_exchange);
	}
	}
	else
		strcat(log_info, "-");

	redraw_flag++;
}


int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100], *p, *q;

	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;
	int	l = 0;

	switch(event){
		case FIELD_DRAW:
			draw_log(gfx, f);
			return 1;
		break;
		case GDK_BUTTON_PRESS:
			l = console_current_line - ((f->y + f->height - b)/line_height);
			if (l < 0)
				l += MAX_CONSOLE_LINES;
			console_selected_line = l;
			if (!strcmp(get_field("r1:mode")->value, "FT8")){
				char ft8_response[100];
				ft8_interpret(console_stream[l].text, ft8_response);
				if (ft8_response[0] != 0){
					//set_field("#text_in", ft8_response);
					update_log_ed();
				}
			}
//			printf("chosen line is %d[%s]\n", l, console_stream[l].text);
			redraw_flag++;
			return 1;
		break;
	}
	return 0;	
}

int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100];

	if (event == FIELD_DRAW){
		time_t now = time_sbitx();
		struct tm *tmp = gmtime(&now);

		sprintf(buff, "%s | %s", mycallsign, mygrid);
		draw_text(gfx, f->x+1, f->y+2 , buff, FONT_FIELD_LABEL);
		
		sprintf(buff, "UTC:%04d/%02d/%02d %02d:%02d:%02d",  
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
		int width = measure_text(gfx, buff, FONT_FIELD_LABEL);
		draw_text(gfx, f->x + f->width - width - 1, f->y + 2, buff, FONT_FIELD_LABEL);
		return 1;
	}
	return 0;
}

void execute_app(char *app){
	int pid = fork();
	if (!pid){
		system(app);
		exit(0);	
	}
}

int do_text(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int width, offset, text_length, line_start, y;	
	char this_line[MAX_FIELD_LENGTH];
	int text_line_width = 0;

	if (event == FIELD_EDIT){
		//if it is a command, then execute it and clear the field
		if (f->value[0] == COMMAND_ESCAPE &&  strlen(f->value) > 1 && (a == '\n' || a == MIN_KEY_ENTER)){
			cmd_exec(f->value + 1);
			f->value[0] = 0;
			update_field(f);
			redraw_flag++;
		}
		else if ((a =='\n' || a == MIN_KEY_ENTER) && !strcmp(get_field("r1:mode")->value, "FT8") 
			&& f->value[0] != COMMAND_ESCAPE){
			ft8_tx(f->value, atoi(get_field("#rx_pitch")->value));
			f->value[0] = 0;		
		}
		else if (a >= ' ' && a <= 127 && strlen(f->value) < f->max-1){
			int l = strlen(f->value);
			f->value[l++] = a;
			f->value[l] = 0;
		}
		else if (a == MIN_KEY_BACKSPACE && strlen(f->value) > 0){
			int l = strlen(f->value) - 1;
			f->value[l] = 0;
		}
		
		return 1;
	}
	else if (event == FIELD_DRAW){

		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
		text_length = strlen(f->value);
		line_start = 0;
		y = f->y + 2;
		text_line_width = 0;

		while(text_length > 0){
			if (text_length > console_cols){
				strncpy(this_line, f->value + line_start, console_cols);
				this_line[console_cols] = 0;
			}
			else
				strcpy(this_line, f->value + line_start);		
			draw_text(gfx, f->x + 2, y, this_line, f->font_index);
			text_line_width= measure_text(gfx, this_line, f->font_index);
			y += 14;
			line_start += console_cols;
			text_length -= console_cols;
		}
		//draw the text cursor, if there is no text, the text baseline is zero
		if (strlen(f->value))
			y -= 14;
		fill_rect(gfx, f->x + text_line_width + 5, y+3, 9, 10, f->font_index);
		return 1;
	}
	return 0;
}


int do_pitch(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

	int	v = atoi(f->value);

	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max){
			v += f->step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			v -= f->step;
		}
		sprintf(f->value, "%d", v);
		update_field(f);
		redraw_flag++;
		modem_set_pitch(v);
		return 1;
	}
	return 0;
}

//called for RIT as well as the main tuning
int do_tuning(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

static struct timespec last_change_time, this_change_time;

	int	v = atoi(f->value);

  int temp_tuning_step = tuning_step;

	if (event == FIELD_EDIT){

  if (!strcmp(get_field("tuning_acceleration")->value, "ON")){
    clock_gettime(CLOCK_MONOTONIC_RAW, &this_change_time);
    uint64_t delta_us = (this_change_time.tv_sec - last_change_time.tv_sec) * 1000000 + (this_change_time.tv_nsec - last_change_time.tv_nsec) / 1000;
    char temp_char[100];
    //sprintf(temp_char, "delta: %d", delta_us);
    //strcat(temp_char,"\r\n");
    //write_console(FONT_LOG, temp_char);
    clock_gettime(CLOCK_MONOTONIC_RAW, &last_change_time);
    if (delta_us < atof(get_field("tuning_accel_thresh2")->value)){
      if (tuning_step < 10000){
        tuning_step = tuning_step * 100;
        //sprintf(temp_char, "x100 activated\r\n");
        //write_console(FONT_LOG, temp_char);
      }
    } else if (delta_us < atof(get_field("tuning_accel_thresh1")->value)){
      if (tuning_step < 1000){
        tuning_step = tuning_step * 10;
        //printf(temp_char, "x10 activated\r\n");
        //write_console(FONT_LOG, temp_char);
      }
    }
  }

		if (a == MIN_KEY_UP && v + f->step <= f->max){
			//this is tuning the radio
			if (!strcmp(get_field("#rit")->value, "ON")){
				if(rit_delta < MAX_RIT)
					rit_delta += tuning_step;
				else
					return 1;
			}
			else
				v = (v / tuning_step + 1)*tuning_step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			if (!strcmp(get_field("#rit")->value, "ON")){
				if (rit_delta > -MAX_RIT)
					rit_delta -= tuning_step;
				else
					return 1;
			}
			else
				v = (v / tuning_step - 1)*tuning_step;
			clear_tx_text_buffer();
		}
		
		sprintf(f->value, "%d",  v);
		tuning_step = temp_tuning_step;
		//send the new frequency to the sbitx core
		char buff[100];
		sprintf(buff, "%s=%s", f->cmd, f->value);
		do_cmd(buff);
		//update the GUI
		update_field(f);
		settings_updated++;
		//leave it to us, we have handled it

		return 1;
	}
	else if (event == FIELD_DRAW){
			draw_dial(f, gfx);

			return 1; 
	}
	return 0;	
}

int do_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if(event == GDK_BUTTON_PRESS){
		struct field *f_text = get_field("#text_in");
		if (!strcmp(f->cmd, "#kbd_macro"))
			set_ui(LAYOUT_MACROS);
		else if (!strcmp(f->cmd, "#kbd_bs"))
			edit_field(f_text, MIN_KEY_BACKSPACE);
		else if (!strcmp(f->cmd, "#kbd_enter"))
			edit_field(f_text, '\n');
		else
			edit_field(f_text, f->value[0]);
		focus_since = millis();
		return 1;
	}	
	return 0;
}

void write_call_log(){
	char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it

	char *path = getenv("HOME");
	sprintf(fullpath, "%s/sbitx/data/logbook.txt", path); 

	time_t log_time = time_sbitx();
//	time(&log_time);
	struct tm *tmp = gmtime(&log_time);
			//tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
	
	char *p, mode[10];
	p = get_field("r1:mode")->value;
	if (!strcmp(p, "USB") || !strcmp(p, "LSB") || !strcmp(p, "AM"))
		strcpy(mode, "PH");
	else if (!strcmp(p, "CW") || strcmp(p, "CWR"))
		strcpy(mode, "CW");
	else if (!strcmp(p, "FM"))
		strcpy(mode, "FM");
	else if (!strncmp(p, "RTTY", 4))
		strcpy(mode, "RY");
	else
		strcpy(mode, "DG");

	FILE *pf = fopen(fullpath, "a");
	
	fprintf(pf, "QSO: %7d %s %04d-%02d-%02d %02d%02d %s", 
		atoi(get_field("r1:freq")->value)/1000, mode, 
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday,
			tmp->tm_hour, tmp->tm_min,
			mycallsign);

	int padding = 10 - strlen(mycallsign);
	for (int i = 0; i < padding; i++)
			fputc(' ', pf);

	fprintf(pf, "%3s %5s ", sent_rst, sent_exchange);
	padding = 10 - strlen(contact_callsign);
	fprintf(pf, contact_callsign);
	for(int i = 0; i < padding; i++)
		fputc(' ', pf);
	fprintf(pf, "%3s %5s %d\n", received_rst, received_exchange, tx_id+0); 

	fclose(pf);

	write_console(FONT_LOG, "\nQSO logged with ");
	write_console(FONT_LOG, contact_callsign);
	write_console(FONT_LOG, " ");
	write_console(FONT_LOG, sent_exchange); 
	write_console(FONT_LOG, "\n");

	if (contest_serial > 0){
		contest_serial++;
		sprintf(sent_exchange, "%04d", contest_serial);
	}
	//wipe it clean, deffered for the time being
	//call_wipe();
	redraw_flag++;
}


//this interprets the log being filled in bits and pieces in the following order
// callsign, sent rst, received rst and exchange
void interpret_log(char *text){
	int i, j;
	char *p, *q;
	int mode = mode_id(get_field("r1:mode")->value);

	p = text;
	while(*p == ' ')
		p++;
	
	if (contact_callsign[0] == 0){
		for (i = 0; *p && i < sizeof(contact_callsign) && *p > ' '; i++)
			contact_callsign[i] = *p++;
		contact_callsign[i] = 0;
	}

	while(*p == ' ')
		p++;

	if (sent_rst[0] == 0){
		//the first must be something between 1 and 5
		if ((mode == MODE_CW|| mode == MODE_CWR) && p[0] >= '1' && p[0] <= '5'){
			sent_rst[0] = p[0];
			sent_rst[1] = p[1];
			sent_rst[2] = p[2];
			sent_rst[3] = 0;
			p += 3;
		} 
		else if (p[0] >= '1' && p[0] <= '5' && (toupper(p[1]) == 'N' || isdigit(p[1]))){ //none cw modes
			sent_rst[0] = p[0];
			sent_rst[1] = p[1];
			sent_rst[2] = 0; 
			p += 2;
		}
	}

	while(*p == ' ')
		p++;

	if (received_rst[0] == 0){
		//the first must be something between 1 and 5
		if ((mode == MODE_CW|| mode == MODE_CWR) && p[0] >= '1' && p[0] <= '5'){
			received_rst[0] = p[0];
			received_rst[1] = p[1];
			received_rst[2] = p[2];
			received_rst[3] = 0;
			p += 3;
		} 
		else if (p[0] >= '1' && p[0] <= '5' && (toupper(p[1]) == 'N' || isdigit(p[1]))){ //none cw modes
			received_rst[0] = p[0];
			received_rst[1] = p[1];
			received_rst[2] = 0; 
			p += 2;
		}
	}

	while(*p == ' ')
		p++;

	//the rest is exchange received
	for (i = 0; i < sizeof(received_exchange) && *p > ' '; i++)
		received_exchange[i] = *p++;

	received_exchange[i] = 0;
}

void macro_get_var(char *var, char *s){
	*s = 0;

	if(!strcmp(var, "MYCALL"))
		strcpy(s, mycallsign);
	else if (!strcmp(var, "CALL"))
		strcpy(s, contact_callsign);
	else if (!strcmp(var, "SENTRST"))
		sprintf(s, "%s", sent_rst);
	else if (!strcmp(var, "GRID"))
		strcpy(s, mygrid);
	else if (!strcmp(var, "GRIDSQUARE")){
		strcpy(var, mygrid);
		var[4] = 0;
		strcpy(s, var);
	}
	else if (!strcmp(var, "EXCH")){
		strcpy(s, sent_exchange);
	}
	else if (!strcmp(var, "WIPE"))
		call_wipe();
	else if (!strcmp(var, "LOG")){
		write_call_log();
	}
	else
		*s = 0;
}

void qrz(char *callsign){
	char 	bash_line[1000];
	sprintf(bash_line, "Querying qrz.com for %s\n", callsign);
	write_console(FONT_LOG, bash_line);
	sprintf(bash_line, "chromium-browser https://qrz.com/DB/%s &", callsign);
	execute_app(bash_line);
}

int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[256], *mode;

	if(event == GDK_BUTTON_PRESS){
		int fn_key = atoi(f->cmd+3); // skip past the '#mf' and read the function key number

		if (!strcmp(f->cmd, "#mfkbd")){
			set_ui(LAYOUT_KBD);
			return 1;
		}
		else if (!strcmp(f->cmd, "#mfqrz") && strlen(contact_callsign) > 0){
			qrz(contact_callsign);
			return 1;
		}
		else if (!strcmp(f->cmd, "#mfwipe")){
			call_wipe();
			return 1;
		}	
		else if (!strcmp(f->cmd, "#mflog")){
			write_call_log();
			return 1;
		}
		else 
		 	macro_exec(fn_key, buff);
	
		mode = get_field("r1:mode")->value;

		//add the end of transmission to the expanded buffer for the fldigi modes
		if (!strcmp(mode, "RTTY") || !strcmp(mode, "PSK31")){
			strcat(buff, "^r");
			tx_on();
		}

		if (!strcmp(mode, "FT8") && strlen(buff)){
			//we use the setting of the PITCH control for tx freq
			ft8_tx(buff, atoi(get_field("#rx_pitch")->value));
			set_field("#text_in", "");
			//write_console(FONT_LOG_TX, buff);
		}
		else if (strlen(buff)){
			set_field("#text_in", buff);
			//put it in the text buffer and hope it gets transmitted!
		}
		return 1;
	}
	else if (event == FIELD_DRAW){
		int width, offset, text_length, line_start, y;	
		char this_line[MAX_FIELD_LENGTH];
		int text_line_width = 0;

		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

		width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		offset = f->width/2 - width/2;
		if (strlen(f->value) == 0)
			draw_text(gfx, f->x +5, f->y+13 , f->label , FONT_FIELD_LABEL);
		else {
			if (strlen(f->label)){
				draw_text(gfx, f->x+5, f->y+5 ,  f->label, FONT_FIELD_LABEL);
				draw_text(gfx, f->x+5 , f->y+f->height - 20 , f->value , f->font_index);
			}
			else
				draw_text(gfx, f->x+offset , f->y+5, f->value , f->font_index);
			}	
		return 1;
	}

	return 0;
}

int do_record(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){

		if (f_focus == f)
			rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_SELECTED_BOX, 2);
		else if (f_hover == f)
			rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
		else 
			rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

		int width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		int offset = f->width/2 - width/2;
		draw_text(gfx, f->x + offset, f->y+5 , f->label, FONT_FIELD_LABEL);

		if (record_start){
			width = measure_text(gfx, f->value, f->font_index);
			offset = f->width/2 - width/2;
			time_t duration_seconds = time(NULL) - record_start;
			int minutes = duration_seconds/60;
			int seconds = duration_seconds % 60;
			char duration[12];
			sprintf(duration, "%d:%02d", minutes, seconds); 	
			draw_text(gfx, f->x+10 , f->y+25 , duration , f->font_index);
		}
		else
			draw_text(gfx, f->x+offset , f->y+25 , "OFF" , f->font_index);
		return 1;
	}
	return 0;
}

void tx_on(){
	char response[100];

	struct field *f = get_field("r1:mode");
	if (f){
		if (!strcmp(f->value, "CW"))
			tx_mode = MODE_CW;
		else if (!strcmp(f->value, "CWR"))
			tx_mode = MODE_CWR;
		else if (!strcmp(f->value, "USB"))
			tx_mode = MODE_USB;
		else if (!strcmp(f->value, "LSB"))
			tx_mode = MODE_LSB;
		else if (!strcmp(f->value, "NBFM"))
			tx_mode = MODE_NBFM;
		else if (!strcmp(f->value, "AM"))
			tx_mode = MODE_AM;
		else if (!strcmp(f->value, "2TONE"))
			tx_mode = MODE_2TONE;
		else if (!strcmp(f->value, "DIGITAL"))
			tx_mode = MODE_DIGITAL;
	}

	if (in_tx == 0){
		//tx line on/off is handled by sbitx.c
		//digitalWrite(TX_LINE, HIGH);
		sdr_request("tx=on", response);	
		in_tx = 1;
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
	}
	// let the modems decide this
	//if (tx_mode == MODE_DIGITAL || tx_mode == MODE_RTTY || tx_mode == MODE_PSK31)
		//sound_input(1);

	tx_start_time = millis();
}

void tx_off(){
	char response[100];

	modem_abort();

	if (in_tx == 1){
		//the tx_line on/off is done by the sbitx.c
		//digitalWrite(TX_LINE, LOW);
		sdr_request("tx=off", response);	
		in_tx = 0;
		sdr_request("key=up", response);
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
	}
	sound_input(0); //it is a low overhead call, might as well be sure
}

/*
void swap_ui(int id){
	struct field *f = get_field("#kbd_q");

	if (f->y > 1000){
		// the "#kbd" is out of screen, get it up and "#mf" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4))
				active_layout[i].y -= 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3))
				active_layout[i].y += 1000;
		}
	}
	else {
		// the "#mf" is out of screen, get it up and "#kbd" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++)
			if (!strncmp(active_layout[i].cmd, "#kbd", 4))
				active_layout[i].y += 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3))
				active_layout[i].y -= 1000;
	}
	redraw_flag++;
}
*/

void set_ui(int id){
	struct field *f = get_field("#kbd_q");

	if (id == LAYOUT_KBD){
		// the "#kbd" is out of screen, get it up and "#mf" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			
		}
	}
	if (id == LAYOUT_MACROS) {
		// the "#mf" is out of screen, get it up and "#kbd" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++)
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
	}
	current_layout = id;
	redraw_flag++;
}

int static cw_keydown = 0;
int	static cw_hold_until = 0;
int static cw_hold_duration = 150;

static void cw_key(int state){
	char response[100];
	if (state == 1 && cw_keydown == 0){
		sdr_request("key=down", response);
		cw_keydown = 1;
	}
	else if (state == 0 && cw_keydown == 1){
		cw_keydown = 0;
	}
	//printf("cw key = %d\n", cw_keydown);
}


static int control_down = 0;
static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	key_modifier = 0;

	if (event->keyval == MIN_KEY_CONTROL){
		control_down = 0;
	}

	if (event->keyval == MIN_KEY_TAB){
		tx_off();
  }

}

static gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	char request[1000], response[1000];

	if (event->keyval == MIN_KEY_CONTROL){
		control_down = 1;
	}

	if (control_down){
		GtkClipboard *clip;
		struct field *f;	
		switch(event->keyval){
			case 'r':
				tx_off();
				break;
			case 't':
				tx_on();
				break;
			case 'm':
				if (current_layout == LAYOUT_MACROS)
					set_ui(LAYOUT_KBD);
				else
					set_ui(LAYOUT_MACROS);
				break;
			case 'q':
				tx_off();
				set_field("#record", "OFF");
				save_user_settings(1);
				exit(0);
				break;
			case 'c':
				f = get_field("#text_in");
				clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_text(clip, f->value, strlen(f->value));
				break; 
			case 'l':
				write_call_log();
				break;
			case 'v':
				clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				if (clip){
					int i = 0;
					gchar *text = gtk_clipboard_wait_for_text(clip);
					f = get_field("#text_in");
					if (text){
						i = strlen(f->value);
						while(i < MAX_FIELD_LENGTH-1 && *text){
							if (*text >= ' ' || *text == '\n' || 
											(*text >= ' ' && *text <= 128))
								f->value[i++] = *text;  
							text++;	
						}
						f->value[i] = 0;
						update_field(f);
					}
				}
			break;
		}
		return FALSE;
	}

	if (f_focus && f_focus->value_type == FIELD_TEXT){
		edit_field(f_focus, event->keyval); 
		return FALSE;
	}
		
//	printf("keyPress %x %x\n", event->keyval, event->state);
	//key_modifier = event->keyval;
	switch(event->keyval){
		case MIN_KEY_ESC:
			modem_abort();
			tx_off();
			call_wipe();
			update_log_ed();
			redraw_flag++;
			break;
		case MIN_KEY_UP:
			if (f_focus == NULL && f_hover > active_layout){
				hover_field(f_hover - 1);
				//printf("Up, hover %s\n", f_hover->cmd);
			}else if (f_focus){
				edit_field(f_focus, MIN_KEY_UP);
			}
			break;
		case MIN_KEY_DOWN:
			if (f_focus == NULL && f_hover && strcmp(f_hover->cmd, "")){
				hover_field(f_hover + 1);
				//printf("Down, hover %d\n", f_hover);
			}
			else if (f_focus){
				edit_field(f_focus, MIN_KEY_DOWN);
			}
			break;
		case 65507:
			key_modifier |= event->keyval;
			//printf("key_modifier set to %d\n", key_modifier);
			break;
		default:
			//by default, all text goes to the text_input control
			if (event->keyval == MIN_KEY_ENTER)
				edit_field(get_field("#text_in"), '\n');
			else if (MIN_KEY_F1 <= event->keyval && event->keyval <= MIN_KEY_F12){
				int fn_key = event->keyval - MIN_KEY_F1 + 1;
				char fname[10];
				sprintf(fname, "#mf%d", fn_key);
				do_macro(get_field(fname), NULL, GDK_BUTTON_PRESS, 0, 0, 0);
			} 
			else
				edit_field(get_field("#text_in"), event->keyval);
			//if (f_focus)
			//	edit_field(f_focus, event->keyval); 
			//printf("key = %d (%c)\n", event->keyval, (char)event->keyval); 	
	}
  return FALSE; 
}

static gboolean on_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer data) {
	
	if (f_focus){
		if (event->direction == 0){
     if (!strcmp(get_field("reverse_scrolling")->value, "ON")){
	  		edit_field(f_focus, MIN_KEY_DOWN);
      } else {
		  	edit_field(f_focus, MIN_KEY_UP);
      }
		} else {
      if (!strcmp(get_field("reverse_scrolling")->value, "ON")){
			  edit_field(f_focus, MIN_KEY_UP);
      } else {
			  edit_field(f_focus, MIN_KEY_DOWN);
      }
   }
	}
		
}


static gboolean on_window_state (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	mouse_down = 0;
}

static gboolean on_mouse_release (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	struct field *f;

	mouse_down = 0;
	if (event->type == GDK_BUTTON_RELEASE && event->button == GDK_BUTTON_PRIMARY){
		//printf("mouse release at %d, %d\n", (int)(event->x), (int)(event->y));
	}
  /* We've handled the event, stop processing */
  return TRUE;
}


static gboolean on_mouse_move (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	char buff[100];

	if (!mouse_down)
		return false;

	int x = (int)(event->x);
	int y = (int)(event->y);

	// if a control is in focus and it handles the mouse drag, then just do that
	// else treat it as a spin up/down of the control
	if (f_focus){

			if (!f_focus->fn ||  !f_focus->fn(f_focus, NULL, GDK_MOTION_NOTIFY, event->x, event->y, 0)){
				//just emit up or down
				if(last_mouse_x < x || last_mouse_y > y)
					edit_field(f_focus, MIN_KEY_UP);
				else if (last_mouse_x > x || last_mouse_y < y)
					edit_field(f_focus, MIN_KEY_DOWN);
			}
		}
	last_mouse_x = x;
	last_mouse_y = y;

	return true;
}

static gboolean on_mouse_press (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	struct field *f;

	if (event->type == GDK_BUTTON_RELEASE){
		mouse_down = 0;
		//puts("mouse up in on_mouse_press");
	}
	else if (event->type == GDK_BUTTON_PRESS /*&& event->button == GDK_BUTTON_PRIMARY*/){

		//printf("mouse event at %d, %d\n", (int)(event->x), (int)(event->y));
		for (int i = 0; active_layout[i].cmd[0] > 0; i++) {
			f = active_layout + i;
			if (f->x < event->x && event->x < f->x + f->width 
					&& f->y < event->y && event->y < f->y + f->height){
				focus_field(f);
				if (f->fn){
					//we get out of the loop just to prevent two buttons from responding
					if (f->fn(f, NULL, GDK_BUTTON_PRESS, event->x, event->y, event->button))
						break;	
				}
			} 
		}
		last_mouse_x = (int)event->x;
		last_mouse_y = (int)event->y;
		mouse_down = 1;
	}
  /* We've handled the event, stop processing */
  return FALSE;
}


/*
Turns out (after two days of debugging) that GTK is not thread-safe and
we cannot invalidate the spectrum from another thread .
This redraw is called from another thread. Hence, we set a flag here 
that is read by a timer tick from the main UI thread and the window
is posted a redraw signal that in turn triggers the redraw_all routine.
Don't ask me, I only work around here.
*/
void redraw(){
	redraw_flag++;
}

/* hardware specific routines */

void init_gpio_pins(){
	for (int i = 0; i < 15; i++){
		pinMode(pins[i], INPUT);
		pullUpDnControl(pins[i], PUD_UP);
	}

	pinMode(PTT, INPUT);
	pullUpDnControl(PTT, PUD_UP);
	pinMode(DASH, INPUT);
	pullUpDnControl(DASH, PUD_UP);
}

int read_switch(int i){
	return digitalRead(i) == HIGH ? 0 : 1;
}

int key_poll(){

	int key = 0;
	if (digitalRead(PTT) == LOW)
		key |= CW_DASH;
	if (digitalRead(DASH) == LOW)
		key |= CW_DOT;
// zzzzzz
// key |= query_cw_paddle_isr_key_memory();
// clear_cw_paddle_isr_key_memory();
	//printf("key %d\n", key);
	return key;

}

void enc_init(struct encoder *e, int speed, int pin_a, int pin_b){
	e->pin_a = pin_a;
	e->pin_b = pin_b;
	e->speed = speed;
	e->history = 5;
}

int enc_state (struct encoder *e) {
	return (digitalRead(e->pin_a) ? 1 : 0) + (digitalRead(e->pin_b) ? 2: 0);
}

int enc_read(struct encoder *e) {
  int result = 0; 
  int newState;
  
  newState = enc_state(e); // Get current state  
    
  if (newState != e->prev_state)
     delay (1);
  
  if (enc_state(e) != newState || newState == e->prev_state)
    return 0; 

  //these transitions point to the encoder being rotated anti-clockwise
  if ((e->prev_state == 0 && newState == 2) || 
    (e->prev_state == 2 && newState == 3) || 
    (e->prev_state == 3 && newState == 1) || 
    (e->prev_state == 1 && newState == 0)){
      e->history--;
      //result = -1;
    }
  //these transitions point to the enccoder being rotated clockwise
  if ((e->prev_state == 0 && newState == 1) || 
    (e->prev_state == 1 && newState == 3) || 
    (e->prev_state == 3 && newState == 2) || 
    (e->prev_state == 2 && newState == 0)){
      e->history++;
    }
  e->prev_state = newState; // Record state for next pulse interpretation
  if (e->history > e->speed){
    result = 1;
    e->history = 0;
  }
  if (e->history < -e->speed){
    result = -1;
    e->history = 0;
  }
  return result;
}

void hw_init(){
	wiringPiSetup();
	init_gpio_pins();

	enc_init(&enc_a, ENC_FAST, ENC1_B, ENC1_A);
	enc_init(&enc_b, ENC_FAST, ENC2_A, ENC2_B);

	int e = g_timeout_add(1, ui_tick, NULL);

	wiringPiISR(ENC2_A, INT_EDGE_BOTH, tuning_isr);
	wiringPiISR(ENC2_B, INT_EDGE_BOTH, tuning_isr);
	wiringPiISR(PTT, INT_EDGE_FALLING, cw_paddle_isr);
	wiringPiISR(DASH, INT_EDGE_FALLING, cw_paddle_isr);
}

// zzzzz
void cw_paddle_isr(void){


  if (digitalRead(PTT) == LOW)
    cw_paddle_isr_key_memory |= CW_DASH;
  if (digitalRead(DASH) == LOW)
    cw_paddle_isr_key_memory |= CW_DOT;
}

int query_cw_paddle_isr_key_memory(){

  return cw_paddle_isr_key_memory;

}

void clear_cw_paddle_isr_key_memory(){

  cw_paddle_isr_key_memory = 0;

}

void hamlib_tx(int tx_input){
  if (tx_input){
    sound_input(1);
		tx_on();
	}
  else {
    sound_input(0);
		tx_off();
	}
}


int get_cw_delay(){
	return cw_delay;
}

int get_cw_input_method(){
	return cw_input_method;
}

int get_pitch(){
	struct field *f = get_field("#rx_pitch");
	return atoi(f->value);
}

int get_cw_tx_pitch(){
	return cw_tx_pitch;
}

int get_data_delay(){
	return data_delay;
}

int get_wpm(){
	struct field *f = get_field("#tx_wpm");
	return atoi(f->value);
}

static int tuning_ticks = 0;
void tuning_isr(void){
	int tuning = enc_read(&enc_b);
	if (tuning < 0)
		tuning_ticks++;
	if (tuning > 0)
		tuning_ticks--;	
}

gboolean ui_tick(gpointer gook){


  // this routine is executed every 1 mS

	int static ticks = 0;

	ticks++;

	//update all the fields, we should instead mark fields dirty and update only those
	if (redraw_flag){
		for (struct field *f = active_layout; f->cmd[0] > 0; f++)
			update_field(f);
		redraw_flag = 0;
	}

  //char message[100];
	
	// check the tuning knob
	struct field *f = get_field("r1:freq");

	if (abs(tuning_ticks) > 5)
		tuning_ticks *= 4;
	while (tuning_ticks > 0){
		edit_field(f, MIN_KEY_DOWN);
		tuning_ticks--;
    //sprintf(message, "tune-\r\n");
    //write_console(FONT_LOG, message);

	}
	while (tuning_ticks < 0){
		edit_field(f, MIN_KEY_UP);
		tuning_ticks++;
    //sprintf(message, "tune+\r\n");
    //write_console(FONT_LOG, message);
	}

	if (ticks == 100){  // execute this stuff every 100 mS

		struct field *f = get_field("spectrum");
		update_field(f);	//move this each time the spectrum watefall index is moved
		f = get_field("waterfall");
		update_field(f);
		f = get_field("#console");
		update_field(f);
		ticks = 0;
		update_field(get_field("#console"));
		update_field(get_field("#status"));

		if (digitalRead(ENC1_SW) == 0)
				focus_field(get_field("r1:volume"));

		if (record_start)
			update_field(get_field("#record"));

		// alternate character from the softkeyboard upon long press
		if (f_focus && focus_since + 500 < millis() 
						&& !strncmp(f_focus->cmd, "#kbd_", 5) && mouse_down){
			//emit the symbol
			struct field *f_text = get_field("#text_in");
			//replace the previous character with teh shifted one
			edit_field(f_text,MIN_KEY_BACKSPACE); 
			edit_field(f_text, f_focus->label[0]);
			focus_since = millis();
		}

    // check if low and high settings are stepping on each other
    char new_value[20];
    while (atoi(get_field("r1:low")->value) > atoi(get_field("r1:high")->value)){
      sprintf(new_value, "%d", atoi(get_field("r1:high")->value)+get_field("r1:high")->step);
      set_field("r1:high",new_value);
    }


    static char last_mouse_pointer_value[16];

    int cursor_type;

    if (strcmp(get_field("mouse_pointer")->value, last_mouse_pointer_value)){
      sprintf(last_mouse_pointer_value,get_field("mouse_pointer")->value);
      if (!strcmp(last_mouse_pointer_value,"BLANK")){
        cursor_type = GDK_BLANK_CURSOR;
      } else if (!strcmp(last_mouse_pointer_value,"RIGHT")){
        cursor_type = GDK_RIGHT_PTR;
      } else if (!strcmp(last_mouse_pointer_value,"CROSSHAIR")){
        cursor_type = GDK_CROSSHAIR;
      } else {
        cursor_type = GDK_LEFT_PTR;
      }
      GdkCursor* new_cursor;
      new_cursor = gdk_cursor_new_for_display (gdk_display_get_default(),cursor_type);
      gdk_window_set_cursor(gdk_get_default_root_window(), new_cursor);
    }

  }

  // this stuff executed every 1 mS

  modem_poll(mode_id(get_field("r1:mode")->value));
	update_field(get_field("#text_in")); //modem might have extracted some text

  hamlib_slice();
	remote_slice();
	//wsjtx_slice();
	save_user_settings(0);

 
	f = get_field("r1:mode");
	//straight key in CW
	if (f && (!strcmp(f->value, "2TONE") || !strcmp(f->value, "LSB") || 
	!strcmp(f->value, "USB"))){
		if (digitalRead(PTT) == LOW && in_tx == 0)
			tx_on();
		else if (digitalRead(PTT) == HIGH && in_tx == 1)
			tx_off();
	}

	int scroll = enc_read(&enc_a);
	if (scroll && f_focus){
		if (scroll < 0)
			edit_field(f_focus, MIN_KEY_DOWN);
		else
			edit_field(f_focus, MIN_KEY_UP);
	}	
	return TRUE;
}

void ui_init(int argc, char *argv[]){
 
  gtk_init( &argc, &argv );

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 480);
  gtk_window_set_title( GTK_WINDOW(window), "sBITX" );
	gtk_window_set_icon_from_file(GTK_WINDOW(window), "/home/pi/sbitx/sbitx_icon.png", NULL);

  display_area = gtk_drawing_area_new();
  gtk_container_add( GTK_CONTAINER(window), display_area );

  g_signal_connect( G_OBJECT(window), "destroy", G_CALLBACK( gtk_main_quit ), NULL );
  g_signal_connect( G_OBJECT(display_area), "draw", G_CALLBACK( on_draw_event ), NULL );
  g_signal_connect (G_OBJECT (window), "key_press_event", G_CALLBACK (on_key_press), NULL);
  g_signal_connect (G_OBJECT (window), "key_release_event", G_CALLBACK (on_key_release), NULL);
  g_signal_connect (G_OBJECT (window), "window_state_event", G_CALLBACK (on_window_state), NULL);
	g_signal_connect (G_OBJECT(display_area), "button_press_event", G_CALLBACK (on_mouse_press), NULL);
	g_signal_connect (G_OBJECT(window), "button_release_event", G_CALLBACK (on_mouse_release), NULL);
	g_signal_connect (G_OBJECT(display_area), "motion_notify_event", G_CALLBACK (on_mouse_move), NULL);
	g_signal_connect (G_OBJECT(display_area), "scroll_event", G_CALLBACK (on_scroll), NULL);
	g_signal_connect(G_OBJECT(window), "configure_event", G_CALLBACK(on_resize), NULL);

  /* Ask to receive events the drawing area doesn't normally
   * subscribe to. In particular, we need to ask for the
   * button press and motion notify events that want to handle.
   */
  gtk_widget_set_events (display_area, gtk_widget_get_events (display_area)
                                     | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK 
																			| GDK_SCROLL_MASK
                                     | GDK_POINTER_MOTION_MASK);

  gtk_widget_show_all( window );
	gtk_window_fullscreen(GTK_WINDOW(window));
	focus_field(get_field("r1:volume"));
}

/* handle modem callbacks for more data */

long get_freq(){
	return atol(get_field("r1:freq")->value);
}

//this is used to trigger an actual frequency change
//by eventually calling set_operating_freq() through do_cmd
//and update the frequency display as well
void set_freq(long freq){
	char cmd[100];
	sprintf(cmd, "r1:freq:%ld", freq);
	do_cmd(cmd);
}

void set_mode(char *mode){
	struct field *f = get_field("r1:mode");
	char umode[10];
	int i;

	for (i = 0; i < sizeof(umode) - 1 && *mode; i++)
		umode[i] = toupper(*mode++);
	umode[i] = 0;

	if (strstr(f->selection, umode)){
		strcpy(f->value, umode);
		update_field(f);
		redraw_flag++;
	}
	else
		write_console(FONT_LOG, "%s is not a mode\n");

}

void get_mode(char *mode){
	struct field *f = get_field("r1:mode");
	strcpy(mode, f->value);
}

int get_tx_data_byte(char *c){
	//take out the first byte and return it to the modem
	struct field *f = get_field("#text_in");
	int length = strlen(f->value);

	if (f->value[0] == '\\' || !length)
		return 0;
	if (length){
		*c = f->value[0];
		//now shift the buffer down, hopefully, this copies the trailing null too
		for (int i = 0; i < length; i++)
			f->value[i] = f->value[i+1];
	}
	return length;
	update_field(f);
	return *c;
}

int get_tx_data_length(){
	struct field *f = get_field("#text_in");

	if (strlen(f->value) == 0)
		return 0;

	if (f->value[0] != COMMAND_ESCAPE)
		return strlen(get_field("#text_in")->value);
	else
		return 0;
}

int is_in_tx(){
	return in_tx;
}

/* handle the ui request and update the controls */

void change_band(char *request){
	int i, old_band, new_band; 
	int max_bands = sizeof(band_stack)/sizeof(struct band);
	long new_freq, old_freq;
	char buff[100];

	//find the band that has just been selected, the first char is #, we skip it
	for (new_band = 0; new_band < max_bands; new_band++)
		if (!strcmp(request+1, band_stack[new_band].name))
			break;

	//continue if the band is legit
	if (new_band == max_bands)
		return;

	// find out the tuned frequency
	struct field *f = get_field("r1:freq");
	old_freq = atol(f->value);
	f = get_field("r1:mode");
	int old_mode = mode_id(f->value);
	if (old_mode == -1)
		return;

	//first, store this frequency in the appropriate bin
	for (old_band = 0; old_band < max_bands; old_band++)
		if (band_stack[old_band].start <= old_freq && old_freq <= band_stack[old_band].stop)
				break;

	int stack = band_stack[old_band].index;
	if (stack < 0 || stack >= STACK_DEPTH)
		stack = 0;
	if (old_band < max_bands){
		//update the old band setting 
		if (stack >= 0 && stack < STACK_DEPTH){
				band_stack[old_band].freq[stack] = old_freq;
				band_stack[old_band].mode[stack] = old_mode;
//				printf("bandstack, old band %s / stack %d being saved as  %ld / mode %d\n", 
//					band_stack[old_band].name, stack, old_freq, old_mode);
		}
	}

	//if we are still in the same band, move to the next position
	if (new_band == old_band){
		stack = ++band_stack[new_band].index;
		//move the stack and wrap the band around
		if (stack >= STACK_DEPTH)
			stack = 0;
		band_stack[new_band].index = stack;
//		printf("Band stack moved to %s, stack %d\n", band_stack[new_band].name, stack);
	}
	stack = band_stack[new_band].index;
//	printf("Band stack changed to %s, stack %d : %d / mode %d\n", band_stack[new_band].name, stack,
//			band_stack[new_band].freq[stack], band_stack[new_band].mode[stack]);
	sprintf(buff, "%d", band_stack[new_band].freq[stack]);
	char resp[100];
	set_operating_freq(band_stack[new_band].freq[stack], resp);
	set_field("r1:freq", buff);	
	set_field("r1:mode", mode_name[band_stack[new_band].mode[stack]]);	

	clear_tx_text_buffer();
}



void utc_set(char *args){
	int n[7], i;
	char *p, *q;
	struct tm t;
	time_t gm_now;

	i = 0;
	p =  strtok(args, "-/;: ");
	if (p){
		n[0] = atoi(p);
		for (i = 1; i < 7; i++){
			p = strtok(NULL, "-/;: ");
			if (!p)
				break;
			n[i] = atoi(p);
		}
	}	

	if (i != 6 ){
		write_console(FONT_LOG, 
			"Sets the current UTC Time for logging etc.\nUsage \\utc yyyy mm dd hh mm ss\nWhere\n"
			"  yyyy is a four digit year like 2022\n"
			"  mm is two digit month [1-12]\n"
			"  dd is two digit day of the month [0-31]\n"
			"  hh is two digit hour in 24 hour format (UTC)\n"
			"  mm is two digit minutes in 24 hour format(UTC)\n"
			"  ss is two digit seconds in [0-59]\n"
			"ex: \\utc 2022 07 14 8:40:00\n"); 
			return;
	}
	if (n[0] < 2000)
		n[0] += 2000;
	t.tm_year = n[0] - 1900;
	t.tm_mon = n[1] - 1;
	t.tm_mday = n[2]; 
	t.tm_hour = n[3];
	t.tm_min = n[4];
	t.tm_sec = n[5];

	tzname[0] = tzname[1] = "GMT";
	timezone = 0;
	daylight = 0;
	setenv("TZ", "UTC", 1);	
	gm_now = mktime(&t);

	write_console(FONT_LOG, "UTC time is set\n");
	time_delta =(long)gm_now -(long)(millis()/1000l);
	printf("time_delta = %ld\n", time_delta);
}

void do_cmd(char *cmd){	
	char request[1000], response[1000], buff[100];
	
	strcpy(request, cmd);			//don't mangle the original, thank you

	if (!strcmp(request, "#close"))
		gtk_window_iconify(GTK_WINDOW(window));
	else if (!strcmp(request, "#off")){
		tx_off();
		set_field("#record", "OFF");
		save_user_settings(1);
		exit(0);
	}
	else if (!strcmp(request, "#tx")){	
		tx_on();
	}
	else if (!strcmp(request, "#rx")){
		tx_off();
	}
	else if (!strncmp(request, "#rit", 4))
		update_field(get_field("r1:freq"));
	else if (!strncmp(request, "#split", 5)){
		update_field(get_field("r1:freq"));	
		if (!strcmp(get_field("#vfo")->value, "B"))
			set_field("#vfo", "A");
	}
	else if (!strcmp(request, "#vfo=B")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		if (!strcmp(vfo->value, "B")){
			vfo_a_freq = atoi(f->value);
			sprintf(buff, "%d", vfo_b_freq);
			set_field("r1:freq", buff);
			settings_updated++;
		}
	}
	else if (!strcmp(request, "#vfo=A")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		//printf("vfo old %s, new %s\n", vfo->value, request);
		if (!strcmp(vfo->value, "A")){
			vfo_b_freq = atoi(f->value);
			sprintf(buff, "%d", vfo_a_freq);
			set_field("r1:freq", buff);
			settings_updated++;
		}
	}
	//tuning step
  else if (!strcmp(request, "#step=1MHz"))
    tuning_step = 1000000;
	else if (!strcmp(request, "#step=100KHz"))
		tuning_step = 100000;
	else if (!strcmp(request, "#step=10KHz"))
		tuning_step = 10000;
	else if (!strcmp(request, "#step=1KHz"))
		tuning_step = 1000;
	else if (!strcmp(request, "#step=100Hz"))
		tuning_step = 100;
	else if (!strcmp(request, "#step=10Hz"))
		tuning_step = 10;

	//spectrum bandwidth
	else if (!strcmp(request, "#span=2.5KHz"))
		spectrum_span = 2500;
	else if (!strcmp(request, "#span=6KHz"))
		spectrum_span = 6000;
	else if (!strcmp(request, "#span=10KHz"))
		spectrum_span = 10000;
	else if (!strcmp(request, "#span=25KHz"))
		spectrum_span = 25000;
		
	//handle the band stacking
	else if (!strcmp(request, "#80m") || 
		!strcmp(request, "#40m") || 
		!strcmp(request, "#30m") || 
		!strcmp(request, "#20m") || 
		!strcmp(request, "#17m") || 
		!strcmp(request, "#15m") || 
		!strcmp(request, "#12m") || 
		!strcmp(request, "#10m")){
		change_band(request);		
	}
	else if (!strcmp(request, "#record=ON")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it

		char *path = getenv("HOME");
		time(&record_start);
		struct tm *tmp = localtime(&record_start);
		sprintf(fullpath, "%s/sbitx/audio/%04d%02d%02d-%02d%02d-%02d.wav", path, 
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 

		char request[300], response[100];
		sprintf(request, "record=%s", fullpath);
		sdr_request(request, response);
		write_console(FONT_LOG, "Recording ");
		write_console(FONT_LOG, fullpath);
		write_console(FONT_LOG, "\n");
	}
	else if (!strcmp(request, "#record=OFF")){
		sdr_request("record", "off");
		write_console(FONT_LOG, "Recording stopped\n");
		record_start = 0;
	}
	else if (!strcmp(request, "#mfqrz") && strlen(contact_callsign) > 0)
		qrz(contact_callsign);

	//this needs to directly pass on to the sdr core
	else if(request[0] != '#'){
		//translate the frequency of operating depending upon rit, split, etc.
		if (!strncmp(request, "r1:freq", 7))
			set_operating_freq(atoi(request+8), response);
		else
			sdr_request(request, response);
		if (!strncmp(request, "tx_power=", strlen("tx_power="))){
			band_stack_update_power(atoi(request+strlen("tx_power=")));
		}
	}
}


void cmd_exec(char *cmd){
	int i, j;
	int mode = mode_id(get_field("r1:mode")->value);

	char args[MAX_FIELD_LENGTH];
	char exec[20];

  args[0] = 0;

	//copy the exec
	for (i = 0; *cmd > ' ' && i < sizeof(exec) - 1; i++)
		exec[i] = *cmd++;
	exec[i] = 0; 

	//skip the spaces
	while(*cmd == ' ')
		cmd++;

	j = 0;
	for (i = 0; *cmd && i < sizeof(args) - 1; i++){
		if (*cmd > ' ')
				j = i;
		args[i] = *cmd++;
	}
	args[++j] = 0;

	char response[100];
	if (!strcmp(exec, "callsign")){
		strcpy(mycallsign,args); 
		sprintf(response, "\n[Your callsign is set to %s]\n", mycallsign);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "grid")){
		strcpy(mygrid, args);
		sprintf(response, "\n[Your grid is set to %s]\n", mygrid);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "utc")){
		utc_set(args);
	}
	else if (!strcmp(exec, "l")){
		interpret_log(args);
		update_log_ed();
	}
	else if (!strcmp(exec, "logbook")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it
		char *path = getenv("HOME");
		sprintf(fullpath, "mousepad %s/sbitx/data/logbook.txt", path); 
		execute_app(fullpath);
	}
	else if (!strcmp(exec, "clear")){
		console_init();
		redraw_flag++;
	}
	else if(!strcmp(exec, "macro")){
		if (!strcmp(args, "list"))
			macro_list();
		else if (!macro_load(args)){
			set_ui(LAYOUT_MACROS);
			strcpy(current_macro, args);
			settings_updated++;
			redraw_flag++;
		}
		else if (strlen(current_macro)){
			write_console(FONT_LOG, "current macro is ");
			write_console(FONT_LOG, current_macro);
			write_console(FONT_LOG, "\n");
		}
		else
			write_console(FONT_LOG, "macro file not loaded\n");
	}
	else if (!strcmp(exec, "exchange")){
		sent_exchange[0] = 0;
		contest_serial = 0;

		if (atoi(args) == 1)
			contest_serial = 1;
		else if (strlen(args) > 1)
			strcpy(sent_exchange, args);
		else
			sent_exchange[0] = 0;
		write_console(FONT_LOG, "Exchange set to [");
		if (contest_serial > 0){
			sprintf(sent_exchange, "%04d", contest_serial);
		}
		write_console(FONT_LOG, sent_exchange);
		write_console(FONT_LOG, "]\n");
	}
	else if(!strcmp(exec, "freq") || !strcmp(exec, "f")){
		long freq = atol(args);
		if (freq == 0){
			write_console(FONT_LOG, "Usage: \f xxxxx (in Hz or KHz)\n");
		}
		else if (freq < 30000)
			freq *= 1000;

		if (freq > 0){
			char freq_s[20];
			sprintf(freq_s, "%ld",freq);
			set_field("r1:freq", freq_s);
		}
	}
	else if (!strcmp(exec, "cwdelay")){
		if (strlen(args)){
			int d = atoi(args);
			if (d < 50 || d > 2000)
				write_console(FONT_LOG, "cwdelay should be between 100 and 2000 msec");
			else 
				cw_delay = d;
		}	
		char buff[10];
		sprintf(buff, "cwdelay: %d msec\n", cw_delay);
		write_console(FONT_LOG, buff);
	}

  else if (!strcmp(exec, "exit")){
    tx_off();
    set_field("#record", "OFF");
    save_user_settings(1);
    exit(0);
  }


  else if ((!strcmp(exec, "lsb")) || (!strcmp(exec, "LSB"))){
    set_mode("LSB");
  }
  else if ((!strcmp(exec, "usb")) || (!strcmp(exec, "USB"))){
    set_mode("USB"); 
  }
  else if ((!strcmp(exec, "cw")) || (!strcmp(exec, "CW"))){
    set_mode("CW"); 
  }
  else if ((!strcmp(exec, "cwr")) || (!strcmp(exec, "CWR"))){
    set_mode("CWR");
  }
  else if ((!strcmp(exec, "rtty")) || (!strcmp(exec, "RTTY"))){
    set_mode("RTTY");
  }
  else if ((!strcmp(exec, "ft8")) || (!strcmp(exec, "FT8"))){
    set_mode("FT8");
  }
  else if ((!strcmp(exec, "psk")) || (!strcmp(exec, "psk31")) || (!strcmp(exec, "PSK")) || (!strcmp(exec, "PSK31"))){
    set_mode("PSK31");
  }
  else if ((!strcmp(exec, "digital")) || (!strcmp(exec, "DIGITAL"))  || (!strcmp(exec, "dig")) || (!strcmp(exec, "DIG"))){
    set_mode("DIGITAL");
  }
  else if ((!strcmp(exec, "2tone")) || (!strcmp(exec, "2TONE"))){
    set_mode("2TONE");
  }

  else if ((!strcmp(exec, "help")) || (!strcmp(exec, "?"))){
    write_console(FONT_LOG, "Help\r\n\r\n");
    write_console(FONT_LOG, "\\audio\r\n");
    write_console(FONT_LOG, "\\callsign [your callsign]\r\n");
    write_console(FONT_LOG, "\\clear - clear the console display\r\n");
    write_console(FONT_LOG, "\\cwinput [key|keyer|kbd]\r\n");
    write_console(FONT_LOG, "\\cwdelay\r\n");
    write_console(FONT_LOG, "\\cw_tx_pitch\r\n");
    write_console(FONT_LOG, "\\exchange\r\n");
    write_console(FONT_LOG, "\\freq\r\n");
    write_console(FONT_LOG, "\\ft8mode [auto|semiauto|manual]\r\n");
    write_console(FONT_LOG, "\\grid [your grid]\r\n");
    write_console(FONT_LOG, "\\l [callsign] [rst]\r\n");
    write_console(FONT_LOG, "\\logbook\r\n");
    write_console(FONT_LOG, "\\macro [macro name]\r\n");
    write_console(FONT_LOG, "\\mode [CW|CWR|USB|LSB|RTTY|FT8|DIGITAL|2TONE]\r\n");
    write_console(FONT_LOG, "\\qrz [callsign]\r\n");
    write_console(FONT_LOG, "\\r - receive\r\n");
    write_console(FONT_LOG, "\\sidetone\r\n");
    write_console(FONT_LOG, "\\t - transmit\r\n");
    write_console(FONT_LOG, "\\telnet [server]:[port]\r\n");
    write_console(FONT_LOG, "\\tclose - close telnet session\r\n");
    write_console(FONT_LOG, "\\txpitch [100-3000]\r\n");
    write_console(FONT_LOG, "\\wpm [cw words per minute]\r\n");
    write_console(FONT_LOG, "Do \\h2 command for help page 2...\r\n");
  }

  else if (!strcmp(exec, "h2")){ 
    write_console(FONT_LOG, "Help - Page 2\r\n\r\n");
    write_console(FONT_LOG, "\\exit\r\n\r\n");
    write_console(FONT_LOG, "\\s - view settings\r\n");
    write_console(FONT_LOG, "\\mp [BLANK|LEFT|RIGHT|CROSSHAIR] - mouse pointer style\r\n");
    write_console(FONT_LOG, "\\rs [ON|OFF] - reverse scrolling\r\n\r\n");
    write_console(FONT_LOG, "\\ta [ON|OFF] - Turns tuning acceleration on and off\r\n");
    write_console(FONT_LOG, "\\tat1 [100-99999] - 1st threshold at which acceleration occurs (default: 10,000)\r\n");
    write_console(FONT_LOG, "\\tat2 [100-99999] - 2nd threshold at which acceleration occurs (default: 500)\r\n\r\n");
    write_console(FONT_LOG, "\\cw \\cwr \\usb \\lsb \\rtty \\ft8 \\digital \\dig \\2tone\r\n");
  }

  else if ((!strcmp(exec, "s")) || (!strcmp(exec, "settings"))){
    write_console(FONT_LOG, "Settings\r\n\r\n");
    char temp_string[100];
    sprintf(temp_string,"Reverse Scrolling: ");
    strcat(temp_string,get_field("reverse_scrolling")->value);
    write_console(FONT_LOG, temp_string);
    sprintf(temp_string,"\r\nTuning Acceleration: ");
    strcat(temp_string,get_field("reverse_scrolling")->value);
    write_console(FONT_LOG, temp_string);
    sprintf(temp_string,"\r\nTuning Acceleration Threshold 1: ");
    strcat(temp_string,get_field("tuning_accel_thresh1")->value);
    write_console(FONT_LOG, temp_string);
    sprintf(temp_string,"\r\nTuning Acceleration Threshold 2: ");
    strcat(temp_string,get_field("tuning_accel_thresh2")->value);
    write_console(FONT_LOG, temp_string);
    sprintf(temp_string,"\r\nMouse Pointer: ");
    strcat(temp_string,get_field("mouse_pointer")->value);
    write_console(FONT_LOG, temp_string);
    write_console(FONT_LOG, "\r\n");

  }

	else if (!strcmp(exec, "ft8mode")){
		switch(args[0]){
			case 'a':
			case 'A':
				ft8_setmode(FT8_AUTO);
				write_console(FONT_LOG, "ft8mode set to auto\n");
				break;
			case 's':
			case 'S':
				ft8_setmode(FT8_SEMI);
				write_console(FONT_LOG, "ft8mode set to semiauto\n");
				break;
			case 'm':
			case 'M':
				ft8_setmode(FT8_MANUAL);
				write_console(FONT_LOG, "ft8mode set to manual\n");
				break;
			default:
				write_console(FONT_LOG, "Usage: \\ft8mode auto or semi or manual\n");
				break;
		}
	}
	else if (!strcmp(exec, "sidetone")){
		char buff[50];
		if (strlen(args)){
			int temp_sidetone = atoi(args);
			if (temp_sidetone >= 0 && temp_sidetone <= 100){
				sidetone = temp_sidetone;
				sprintf(buff, "sidetone=%d", sidetone);
				char response[10]; 
				sdr_request(buff, response);
			}
		}
		sprintf(buff, "sidetone: set to %d/100\n", sidetone);
		write_console(FONT_LOG, buff);
	}
	else if (!strcmp(exec, "cwinput")){
		if (strlen(args)){
			if (!strcmp(args, "kbd"))
				cw_input_method = CW_KBD;
			else if(!strcmp(args, "key"))
				cw_input_method = CW_STRAIGHT;
			else if (!strcmp(args, "keyer"))
				cw_input_method = CW_IAMBIC;
		}
		char buff[40];
		if (cw_input_method == CW_KBD)
			strcpy(buff, "cwinput = kbd [kbd/key/keyer]");
		else if (cw_input_method == CW_STRAIGHT)
			strcpy(buff, "cwinput = key [kbd/key/keyer]");
		else if (cw_input_method == CW_IAMBIC)
			strcpy(buff, "cwinput = keyer [kbd/key/keyer]");
		else
			strcpy(buff, "cwinput  = [kbd/key/keyer]");
		write_console(FONT_LOG, buff);
	}
	else if (!strcmp(exec, "qrz")){
		if(strlen(args))
			qrz(args);
		else if (strlen(contact_callsign))
			qrz(contact_callsign);
		else
			write_console(FONT_LOG, "/qrz [callsign]\n");
	}
	else if (!strcmp(exec, "mode") || !strcmp(exec, "m"))
		set_mode(args);
	else if (!strcmp(exec, "t"))
		tx_on();
	else if (!strcmp(exec, "r"))
		tx_off();
	else if (!strcmp(exec, "telnet"))
		telnet_open(args);
	else if (!strcmp(exec, "tclose"))
		telnet_close(args);
	else if (!strcmp(exec, "tel"))
		telnet_write(args);
	else if (!strcmp(exec, "txpitch")){
		if (strlen(args)){
			int t = atoi(args);	
			if (t > 100 && t < 4000)
				cw_tx_pitch = t;
			else
				write_console(FONT_LOG, "cw pitch should be 100-4000");
		}
		char buff[100];
		sprintf(buff, "cw txpitch is set to %d Hz\n", cw_tx_pitch);
		write_console(FONT_LOG, buff);
		redraw_flag++;
	}
	else {
		//see if it matches any of the fields of the UI that have FIELD_NUMBER 
		char field_name[32];
		struct field *f = get_field_by_label(exec);
		if (f){
			//convert all the letters to uppercase
			for(char *p = args; *p; p++)
					*p = toupper(*p);
			if(set_field(f->cmd, args)){
				write_console(FONT_LOG, "Invalid setting\r\n");
      } else {
        write_console(FONT_LOG, "\\");
        write_console(FONT_LOG, exec);
        write_console(FONT_LOG, " ");
        write_console(FONT_LOG, args);        
        write_console(FONT_LOG, "\r\n");
      }
		}
	}
	save_user_settings(0);
}

int main( int argc, char* argv[] ) {

	puts(VER_STR);
	active_layout = main_controls;

	//unlink any pending ft8 transmission
	unlink("/home/pi/sbitx/ft8tx_float.raw");
	call_wipe();
	strcpy(sent_exchange, "");

	ui_init(argc, argv);
	hw_init();
	console_init();

	setup();
	struct field *f;
	f = active_layout;

	//initialize the modulation display

	tx_mod_max = get_field("spectrum")->width;
	tx_mod_buff = malloc(sizeof(int32_t) * tx_mod_max);
	memset(tx_mod_buff, 0, sizeof(int32_t) * tx_mod_max);
	tx_mod_index = 0;
	init_waterfall();

	//set the radio to some decent defaults
	do_cmd("r1:freq=7100000");
	do_cmd("r1:mode=LSB");	
	do_cmd("#step=1000");	
  do_cmd("#span=25KHZ");
	strcpy(vfo_a_mode, "USB");
	strcpy(vfo_b_mode, "LSB");
	strcpy(mycallsign, "vu2lch");
	strcpy(mygrid, "Mk97fj");
	current_macro[0] = 0;
	vfo_a_freq = 14000000;
	vfo_b_freq = 7000000;
	
	f = get_field("spectrum");
	update_field(f);
	set_volume(20000000);

	set_field("r1:freq", "7000000");
	set_field("r1:mode", "USB");
	set_field("tx_gain", "24");
	set_field("tx_power", "40");
	set_field("r1:gain", "41");
	set_field("r1:volume", "85");

	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/user_settings.ini");
  if (ini_parse(directory, user_settings_handler, NULL)<0){
    printf("Unable to load ~/sbitx/data/user_settings.ini\n");
  }

	if (strlen(current_macro))
		macro_load(current_macro);
	char buff[1000];

	//now set the frequency of operation and more to vfo_a
  sprintf(buff, "%d", vfo_a_freq);
  set_field("r1:freq", buff);

	console_init();
	write_console(FONT_LOG, VER_STR);
  write_console(FONT_LOG, "\r\nEnter \\help for help\r\n");

	if (strcmp(mycallsign, "N0BDY")){
		sprintf(buff, "\nWelcome %s your grid is %s\n", mycallsign, mygrid);
		write_console(FONT_LOG, buff);
	}
	else 
		write_console(FONT_LOG, "Set your with '\\callsign [yourcallsign]'\n"
		"Set your 6 letter grid with '\\grid [yourgrid]\n");

	set_field("#text_in", "");

	// you don't want to save the recently loaded settings
	settings_updated = 0;
  hamlib_start();
	remote_start();

	int not_synchronized = 0;
	FILE *pf = popen("chronyc tracking", "r");
	while(fgets(buff, sizeof(buff), pf)) 
		if(strstr(buff, "Not synchronised"))
			not_synchronized = 1; 
	fclose(pf);

	if (not_synchronized)
		write_console(FONT_LOG, "Enter the precise UTC time using \\utc command\n"
		"ex: \\utc 2022/07/15 23:034:00\n"
		"Hit enter for the command at the exact time\n");

  gtk_main();
  
  return 0;
}

