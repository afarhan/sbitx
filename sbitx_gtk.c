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
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <cairo.h>
#include <wiringPi.h>

/* Front Panel controls */
char pins[15] = {0, 2, 3, 6, 7, 
								10, 11, 12, 13, 14, 
								21, 22, 23, 25, 27};

#define ENC1_A (0)
#define ENC1_B (2)
#define ENC1_SW (3)

#define ENC2_A (12)
#define ENC2_B (13)
#define ENC2_SW (14)

#define SW1 (6)
#define SW2 (10)
#define SW3 (11)
#define SW4 (7)
#define SW5 (22)
#define PTT (21)

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

char output_pins[] = {
	TX_LINE, BAND_SELECT	
};

static int xit = 512; 

GtkWidget *display_area = NULL;
int screen_width, screen_height;

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

float palette[][3] = {
	{1,1,1},
	{0,1,1},
	{0.5,0.5,0.5},
	{1,1,1},
	{0,0,0},
	{1,1,0},
	{1,0,1},
	//spectrum
	{0,0,0},	//SPECTRUM_BACKGROUND
	{0.25, 0.25, 0.25}, //SPECTRUM_GRID
	{1,1,0},	//SPECTRUM_PLOT
	{1,1,1}, 	//SPECTRUM_NEEDLE
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
	{FONT_FIELD_LABEL, 0, 1, 1, "Mono", 12, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FIELD_VALUE, 1, 1, 1, "Mono", 12, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_FIELD, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_VALUE, 0, 1, 1, "Arial", 20, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL, 0, 1, 1, "Mono", 10, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
};
/*
	Warning: The field selection is used for TOGGLE and SELECTION fields
	each selection by the '/' should be unique. otherwise, the simple logic will
	get confused 

	the fields are referred implicitly by their index into the mains control table.
	This maybe suboptimal, but it does its job for the time being.
*/

#define R1FREQ_CONTROL 1
#define R1MODE_CONTROL 2
#define BAND_CONTROL 3
#define R1GAIN_CONROL 4

#define FREQUENCY_DISPLAY 0 
#define SPECTRUM_CONTROL 7 
#define WATERFALL_CONTROL 9 

#define MAX_MAIN_CONTROLS 7 

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

struct field main_controls[] = {
	{ "r1:freq=", 10, 10, 130, 35, "", 5, "14000000", FIELD_NUMBER, FONT_LARGE_VALUE, "", 500000, 21500000, 100},
	{ "r1:mode=", 10, 235, 80, 20, "Mode", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, "USB/LSB/CW/CWR", 0,0, 0},
	{  "band=", 10, 260, 80, 20, "Band", 40, "80M", FIELD_SELECTION, FONT_FIELD_VALUE, "160M/80M/60M/40M/30M/20M/17M/15M", 0,0, 0},
	{ "r1:gain=", 10, 285, 80, 20, "Vol", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 100, 1},
	{ "r1:b1=", 10, 310, 80, 20, "R1", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, "", 0, 1024, 1},
	{ "rit=", 10, 335, 80, 20, "RIT", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, "ON/OFF", 0,0, 0},
	{ "quit", 10, 360, 80, 20, "Quit", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0, 0},
	{ "cmd", 10, 385, 80, 20, "Cmd:", 40, "", FIELD_TEXT, FONT_FIELD_VALUE, "", 0, 40, 0},

	
	/* beyond MAX_MAIN_CONROLS are the static text display */
	//{8, "tune", 100,100, 100,20, "", 40, "14.000.000", FIELD_STATIC, FONT_LARGE_VALUE, "", 0,0,0},
	{"spectrum", 150, 10 , 512, 100, "Spectrum ", 70, "7050 KHz", FIELD_STATIC, FONT_SMALL, "", 0,0,0},   
	{"waterfall", 150, 130 , 512, 100, "Waterfall ", 70, "7050 KHz", FIELD_STATIC, FONT_SMALL, "", 0,0,0},
	{"", 0, 0 ,0, 0, "end ", 1, "", FIELD_STATIC, FONT_SMALL, "", 0,0,0},
};

//static int field_in_focus = -1;
//static int field_in_hover = 0;

static struct field *f_focus = NULL;
static struct field *f_hover = main_controls;

void do_cmd(char *cmd_string);

static int measure_text(cairo_t *gfx, char *text){
	cairo_text_extents_t ext;
	
	cairo_select_font_face(gfx, "Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(gfx, field_font_size);
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
static int waterfall_depth = 30;
static int  *wf;

void init_waterfall(){
	int needed = (MAX_BINS/2) * waterfall_depth * sizeof(int);
	wf = malloc((MAX_BINS/2) * waterfall_depth * sizeof(int));
	if (!wf){
		puts("*Error: malloc failed on waterfall buffer");
		exit(0);
	}
	puts("setting the wf to zero");
	memset(wf, 0, (MAX_BINS/2) * waterfall_depth * sizeof(int));
}

void draw_waterfall(cairo_t *gfx){
	int i,j, step, amplitude;	
	float x,y,x_scale;
	struct field *f;
	int *p = wf;

	f = get_field("waterfall");

	//now, draw them!!!
	step = (MAX_BINS/2);
	x_scale = (1.0 * f->width )/(MAX_BINS/2);
	y = f->y;
	
	for (j = 0; j < waterfall_depth; j++){
		amplitude = 0;
		for (i = 0; i < MAX_BINS/2; i++){
			amplitude += *p++;
			if (i % step == 0){
  			cairo_set_source_rgb(gfx, amplitude, 1, 1);
				cairo_rectangle(gfx, x, y, 1, 1);
  			cairo_fill(gfx);
				x++;
				amplitude = 0;
			}
		}
		y++;
	} 

	//make space for the next line of waterfall
	memmove(wf + (MAX_BINS/2), wf, sizeof(int) * (MAX_BINS/2) * (waterfall_depth -1));
}

void draw_spectrum(cairo_t *gfx){
	int x, y, sub_division, i, wf_index;
	float x_scale = 0.0;
	struct field *f;

	f = get_field("spectrum");
	sub_division = f->width / 10;
	
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	for (i =  0; i <= f->height; i += f->height/10){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + f->height); 
	}
	cairo_stroke(gfx);

	//we only plot the second half of the bins (on the lower sideband
	x_scale = (1.0 * f->width )/(MAX_BINS/2);
	int last_y = 100;
	wf_index = 0;

	cairo_set_source_rgb(gfx, palette[SPECTRUM_PLOT][0], 
		palette[SPECTRUM_PLOT][1], palette[SPECTRUM_PLOT][2]);
	cairo_move_to(gfx, f->x, f->y + f->height);

	for (i = MAX_BINS/2; i < MAX_BINS; i++){
		int x, y;

		// the center fft bin is at zero, from MAX_BINS/2 onwards,
		// the bins are at lowest frequency (-ve frequency)
		int offset = i;
		offset = (offset - MAX_BINS/2);
		x = (x_scale * offset);
		y = power2dB(cnrmf(fft_bins[i])) + waterfall_offset; 
		if ( y <  0)
			y = 0;
		if (y > 100)
			y = 100;
		wf[wf_index++] = y;
		y = 100-y;
		cairo_line_to(gfx, f->x + x, f->y + y);
	}
	cairo_stroke(gfx);

	
	//draw the needle
	for (struct rx *r = rx_list; r; r = r->next){

		int needle_x  = (f->width*(MAX_BINS/2 - r->tuned_bin))/(MAX_BINS/2);
		//int needle_x =  bin2display(r->tuned_bin);

		fill_rect(gfx, f->x + needle_x, f->y, 1, f->height,  SPECTRUM_NEEDLE);
	}
/*
	draw_waterfall();
*/
}

void draw_field(cairo_t *gfx, struct field *f){
	struct font_style *s = font_table + 0;
	if (!strcmp(f->cmd, "spectrum")){
		draw_spectrum(gfx);
		return;
	}
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
		
	fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
	draw_text(gfx, f->x+1, f->y+2 ,  f->label, FONT_FIELD_LABEL);
	draw_text(gfx, f->x + f->label_width, f->y+2 , f->value , f->font_index);
}

void redraw_main_screen(cairo_t *gfx){
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
			draw_field(gfx, main_controls + i);
		}
	}
	draw_spectrum(gfx);
}

/* gtk specific routines */
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, gpointer user_data ) {
	redraw_main_screen(cr);	
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
		printf("focs lost\n");
	update_field(f_hover);
}

static void edit_field(struct field *f, int action){
	//struct field *f = main_controls + field_index;
	int v;

	printf("mask %d\n", key_modifier);
	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(f->value);
		if (action == MIN_KEY_UP && v + f->step <= f->max){
			if (key_modifier && !strcmp(f->cmd, "r1:freq="))
				v += f->step * 20;
			else
				v += f->step;
		}
		else if (action == MIN_KEY_DOWN && v - f->step >= f->min){
			if (key_modifier && !strcmp(f->cmd, "r1:freq="))
				v -= f->step * 20;
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
	sprintf(buff, "%s%s", f->cmd, f->value);
	do_cmd(buff);
	update_field(f);
}

static int in_tx = 0;
static void tx_on(){
	char response[100];

	if (in_tx == 0){
		digitalWrite(TX_LINE, HIGH);
		sdr_request("tx:on", response);	
		in_tx = 1;
		puts("tx on");
	}
}

static void tx_off(){
	char response[100];

	if (in_tx == 1){
		digitalWrite(TX_LINE, LOW);
		sdr_request("tx:off", response);	
		in_tx = 0;
		puts("tx off");
	}
}

static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	key_modifier = 0;
/*
	if (event->keyval == MIN_KEY_TAB)
		tx_off();
*/
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
	
	printf("Scroll = %d\n", event->direction);
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
		for (int i = 0; i < MAX_MAIN_CONTROLS; i++){
			f = main_controls + i;
			if (f->x < event->x && event->x < f->x + f->width 
					&& f->y < event->y && event->y < f->y + f->height)
				focus_field(f);
			/*else 
				focus_field(-1);	*/
		}
	}
/*
  if (event->button == GDK_BUTTON_PRIMARY)
    {
      draw_brush (widget, event->x, event->y);
    }
  else if (event->button == GDK_BUTTON_SECONDARY)
    {
      clear_surface ();
      gtk_widget_queue_draw (widget);
    }
*/
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
	pinMode(BAND_SELECT, OUTPUT);
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


gboolean ui_tick(gpointer gook){
	int static ticks = 0;

	ticks++;
	if (ticks >= 10){
		struct field *f = get_field("spectrum");

		update_field(f);	//move this each time the spectrum watefall index is moved
		redraw_flag = 0;
		ticks = 0;
	}

	// check the tuning knob
	struct field *f = get_field("r1:freq=");
	int tuning = enc_read(&enc_b);
	if (tuning < 0)
		edit_field(f, MIN_KEY_DOWN);	
	else if (tuning > 0)
		edit_field(f, MIN_KEY_UP);

	//check the push-to-talk
	if (digitalRead(PTT) == LOW && in_tx == 0)
		tx_on();	
	else if (digitalRead(PTT) == HIGH && in_tx == 1)
		tx_off();
	
	return TRUE;
}

void ui_init(int argc, char *argv[]){
  GtkWidget *window;
  
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

  gtk_widget_show_all( window );
	init_waterfall();
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
	
	f_freq = get_field("r1:freq=");
	f_band = get_field("band=");

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
	else if (!strcmp(f_band->value, "40M"))
		new_freq = 7000000 + freq_khz;
	else if (!strcmp(f_band->value, "160M"))
		new_freq = 1800000 + freq_khz;
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
	if(!strncmp(request, "r1:", 3))
		sdr_request(request, response);
	else if(!strncmp(request, "band=", 5))
		switch_band(request);
}


int main( int argc, char* argv[] ) {

	puts("sBITX v0.30");
	ui_init(argc, argv);
	wiringPiSetup();
	setup();
	struct field *f;

	f = main_controls;
	sprintf(f->value, "14293000");
	f = get_field("spectrum");
	update_field(f);
	set_rx_freq(14294300);
	set_volume(3000000);
	init_gpio_pins();
	enc_init(&enc_a, ENC_SLOW, ENC1_A, ENC1_B);
	enc_init(&enc_b, ENC_FAST, ENC2_A, ENC2_B);

	if (argc > 1 && !strcmp(argv[1], "calibrate"))
		do_calibration();

	int e = g_timeout_add(10, ui_tick, NULL);
	printf("g_timeout_add() = %d\n", e);
  gtk_main();

  return 0;
}

