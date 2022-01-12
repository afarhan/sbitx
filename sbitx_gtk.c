#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <complex.h>
#include <fftw3.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "ini.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <cairo.h>
#include <wiringPi.h>
#include <wiringSerial.h>

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

#define SW1 (6)
#define SW2 (10)
#define SW3 (11)
#define SW4 (7)
#define SW5 (22)
#define PTT (7)

#define ENC_FAST 1
#define ENC_SLOW 5

struct encoder {
	int pin_a,  pin_b;
	int speed;
	int prev_state;
	int history;
};

struct encoder enc_a, enc_b;

#define TX_LINE 4
#define BAND_SELECT 5
#define LPF_A 5
#define LPF_B 6
#define LPF_C 10
#define LPF_D 11

char output_pins[] = {
	TX_LINE, BAND_SELECT	
};

static int serial_fd = -1;
static int xit = 512; 
static int tuning_step = 1000;

struct band {
	int frequency, low,  high, mode;
};

#define BAND10M	0
#define BAND13M	1
#define BAND15M 2	
#define BAND17M 3	
#define BAND20M 4	
#define BAND30M 5
#define BAND40M 6 
#define BAND60M 7 
#define BAND80M 8 

struct band_memory{
    int frequency;
    int mode;
};



GtkWidget *display_area = NULL;
int screen_width, screen_height;
int spectrum_span = 48000;

void do_cmd(char *cmd);
#define MIN_KEY_UP 0xFF52
#define MIN_KEY_DOWN	0xFF54
#define MIN_KEY_LEFT 0xFF51
#define MIN_KEY_RIGHT 0xFF53
#define MIN_KEY_ENTER 0xFF0D
#define MIN_KEY_ESC	0xFF1B
#define MIN_KEY_BACKSPACE 0xFF08
#define MIN_KEY_TAB 0xFF09

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
	{0.25, 0.25, 0.25}, //SPECTRUM_GRID
	{1,1,0},	//SPECTRUM_PLOT
	{1,1,1}, 	//SPECTRUM_NEEDLE
	{0.5,0.5,0.5}, COLOR_CONTROL_BOX
};

char *ui_font = "Sans";
int field_font_size = 12;

/* 	the field in focus will be exited when you hit an escape
		the field in focus will be changeable until it loses focus
		hover will always be on the field in focus.
		if the focus is -1,then hover works
*/

guint key_modifier = 0;

#define FIELD_NUMBER 0
#define FIELD_BUTTON 1
#define FIELD_TOGGLE 2
#define FIELD_SELECTION 3
#define FIELD_TEXT 4
#define FIELD_STATIC 5

#define MAX_FIELD_LENGTH 128

#define FONT_FIELD_LABEL 0
#define FONT_FIELD_VALUE 1
#define FONT_LARGE_FIELD 2
#define FONT_LARGE_VALUE 3
#define FONT_SMALL 4

struct font_style {
	int index;
	double r, g, b;
	char name[32];
	int height;
	int weight;
	int type;
};
struct font_style font_table[] = {
	{FONT_FIELD_LABEL, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FIELD_VALUE, 1, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_FIELD, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_VALUE, 1, 1, 1, "Arial", 24, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL, 0, 1, 1, "Mono", 10, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
};
/*
	Warning: The field selection is used for TOGGLE and SELECTION fields
	each selection by the '/' should be unique. otherwise, the simple logic will
	get confused 
*/

struct field {
	char	*cmd;
	int		x, y, width, height;
	char	label[30];
	int 	label_width;
	char	value[MAX_FIELD_LENGTH];
	char	value_type; //NUMBER, SELECTION, TEXT, TOGGLE, BUTTON
	int 	font_index; //refers to font_style table
	char  selection[1000];
	int	 	min, max, step;
};

// the cmd fields that have '#' are not to be sent to the sdr
struct field main_controls[] = {
	{ "r1:freq", 500, 0, 130, 49, "", 5, "14000000", FIELD_NUMBER, FONT_LARGE_VALUE, "", 500000, 30000000, 100},

	// Main RX
	{"#r1", 70, 55 ,100, 50, "MAIN RX", 1, "MAIN RX", FIELD_STATIC, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},
	{"#rit", 235, 50 ,55, 50, "RIT", 1, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},
	{ "r1:volume", 70, 100, 55, 50, "VOLUME", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 1024, 1},
	{ "r1:mode", 125, 100, 55, 50, "MODE", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, "USB/LSB/CW/CWR/2TONE", 0,0, 0},
	{ "r1:low", 180, 100, 55, 50, "LOW", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE, "", 300,4000, 50},
	{ "r1:high", 235, 100, 55, 50, "HIGH", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE, "", 300, 4000, 50},

	{ "r1:agc", 70, 150, 55, 50, "AGC", 40, "SLOW", FIELD_SELECTION, FONT_FIELD_VALUE, "OFF/SLOW/FAST", 0, 1024, 1},
	{ "r1:gain", 125, 150, 55, 50, "IF GAIN", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 100, 1},
	//{  "#band", 180, 150, 55, 50, "Band", 40, "80M", FIELD_SELECTION, FONT_FIELD_VALUE, "160M/80M/60M/40M/30M/20M/17M/15M/10M", 0,0, 0},
	{  "#band", 180, 150, 55, 50, "Band", 40, "80M", FIELD_SELECTION, FONT_FIELD_VALUE, "10M/15M/17M/20M/30M/40M/60M/80M", 0,0, 0},
	{"r1:mute", 235, 150 ,55, 50, "MUTE", 1, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},

	// Sub RX
	{"#r2", 70, 205 ,100, 50, "SUB RX", 1, "MAIN RX", FIELD_STATIC, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},
	{ "r2:volume", 70, 250, 55, 50, "VOLUME", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 1024, 1},
	{ "r2:mode", 125, 250, 55, 50, "MODE", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, "USB/LSB/CW/CWR/2TONE", 0,0, 0},
	{ "r2:low", 180, 250, 55, 50, "LOW", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE, "", 300,4000, 50},
	{ "r2:high", 235, 250, 55, 50, "HIGH", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE, "", 300, 4000, 50},

	{ "r2:agc", 70, 300, 55, 50, "AGC", 40, "SLOW", FIELD_SELECTION, FONT_FIELD_VALUE, "SLOW/FAST", 0, 1024, 1},
	{ "r2:gain", 125, 300, 55, 50, "IF GAIN", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 100, 1},
	{"r2:send", 180, 300 ,55, 50, "SEND", 1, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},
	{"r2:mute", 235, 300 ,55, 50, "MUTE", 1, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},

	//tx 
	{ "tx_gain", 70, 370, 55, 50, "MIC", 40, "50", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 100, 1},
	{ "tx_power", 125, 370, 55, 50, "DRIVE", 40, "40", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 100, 1},
	{ "tx_comp", 180, 370, 55, 50, "COMP", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 10, 1},
	{ "tx_bw", 235, 370, 55, 50, "BW", 40, "2.7KHz", FIELD_SELECTION, FONT_FIELD_VALUE, "3KHz/2.2KHz/1.8KHz", 0,0, 0},

	{ "tx_wpm", 70, 420, 55, 50, "WPM", 40, "12", FIELD_NUMBER, FONT_FIELD_VALUE, "", 1, 50, 1},
	{ "tx_key", 125, 420, 55, 50, "KEY", 40, "HARD", FIELD_SELECTION, FONT_FIELD_VALUE, "SOFT/HARD", 0, 0, 0},
	{ "tx_vox", 180, 420, 55, 50, "VOX", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0, 0, 0},
	{ "tx_record", 235, 420, 55, 50, "RECORD", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0, 0},
	
	// top row
	{ "#split", 340, 0, 55, 50, "SPLIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0,0},
	{"#step", 390, 0 ,50, 50, "STEP", 1, "50Hz", FIELD_SELECTION, FONT_FIELD_VALUE, "10KHz/1KHz/200Hz/50Hz", 0,0,0},
	{"#step", 440, 0 ,50, 50, "VFO", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, "A/B", 0,0,0},
	{"#span", 690, 0 ,50, 50, "SPAN", 1, "25KHz", FIELD_SELECTION, FONT_FIELD_VALUE, "25KHz/10KHz/3KHz", 0,0,0},

	/* beyond MAX_MAIN_CONROLS are the static text display */
	{"spectrum", 340, 50, 400, 200, "Spectrum ", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, "", 0,0,0},   
	{"waterfall", 340, 270 , 400, 210, "Waterfall ", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, "", 0,0,0},
	{"#close", 750, 0 ,50, 50, "CLOSE", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#off", 750, 430 ,50, 50, "OFF", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},

	/* band stack registers */
	{"#10m", 0, 1 ,50, 50, "10 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#13m", 0, 50 ,50, 50, "13 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#15m", 0, 100 ,50, 50, "15 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#17m", 0, 150 ,50, 50, "17 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#20m", 0, 200 ,50, 50, "20 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#30m", 0, 250 ,50, 50, "30 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#40m", 0, 300 ,50, 50, "40 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#60m", 0, 350 ,50, 50, "60 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},
	{"#80m", 0, 400 ,50, 50, "80 M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0},

	
};


void write_gui_ini(){
  FILE *pf = fopen("gui_main.ini", "w");
	int total = sizeof(main_controls) / sizeof(struct field);
	for (int i = 0; i < total; i++){
		struct field *f = main_controls + i;
    fprintf(pf, "[%s]\n", f->cmd);
    fprintf(pf, "x = %d\n", f->x);
    fprintf(pf, "y = %d\n", f->y);
    fprintf(pf, "width = %d\n", f->width);
    fprintf(pf, "height = %d\n", f->height);
    fprintf(pf, "label = %s\n", f->label);
    fprintf(pf, "label_width = %d\n", f->label_width);
    fprintf(pf, "value = %s\n", f->value);
    fprintf(pf, "value = %d\n", f->value_type);
    fprintf(pf, "font_index = %d\n", f->font_index);
    fprintf(pf, "selection = %s\n", f->selection);
    fprintf(pf, "min = %d\n", f->min);
    fprintf(pf, "max = %d\n", f->max);
    fprintf(pf, "step = %d\n\n", f->step);
  }
  fclose(pf);
}


//static int field_in_focus = -1;
//static int field_in_hover = 0;

static struct field *f_focus = NULL;
static struct field *f_hover = main_controls;

//the main app window
GtkWidget *window;

void do_cmd(char *cmd_string);

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

static void rect(cairo_t *gfx, int x, int y, int w, int h, int color, int thickness){
  cairo_set_source_rgb( gfx, palette[color][0], palette[color][1], palette[color][2]);
	cairo_set_line_width(gfx, thickness);
	cairo_rectangle(gfx, x, y, w, h);
  cairo_stroke(gfx);
}

struct field *get_field(char *cmd){
	for (int i = 0; i < sizeof(main_controls)/sizeof(struct field); i++)
		if (!strcmp(main_controls[i].cmd, cmd))
			return main_controls + i;
	return NULL;
}

/* rendering of the fields */

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
	puts("setting the wf to zero");
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


void draw_waterfall(GtkWidget *widget, cairo_t *gfx){
	struct field *f;
	f = get_field("waterfall");

	memmove(waterfall_map + f->width * 3, waterfall_map, f->width * (f->height - 1) * 3);

	int index = 0;
	for (int i = 0; i < f->width; i++){
			int v = wf[i];
			v *= 5;
			if (v > 255)
				v = 255;
			if (v < 128){
				waterfall_map[index++] = 0;
				waterfall_map[index++] = v;
				waterfall_map[index++] = 255-v; 
			}else {
				waterfall_map[index++] = v;
				waterfall_map[index++] = 255-v;
				waterfall_map[index++] = 0; 
			}
	}

	gdk_cairo_set_source_pixbuf(gfx, waterfall_pixbuf, f->x, f->y);		
	cairo_paint(gfx);
	cairo_fill(gfx);
}

void draw_spectrum(GtkWidget *widget, cairo_t *gfx){
	int y, sub_division, i;
	struct field *f;

	f = get_field("spectrum");
	sub_division = f->width / 10;

	// clear the spectrum	
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	//draw the horizontal grid
	for (i =  0; i <= f->height; i += f->height/10){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	//draw the vertical grid
	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + f->height); 
	}
	cairo_stroke(gfx);

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
	cairo_move_to(gfx, f->x, f->y + f->height);

	float x = 0;
	int j = 0;
	for (i = starting_bin; i <= ending_bin; i++){
		int y;

		// the center fft bin is at zero, from MAX_BINS/2 onwards,
		// the bins are at lowest frequency (-ve frequency)
		int offset = i;
		offset = (offset - MAX_BINS/2);
		//y axis is the power  in db of each bin, scaled to 100 db
		y = ((power2dB(cnrmf(fft_bins[i])) + waterfall_offset) * f->height)/100; 
		// limit y inside the spectrum display box
		if ( y <  0)
			y = 0;
		if (y > f->height)
			y = f->height - 1;
		//the plot should be increase upwards
		cairo_line_to(gfx, f->x + (int)x, f->y + f->height - y);

		//fill the waterfall
		for (int k = 0; k <= 1 + (int)x_step; k++)
			wf[k + (int)x] = (y * 100)/f->height;
		x += x_step;
	}
	cairo_stroke(gfx);

	//draw the needle
	for (struct rx *r = rx_list; r; r = r->next){
		int needle_x  = (f->width*(MAX_BINS/2 - r->tuned_bin))/(MAX_BINS/2);
		fill_rect(gfx, f->x + needle_x, f->y, 1, f->height,  SPECTRUM_NEEDLE);
	}

	draw_waterfall(widget, gfx);
}

void draw_field(GtkWidget *widget, cairo_t *gfx, struct field *f){
	struct font_style *s = font_table + 0;
	if (!strcmp(f->cmd, "spectrum")){
		draw_spectrum(widget, gfx);
		return;
	}
	if (!strcmp(f->cmd, "waterfall")){
		return;
	}

	fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_SELECTED_BOX, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
	else if (f->value_type != FIELD_STATIC)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	int width, offset;	
	
	switch(f->value_type){
		case FIELD_SELECTION:
		case FIELD_NUMBER:
		case FIELD_TOGGLE:
		case FIELD_TEXT:
			width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
			offset = f->width/2 - width/2;
			draw_text(gfx, f->x + offset, f->y+5 ,  f->label, FONT_FIELD_LABEL);
			width = measure_text(gfx, f->value, f->font_index);
			offset = f->width/2 - width/2;
			if (!strlen(f->label))
				draw_text(gfx, f->x + offset , f->y+6, f->value, f->font_index);
			else
				draw_text(gfx, f->x+offset , f->y+25 , f->value , f->font_index);
			break;
		case FIELD_BUTTON:
			width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
			offset = f->width/2 - width/2;
			draw_text(gfx, f->x + offset, f->y+13 , f->label , FONT_FIELD_LABEL);
			break;
		case FIELD_STATIC:
			draw_text(gfx, f->x, f->y, f->label, FONT_FIELD_LABEL);
			break;
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
	int total = sizeof(main_controls) / sizeof(struct field);
	for (int i = 0; i < total; i++){
		double cx1, cx2, cy1, cy2;
		struct field *f = main_controls + i;
		cx1 = f->x;
		cx2 = cx1 + f->width;
		cy1 = f->y;
		cy2 = cy1 + f->height;
		if (cairo_in_clip(gfx, cx1, cy1) || cairo_in_clip(gfx, cx2, cy2)){
			draw_field(widget, gfx, main_controls + i);
		}
	}
	draw_spectrum(widget, gfx);
}

/* gtk specific routines */
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, gpointer user_data ) {
	redraw_main_screen(widget, cr);	
  return FALSE;
}

static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
	printf("size changed to %d x %d\n", event->width, event->height);
	screen_width = event->width;
	screen_height = event->height;
}

static void update_field(struct field *f){
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


static void edit_field(struct field *f, int action){
	//struct field *f = main_controls + field_index;
	int v;

	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(f->value);
		if (action == MIN_KEY_UP && v + f->step <= f->max){
			if (!strcmp(f->cmd, "r1:freq") || !strcmp(f->cmd, "r2:freq"))
				v += tuning_step;
			else
				v += f->step;
		}
		else if (action == MIN_KEY_DOWN && v - f->step >= f->min){
			if (!strcmp(f->cmd, "r1:freq") || !strcmp(f->cmd, "r2:freq"))
				v -= tuning_step;
			else
				v -= f->step;
		}
		sprintf(f->value, "%d",  v);
	}
	else if (f->value_type == FIELD_SELECTION){
		char *p, *prev, *next, b[100];
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
				strcpy(f->value, prev); 
		}
		else if (action == MIN_KEY_UP && prev){
			strcpy(f->value, prev);
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
	else if (f->value_type = FIELD_TEXT){
		puts("field text");
		if (action >= ' ' && action <= 127 && strlen(f->value) < f->max){
			puts("adding character");
			int l = strlen(f->value);
			f->value[l++] = action;
			f->value[l] = 0;
		}
		else if (action == MIN_KEY_BACKSPACE && strlen(f->value) > 0){
			int l = strlen(f->value) - 1;
			f->value[l] = 0;
		}
	}

	//send a command to the receiver
	char buff[200];
	sprintf(buff, "%s=%s", f->cmd, f->value);
	do_cmd(buff);
	update_field(f);
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
	}
	if (f_focus)
		printf("focus is on %d\n", f_focus->cmd);
	else
		printf("focus lost\n");
	update_field(f_hover);

	//is it a toggle field?
	if (f_focus->value_type == FIELD_TOGGLE)
		edit_field(f_focus, MIN_KEY_DOWN);	

	//if the button has been pressed, do the needful
	if (f_focus->value_type == FIELD_TOGGLE || f_focus->value_type == FIELD_BUTTON)
		do_cmd(f->cmd);
}

void set_field(char *id, char *value){
	struct field *f = get_field(id);
	int v;

	if (!f){
		printf("*Error: field[%s] not found. Check for typo?\n", id);
		return;
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
		}
		else
			strcpy(f->value, value);
	}
	else if (f->value_type == FIELD_BUTTON){
		NULL; // ah, do nothing!
	}
	else if (f->value_type = FIELD_TEXT){
		if (strlen(value) > f->max || strlen(value) < f->min)
			printf("*Error: field[%s] can't be set to [%s], improper size.\n", f->cmd, value);
		else
			strcpy(f->value, value);
	}

	//send a command to the receiver
	char buff[200];
	sprintf(buff, "%s=%s", f->cmd, f->value);
	do_cmd(buff);
	update_field(f);
}

static int in_tx = 0;
static void tx_on(){
	char response[100];

	if (in_tx == 0){
		digitalWrite(TX_LINE, HIGH);
		sdr_request("tx=on", response);	
		in_tx = 1;
	}
}

static void tx_off(){
	char response[100];

	if (in_tx == 1){
		digitalWrite(TX_LINE, LOW);
		sdr_request("tx=off", response);	
		in_tx = 0;
	}
}

static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	key_modifier = 0;

	if (event->keyval == MIN_KEY_TAB){
		tx_off();
    puts("Transmit off");
  }

}

static gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	char request[1000], response[1000];

	//printf("keyPress %x %x\n", event->keyval, event->state);
	//key_modifier = event->keyval;
	switch(event->keyval){
		case MIN_KEY_UP:
			if (f_focus == NULL && f_hover > main_controls){
				hover_field(f_hover - 1);
				//printf("Up, hover %s\n", f_hover->cmd);
			}else if (f_focus){
				edit_field(f_focus, MIN_KEY_UP);
			}
			break;
		case MIN_KEY_DOWN:
			if (f_focus == NULL && strcmp(f_hover->cmd, "")){
				hover_field(f_hover + 1);
				//printf("Down, hover %d\n", f_hover);
			}
			else if (f_focus){
				edit_field(f_focus, MIN_KEY_DOWN);
			}
			break;
		case MIN_KEY_ESC:
			focus_field(NULL);
			break;
		case MIN_KEY_ENTER:
			if (f_focus == NULL )
				focus_field(f_hover);
			break;
		case 65507:
			key_modifier |= event->keyval;
			//printf("key_modifier set to %d\n", key_modifier);
			break;
		case MIN_KEY_TAB:
			tx_on();
      puts("Tx is ON!");
			break;
		case 't':
			tx_on();
			break;
		case 'r':
			tx_off();
			break;
		case 'u':
			if (xit < 2048)
				xit += 32;
			sprintf(request, "xit=%d", xit);
			sdr_request(request, response);
			break;
		case 'i':
			if(xit > 32)
				xit -= 32;
			sprintf(request, "xit=%d", xit);
			sdr_request(request, response);
			break;
		default:
			if (f_focus)
				edit_field(f_focus, event->keyval); 
			printf("key = %d (%c)\n", event->keyval, (char)event->keyval); 	
	}
  return FALSE; 
}

static gboolean on_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer data) {
	
	if (f_focus){
		if (event->direction == 0)
			edit_field(f_focus, MIN_KEY_UP);
		else
			edit_field(f_focus, MIN_KEY_DOWN);
	}
		
}

static gboolean on_mouse_press (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	struct field *f;
	if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY){
		printf("mouse event at %d, %d\n", (int)(event->x), (int)(event->y));
		for (int i = 0; i < sizeof(main_controls)/sizeof(struct field); i++) {
			f = main_controls + i;
			if (f->x < event->x && event->x < f->x + f->width 
					&& f->y < event->y && event->y < f->y + f->height)
				focus_field(f);
		}
	}
  /* We've handled the event, stop processing */
  return TRUE;
}


/*
Turns out (after two days of debugging) that GTK is not thread-safe and
we cannot invalidate the spectrum from another thread .
This redraw is called from another thread. Hence, we set a flag here 
that is read by a timer tick from the main UI thread and the window
is posted a redraw signal that in turn triggers the redraw_all routine.
Don't ask me, I only work around here.
*/
static int redraw_flag = 1; 
void redraw(){
	redraw_flag++;
}

void init_gpio_pins(){
	for (int i = 0; i < 15; i++){
		pinMode(pins[i], INPUT);
		pullUpDnControl(pins[i], PUD_UP);
	}
	pinMode(TX_LINE, OUTPUT);
	pinMode(LPF_A, OUTPUT);
	pinMode(LPF_B, OUTPUT);
	pinMode(LPF_C, OUTPUT);
	pinMode(LPF_D, OUTPUT);
	pinMode(PTT, INPUT);
	pullUpDnControl(PTT, PUD_UP);
  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);
	digitalWrite(TX_LINE, LOW);
	digitalWrite(BAND_SELECT, LOW);
}


/* Front Panel Routines */


int read_switch(int i){
	return digitalRead(i) == HIGH ? 0 : 1;
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

char cat_command[100];

void cat_init(){
  serial_fd = serialOpen("/dev/pts/2", 38400);
  if (serial_fd == -1){
    puts("*Error: Serial port didn't open\n");
  }
  strcpy(cat_command, ""); 
}


void cat_poll(){
  char c, buff[100];

  while(serialDataAvail(serial_fd)){
    c = serialGetchar(serial_fd);
    int l = strlen(cat_command);
    if (l < sizeof(cat_command) - 1){
      cat_command[l++] = c;
      cat_command[l] = 0;
    }
    if (c == ';'){
      if (!strcmp(cat_command, "ID;"))
        serialPuts(serial_fd, "ID020;");
      else if (!strncmp(cat_command, "FA", 2)){ //read the frequency
	      struct field *f = get_field("r1:freq");
	      int freq = atoi(f->value);	
        if (cat_command[2] != ';'){
          int freq = atoi(cat_command + 5);
	        sprintf(f->value, "%d", freq);
	        sprintf(buff, "r1:freq=%d", freq);	
	        update_field(f);
	        do_cmd(buff);
        }
        serialPrintf(serial_fd, "FA000%08d;", atoi(f->value));
      }
      else if(!strncmp(cat_command, "IF", 2)){
	      struct field *f = get_field("r1:freq");
        serialPrintf(serial_fd, "IF000%8d     +00000000002000000 ;", f->value);
        //                       12345678456789012345678901234567890
      }
      else if (!strncmp(cat_command, "PS", 2))
        serialPrintf(serial_fd, "PS1;");
      else if (!strncmp(cat_command, "AI", 2))
        serialPrintf(serial_fd, "AI1;");
      else if (!strncmp(cat_command, "MD", 2)){
          switch(cat_command[2]){
          case '1': set_field("r1:mode", "LSB");break;
          case '2': set_field("r1:mode", "USB");break;
          case '3': set_field("r1:mode", "CW");break;
          case '4': set_field("r1:mode", "NBFM");break;
          case '7': set_field("r1:mode", "CWR");break;
          }
          struct field *f = get_field("r1:mode");
          if (!strcmp(f->value, "USB"))
            serialPuts(serial_fd, "MD2;");
          else if (!strcmp(f->value, "LSB"))
            serialPuts(serial_fd, "MD1;");
          else if (!strcmp(f->value, "CW"))
            serialPuts(serial_fd, "MD3;");
          else if (!strcmp(f->value, "CWR"))
            serialPuts(serial_fd, "MD7;");
          else if (!strcmp(f->value, "NBFM"))
            serialPuts(serial_fd, "MD4;");
      }
      else if (!strcmp(cat_command, "RX;")){
        serialPuts(serial_fd, "RX0;");
      }
    
      printf("*** CAT : %s\n", cat_command);
      cat_command[0] = 0;
    }
  }
}

gboolean ui_tick(gpointer gook){
	int static ticks = 0;

	ticks++;
	if (ticks >= 10){
		struct field *f = get_field("spectrum");
		update_field(f);	//move this each time the spectrum watefall index is moved
		f = get_field("waterfall");
		update_field(f);
		redraw_flag = 0;
		ticks = 0;
	}

  cat_poll();

	// check the tuning knob
	struct field *f = get_field("r1:freq");
	int tuning = enc_read(&enc_b);
	if (tuning != 0){
		if (tuning < 0)
			edit_field(f, MIN_KEY_DOWN);	
		else if (tuning > 0)
			edit_field(f, MIN_KEY_UP);
	}

 
   
	//check the push-to-talk
	if (digitalRead(PTT) == LOW && in_tx == 0)
		tx_on();	
	else if (digitalRead(PTT) == HIGH && in_tx == 1)
		tx_off();
  
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

  window = gtk_window_new( GTK_WINDOW_TOPLEVEL );
  gtk_window_set_default_size( GTK_WINDOW(window), 800, 480 );
  gtk_window_set_title( GTK_WINDOW(window), "sBITX" );
  
  display_area = gtk_drawing_area_new();

  gtk_container_add( GTK_CONTAINER(window), display_area );

  g_signal_connect( G_OBJECT(window), "destroy", G_CALLBACK( gtk_main_quit ), NULL );
  g_signal_connect( G_OBJECT(display_area), "draw", G_CALLBACK( on_draw_event ), NULL );
  g_signal_connect (G_OBJECT (window), "key_press_event", G_CALLBACK (on_key_press), NULL);
  g_signal_connect (G_OBJECT (window), "key_release_event", G_CALLBACK (on_key_release), NULL);
	g_signal_connect (G_OBJECT(display_area), "button_press_event", G_CALLBACK (on_mouse_press), NULL);
	g_signal_connect (G_OBJECT(display_area), "scroll_event", G_CALLBACK (on_scroll), NULL);
	g_signal_connect(G_OBJECT(window), "configure_event", G_CALLBACK(on_resize), NULL);

  /* Ask to receive events the drawing area doesn't normally
   * subscribe to. In particular, we need to ask for the
   * button press and motion notify events that want to handle.
   */
  gtk_widget_set_events (display_area, gtk_widget_get_events (display_area)
                                     | GDK_BUTTON_PRESS_MASK
																			| GDK_SCROLL_MASK
                                     | GDK_POINTER_MOTION_MASK);

	init_waterfall();
  gtk_widget_show_all( window );
	gtk_window_fullscreen(GTK_WINDOW(window));
}

/* handle the ui request and update the controls */


void set_rx_freq(int f){
	freq_hdr = f;

	set_lo(f - ((rx_list->tuned_bin * 96000)/MAX_BINS));
}

void switch_band(){
	struct field *f_freq, *f_band;
	int old_freq, freq_khz, new_freq;
	char buff[100];
	
	f_freq = get_field("r1:freq");
	f_band = get_field("#band");

	old_freq = atoi(f_freq->value);	
	if (old_freq >= 1800000 && old_freq < 2000000)
		freq_khz = old_freq - 1800000;
	else if (old_freq >= 3500000 && old_freq < 4000000)
		freq_khz = old_freq - 3500000;
	else if (old_freq >= 5500000 && old_freq < 5600000)
		freq_khz = old_freq - 5500000;
	else
		freq_khz = old_freq % 1000000; 

	if (!strcmp(f_band->value, "20M"))
		new_freq = 14000000 + freq_khz;
	else if (!strcmp(f_band->value, "15M"))
		new_freq = 21000000 + freq_khz;
	else if (!strcmp(f_band->value, "10M"))
		new_freq = 28000000 + freq_khz;
	else if (!strcmp(f_band->value, "40M"))
		new_freq = 7000000 + freq_khz;
	else if (!strcmp(f_band->value, "80M"))
		new_freq = 3500000 + freq_khz;
	else if (!strcmp(f_band->value, "160M"))
		new_freq = 1800000 + freq_khz;
	else if (!strcmp(f_band->value, "30M"))
		new_freq = 10000000 + freq_khz;
	else if (!strcmp(f_band->value, "17M"))
		new_freq = 18000000 + freq_khz;
	else if (!strcmp(f_band->value, "60M"))
		new_freq = 5500000 + freq_khz;
	else
		new_freq = old_freq;

	sprintf(f_freq->value, "%d", new_freq);
	sprintf(buff, "r1:freq=%d", new_freq);	
	update_field(f_freq);
	do_cmd(buff);
}

void do_calibration(){
	int fxtal_new, bfo_new;
	//check if the calibration has been invoked
	puts("Calibration Procedure\n"
		"Monitor the signal at TP1. It should be approximately at 10 MHz\n"
		"Use the tuning dial to set it to exactly 10 MHz\n"
		"(You can monitor it on  radio at 10.000.000 MHz and tune the main knob\n"
		" until it is exactly zero-beat\n"
		"When you are done. Press the Main tuning knob to save the setting\n"); 

	si570_freq(10000000);
	while (!read_switch(ENC2_SW)){
		int steps = enc_read(&enc_b);
		if (steps){
			fxtal += 20 * steps;
			si570_freq(10000000);
			printf("\r %ld ", (long)fxtal);
			fflush(stdout);
		}
	}	
	//debounce the encoder switch
	while(read_switch(ENC2_SW))
		delay(10);
	delay(100);

	fxtal_new = fxtal;
	printf("\nThe new Si570's calculated frequency is %ld\n", fxtal_new);
	puts("We have to measure the frequency of the second oscillator exactly\n"
	"We do this by beating it with the Si570.\n"
	"Tune the frequency until the audio pitch drops"
	" and the audio is  at perfect zero-beat.\n"
	"Press the tuning knob to confirm"); 

	set_lo(0);
	while (!read_switch(ENC2_SW)){
		int steps = enc_read(&enc_b);
		if (steps){
			bfo_freq += 10 * steps;
			set_lo(0);
			printf("\r %ld ", (long)bfo_freq);
			fflush(stdout);
		}
	}	
	//debounce the encoder switch
	while(read_switch(ENC2_SW))
		delay(10);
	delay(100);

	printf("\nBFO is at %ld\n", (long) bfo_freq);	
}

void do_cmd(char *cmd){	
	char request[1000], response[1000];
	
	strcpy(request, cmd);			//don't mangle the original, thank you
	if(!strncmp(request, "#band", 4))
		switch_band(request);
	else if (!strcmp(request, "#close"))
		gtk_window_iconify(GTK_WINDOW(window));
	else if (!strcmp(request, "#off"))
		exit(0);

	//tuning step
	else if (!strcmp(request, "#step=10KHz"))
		tuning_step = 10000;
	else if (!strcmp(request, "#step=1KHz"))
		tuning_step = 1000;
	else if (!strcmp(request, "#step=200Hz"))
		tuning_step = 200;
	else if (!strcmp(request, "#step=50Hz"))
		tuning_step = 50;

	//spectrum bandwidth
	else if (!strcmp(request, "#span=3KHz"))
		spectrum_span = 3000;
	else if (!strcmp(request, "#span=10KHz"))
		spectrum_span = 10000;
	else if (!strcmp(request, "#span=25KHz"))
		spectrum_span = 25000;
		

	//this needs to directly pass on to the sdr core
	else if(request[0] != '#')
		sdr_request(request, response);
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
    
    // if it is an empty section
    else if (strlen(section) == 0){
      sprintf(cmd, "#%s", name);
      set_field(cmd, new_value); 
    }
    return 1;
}



int main( int argc, char* argv[] ) {

	puts("sBITX v0.30");
	ui_init(argc, argv);
	wiringPiSetup();
	init_gpio_pins();
	setup();
	struct field *f;

	f = main_controls;

  /* write_gui_ini was used to dump the gui layout controls into an ini file */
  //write_gui_ini(); 

	//set the radio to some decent defaults
	do_cmd("r1:freq=7100000");
	do_cmd("r1:mode=LSB");	
	do_cmd("#step=1000");	
  do_cmd("#span=25KHZ");
	
	f = get_field("spectrum");
	update_field(f);
	set_volume(3000000);
	enc_init(&enc_a, ENC_FAST, ENC1_B, ENC1_A);
	enc_init(&enc_b, ENC_SLOW, ENC2_B, ENC2_A);

	if (argc > 1 && !strcmp(argv[1], "calibrate"))
		do_calibration();

	int e = g_timeout_add(10, ui_tick, NULL);
	printf("g_timeout_add() = %d\n", e);

	set_field("#band", "40M");
	set_field("r1:freq", "7000000");
	set_field("r1:mode", "USB");
	set_field("tx_gain", "24");
	set_field("tx_power", "100");
	set_field("r1:gain", "41");
	set_field("r1:volume", "85");

  if (ini_parse("sbitx_user_settings.ini", user_settings_handler, NULL)<0){
    printf("Unable to load sbitx_user_settings.ini\n");
  }

  cat_init();
  gtk_main();
  
  return 0;
}

