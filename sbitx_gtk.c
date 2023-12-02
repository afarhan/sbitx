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
#include <sys/file.h>
#include <errno.h>
#include <sys/file.h>
#include <errno.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include "sdr.h"
#include "sound.h"
#include "sdr_ui.h"
#include "ini.h"
#include "hamlib.h"
#include "remote.h"
#include "modem_ft8.h"
#include "i2cbb.h"
#include "webserver.h"
#include "logbook.h"

#define FT8_START_QSO 1
#define FT8_CONTINUE_QSO 0
void ft8_process(char *received, int operation);

/* command  buffer for commands received from the remote */
struct Queue q_remote_commands;
struct Queue q_tx_text;

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

#define DS3231_I2C_ADD 0x68
//time sync, when the NTP time is not synced, this tracks the number of seconds 
//between the system cloc and the actual time set by \utc command
static long time_delta = 0;

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
#define COLOR_RX_PITCH 13
#define SELECTED_LINE 14
#define COLOR_FIELD_SELECTED 15 
#define COLOR_TX_PITCH 16

float palette[][3] = {
	{1,1,1}, 		// COLOR_SELECTED_TEXT
	{0,1,1},		// COLOR_TEXT
	{0.5,0.5,0.5}, //COLOR_TEXT_MUTED
	{1,1,0},		// COLOR_SELECTED_BOX
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
	{0,1,0},	//COLOR_RX__PITCH
	{0.1, 0.1, 0.2}, //SELECTED_LINE
	{0.1, 0.1, 0.2}, // COLOR_FIELD_SELECTED
	{1,0,0},	//COLOR_TX_PITCH
};

char *ui_font = "Sans";
int field_font_size = 12;
int screen_width=800, screen_height=480;

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
	{FONT_LOG, 1, 1, 1, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL_FIELD_VALUE, 1, 1, 1, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_CW_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_CW_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FLDIGI_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FLDIGI_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_TELNET, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_QUEUED, 0.5, 0.5, 0.5, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_REPLY, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
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
struct Queue q_web;


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
void set_bandwidth(int hz);

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
GtkWidget *text_area = NULL;

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
	int 	section;
	char is_dirty;
	char update_remote;
	void *data;
};

#define STACK_DEPTH 4

struct band {
	char name[10];
	int	start;
	int	stop;
	//int	power;
	//int	max;
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
static struct field *f_last_text = NULL;

//variables to power up and down the tx

static int in_tx = TX_OFF;
static int key_down = 0;
static int tx_start_time = 0;

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
	{"80M", 3500000, 4000000, 0, 
		{3500000,3574000,3600000,3700000},{MODE_CW, MODE_USB, MODE_CW,MODE_LSB}},
	{"40M", 7000000,7300000, 0,
		{7000000,7040000,7074000,7150000},{MODE_CW, MODE_CW, MODE_USB, MODE_LSB}},
	{"30M", 10100000, 10150000, 0,
		{10100000, 10100000, 10136000, 10150000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"20M", 14000000, 14400000, 0,
		{14010000, 14040000, 14074000, 14200000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"17M", 18068000, 18168000, 0,
		{18068000, 18100000, 18110000, 18160000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"15M", 21000000, 21500000, 0,
		{21010000, 21040000, 21074000, 21250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"12M", 24890000, 24990000, 0,
		{24890000, 24910000, 24950000, 24990000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"10M", 28000000, 29700000, 0,
		{28000000, 28040000, 28074000, 28250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
};


#define VFO_A 0 
#define VFO_B 1 
//int	vfo_a_freq = 7000000;
//int	vfo_b_freq = 14000000;
char vfo_a_mode[10];
char vfo_b_mode[10];

//usefull data for macros, logging, etc
int	tx_id = 0;

//recording duration in seconds
time_t record_start = 0;
int	data_delay = 700;

#define MAX_RIT 25000

int spectrum_span = 48000;
extern int spectrum_plot[];
extern int fwdpower, vswr;

void do_control_action(char *cmd);
void cmd_exec(char *cmd);


int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_waterfall(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_tuning(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_text(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_pitch(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_toggle_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_mouse_move(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_record(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_bandwidth(struct field *f, cairo_t *gfx, int event, int a, int b, int c);

struct field *active_layout = NULL;
char settings_updated = 0;
#define LAYOUT_KBD 0
#define LAYOUT_MACROS 1
int current_layout = LAYOUT_KBD;

#define COMMON_CONTROL 1
#define FT8_CONTROL 2
#define CW_CONTROL 4
#define VOICE_CONTROL 8
#define DIGITAL_CONTROL 16


// the cmd fields that have '#' are not to be sent to the sdr
struct field main_controls[] = {
	/* band stack registers */
	{"#10m", NULL, 50, 5, 40, 40, "10M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#12m", NULL, 90, 5, 40, 40, "12M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#15m", NULL, 130, 5, 40, 40, "15M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0i,COMMON_CONTROL},
	{"#17m", NULL, 170, 5, 40, 40, "17M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#20m", NULL, 210, 5, 40, 40, "20M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#30m", NULL, 250, 5, 40, 40, "30M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#40m", NULL, 290, 5, 40, 40, "40M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#80m", NULL, 330, 5, 40, 40, "80M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{ "#record", do_record, 380, 5, 40, 40, "REC", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0, 0,COMMON_CONTROL},
	{ "#web", NULL, 430,5,  40, 40, "WEB", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0, 0,COMMON_CONTROL},
	{ "r1:gain", NULL, 375, 5, 40, 40, "IF", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1,COMMON_CONTROL},
	{ "r1:agc", NULL, 415, 5, 40, 40, "AGC", 40, "SLOW", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"OFF/SLOW/MED/FAST", 0, 1024, 1,COMMON_CONTROL},
	{ "tx_power", NULL, 455, 5, 40, 40, "DRIVE", 40, "40", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 1, 100, 1,COMMON_CONTROL},


	{ "r1:freq", do_tuning, 600, 0, 150, 49, "FREQ", 5, "14000000", FIELD_NUMBER, FONT_LARGE_VALUE, 
		"", 500000, 30000000, 100,COMMON_CONTROL},

	{ "r1:volume", NULL, 755, 5, 40, 40, "AUDIO", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1,COMMON_CONTROL},

	{"#step", NULL, 560, 5 ,40, 40, "STEP", 1, "10Hz", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"10K/1K/100H/10H", 0,0,0,COMMON_CONTROL},
	{"#span", NULL, 560, 50 , 40, 40, "SPAN", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"25K/10K/6K/2.5K", 0,0,0,COMMON_CONTROL},

	{"#rit", NULL, 600, 50, 40, 40, "RIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0,COMMON_CONTROL},
	{"#vfo", NULL, 640, 50 , 40, 40, "VFO", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"A/B", 0,0,0,COMMON_CONTROL},
	{"#split", NULL, 680, 50, 40, 40, "SPLIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0,COMMON_CONTROL},

	{ "#bw", do_bandwidth, 495, 5, 40, 40, "BW", 40, "", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 50, 5000, 50,COMMON_CONTROL},

	{ "r1:mode", NULL, 5, 5, 40, 40, "MODE", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"USB/LSB/CW/CWR/FT8/PSK31/RTTY/DIGITAL/2TONE", 0,0,0, COMMON_CONTROL},

	/* logger controls */

	{"#contact_callsign", do_text, 5, 50, 85, 20, "CALL", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0,11,0,COMMON_CONTROL},
	{"#rst_sent", do_text, 90, 50, 50, 20, "SENT", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#rst_received", do_text, 140, 50, 50, 20, "RECV", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#exchange_received", do_text, 190, 50, 50, 20, "EXCH", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#exchange_sent", do_text, 240, 50, 50, 20, "NR", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#enter_qso", NULL, 290, 50, 40, 40, "LOG\u00bb", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#wipe", NULL, 330, 50, 40, 40, "WIPE", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{"#mfqrz", NULL, 370, 50, 40, 40, "QRZ", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{"#text_in", do_text, 5, 70, 285, 20, "TEXT", 70, "text box", FIELD_TEXT, FONT_LOG, 
		"nothing valuable", 0,128,0,COMMON_CONTROL},

	{ "#toggle_kbd", do_toggle_kbd, 495, 50, 40, 40, "KBD", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0, 0,COMMON_CONTROL},

/* end of common controls */

	//tx 
	{ "tx_gain", NULL, 550, -350, 50, 50, "MIC", 40, "50", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1, VOICE_CONTROL},

	{ "tx_compress", NULL, 600, -350, 50, 50, "COMP", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"ON/OFF", 0,100,10, VOICE_CONTROL},
	{ "#tx_wpm", NULL, 650, -350, 50, 50, "WPM", 40, "12", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 1, 50, 1, CW_CONTROL},
	{ "rx_pitch", do_pitch, 700, -350, 50, 50, "PITCH", 40, "600", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 100, 3000, 10, FT8_CONTROL | DIGITAL_CONTROL},
	

	{ "#tx", NULL, 1000, -1000, 50, 50, "TX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0, VOICE_CONTROL},

	{ "#rx", NULL, 650, -400, 50, 50, "RX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0, VOICE_CONTROL},
	

	{"r1:low", NULL, 400, -400, 50, 50, "LOW", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0,4000, 50, 0},
	{"r1:high", NULL, 450, -400, 50, 50, "HIGH", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 300, 10000, 50, 0},

	{"spectrum", do_spectrum, 400, 101, 400, 100, "SPECTRUM", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0, COMMON_CONTROL},  
	{"#status", do_status, -1000, -1000, 400, 29, "STATUS", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"status", 0,0,0, 0},  

	{"waterfall", do_waterfall, 400, 201 , 400, 99, "WATERFALL", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0, COMMON_CONTROL},
	{"#console", do_console, 0, 100, 400, 200, "CONSOLE", 70, "console box", FIELD_CONSOLE, FONT_LOG, 
		"nothing valuable", 0,0,0, COMMON_CONTROL},

	{"#log_ed", NULL, 0, 480, 480, 20, "", 70, "", FIELD_STATIC, FONT_LOG, 
		"nothing valuable", 0,128,0, 0},
  // other settings - currently off screen
  { "reverse_scrolling", NULL, 1000, -1000, 50, 50, "RS", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0, 0},
  { "tuning_acceleration", NULL, 1000, -1000, 50, 50, "TA", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0, 0},
  { "tuning_accel_thresh1", NULL, 1000, -1000, 50, 50, "TAT1", 40, "10000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100, 0},
  { "tuning_accel_thresh2", NULL, 1000, -1000, 50, 50, "TAT2", 40, "500", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100, 0},
  { "mouse_pointer", NULL, 1000, -1000, 50, 50, "MP", 40, "LEFT", FIELD_SELECTION, FONT_FIELD_VALUE,
    "BLANK/LEFT/RIGHT/CROSSHAIR", 0,0,0,0},

	// Settings Panel
	{"#mycallsign", NULL, 1000, -1000, 400, 149, "MYCALLSIGN", 70, "CALL", FIELD_TEXT, FONT_SMALL, 
		"", 3,10,1,0},
	{"#mygrid", NULL, 1000, -1000, 400, 149, "MYGRID", 70, "NOWHERE", FIELD_TEXT, FONT_SMALL, 
		"", 4,6,1,0},
	{"#passkey", NULL, 1000, -1000, 400, 149, "PASSKEY", 70, "123", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1,0},

	//moving global variables into fields 	
  { "#vfo_a_freq", NULL, 1000, -1000, 50, 50, "VFOA", 40, "14000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 500000,30000000,1,0},
  {"#vfo_b_freq", NULL, 1000, -1000, 50, 50, "VFOB", 40, "7000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 500000,30000000,1,0},
  {"#rit_delta", NULL, 1000, -1000, 50, 50, "RIT_DELTA", 40, "000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -25000,25000,1,0},

  { "#cwinput", NULL, 1000, -1000, 50, 50, "CW_INPUT", 40, "KEYBOARD", FIELD_SELECTION, FONT_FIELD_VALUE,
		"IAMBIC/IAMBICB/STRAIGHT", 0,0,0, CW_CONTROL},
  { "#cwdelay", NULL, 1000, -1000, 50, 50, "CW_DELAY", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 50, 1000, 50, CW_CONTROL},
	{ "#tx_pitch", NULL, 400, -1000, 50, 50, "TX_PITCH", 40, "600", FIELD_NUMBER, FONT_FIELD_VALUE, 
    "", 300, 3000, 10, FT8_CONTROL},
	{ "sidetone", NULL, 1000, -1000, 50, 50, "SIDETONE", 40, "25", FIELD_NUMBER, FONT_FIELD_VALUE, 
    "", 0, 100, 5, CW_CONTROL},
	{"#sent_exchange", NULL, 1000, -1000, 400, 149, "SENT_EXCHANGE", 70, "", FIELD_TEXT, FONT_SMALL, 
		"", 0,10,1, COMMON_CONTROL},
  { "#contest_serial", NULL, 1000, -1000, 50, 50, "CONTEST_SERIAL", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,1000000,1, COMMON_CONTROL},
	{"#current_macro", NULL, 1000, -1000, 400, 149, "MACRO", 70, "", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1, COMMON_CONTROL},
  { "#fwdpower", NULL, 1000, -1000, 50, 50, "POWER", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,10000,1, COMMON_CONTROL},
  { "#vswr", NULL, 1000, -1000, 50, 50, "REF", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,10000, 1, COMMON_CONTROL},
  { "bridge", NULL, 1000, -1000, 50, 50, "BRIDGE", 40, "100", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 10,100, 1, COMMON_CONTROL},
	//cw, ft8 and many digital modes need abort
	{"#abort", NULL, 370, 50, 40, 40, "ESC", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,CW_CONTROL}, 

	//FT8 should be 4000 Hz
  {"#bw_voice", NULL, 1000, -1000, 50, 50, "BW_VOICE", 40, "2200", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},
  {"#bw_cw", NULL, 1000, -1000, 50, 50, "BW_CW", 40, "400", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},
  {"#bw_digital", NULL, 1000, -1000, 50, 50, "BW_DIGITAL", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},

	//FT8 controls
	{"#ft8_auto", NULL, 1000, -1000, 50, 50, "FT8_AUTO", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0, FT8_CONTROL},
	{"#ft8_tx1st", NULL, 1000, -1000, 50, 50, "FT8_TX1ST", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0, FT8_CONTROL},
  { "#ft8_repeat", NULL, 1000, -1000, 50, 50, "FT8_REPEAT", 40, "5", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 1, 10, 1, FT8_CONTROL},

	{"#telneturl", NULL, 1000, -1000, 400, 149, "TELNETURL", 70, "dxc.nc7j.com:7373", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1, 0},

	//soft keyboard
	{"#kbd_q", do_kbd, 0, 300 ,50, 50, "", 1, "Q", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_w", do_kbd, 50, 300, 50, 50, "", 1, "W", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_e", do_kbd, 100, 300, 50, 50, "", 1, "E", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_r", do_kbd, 150, 300, 50, 50, "", 1, "R", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_t", do_kbd, 200, 300, 50, 50, "", 1, "T", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_y", do_kbd, 250, 300, 50, 50, "", 1, "Y", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_u", do_kbd, 300, 300, 50, 50, "", 1, "U", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_i", do_kbd, 350, 300, 50, 50, "", 1, "I", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_o", do_kbd, 400, 300, 50, 50, "", 1, "O", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_p", do_kbd, 450, 300, 50, 50, "", 1, "P", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_@", do_kbd, 500, 300, 50, 50, "", 1, "@", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_1", do_kbd, 550, 300, 50, 50, "", 1, "1", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_2", do_kbd, 600, 300, 50, 50, "", 1, "2", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_3", do_kbd, 650, 300, 50, 50, "", 1, "3", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_bs", do_kbd, 700, 300, 100, 50, "", 1, "DEL", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0},

	{"#kbd_alt", do_kbd, 0, 350 ,50, 50, "", 1, "CMD", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_a", do_kbd, 50, 350 ,50, 50, "*", 1, "A", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_s", do_kbd, 100, 350, 50, 50, "", 1, "S", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_d", do_kbd, 150, 350, 50, 50, "", 1, "D", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_f", do_kbd, 200, 350, 50, 50, "", 1, "F", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_g", do_kbd, 250, 350, 50, 50, "", 1, "G", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_h", do_kbd, 300, 350, 50, 50, "", 1, "H", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_j", do_kbd, 350, 350, 50, 50, "", 1, "J", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_k", do_kbd, 400, 350, 50, 50, "'", 1, "K", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_l", do_kbd, 450, 350, 50, 50, "", 1, "L", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_/", do_kbd, 500, 350, 50, 50, "", 1, "/", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_4", do_kbd, 550, 350, 50, 50, "", 1, "4", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_5", do_kbd, 600, 350, 50, 50, "", 1, "5", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_6", do_kbd, 650, 350, 50, 50, "", 1, "6", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_enter", do_kbd, 700, 400, 100, 50, "", 1, "Enter", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
 
	{"#kbd_ ", do_kbd, 0, 400, 50, 50, "", 1, "SPACE", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_z", do_kbd, 50, 400, 50, 50, "", 1, "Z", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_x", do_kbd, 100, 400, 50, 50, "", 1, "X", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_c", do_kbd, 150,	400, 50, 50, "", 1, "C", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_v", do_kbd, 200, 400, 50, 50, "", 1, "V", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_b", do_kbd, 250, 400, 50, 50, "", 1, "B", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_n", do_kbd, 300, 400, 50, 50, "", 1, "N", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_m", do_kbd, 350, 400, 50, 50, "", 1, "M", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_,", do_kbd, 400, 400, 50, 50, "", 1, ",", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_.", do_kbd, 450, 400, 50, 50, "", 1, ".", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_?", do_kbd, 500, 400, 50, 50, "", 1, "?", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_7", do_kbd, 550, 400, 50, 50, "", 1, "7", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_8", do_kbd, 600, 400, 50, 50, "", 1, "8", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_9", do_kbd, 650, 400, 50, 50, "", 1, "9", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_0", do_kbd, 700, 350, 50, 50, "", 1, "0", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//macros keyboard

	//row 1
	{"#mf1", do_macro, 0, 1360, 65, 40, "F1", 1, "CQ", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf2", do_macro, 65, 1360, 65, 40, "F2", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf3", do_macro, 130, 1360, 65, 40, "F3", 1, "Reply", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf4", do_macro, 195, 1360, 65, 40, "F4", 1, "RRR", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf5", do_macro, 260, 1360, 70, 40, "F5", 1, "73", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf6", do_macro, 330, 1360, 70, 40, "F6", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//row 2

	{"#mf7", do_macro, 0, 1400, 65, 40, "F7", 1, "Exch", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf8", do_macro, 65, 1400, 65, 40, "F8", 1, "Tu", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf9", do_macro, 130, 1400, 65, 40, "F9", 1, "Rpt", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf10", do_macro, 195, 1400, 65, 40, "F10", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf11", do_macro, 260, 1400, 70, 40, "F11", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf12", do_macro, 330, 1400, 70, 40, "F12", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//row 3



	{"#mfedit", do_macro, 195, 1440, 65, 40, "Edit", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mfspot"	, do_macro, 260, 1440, 70, 40, "Spot", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mfkbd", do_macro, 330, 1440, 70, 40, "Kbd", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//the last control has empty cmd field 
	{"", NULL, 0, 0 ,0, 0, "#", 1, "Q", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0,0},
};


struct field *get_field(const char *cmd);
void update_field(struct field *f);
void tx_on();
void tx_off();

//#define MAX_CONSOLE_LINES 1000
//char *console_lines[MAX_CONSOLE_LINES];
int last_log = 0;

struct field *get_field(const char *cmd){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcmp(active_layout[i].cmd, cmd))
			return active_layout + i;
	return NULL;
}

//set the field directly to a particuarl value, programmatically
int set_field(char *id, char *value){
	struct field *f = get_field(id);
	int v;
	int debug = 0;

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
		if (debug)
			printf("field selection [%s]\n");
		strcpy(b, f->selection);
		p = strtok(b, "/");
		if (debug)
			printf("first token [%s]\n", p);
		while(p){
			if (!strcmp(value, p))
				break;
			else
				prev = p;
			p = strtok(NULL, "/");
			if (debug)
				printf("second token [%s]\n", p);
		}	
		//set to the first option
		if (p == NULL){
			if (prev)
				strcpy(f->value, prev);
			printf("*Error: setting field[%s] to [%s] not permitted\n", f->cmd, value);
			return 1;
		}
		else{
			if (debug)
				printf("Setting field to %s\n", value);
			strcpy(f->value, value);
		}
	}
	else if (f->value_type == FIELD_BUTTON){
		strcpy(f->value, value);
		do_control_action(f->label);
	}
	else if (f->value_type == FIELD_TEXT){
		if (strlen(value) > f->max || strlen(value) < f->min){
			printf("*Error: field[%s] can't be set to [%s], improper size.\n", f->cmd, value);
			return 1;
		}
		else
			strcpy(f->value, value);
	}

	if (!strcmp(id, "#rit") || !strcmp(id, "#ft8_auto"))
		debug = 1; 

	//send a command to the radio 
	char buff[200];
	sprintf(buff, "%s %s", f->label, f->value);
	do_control_action(buff);

	update_field(f);
	return 0;
}

struct field *get_field_by_label(char *label){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcasecmp(active_layout[i].label, label))
			return active_layout + i;
	return NULL;
}

const char *field_str(char *label){
	struct field *f = get_field_by_label(label);
	if (f)
		return f->value;
	else
		return NULL; 
}

int field_int(char *label){
	struct field *f = get_field_by_label(label);
	if (f){
		return atoi(f->value);
	}
	else {
		printf("field_int: %s not found\n", label);
		return -1;
	}
}

int field_set(char *label, char *new_value){
	struct field *f = get_field_by_label(label);
	if (!f)
		return -1;
	int r = set_field(f->cmd, new_value); 
	update_field(f);
}

int get_field_value(char *cmd, char *value){
	struct field *f = get_field(cmd);
	if (!f)
		return -1;
	strcpy(value, f->value);
	return 0;
}

int get_field_value_by_label(char *label, char *value){
	struct field *f = get_field_by_label(label);
	if (!f)
		return -1;
	strcpy(value, f->value);
	return 0;
}

int remote_update_field(int i, char *text){
	struct field * f = active_layout + i;

	if (f->cmd[0] == 0)
		return -1;
	
	//always send status afresh
	if (!strcmp(f->label, "STATUS")){
		//send time
		time_t now = time_sbitx();
		struct tm *tmp = gmtime(&now);
		sprintf(text, "STATUS %04d/%02d/%02d %02d:%02d:%02dZ",  
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
		return 1;
	}

	strcpy(text, f->label);
	strcat(text, " ");
	strcat(text, f->value);
	int update = f->update_remote;
	f->update_remote = 0;

	//debug on
//	if (!strcmp(f->cmd, "#text_in") && strlen(f->value))
//		printf("#text_in [%s] %d\n", f->value, update);
	//debug off
	return update;
}


// log is a special field that essentially is a like text
// on a terminal

void console_init(){
	for (int i =0;  i < MAX_CONSOLE_LINES; i++){
		console_stream[i].text[0] = 0;
		console_stream[i].style = console_style;
	}
	struct field *f = get_field("#console");
	console_current_line = 0;
	f->is_dirty = 1;
}

void web_add_string(char *string){
	while (*string){
		q_write(&q_web, *string++);
	}
}

void  web_write(int style, char *data){
	char tag[20];

	switch(style){
		case FONT_FT8_REPLY:
		case FONT_FT8_RX:
			strcpy(tag, "WSJTX-RX");
			break;
		case FONT_FLDIGI_RX:
			strcpy(tag, "FLDIGI-RX");
			break;
		case FONT_CW_RX:
			strcpy(tag, "CW-RX");
			break;
		case FONT_FT8_TX:
			strcpy(tag, "WSJTX-TX");
			break;
		case FONT_FT8_QUEUED:
			strcpy(tag, "WSJTX-Q");
			break;
		case FONT_FLDIGI_TX:
			strcpy(tag, "FLDIGI-TX");
			break;
		case FONT_CW_TX:
			strcpy(tag, "CW-TX");
			break;
		case FONT_TELNET:
			strcpy(tag, "TELNET");
			break;
		default:
			strcpy(tag, "LOG");
	}

	web_add_string("<");
	web_add_string(tag);		
	web_add_string(">");
	while (*data){
				switch(*data){
				case '<':
					q_write(&q_web, '&');
					q_write(&q_web, 'l');
					q_write(&q_web, 't');
					q_write(&q_web, ';');
					break;
				case '>':
					q_write(&q_web, '&');
					q_write(&q_web, 'g');
					q_write(&q_web, 't');
					q_write(&q_web, ';');
					break;
			 	case '"':
					q_write(&q_web, '&');
					q_write(&q_web, 'q');
					q_write(&q_web, 'u');
					q_write(&q_web, 't');
					q_write(&q_web, 'e');
					q_write(&q_web, ';');
					break;
				case '\'':
					q_write(&q_web, '&');
					q_write(&q_web, 'a');
					q_write(&q_web, 'p');
					q_write(&q_web, 'o');
					q_write(&q_web, 's');
					q_write(&q_web, ';');
					break;
				case '\n':
					q_write(&q_web, '&');
					q_write(&q_web, '#');
					q_write(&q_web, 'x');
					q_write(&q_web, 'A');
					q_write(&q_web, ';');
					break;	
				default:
					q_write(&q_web, *data);
			}
			data++;
	}			
	web_add_string("</");
	web_add_string(tag);
	web_add_string(">");
}

int console_init_next_line(){
	console_current_line++;
	if (console_current_line == MAX_CONSOLE_LINES)
		console_current_line = 0;
	console_stream[console_current_line].text[0] = 0;	
	console_stream[console_current_line].style = console_style;
	return console_current_line;
}

void write_to_remote_app(int style, char *text){
	remote_write("{");
	remote_write(text);
	remote_write("}");
}

void write_console(int style, char *text){
	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/display_log.txt");

	web_write(style, text);
	//move to a new line if the style has changed
	if (style != console_style){
		q_write(&q_web, '{');
		q_write(&q_web, style + 'A');
		console_style = style;
		if (strlen(console_stream[console_current_line].text)> 0)
			console_init_next_line();	
		console_stream[console_current_line].style = style;
		switch(style){
			case FONT_FT8_RX:
			case FONT_FLDIGI_RX:
			case FONT_CW_RX:
				break;
			case FONT_FT8_TX:
			case FONT_FLDIGI_TX:
			case FONT_CW_TX:
			case FONT_FT8_REPLY:
				break;
			default:
				break;
		}
	}

	if (strlen(text) == 0)
		return;

/*
	//write to the scroll
	FILE *pf = fopen(directory, "a");
	if (pf){
		fwrite(text, strlen(text), 1, pf);
		fclose(pf);
		pf = NULL;
	}
*/	
	write_to_remote_app(style, text);

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

	struct field *f = get_field("#console");
	if (f)
		f->is_dirty = 1;
}

void draw_console(cairo_t *gfx, struct field *f){
	char this_line[1000];
	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;

	rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	//estimate!
	int char_width = measure_text(gfx, "01234567890123456789", f->font_index)/20;
	console_cols = f->width / char_width;
	int y = f->y; 
	int j = 0;

	int start_line = console_current_line - n_lines;
	if (start_line < 0)
		start_line += MAX_CONSOLE_LINES;

 	for (int i = 0; i <= n_lines; i++){
		struct console_line *l = console_stream + start_line;
		if (start_line == console_selected_line)
			fill_rect(gfx, f->x, y+1, f->width, font_table[l->style].height+1, SELECTED_LINE);
		draw_text(gfx, f->x+1, y, l->text, l->style);
		start_line++;
		y += line_height;
		if(start_line >= MAX_CONSOLE_LINES)
			start_line = 0;
	}
}

int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100], *p, *q;

	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;
	int	l = 0;
	int start_line;

	switch(event){
		case FIELD_DRAW:
			draw_console(gfx, f);
			return 1;
		break;
		case GDK_BUTTON_PRESS:
		case GDK_MOTION_NOTIFY:
			start_line = console_current_line - n_lines;
			l = start_line + ((b - f->y)/line_height);
			if (l < 0)
				l += MAX_CONSOLE_LINES;
			console_selected_line = l;	
			f->is_dirty = 1;
			return 1;
		break;
		case GDK_BUTTON_RELEASE:
			if (!strcmp(get_field("r1:mode")->value, "FT8")){
				char ft8_message[100], ft8_response[100];
				strcpy(ft8_message, console_stream[console_selected_line].text);
				ft8_process(ft8_message, FT8_START_QSO);
			}
			f->is_dirty = 1;
			return 1;
		break;
	}
	return 0;	
}

void draw_field(GtkWidget *widget, cairo_t *gfx, struct field *f){
	struct font_style *s = font_table + 0;

	//push this to the web as well
	
	f->is_dirty = 0;
	if (f->x <= -1000)
		return;

	//if there is a handling function, use that else
	//skip down to the default behaviour of the controls
	if (f->fn){
		if(f->fn(f, gfx, FIELD_DRAW, -1, -1, 0)){
			f->is_dirty = 0;
			return;
		}
	}

	if (f_focus == f)
		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_FIELD_SELECTED);
	else 
		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width-1,f->height, SELECTED_LINE, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
	else if (f->value_type != FIELD_STATIC)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	int width, offset_x, text_length, line_start, y, label_height, 
		value_height, value_font, label_font;	
	char this_line[MAX_FIELD_LENGTH];
	int text_line_width = 0;

	int label_y;
	int use_reduced_font = 0;
	char *label = f->label;

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
		case FIELD_BUTTON:
			label_height = font_table[FONT_FIELD_LABEL].height;
			width = measure_text(gfx, label, FONT_FIELD_LABEL);
			//skip the underscore in the label if it is too wide
			if (width > f->width && strchr(label, '_')){
				label = strchr(label, '_') + 1;
				width = measure_text(gfx, label, FONT_FIELD_LABEL);
			}

			offset_x = f->x + f->width/2 - width/2;
			//is it a two line display or a single line?
			if (f->value_type == FIELD_BUTTON && !f->value[0]){
				label_y = f->y + (f->height - label_height)/2;
				draw_text(gfx, offset_x,label_y, f->label, FONT_FIELD_LABEL);
			} 
			else {
				value_height = font_table[FONT_FIELD_VALUE].height;
				label_y = f->y + ((f->height  - label_height  - value_height)/2);
				draw_text(gfx, offset_x, label_y, label, FONT_FIELD_LABEL);
				width = measure_text(gfx, f->value, FONT_FIELD_VALUE);
				label_y += font_table[FONT_FIELD_LABEL].height;
				draw_text(gfx, f->x + f->width/2 - width/2, label_y, f->value, 
					FONT_FIELD_VALUE);
			}
      break;
		case FIELD_STATIC:
			draw_text(gfx, f->x, f->y, f->label, FONT_FIELD_LABEL);
			break;
		case FIELD_CONSOLE:
			//draw_console(gfx, f);
			break;
	}
}

static int mode_id(const char *mode_str){
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
	static int last_save_at = 0;
	char file_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	//attempt to save settings only if it has been 30 seconds since the 
	//last time the settings were saved
	int now = millis();
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

  // save the field values
	int i;
	for (i= 0; active_layout[i].cmd[0] > 0; i++){
		fprintf(f, "%s=%s\n", active_layout[i].cmd, active_layout[i].value);
	}

	//now save the band stack
	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		fprintf(f, "\n[%s]\n", band_stack[i].name);
		//fprintf(f, "power=%d\n", band_stack[i].power);
		for (int j = 0; j < STACK_DEPTH; j++)
			fprintf(f, "freq%d=%d\nmode%d=%d\n", j, band_stack[i].freq[j], j, band_stack[i].mode[j]);
	}


	fclose(f);
	settings_updated = 0;
}


void enter_qso(){
	const char *callsign = field_str("CALL");
	const char *rst_sent = field_str("SENT");
	const char *rst_received = field_str("RECV");

	// skip empty or half filled log entry
	if (strlen(callsign) < 3 || strlen(rst_sent) < 2 || strlen(rst_received) < 2){
		printf("log entry is empty [%s], [%s], [%s], no log created\n", callsign, rst_sent, rst_received);
		return;
	}
 
	if (logbook_count_dup(field_str("CALL"), 120)){
		printf("duplicate log entry aborted within 60 seconds\n");
		return;
	}	
	logbook_add(get_field("#contact_callsign")->value, 
		get_field("#rst_sent")->value, 
		get_field("#exchange_sent")->value, 
		get_field("#rst_received")->value, 
		get_field("#exchange_received")->value);
	char buff[100];
	sprintf(buff, "Logged: %s %s-%s %s-%s\n", 
		field_str("CALL"), field_str("SENT"), field_str("NR"), 
		field_str("RECV"), field_str("EXCH"));
	write_console(FONT_LOG, buff);
}

static int user_settings_handler(void* user, const char* section, 
            const char* name, const char* value)
{
    char cmd[1000];
    char new_value[200];

    strcpy(new_value, value);
    if (!strcmp(section, "r1")){
      sprintf(cmd, "%s:%s", section, name);
      set_field(cmd, new_value);
    }
    else if (!strcmp(section, "tx")){
      strcpy(cmd, name);
      set_field(cmd, new_value);
    }
		else if (!strncmp(cmd, "#kbd", 4)){
			return 1; //skip the keyboard values
		}
    // if it is an empty section
    else if (strlen(section) == 0){
      sprintf(cmd, "%s", name);
			//skip the button actions 
			struct field *f = get_field(cmd);
			if (f){
				if (f->value_type != FIELD_BUTTON)
      		set_field(cmd, new_value); 
			}
    }

		//band stacks
		int band = -1;
		if (!strcmp(section, "80M"))
			band = 0;
		else if (!strcmp(section, "40M"))
			band = 1;
		else if (!strcmp(section, "30M"))
			band = 2;
		else if (!strcmp(section, "20M"))
			band = 3;
		else if (!strcmp(section, "17M"))
			band = 4;
		else if (!strcmp(section, "15M"))
			band = 5;
		else if (!strcmp(section, "12M"))	
			band = 6;
		else if (!strcmp(section, "10M"))
			band = 7;	

		if (band >= 0  && !strcmp(name, "freq0"))
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
		if (i % 48 == 0 && i > 0){
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
static int  *wf = NULL;
GdkPixbuf *waterfall_pixbuf = NULL;
guint8 *waterfall_map = NULL;

void init_waterfall(){
	struct field *f = get_field("waterfall");

	if (wf)
		free(wf);
	//this will store the db values of waterfall
	wf = malloc((MAX_BINS/2) * f->height * sizeof(int));
	if (!wf){
		puts("*Error: malloc failed on waterfall buffer");
		exit(0);
	}
	memset(wf, 0, (MAX_BINS/2) * f->height * sizeof(int));

	if (waterfall_map)
		free(waterfall_map);
	//this will store the bitmap pixles, 3 bytes per pixel
	waterfall_map = malloc(f->width * f->height * 3);
	for (int i = 0; i < f->width; i++)
		for (int j = 0; j < f->height; j++){
			int row = j * f->width * 3;
			int	index = row + i * 3;
			waterfall_map[index++] = 0;
			waterfall_map[index++] = 0;//i % 256;
			waterfall_map[index++] =0;// j % 256; 
	}
	if (waterfall_pixbuf)
		g_object_unref(waterfall_pixbuf);
	waterfall_pixbuf = gdk_pixbuf_new_from_data(waterfall_map,
		GDK_COLORSPACE_RGB, FALSE, 8, f->width, f->height, f->width*3, NULL,NULL);
		// format,         alpha?, bit,  widht,    height, rowstride, destryfn, data

//	printf("%ld return from pixbuff", (int)waterfall_pixbuf);	
}

void draw_tx_meters(struct field *f, cairo_t *gfx){
	char meter_str[100];
	int vswr = field_int("REF");
	int power = field_int("POWER");

	//power is in 1/10th of watts and vswr is also 1/10th
	if (power < 30)
		vswr = 10;
	
	sprintf(meter_str, "Power: %d Watts", field_int("POWER")/10);
	draw_text(gfx, f->x + 20 , f->y + 5 , meter_str, FONT_FIELD_LABEL);
	sprintf(meter_str, "VSWR: %d.%d", vswr/10,vswr%10);
	draw_text(gfx, f->x + 200 , f->y + 5 , meter_str, FONT_FIELD_LABEL);
}

void draw_waterfall(struct field *f, cairo_t *gfx){

	if (in_tx){
		draw_tx_meters(f, gfx);
		return;
	}
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
	grid_height = f->height - (font_table[FONT_SMALL].height * 4 / 3); 

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
	int y, sub_division, i, grid_height, bw_high, bw_low, pitch, tx_pitch;
	float span;
	struct field *f;
	long	freq, freq_div;
	char	freq_text[20];

	if (in_tx){
		draw_modulation(f_spectrum, gfx);
		return;
	}

	pitch = field_int("PITCH");
	tx_pitch = field_int("TX_PITCH");
	struct field *mode_f = get_field("r1:mode");
	freq = atol(get_field("r1:freq")->value);

	span = atof(get_field("#span")->value);
	bw_high = atoi(get_field("r1:high")->value);
	bw_low = atoi(get_field("r1:low")->value);
	grid_height = f_spectrum->height - ((font_table[FONT_SMALL].height * 4) /3);
	sub_division = f_spectrum->width / 10;

	// the step is in khz, we multiply by 1000 and div 10(divisions) = 100 
	freq_div = span * 100;  

	//calculate the position of bandwidth strip
	int filter_start, filter_width;

	if(!strcmp(mode_f->value, "CWR") || !strcmp(mode_f->value, "LSB")){
	 	filter_start = f_spectrum->x + (f_spectrum->width/2) - 
			((f_spectrum->width * bw_high)/(span * 1000)); 
		if (filter_start < f_spectrum->x){
	 	  filter_width = ((f_spectrum->width * (bw_high -bw_low))/(span * 1000)) - (f_spectrum->x - filter_start); 
			filter_start = f_spectrum->x;
    } else {
	 	  filter_width = (f_spectrum->width * (bw_high -bw_low))/(span * 1000); 
    }
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) -
			((f_spectrum->width * pitch)/(span * 1000));
	}
	else {
		filter_start = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * bw_low)/(span * 1000)); 
		if (filter_start < f_spectrum->x)
			filter_start = f_spectrum->x;
		filter_width = (f_spectrum->width * (bw_high-bw_low))/(span * 1000); 
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * pitch)/(span * 1000));
		tx_pitch = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * tx_pitch)/(span * 1000));
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
	int starting_bin = (3 *MAX_BINS)/4 - n_bins/2;
	int ending_bin = starting_bin + n_bins; 

	float x_step = (1.0 * f->width )/n_bins;

	//start the plot
	cairo_set_source_rgb(gfx, palette[SPECTRUM_PLOT][0], 
		palette[SPECTRUM_PLOT][1], palette[SPECTRUM_PLOT][2]);
	cairo_move_to(gfx, f->x + f->width, f->y + grid_height);

//	float x = fmod((1.0 * spectrum_span), 46.875);
	float x = 0;
	int j = 0;

	for (i = starting_bin; i <= ending_bin; i++){
		int y;

		// the center fft bin is at zero, from MAX_BINS/2 onwards,
		// the bins are at lowest frequency (-ve frequency)
		//y axis is the power  in db of each bin, scaled to 80 db
		y = ((spectrum_plot[i] + waterfall_offset) * f->height)/80; 
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
		if (f->width <= x)
			x = f->width - 1;
	}

	cairo_stroke(gfx);
 
  if (pitch >= f_spectrum->x){
    cairo_set_source_rgb(gfx, palette[COLOR_RX_PITCH][0],
			palette[COLOR_RX_PITCH][1], palette[COLOR_RX_PITCH][2]);
    if(!strcmp(mode_f->value, "USB") || !strcmp(mode_f->value, "LSB")){ // for LSB and USB draw pitch line at center
	    cairo_move_to(gfx, f->x + (f->width/2), f->y);
	    cairo_line_to(gfx, f->x + (f->width/2), f->y + grid_height); 
    } else {
	    cairo_move_to(gfx, pitch, f->y);
	    cairo_line_to(gfx, pitch, f->y + grid_height); 
    }
   	cairo_stroke(gfx);
  }

  if (tx_pitch >= f_spectrum->x && !strcmp(mode_f->value, "FT8")){
    cairo_set_source_rgb(gfx, palette[COLOR_TX_PITCH][0],
			palette[COLOR_TX_PITCH][1], palette[COLOR_TX_PITCH][2]);
	  cairo_move_to(gfx, tx_pitch, f->y);
	  cairo_line_to(gfx, tx_pitch, f->y + grid_height); 
   	cairo_stroke(gfx);
  }
	//draw the needle
	for (struct rx *r = rx_list; r; r = r->next){
		int needle_x  = (f->width*(MAX_BINS/2 - r->tuned_bin))/(MAX_BINS/2);
		fill_rect(gfx, f->x + needle_x, f->y, 1, grid_height,  SPECTRUM_NEEDLE);
	}
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
	struct field *vfo_a = get_field("#vfo_a_freq");
	struct field *vfo_b = get_field("#vfo_b_freq");
	struct field *rit_delta = get_field("#rit_delta");
	char buff[20];

  char temp_str[20];

	fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);

	//update the vfos
	if (vfo->value[0] == 'A')
			strcpy(vfo_a->value, f->value);
	else
			strcpy(vfo_b->value, f->value);

  if (!strcmp(rit->value, "ON")){
    if (!in_tx){
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(temp_str, "%d", (atoi(f->value) + atoi(rit_delta->value)));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
    else {
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
      sprintf(temp_str, "%d", (atoi(f->value) + atoi(rit_delta->value)));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(split->value, "ON")){
    if (!in_tx){
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "RX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
    else {
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
      sprintf(buff, "RX:%d", atoi(f->value) + atoi(rit_delta->value));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(vfo->value, "A")){
    if (!in_tx){
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "A:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    } else {
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
  }
  else{ /// VFO B is active
    if (!in_tx){
			strcpy(temp_str, vfo_a->value);
      //sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "B:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }else {
			strcpy(temp_str, vfo_a->value);
      //sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
  }
}

void invalidate_rect(int x, int y, int width, int height){
	if (display_area){
		gtk_widget_queue_draw_area(display_area, x, y, width, height);
	}
}

//the keyboard appears at the bottom 150 pixels of the window
void keyboard_display(int show){
	struct field *f;

	//we start the height at -200 because the first key
	//will bump it down by a row
	int height = screen_height - 200; 
	for (f = active_layout; f->cmd[0]; f++){
		if (!strncmp(f->cmd,"#kbd", 4)){
			//start of a new line? move down
			if (f->x == 0)
				height += 50;
			update_field(f);
			if (show && f->y < 0)
				f->y = height;
			else if (!show)
				f->y = -1000;
			update_field(f);
		}
	}
}

void field_move(char *field_label, int x, int y, int width, int height){
	struct field *f = get_field_by_label(field_label);
	if (!f)
		return;
	f->x = x;
	f->y = y;

	f->width = width;
	f->height = height;
	update_field(f);
	if (!strcmp(field_label, "WATERFALL"))
		init_waterfall();
}

//scales the ui as per current screen width from
//the nominal 800x480 size of the original layout
static void layout_ui(){
	int x1, y1, x2, y2;
	struct field *f;
	
	x1 =0 ;
	x2 = screen_width;
	y1 = 100;
	y2 = screen_height;

	//first move all the controls that are not common out of sight
	for (f = active_layout; f->cmd[0]; f++)
		if (!(f->section & COMMON_CONTROL)){
			update_field(f);
			f->y = -1000;
			update_field(f);
		}

	//locate the kbd to the right corner
	field_move("KBD", screen_width - 47, screen_height-47, 45, 45);

	//now, move the main radio controls to the right
	field_move("FREQ", x2-230, 0, 180, 40);
	field_move("AUDIO", x2-45, 5, 40, 40);
	field_move("IF", x2-45, 50, 40, 40);
	field_move("DRIVE", x2-85, 50, 40, 40);
	field_move("BW", x2-125, 50, 40, 40);
	field_move("AGC", x2-165, 50, 40, 40);

	field_move("STEP", x2-270, 5, 40, 40);
	field_move("RIT", x2-310, 5, 40, 40);
	field_move("SPLIT", x2-310, 50, 40, 40);
	field_move("VFO", x2-270, 50, 40, 40);
	field_move("SPAN", x2-225, 50, 40, 40);
	

	if (!strcmp(field_str("KBD"), "ON")){
		//take out 3 button widths from the bottom
		y2 = screen_height - 150;
		keyboard_display(1);
	}
	else
		keyboard_display(0);
	
	int m_id = mode_id(field_str("MODE"));
	int button_width = 100;
	switch(m_id){
		case MODE_FT8:
			field_move("CONSOLE", 5, y1, 350, y2-y1-55);
			field_move("SPECTRUM", 360, y1, x2-365, 100);
			field_move("WATERFALL", 360, y1+100, x2-365, y2-y1-155);
			field_move("ESC", 5, y2-50, 40, 45);
			field_move("F1", 50, y2-50, 50, 45);
			field_move("F2", 100, y2-50, 50, 45);
			field_move("F3", 150, y2-50, 50, 45);
			field_move("F4", 200, y2-50, 50, 45);
			field_move("F5", 250, y2-50, 50, 45);
			field_move("F6", 300, y2-50, 50, 45);
			field_move("F7", 350, y2-50, 50, 45);
			field_move("F8", 400, y2-50, 45, 45);
			field_move("FT8_REPEAT", 450, y2-50, 50, 45);
			field_move("FT8_TX1ST", 500, y2-50, 50, 45);
			field_move("FT8_AUTO", 550, y2-50, 50, 45);
			field_move("TX_PITCH", 600, y2-50, 73, 45);
			field_move("PITCH", 675, y2-50, 73, 45);
		break;
		case MODE_CW:
		case MODE_CWR:
			field_move("CONSOLE", 5, y1, 350, y2-y1-105);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+70, x2-365, y2-y1-175);
			// first line below the decoder/waterfall
			y1 = y2 - 97;
			field_move("ESC", 5, y1, 70, 45);
			field_move("WPM",75, y1, 75, 45);
			field_move("PITCH", 150, y1, 75, 45);
			field_move("CW_DELAY", 225, y1, 75, 45);
			field_move("CW_INPUT", 300, y1, 75 , 45);
			field_move("SIDETONE", 375, y1, 75, 45);

			y1 += 50;
			field_move("F1", 5, y1, 70, 45);
			field_move("F2", 75, y1, 75, 45);
			field_move("F3", 150, y1, 75, 45);
			field_move("F4", 225, y1, 75, 45);
			field_move("F5", 300, y1, 75, 45);
			field_move("F6", 375, y1, 75, 45);
			field_move("F7", 450, y1, 75, 45);
			field_move("F8", 525, y1, 75, 45);
			field_move("F9", 600, y1, 75, 45);
			field_move("F10", 675, y1, 70, 45);
			break;
		case MODE_USB:
		case MODE_LSB:
		case MODE_AM:
		case MODE_NBFM:
			field_move("CONSOLE", 5, y1, 350, y2-y1-55);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+70, x2-365, y2-y1-125);
			y1 = y2 -50;
			field_move("MIC", 5, y1, 45, 45);
			field_move("TX", 260, y1, 95, 45);
			field_move("RX", 360, y1, 95, 45);
		break;
		default:
			field_move("CONSOLE", 5, y1, 350, y2-y1-110);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+70, x2-365, y2-y1-180);
			y1 = y2 - 105;
			field_move("F1", 5, y1, 90, 45);
			field_move("F2", 100, y1, 95, 45);
			field_move("F3", 200, y1, 100, 45);
			field_move("F4", 300, y1, 100, 45);
			field_move("F5", 400, y1, 100, 45);
			field_move("F6", 500, y1, 100, 45);
			field_move("F7", 600, y1, 100, 45);
			field_move("F8", 700, y1, 95, 45);
			y1 += 50;
			field_move("F9", 5, y1, 95, 45);
			field_move("F10", 100, y1, 100, 45);
			field_move("F11",200, y1, 100, 45);
			field_move("F12",300, y1, 95, 45);
			field_move("PITCH", 550, y1, 50, 45);
			field_move("SIDETONE", 600, y1, 95, 45);
		break;	
	}
	invalidate_rect(0,0,screen_width, screen_height);
}
void dump_ui(){
	FILE *pf = fopen("main_ui.ini", "w");
	for (int i = 0; active_layout[i].cmd[0] > 0; i++){
		struct field *f = active_layout + i;
		fprintf(pf, "\n[%s]\n", f->cmd);
		fprintf(pf, "label: %s\n", f->label);
		fprintf(pf, "x:%d\n", f->x);
		fprintf(pf, "y:%d\n", f->y);
		fprintf(pf, "width:%d\n", f->width);
		fprintf(pf, "height:%d\n",f->height);
		fprintf(pf, "value:%s\n", f->value);
		fprintf(pf, "type:%d\n", f->value_type);
		fprintf(pf, "font:%d\n", f->font_index);
		fprintf(pf, "selection:%s\n", f->selection);
		fprintf(pf, "min:%d\n", f->min);
		fprintf(pf, "max:%d\n", f->max);
		fprintf(pf, "step:%d\n", f->step);
	}
	fclose(pf);
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
		if (cairo_in_clip(gfx, cx1, cy1) || cairo_in_clip(gfx, cx2, cy2))
			draw_field(widget, gfx, active_layout + i);
		//else if (f->label[0] == 'F')
		//	printf("skipping %s\n", active_layout[i].label);
	}
}

/* gtk specific routines */
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, gpointer user_data ) {
	redraw_main_screen(widget, cr);	
  return FALSE;
}

static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
	screen_width = event->width;
	screen_height = event->height;
//	gtk_container_resize_children(GTK_CONTAINER(window));
//	gtk_widget_set_default_size(display_area, screen_width, screen_height);
	layout_ui();	
	return FALSE;
}

void update_field(struct field *f){
	if (f->y >= 0)
		f->is_dirty = 1;
	f->update_remote = 1;
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
		f->is_dirty = 1;
	 	f->update_remote = 1;
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

	//send a command to the radio
	char buff[200];

//	sprintf(buff, "%s=%s", f->cmd, f->value);

	sprintf(buff, "%s %s", f->label, f->value);
	do_control_action(buff);
	f->is_dirty = 1;
	f->update_remote = 1;
//	update_field(f);
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

	if (f_focus->value_type == FIELD_TEXT)
		f_last_text = f_focus;
  //is it a selection field?
  if (f_focus->value_type == FIELD_SELECTION) 
    edit_field(f_focus, MIN_KEY_UP);

	//if the button has been pressed, do the needful
	if (f_focus->value_type == FIELD_TOGGLE || 
			f_focus->value_type == FIELD_BUTTON)
				do_control_action(f->label);
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
	struct field *vfo_a = get_field("#vfo_a_freq");
	struct field *vfo_b = get_field("#vfo_b_freq");
	struct field *rit_delta = get_field("#rit_delta");

	char freq_request[30];
 
	if (!strcmp(rit->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%d", dial_freq + atoi(rit_delta->value)); 		
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq); 		
	}
	else if (!strcmp(split->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%s", vfo_b->value);
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq);
	}
	else
			sprintf(freq_request, "r1:freq=%d", dial_freq);

	//get back to setting the frequency
	sdr_request(freq_request, response);
}

void abort_tx(){
	set_field("#text_in", "");
	modem_abort();
	tx_off();
}

int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	struct field *f_freq, *f_span, *f_pitch;
	int span, pitch;
  long freq;
	char buff[100];
  int mode = mode_id(get_field("r1:mode")->value);

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
		  //a has the x position of the mouse
		  freq -= ((a - last_mouse_x) * (span/f->width));
		  sprintf(buff, "%ld", freq);
		  set_field("r1:freq", buff);
		  return 1;
		break;
    case GDK_BUTTON_PRESS: 
      if (c == GDK_BUTTON_SECONDARY){ // right click QSY
        f_freq = get_field("r1:freq");
        freq = atoi(f_freq->value);
        f_span = get_field("#span");
        span = atof(f_span->value) * 1000;
        f_pitch = get_field("rx_pitch");
        pitch = atoi(f_pitch->value);
        if (mode == MODE_CW){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) - pitch;
        } else if (mode == MODE_CWR){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) + pitch;
        } else { // other modes may need to be optimized - k3ng 2022-09-02
          freq += (((float)(a - f->x) / (float)f->width) - 0.5) * (float)span;
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

void remote_execute(char *cmd){

	if (q_remote_commands.overflow)
		q_empty(&q_remote_commands);
	while (*cmd)
		q_write(&q_remote_commands, *cmd++);
	q_write(&q_remote_commands, 0);
}


void call_wipe(){
	field_set("CALL", "");
	field_set("SENT", "");
	field_set("RECV", "");
	field_set("EXCH", "");
	field_set("NR", "");
}

void update_titlebar(){
	char buff[100];

	time_t now = time_sbitx();
	struct tm *tmp = gmtime(&now);
	sprintf(buff, "sBitx %s %s %04d/%02d/%02d %02d:%02d:%02dZ",  
		get_field("#mycallsign")->value, get_field("#mygrid")->value,
		tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
 	gtk_window_set_title( GTK_WINDOW(window), buff);

}

// calcualtes the LOW and HIGH settings from bw
// and sets them up, called from UI
void save_bandwidth(int hz){
	char bw[10];

 	int mode = mode_id(get_field("r1:mode")->value);
	sprintf(bw, "%d", hz);
	switch(mode){
		case MODE_CW:
		case MODE_CWR:
			field_set("BW_CW",bw); 
			break;
		case MODE_USB:
		case MODE_LSB:
		case MODE_NBFM:
		case MODE_AM:
			field_set("BW_VOICE",bw); 
			break;
		default:
			field_set("BW_DIGITAL",bw); 
	}
}

void set_filter_high_low(int hz){
	char buff[10], bw_str[10];
	int low, high;

	if (hz < 50)
		return;

	struct field *f_mode = get_field("r1:mode");
	struct field *f_pitch = get_field("rx_pitch");

	switch(mode_id(f_mode->value)){
		case MODE_CW:
		case MODE_CWR:
			low = atoi(f_pitch->value) - hz/2;
			high = atoi(f_pitch->value) + hz/2;
			break;
		case MODE_LSB:
		case MODE_USB:
			low = 300;
			high = low + hz;
			break;
		case MODE_DIGITAL:
			low = atoi(f_pitch->value) - (hz/2);
			high = atoi(f_pitch->value) + (hz/2);
			break;
		case MODE_FT8:
			low = 50;
			high = 4000;
			break;
		default:
			low = 50;
			high = 3000;
	}

	if (low < 50)
		low = 50;
	if (high > 5000)
		high = 5000;

	//now set the bandwidth
	sprintf(buff, "%d", low);
	set_field("r1:low", buff);
	sprintf(buff, "%d", high);
	set_field("r1:high", buff);
}
int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100];

	if (event == FIELD_DRAW){
		time_t now = time_sbitx();
		struct tm *tmp = gmtime(&now);
		sprintf(buff, "%04d/%02d/%02d %02d:%02d:%02dZ",  
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
		int width = measure_text(gfx, buff, FONT_FIELD_LABEL);
		int line_height = font_table[f->font_index].height; 	
		strcpy(f->value, buff);
		f->is_dirty = 1;
		f->update_remote = 1;
		sprintf(buff, "sBitx %s %s %04d/%02d/%02d %02d:%02d:%02dZ",  
			get_field("#mycallsign")->value, get_field("#mygrid")->value,
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
  	gtk_window_set_title( GTK_WINDOW(window), buff);

		return 1;
	}
	return 0;
}

void execute_app(char *app){
	char buff[1000];

	sprintf(buff, "%s 0> /dev/null", app); 
	int pid = fork();
	if (!pid){
		system(buff);
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
		}
		else if ((a =='\n' || a == MIN_KEY_ENTER) && !strcmp(get_field("r1:mode")->value, "FT8") 
			&& f->value[0] != COMMAND_ESCAPE){
			ft8_tx(f->value, field_int("TX_PITCH"));
			f->value[0] = 0;		
		}
		else if (a >= ' ' && a <= 127 && strlen(f->value) < f->max-1){
			int l = strlen(f->value);
			f->value[l++] = a;
			f->value[l] = 0;
		}
		//handle ascii delete 8 or gtk 
		else if ((a == MIN_KEY_BACKSPACE || a == 8) && strlen(f->value) > 0){
			int l = strlen(f->value) - 1;
			f->value[l] = 0;
		}
		f->is_dirty = 1;
		f->update_remote = 1;
		f_last_text = f; 
		return 1;
	}
	else if (event == FIELD_DRAW){
		if (f_focus == f)
			fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_FIELD_SELECTED);
		else
			fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);

		rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_CONTROL_BOX, 1);
		text_length = strlen(f->value);
		line_start = 0;
		y = f->y + 1;
		text_line_width= measure_text(gfx, f->value, f->font_index);
		if (!strlen(f->value))
			draw_text(gfx, f->x + 1, y+1, f->label, FONT_FIELD_LABEL);
		else 
			draw_text(gfx, f->x + 1, y+1, f->value, f->font_index);
		//draw the text cursor, if there is no text, the text baseline is zero
		if (f_focus == f){
			fill_rect(gfx, f->x + text_line_width+3, y+16, 9, 2, COLOR_SELECTED_BOX);
		}
		
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
		modem_set_pitch(v);
		char buff[20], response[20];
		sprintf(buff, "rx_pitch=%d", v);
		sdr_request(buff, response);

		//move the bandwidth accordingly
  	int mode = mode_id(get_field("r1:mode")->value);
		int bw = 4000;
		switch(mode){
			case MODE_CW:
			case MODE_CWR:
				bw = field_int("BW_CW");
				break;
			case MODE_USB:
			case MODE_LSB:
				bw = field_int("BW_VOICE");
				break;
			case MODE_FT8:
				bw = 4000;
				break;	
			default:
				bw = field_int("BW_DIGITAL");
		}
		set_filter_high_low(bw);
		return 1;
	}
		
	return 0;
}

int do_bandwidth(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

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
		modem_set_pitch(v);
		char buff[20], response[20];
		sprintf(buff, "rx_pitch=%d", v);
		sdr_request(buff, response);
		set_filter_high_low(v);
		save_bandwidth(v);
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
				struct field *f = get_field("#rit_delta");
				int rit_delta = atoi(f->value);
				if(rit_delta < MAX_RIT){
					rit_delta += tuning_step;
					char tempstr[100];
					sprintf(tempstr, "%d", rit_delta);
					set_field("#rit_delta", tempstr);
				}
				else
					return 1;
			}
			else
				v = (v / tuning_step + 1)*tuning_step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			if (!strcmp(get_field("#rit")->value, "ON")){
				struct field *f = get_field("#rit_delta");
				int rit_delta = atoi(f->value);
				if (rit_delta > -MAX_RIT){
					rit_delta -= tuning_step;
					char tempstr[100];
					sprintf(tempstr, "%d", rit_delta);
					set_field("#rit_delta", tempstr);
					printf("moved rit to %s\n", f->value);
				}
				else
					return 1;
			}
			else
				v = (v / tuning_step - 1)*tuning_step;
			abort_tx();
		}
		
		sprintf(f->value, "%d",  v);
		tuning_step = temp_tuning_step;
		//send the new frequency to the sbitx core
		char buff[100];
		//sprintf(buff, "%s=%s", f->cmd, f->value);
		sprintf(buff, "%s %s", f->label, f->value);
		do_control_action(buff);
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
		//the default focus is on text input
		struct field *f_text = get_field("#text_in");
		if (f_focus && f_focus->value_type == FIELD_TEXT)
			f_text = f_focus;
		
		if (!strcmp(f->cmd, "#kbd_bs"))
			edit_field(f_text, MIN_KEY_BACKSPACE);
		else if (!strcmp(f->value, "CMD"))
			edit_field(f_text, COMMAND_ESCAPE);
		else if (!strcmp(f->value, "SPACE"))
			edit_field(f_text, ' ');
		else if (!strcmp(f->cmd, "#kbd_enter"))
			edit_field(f_text, '\n');
		else
			edit_field(f_text, f->value[0]);
		focus_since = millis();
		return 1;
	}
	else if (event == FIELD_DRAW){
		int label_height = font_table[FONT_FIELD_LABEL].height;
		int width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		int offset_x = f->x + f->width/2 - width/2;
		int label_y;
		int value_font;

		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);
		//is it a two line display or a single line?
		if (!f->value[0]){
			label_y = f->y + (f->height - label_height)/2;
			draw_text(gfx, offset_x,label_y, f->label, FONT_FIELD_LABEL);
		} 
		else {
			if(width >= f->width+2)
				value_font = FONT_SMALL_FIELD_VALUE;
			else
				value_font = FONT_FIELD_VALUE;
			int value_height = font_table[value_font].height;
			label_y = f->y +3;
			draw_text(gfx, f->x + 3, label_y, f->label, FONT_FIELD_LABEL);
			width = measure_text(gfx, f->value, value_font);
			label_y = f->y + (f->height - label_height)/2;
			draw_text(gfx, f->x + f->width/2 - width/2, label_y, f->value, value_font);
		}
		return 1;
	}	
	return 0;
}


int do_toggle_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if(event == GDK_BUTTON_PRESS){
		focus_field(f_last_text);
		return 1;
	}
	return 0;
}


void open_url(char *url){
	char temp_line[200];

	sprintf(temp_line, "chromium-browser --log-leve=3 "
	"--enable-features=OverlayScrollbar %s"
	"  >/dev/null 2> /dev/null &", url);
	execute_app(temp_line);
}

void qrz(const char *callsign){
	char 	url[1000];

	sprintf(url, "https://qrz.com/DB/%s &", callsign);
	open_url(url);
}

int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[256], *mode;
	char contact_callsign[100];

	strcpy(contact_callsign, get_field("#contact_callsign")->value);

	if(event == GDK_BUTTON_PRESS){
		int fn_key = atoi(f->cmd+3); // skip past the '#mf' and read the function key number

/*		if (!strcmp(f->cmd, "#mfkbd")){
			set_ui(LAYOUT_KBD);
			return 1;
		}
		else if (!strcmp(f->cmd, "#mfqrz") && strlen(contact_callsign) > 0){
			qrz(contact_callsign);
			return 1;
		}
		else 
*/
	 	macro_exec(fn_key, buff);
	
		mode = get_field("r1:mode")->value;

		//add the end of transmission to the expanded buffer for the fldigi modes
		if (!strcmp(mode, "RTTY") || !strcmp(mode, "PSK31")){
			strcat(buff, "^r");
			tx_on(TX_SOFT);
		}

		if (!strcmp(mode, "FT8") && strlen(buff)){
			ft8_tx(buff, atoi(get_field("#tx_pitch")->value));
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
		int	label_y = f->y + ((f->height 
			- font_table[FONT_FIELD_LABEL].height - 5  
			- font_table[FONT_FIELD_VALUE].height)/2);
		draw_text(gfx, f->x + offset, label_y, f->label, FONT_FIELD_LABEL);


		char duration[12];
		label_y += font_table[FONT_FIELD_LABEL].height;

		if (record_start){
			width = measure_text(gfx, f->value, f->font_index);
			offset = f->width/2 - width/2;
			time_t duration_seconds = time(NULL) - record_start;
			int minutes = duration_seconds/60;
			int seconds = duration_seconds % 60;
			sprintf(duration, "%d:%02d", minutes, seconds); 	
		}
		else
			strcpy(duration, "OFF");
		width = measure_text(gfx, duration, FONT_FIELD_VALUE);
		draw_text(gfx, f->x + f->width/2 - width/2, label_y, duration, f->font_index);
		return 1;
	}
	return 0;
}

void tx_on(int trigger){
	char response[100];

	if (trigger != TX_SOFT && trigger != TX_PTT){
		puts("Error: tx_on trigger should be SOFT or PTT");
		return;
	}

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
		sdr_request("tx=on", response);	
		in_tx = trigger; //can be PTT or softswitch
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
		printf("TX\n");
	}

	tx_start_time = millis();
}

void tx_off(){
	char response[100];

	modem_abort();

	if (in_tx){
		sdr_request("tx=off", response);	
		in_tx = 0;
		sdr_request("key=up", response);
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
		printf("RX\n");
	}
	sound_input(0); //it is a low overhead call, might as well be sure
}


static int layout_handler(void* user, const char* section, 
            const char* name, const char* value)
{
	//the section is the field's name

	printf("setting %s:%s to %d\n", section, name, atoi(value));
	struct field *f = get_field(section);
	if (!strcmp(name, "x"))
		f->x = atoi(value);
	else if (!strcmp(name, "y"))
		f->y = atoi(value);
	else if (!strcmp(name, "width"))
		f->width = atoi(value);
	else if (!strcmp(name, "height"))
		f->height = atoi(value);	
}

void set_ui(int id){
	struct field *f = get_field("#kbd_q");

	if (id == LAYOUT_KBD){
		// the "#kbd" is out of screen, get it up and "#mf" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			active_layout[i].is_dirty = 1;	
		}
	}
	if (id == LAYOUT_MACROS) {
		// the "#mf" is out of screen, get it up and "#kbd" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
			active_layout[i].is_dirty = 1;	
		}
	}
	current_layout = id;
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
				tx_on(TX_SOFT);
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
				enter_qso();
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
		if(f_focus->fn)
			f_focus->fn(f_focus, NULL, GDK_BUTTON_RELEASE, 
				(int)(event->x), (int)(event->y),0); 	
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
				if (strncmp(f->cmd, "#kbd", 4))
					focus_field(f);
				else
					do_control_action(f->label);
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
	struct field *f;
	f = get_field("#console");
	f->is_dirty = 1;
	f = get_field("#text_in");
	f->is_dirty = 1;
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

uint8_t dec2bcd(uint8_t val){
	return ((val/10 * 16) + (val %10));
}

uint8_t bcd2dec(uint8_t val){
	return ((val/16 * 10) + (val %16));
}

void rtc_read(){
	uint8_t rtc_time[10];

	i2cbb_write_i2c_block_data(DS3231_I2C_ADD, 0, 0, NULL);

	int e =  i2cbb_read_i2c_block_data(DS3231_I2C_ADD, 0, 8, rtc_time);
	if (e <= 0){
		printf("RTC not detected\n");
		return;
	}
	for (int i = 0; i < 7; i++)
		rtc_time[i] = bcd2dec(rtc_time[i]);

	char buff[100];
	
	//convert to julian
	struct tm t;
	time_t gm_now;

	t.tm_year 	= rtc_time[6] + 2000 - 1900;
	t.tm_mon 	= rtc_time[5] - 1;
	t.tm_mday 	= rtc_time[4];
	t.tm_hour 	= rtc_time[2];
	t.tm_min		= rtc_time[1];
	t.tm_sec		= rtc_time[0];		

	time_t tjulian = mktime(&t);
	
	tzname[0] = tzname[1] = "GMT";
	timezone = 0;
	daylight = 0;
	setenv("TZ", "UTC", 1);	
	gm_now = mktime(&t);

	write_console(FONT_LOG, "RTC detected\n");
	time_delta =(long)gm_now -(long)(millis()/1000l);
}


void rtc_write(int year, int month, int day, int hours, int minutes, int seconds){
	uint8_t rtc_time[10];

	rtc_time[0] = dec2bcd(seconds);
	rtc_time[1] = dec2bcd(minutes);
	rtc_time[2] = dec2bcd(hours);
	rtc_time[3] = 0;
	rtc_time[4] = dec2bcd(day);
	rtc_time[5] = dec2bcd(month);
	rtc_time[6] = dec2bcd(year - 2000);

	for (uint8_t i = 0; i < 7; i++){
  	int e = i2cbb_write_byte_data(DS3231_I2C_ADD, i, rtc_time[i]);
		if (e)
			printf("rtc_write: error writing ds1307 register at %d index\n", i);
	}

/*	int e =  i2cbb_write_i2c_block_data(DS1307_I2C_ADD, 0, 7, rtc_time);
	if (e < 0){
		printf("RTC not written: %d\n", e);
		return;
	}
*/
}

//this will copy the computer time
//to the rtc
void rtc_sync(){
	time_t t = time(NULL);
	struct tm *t_utc = gmtime(&t);

	printf("Checking for valid NTP time ...");
	if (system("ntpstat") != 0){
		printf(".. not found.\n");
		return;
	}
	printf("Syncing RTC to %04d-%02d-%02d %02d:%02d:%02d\n", 
		t_utc->tm_year + 1900,  t_utc->tm_mon + 1, t_utc->tm_mday, 
		t_utc->tm_hour, t_utc->tm_min, t_utc->tm_sec);

	rtc_write( t_utc->tm_year + 1900,  t_utc->tm_mon + 1, t_utc->tm_mday, 
		t_utc->tm_hour, t_utc->tm_min, t_utc->tm_sec);
}

int key_poll(){
	int key = CW_IDLE;
	int input_method = get_cw_input_method();

	if (input_method == CW_IAMBIC || input_method == CW_IAMBICB){	
		if (digitalRead(PTT) == LOW)
			key |= CW_DASH;
		if (digitalRead(DASH) == LOW)
			key |= CW_DOT;
	}
	//straight key
	else if (digitalRead(DASH) == LOW)
			key = CW_DOWN;

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

static int tuning_ticks = 0;
void tuning_isr(void){
	int tuning = enc_read(&enc_b);
	if (tuning < 0)
		tuning_ticks++;
	if (tuning > 0)
		tuning_ticks--;	
}

void query_swr(){
	uint8_t response[4];
	int16_t vfwd, vref;
	int  vswr;
	char buff[20];

	if (!in_tx)
		return;
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return;

	vfwd = vref = 0;

	memcpy(&vfwd, response, 2);
	memcpy(&vref, response+2, 2);
	if (vref >= vfwd)
		vswr = 100;
	else
		vswr = (10*(vfwd + vref))/(vfwd-vref);
	sprintf(buff,"%d", (vfwd * 40)/68);
	set_field("#fwdpower", buff);		
	sprintf(buff, "%d", vswr);
	set_field("#vswr", buff);
}

void hw_init(){
	wiringPiSetup();
	init_gpio_pins();

	enc_init(&enc_a, ENC_FAST, ENC1_B, ENC1_A);
	enc_init(&enc_b, ENC_FAST, ENC2_A, ENC2_B);

	int e = g_timeout_add(1, ui_tick, NULL);

	wiringPiISR(ENC2_A, INT_EDGE_BOTH, tuning_isr);
	wiringPiISR(ENC2_B, INT_EDGE_BOTH, tuning_isr);
}

void hamlib_tx(int tx_input){
  if (tx_input){
    sound_input(1);
		tx_on(TX_SOFT);
	}
  else {
    sound_input(0);
		tx_off();
	}
}


int get_cw_delay(){
	return atoi(get_field("#cwdelay")->value);
}

int get_cw_input_method(){
	struct field *f = get_field("#cwinput");
	if (!strcmp(f->value, "KEYBOARD"))
		return CW_KBD;
	else if (!strcmp(f->value, "IAMBIC"))
		return CW_IAMBIC;
	else if (!strcmp(f->value, "IAMBICB"))
		return CW_IAMBICB;
	else
		return CW_STRAIGHT;
}

int get_pitch(){
	struct field *f = get_field("rx_pitch");
	return atoi(f->value);
}

int get_cw_tx_pitch(){
	struct field *f = get_field("#tx_pitch");
	return atoi(f->value);
}

int get_data_delay(){
	return data_delay;
}

int get_wpm(){
	struct field *f = get_field("#tx_wpm");
	return atoi(f->value);
}

long get_freq(){
	return atol(get_field("r1:freq")->value);
}



void bin_dump(int length, uint8_t *data){
	printf("i2c: ");
	for (int i = 0; i < length; i++)
		printf("%x ", data[i]);
	printf("\n");
}

int  web_get_console(char *buff, int max){
	char c;
	int i;

	if (q_length(&q_web) == 0)
		return 0;
	strcpy(buff, "CONSOLE ");
	buff += strlen("CONSOLE ");
	for (i = 0; (c = q_read(&q_web)) && i < max; i++){
		if (c < 128 && c >= ' ')
			*buff++ = c;
	}
	*buff = 0;
	return i;
}

void web_get_spectrum(char *buff){

  int n_bins = (int)((1.0 * spectrum_span) / 46.875);
  //the center frequency is at the center of the lower sideband,
  //i.e, three-fourth way up the bins.
  int starting_bin = (3 *MAX_BINS)/4 - n_bins/2;
  int ending_bin = starting_bin + n_bins;

  int j = 3;
  if (in_tx){
    strcpy(buff, "TX ");
    for (int i = 0; i < MOD_MAX; i++){
      int y = (2 * mod_display[i]) + 32;
      if (y > 127)
        buff[j++] = 127;
      else if(y > 0 && y <= 95)
        buff[j++] = y + 32;
      else
        buff[j++] = ' ';
    }
  }
  else{
    strcpy(buff, "RX ");
    for (int i = starting_bin; i <= ending_bin; i++){
      int y = spectrum_plot[i] + waterfall_offset;
      if (y > 95)
        buff[j++] = 127;
      else if(y >= 0 )
        buff[j++] = y + 32;
      else
        buff[j++] = ' ';
    }
  }

  buff[j++] = 0;
  return;
}

void set_radio_mode(char *mode){
	char umode[10], request[100], response[100];
	int i;

	printf("Mode: %s\n", mode);
	for (i = 0; i < sizeof(umode) - 1 && *mode; i++)
		umode[i] = toupper(*mode++);
	umode[i] = 0;

	sprintf(request, "r1:mode=%s", umode);
	sdr_request(request, response);
	if (strcmp(response, "ok")){
		printf("mode %d: unavailable\n", umode);
		return;
	}
	int new_bandwidth = 3000;
	switch(mode_id(umode)){
		case MODE_CW:
		case MODE_CWR:
			new_bandwidth = field_int("BW_CW");
			break;
		case MODE_LSB:
		case MODE_USB:
			new_bandwidth = field_int("BW_VOICE");
			break;
		case MODE_FT8:
			new_bandwidth = 4000;
			break;
		default:
			new_bandwidth = field_int("BW_DIGITAL");
	}
	layout_ui();
	//let the bw control trigger the filter
	char bw_str[10];
	sprintf(bw_str, "%d", new_bandwidth);
	field_set("BW", bw_str);

	struct field *f = get_field_by_label("MODE");
	if (strcmp(f->value, umode))
		field_set("MODE", umode);
}


gboolean ui_tick(gpointer gook){
	int static ticks = 0;

	ticks++;

	while (q_length(&q_remote_commands) > 0){
		//read each command until the 
		char remote_cmd[1000];
		int c, i;
		for (i = 0; i < sizeof(remote_cmd)-2 &&  (c = q_read(&q_remote_commands)) > 0; i++){
			remote_cmd[i] = c;
		}
		remote_cmd[i] = 0;

		//echo the keystrokes for chatty modes like cw/rtty/psk31/etc
		if (!strncmp(remote_cmd, "key ", 4))
			for (int i = 4; remote_cmd[i] > 0; i++)
				edit_field(get_field("#text_in"), remote_cmd[i]);	
		else {
			cmd_exec(remote_cmd);
			settings_updated = 1; //save the settings
		}
	}

		for (struct field *f = active_layout; f->cmd[0] > 0; f++){
			if (f->is_dirty){
				if (f->y >= 0){
					GdkRectangle r;
					r.x = f->x;
					r.y = f->y;
					r.width = f->width;
					r.height = f->height;
					invalidate_rect(r.x, r.y, r.width, r.height);
				}
			}
		}
  //char message[100];
	
	// check the tuning knob
	struct field *f = get_field("r1:freq");

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


	if (ticks % 20 == 0){
  	modem_poll(mode_id(get_field("r1:mode")->value));
	}

	int tick_count = 100;
	switch(mode_id(field_str("MODE"))){
		case MODE_CW:
		case MODE_CWR:
			tick_count = 50;
			break;
		case MODE_FT8:
			tick_count = 200;
			break;
		default:
			tick_count = 100; 
	}
	if (ticks >= tick_count){

		char response[6], cmd[10];
		cmd[0] = 1;

		if(in_tx){
			char buff[10];

			sprintf(buff,"%d", fwdpower);
			set_field("#fwdpower", buff);		
			sprintf(buff, "%d", vswr);
			set_field("#vswr", buff);
		}

		struct field *f = get_field("spectrum");
		update_field(f);	//move this each time the spectrum watefall index is moved
		f = get_field("waterfall");
		update_field(f);

		update_titlebar();
/*		f = get_field("#status");
		update_field(f);
*/
		if (digitalRead(ENC1_SW) == 0)
				focus_field(get_field("r1:volume"));

		if (record_start)
			update_field(get_field("#record"));

		// alternate character from the softkeyboard upon long press
		if (f_focus && focus_since + 500 < millis() 
						&& !strncmp(f_focus->cmd, "#kbd_", 5) && mouse_down){
			//emit the symbol
			struct field *f_text = f_focus; //get_field("#text_in");
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

		ticks = 0;
  }
	//update_field(get_field("#text_in")); //modem might have extracted some text

  hamlib_slice();
	remote_slice();
	save_user_settings(0);

 
	f = get_field("r1:mode");
	//straight key in CW
	if (f && (!strcmp(f->value, "2TONE") || !strcmp(f->value, "LSB") || 
	!strcmp(f->value, "USB"))){
		if (digitalRead(PTT) == LOW && in_tx == 0)
			tx_on(TX_PTT);
		else if (digitalRead(PTT) == HIGH && in_tx  == TX_PTT)
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

	//we are using two deprecated functions here
	//if anyone has a hack around them, do submit it
/*
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	screen_width = gdk_screen_width();
	screen_height = gdk_screen_height();
#pragma pop
*/
	q_init(&q_web, 1000);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 480);
  //gtk_window_set_default_size(GTK_WINDOW(window), screen_width, screen_height);
  gtk_window_set_title( GTK_WINDOW(window), "sBITX" );
	gtk_window_set_icon_from_file(GTK_WINDOW(window), "/home/pi/sbitx/sbitx_icon.png", NULL);

  display_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(display_area, 500, 400);
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

  gtk_widget_show_all(window);
	layout_ui();	
	focus_field(get_field("r1:volume"));
	webserver_start();
	f_last_text = get_field_by_label("TEXT");
}

/* handle modem callbacks for more data */


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
	f->is_dirty = 1;
	f->update_remote = 1;
	//update_field(f);
	return length;
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
		if (!strcmp(request, band_stack[new_band].name))
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
		}
	}

	//if we are still in the same band, move to the next position
	if (new_band == old_band){
		stack = ++band_stack[new_band].index;
		//move the stack and wrap the band around
		if (stack >= STACK_DEPTH)
			stack = 0;
		band_stack[new_band].index = stack;
	}
	stack = band_stack[new_band].index;
	sprintf(buff, "%d", band_stack[new_band].freq[stack]);
	char resp[100];
	set_operating_freq(band_stack[new_band].freq[stack], resp);
	field_set("FREQ", buff);
	field_set("MODE", mode_name[band_stack[new_band].mode[stack]]);	
	update_field(get_field("r1:mode"));

  // this fixes bug with filter settings not being applied after a band change, not sure why it's a bug - k3ng 2022-09-03
//  set_field("r1:low",get_field("r1:low")->value);
//  set_field("r1:high",get_field("r1:high")->value);

	abort_tx();
}

void utc_set(char *args, int update_rtc){
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

	rtc_write(n[0], n[1], n[2], n[3], n[4], n[5]);

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



void meter_calibrate(){
	//we change to 40 meters, cw
	printf("starting meter calibration\n"
	"1. Attach a power meter and a dummy load to the antenna\n"
	"2. Adjust the drive until you see 40 watts on the power meter\n"
	"3. Press the tuning knob to confirm.\n");

	set_field("r1:freq", "7035000");
	set_radio_mode("CW");	
	struct field *f_bridge = get_field("bridge");
	set_field("bridge", "100");	
	focus_field(f_bridge);
}

void do_control_action(char *cmd){	
	char request[1000], response[1000], buff[100];

	strcpy(request, cmd);			//don't mangle the original, thank you

	if (!strcmp(request, "CLOSE")){
		gtk_window_iconify(GTK_WINDOW(window));
	}
	else if (!strcmp(request, "OFF")){
		tx_off();
		set_field("#record", "OFF");
		save_user_settings(1);
		exit(0);
	}
	else if (!strncmp(request, "BW ",3)){
		int bw = atoi(request+3);	
		set_filter_high_low(bw); //calls do_control_action again to set LOW and HIGH
		//we have to save this as well
		save_bandwidth(bw);
	}
	else if (!strcmp(request, "WIPE"))
		call_wipe();
	else if (!strcmp(request, "ESC")){
		//empty the text buffer
		modem_abort();
		tx_off();
		call_wipe();
		field_set("TEXT", "");
		modem_abort();
		tx_off();
	}
	else if (!strcmp(request, "TX")){	
		tx_on(TX_SOFT);
	}
	else if (!strcmp(request, "WEB"))
		open_url("http://127.0.0.1:8080");
	else if (!strcmp(request, "RX")){
		tx_off();
	}
	else if (!strncmp(request, "RIT", 3))
		update_field(get_field("r1:freq"));
	else if (!strncmp(request, "SPLIT", 5)){
		update_field(get_field("r1:freq"));	
		if (!strcmp(get_field("#vfo")->value, "B"))
			set_field("#vfo", "A");
	}
	else if (!strcmp(request, "VFO B")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		struct field *vfo_a = get_field("#vfo_a_freq");
		struct field *vfo_b = get_field("#vfo_b_freq");
		if (!strcmp(vfo->value, "B")){
			//vfo_a_freq = atoi(f->value);
			strcpy(vfo_a->value, f->value);
			//sprintf(buff, "%d", vfo_b_freq);
			set_field("r1:freq", vfo_b->value);
			settings_updated++;
		}
	}
	else if (!strcmp(request, "VFO A")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		struct field *vfo_a = get_field("#vfo_a_freq");
		struct field *vfo_b = get_field("#vfo_b_freq");
		//printf("vfo old %s, new %s\n", vfo->value, request);
		if (!strcmp(vfo->value, "A")){
		//	vfo_b_freq = atoi(f->value);
			strcpy(vfo_b->value, f->value);
	//		sprintf(buff, "%d", vfo_a_freq);
			set_field("r1:freq", vfo_a->value);
			settings_updated++;
		}
	}
	else if (!strcmp(request, "KBD ON")){
		layout_ui();
	
	}
	else if (!strcmp(request, "KBD OFF")){
		layout_ui();
	}
	else if (!strcmp(request, "LOG\u00bb")){
			enter_qso();
	}
	//tuning step
  else if (!strcmp(request, "STEP 1M"))
    tuning_step = 1000000;
	else if (!strcmp(request, "STEP 100K"))
		tuning_step = 100000;
	else if (!strcmp(request, "STEP 10K"))
		tuning_step = 10000;
	else if (!strcmp(request, "STEP 1K"))
		tuning_step = 1000;
	else if (!strcmp(request, "STEP 100H"))
		tuning_step = 100;
	else if (!strcmp(request, "STEP 10H"))
		tuning_step = 10;

	//spectrum bandwidth
	else if (!strcmp(request, "SPAN 2.5K"))
		spectrum_span = 2500;
	else if (!strcmp(request, "SPAN 6K"))
		spectrum_span = 6000;
	else if (!strcmp(request, "SPAN 10K"))
		spectrum_span = 10000;
	else if (!strcmp(request, "SPAN 25K"))
		spectrum_span = 25000;
		
	//handle the band stacking
	else if (!strcmp(request, "80M") || 
		!strcmp(request, "40M") || 
		!strcmp(request, "30M") || 
		!strcmp(request, "20M") || 
		!strcmp(request, "17M") || 
		!strcmp(request, "15M") || 
		!strcmp(request, "12M") || 
		!strcmp(request, "10M")){
		change_band(request); 		
	}
	else if (!strcmp(request, "REC ON")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it

		char *path = getenv("HOME");
		time(&record_start);
		struct tm *tmp = localtime(&record_start);
		sprintf(fullpath, "%s/sbitx/audio/%04d%02d%02d-%02d%02d-%02d.wav", path, 
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 

		char request[300], response[100];
		sprintf(request, "record=%s", fullpath);
		sdr_request(request, response);
		sprintf(request, "Recording:%s\n", fullpath);
		write_console(FONT_LOG, request);
	}
	else if (!strcmp(request, "REC OFF")){
		sdr_request("record", "off");
		write_console(FONT_LOG, "Recording stopped\n");
		record_start = 0;
	}
	else if (!strcmp(request, "QRZ") && strlen(field_str("CALL")) > 0)
		qrz(field_str("CALL"));
	else {
		//send this to the radio core
		char args[MAX_FIELD_LENGTH];
		char exec[20];
		int i, j;

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
		
		//translate the frequency of operating depending upon rit, split, etc.
		if (!strncmp(request, "FREQ", 4))
			set_operating_freq(atoi(request+5), response);
		else if (!strncmp(request, "MODE ", 5)){
			set_radio_mode(request+5);
			update_field(get_field("r1:mode"));
		}
		else{
			struct field *f = get_field_by_label(exec); 
			if (f){
				sprintf(request, "%s=%s", f->cmd, args);
				sdr_request(request, response);
			}
		}
	}
}

/*
	These are user/remote entered commands.
	The command format is "CMD VALUE", the CMD is an all uppercase text
	that matches the label of a control.
	The value is taken as a string past the separator space all the way
	to the end of the string, including any spaces.

	It also handles many commands that don't map to a control
	like metercal or txcal, etc.
*/
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

	if (!strcmp(exec, "FT8")){
		ft8_process(args, FT8_START_QSO);
	}
	else if (!strcmp(exec, "callsign")){
		strcpy(get_field("#mycallsign")->value,args); 
		sprintf(response, "\n[Your callsign is set to %s]\n", get_field("#mycallsign")->value);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "metercal")){
		meter_calibrate();
	}
	else if (!strcmp(exec, "abort"))
		abort_tx();
	else if (!strcmp(exec, "rtc"))
		rtc_read();
	else if (!strcmp(exec, "txcal")){
		char response[10];
		sdr_request("txcal=", response);
	}
	else if (!strcmp(exec, "grid")){	
		set_field("#mygrid", args);
		sprintf(response, "\n[Your grid is set to %s]\n", get_field("#mygrid")->value);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "utc")){
		utc_set(args, 1);
	}
	else if (!strcmp(exec, "logbook")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it
		char *path = getenv("HOME");
		sprintf(fullpath, "mousepad %s/sbitx/data/logbook.txt", path); 
		execute_app(fullpath);
	}
	else if (!strcmp(exec, "clear")){
		console_init();
	}
	else if(!strcmp(exec, "macro") || !strcmp(exec, "MACRO")){
		if (!strcmp(args, "list"))
			macro_list(NULL);
		else if (!macro_load(args, NULL)){
			set_ui(LAYOUT_MACROS);
			set_field("#current_macro", args);
		}
		else if (strlen(get_field("#current_macro")->value)){
			write_console(FONT_LOG, "current macro is ");
			write_console(FONT_LOG, get_field("#current_macro")->value);
			write_console(FONT_LOG, "\n");
		}
		else
			write_console(FONT_LOG, "macro file not loaded\n");
	}
	else if (!strcmp(exec, "qso"))
		enter_qso(args);
	else if (!strcmp(exec, "exchange")){
		set_field("#contest_serial", "0");
		set_field("#sent_exchange", "");

		if (strlen(args)){
			set_field("#sent_exchange", args);
			if (atoi(args))
				set_field("#contest_serial", args);
		}
		write_console(FONT_LOG, "Exchange set to [");
		write_console(FONT_LOG, get_field("#sent_exchange")->value);
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
  else if (!strcmp(exec, "exit")){
    tx_off();
    set_field("#record", "OFF");
    save_user_settings(1);
    exit(0);
  }
	else if (!strcmp(exec, "qrz")){
		if(strlen(args))
			qrz(args);
		else
			write_console(FONT_LOG, "/qrz [callsign]\n");
	}
	else if (!strcmp(exec, "mode") || !strcmp(exec, "m") || !strcmp(exec, "MODE")){
		set_radio_mode(args);
		update_field(get_field("r1:mode"));
	}
	else if (!strcmp(exec, "t"))
		tx_on(TX_SOFT);
	else if (!strcmp(exec, "r"))
		tx_off();
	else if (!strcmp(exec, "telnet")){
		if (strlen(args) > 5) 
			telnet_open(args);
		else
			telnet_open(get_field("#telneturl")->value);
	}
	else if (!strcmp(exec, "tclose"))
		telnet_close(args);
	else if (!strcmp(exec, "tel"))
		telnet_write(args);
	else if (!strcmp(exec, "txpitch")){
		if (strlen(args)){
			int t = atoi(args);	
			if (t > 100 && t < 4000)
				set_field("#tx_pitch", args);
			else
				write_console(FONT_LOG, "cw pitch should be 100-4000");
		}
		char buff[100];
		sprintf(buff, "txpitch is set to %d Hz\n", get_cw_tx_pitch());
		write_console(FONT_LOG, buff);
	}
/*	else if (!strcmp(exec, "PITCH")){
		struct field *f = get_field_by_label(exec);
		field_set("PITCH", args);
		focus_field(f);
	}
*/
	
	else if (exec[0] == 'F' && isdigit(exec[1])){
		char buff[1000];
		printf("executing macro %s\n", exec);
		do_macro(get_field_by_label(exec), NULL, GDK_BUTTON_PRESS, 0, 0, 0);
		//macro_exec(atoi(exec+1), buff);
		//if (strlen(buff))
		//	set_field("#text_in", buff);
	}
	else {
		char field_name[32];
		//conver the string to upper if not already so
		for (char *p = exec; *p; p++)
			*p =  toupper(*p);
		struct field *f = get_field_by_label(exec);
		if (f){
			//convert all the letters to uppercase
			for(char *p = args; *p; p++)
				*p = toupper(*p);
			if(set_field(f->cmd, args)){
				write_console(FONT_LOG, "Invalid setting:");
      } else {
				//this is an extract from focus_field()
				//it shifts the focus to the updated field
				//without toggling/jumping the value 
				struct field *prev_hover = f_hover;
				struct field *prev_focus = f_focus;
				f_focus = NULL;
				f_focus = f_hover = f;
				focus_since = millis();
				update_field(f_hover);
      }
		}
	}
	save_user_settings(0);
}

// From https://stackoverflow.com/questions/5339200/how-to-create-a-single-instance-application-in-c-or-c
void ensure_single_instance(){
	int pid_file = open("/tmp/sbitx.pid", O_CREAT | O_RDWR, 0666);
	int rc = flock(pid_file, LOCK_EX | LOCK_NB);
	if(rc) {
    if(EWOULDBLOCK == errno){
			printf("Another instance of sbitx is already running\n");
			exit(0);
		}	
	}
}

int main( int argc, char* argv[] ) {

	puts(VER_STR);
	active_layout = main_controls;

//	ensure_single_instance();

	//unlink any pending ft8 transmission
	unlink("/home/pi/sbitx/ft8tx_float.raw");
	call_wipe();
	
	ui_init(argc, argv);
	hw_init();
	console_init();

	q_init(&q_remote_commands, 1000); //not too many commands
	q_init(&q_tx_text, 100); //best not to have a very large q 
	setup();

	rtc_sync();

	struct field *f;
	f = active_layout;

	//initialize the modulation display

	tx_mod_max = get_field("spectrum")->width;
	tx_mod_buff = malloc(sizeof(int32_t) * tx_mod_max);
	memset(tx_mod_buff, 0, sizeof(int32_t) * tx_mod_max);
	tx_mod_index = 0;
	init_waterfall();

	//set the radio to some decent defaults
	do_control_action("FREQ 7100000");
	do_control_action("MODE LSB");	
	do_control_action("STEP 1K");	
  do_control_action("SPAN 25K");

	strcpy(vfo_a_mode, "USB");
	strcpy(vfo_b_mode, "LSB");
	set_field("#mycallsign", "VU2LCH");
	//vfo_a_freq = 14000000;
	//vfo_b_freq = 7000000;
	
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
    printf("Unable to load ~/sbitx/data/user_settings.ini\n"
		"Loading default.ini instead\n");
		strcpy(directory, path);
		strcat(directory, "/sbitx/data/default_settings.ini");
  	ini_parse(directory, user_settings_handler, NULL);
  }

	//the logger fields may have an unfinished qso details
	call_wipe();

	if (strlen(get_field("#current_macro")->value))
		macro_load(get_field("#current_macro")->value, NULL);

	char buff[1000];

	//now set the frequency of operation and more to vfo_a
  set_field("r1:freq", get_field("#vfo_a_freq")->value);

	console_init();
	write_console(FONT_LOG, VER_STR);
  write_console(FONT_LOG, "\r\nEnter \\help for help\r\n");

	if (strcmp(get_field("#mycallsign")->value, "NOBODY")){
		sprintf(buff, "\nWelcome %s your grid is %s\n", 
		get_field("#mycallsign")->value, get_field("#mygrid")->value);
		write_console(FONT_LOG, buff);
	}
	else 
		write_console(FONT_LOG, "Set your with '\\callsign [yourcallsign]'\n"
		"Set your 6 letter grid with '\\grid [yourgrid]\n");

	set_field("#text_in", "");
	field_set("REC", "OFF");
	field_set("KBD", "OFF");

	// you don't want to save the recently loaded settings
	settings_updated = 0;
  hamlib_start();
	remote_start();

	rtc_read();

//	open_url("http://127.0.0.1:8080");
//	execute_app("chromium-browser --log-leve=3 "
//	"--enable-features=OverlayScrollbar http://127.0.0.1:8080"
//	"  &>/dev/null &");
  gtk_main();
  
  return 0;
}

