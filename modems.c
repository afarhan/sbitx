#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"

typedef float float32_t;
extern char contact_callsign[];
extern char sent_rst[];
extern char received_rst[];
extern char sent_exchange[];
extern char received_exchange[];
extern char contact_grid[];

//when running
#define QSO_STATE_ZOMBIE 0

#define QSO_STATE_CQ_CALLING 6
#define QSO_STATE_CQ_REPORT 2 
#define QSO_STATE_CQ_RRR 4

//when search & pounding
#define QSO_STATE_REPLY_GRID 1
#define QSO_STATE_REPLY_REPORT 3

#define QSO_STATE_73 5   //we are done anyway

int qso_state = QSO_STATE_ZOMBIE;

/*
	This file implements modems for :
	1. Fldigi: We use fldigi as a proxy for all the modems that it implements

	2. CW : transmit only

	General:

	There are three functions called to implement almost all the digital modes
	1. There is the modem_init() that is used to initialize all the different modems.
	2. The modem_poll() is called about 10 times a second to check if any transmit/receiver
		 changeover is needed, etc.
	3. On receive, each time a block of samples is received, modem_rx() is called and 
		 it despatches the block of samples to the currently selected modem. 
		 The demodulators call write_console() to call the routines to display the decoded text.
	4. During transmit, modem_next_sample() is repeatedly called by the sdr to accumulate
		 samples. In turn the sample generation routines call get_tx_data_byte() to read the next
		 text/ascii byte to encode.

	The strangness of FT8:
 
	1. FT8 handles short blocks of data every 15 seconds. On the 0, 15th, 30th and 45th second of 
	the minute. It assumes that the clock is synced to the GMT's second and minute ticks.

	3. The ft8_rx() routine accumulates samples until it has enough samples and it is time to 
	decode. decoding can stall the CPU for considerable time, hence, it does takes the kludgy route
	of writing all the samples to a temporary file /tmp/ft8rx.raw and then turning on the 
	ft8_do_decode flag. A separate thread, waiting for the flag runs the decoder that outputs each
	decode into a separate line.

	4. For transmit, the text block received by ft8_tx() is encoded quickly (as encoding is much
	simpler and quicker code) into a temporary file at /tmp/ft8tx.raw. On the next 15th second
	boundary, this file is read and returned as samples to the SDR. Once done, the temp file
	is deleted.


*/


static int current_mode = -1;
static unsigned long millis_now = 0;
static int bytes_available = 0;

/*******************************************************
**********                  FT8                  *******
********************************************************/


#define FT8_MAX_BUFF (12000 * 18) 
unsigned int wallclock = 0;
int32_t ft8_rx_buff[FT8_MAX_BUFF];
float ft8_rx_buffer[FT8_MAX_BUFF];
float ft8_tx_buff[FT8_MAX_BUFF];
char ft8_tx_text[128];
int ft8_rx_buff_index = 0;
int ft8_tx_buff_index = 0;
int	ft8_tx_nsamples = 0;
int ft8_do_decode = 0;
int	ft8_do_tx = 0;
int	ft8_pitch = 0;
int	ft8_mode = FT8_SEMI;
pthread_t ft8_thread;
int ft8_tx1st = 1;
int ft8_repeat = 5;

int sbitx_ft8_encode(char *message, int32_t freq,  float *signal, bool is_ft4);

void ft8_setmode(int config){
	switch(config){
		case FT8_MANUAL:
			ft8_mode = FT8_MANUAL;
			write_console(FONT_LOG, "FT8 is manual now.\nSend messages through the keyboard\n");
			break;
		case FT8_SEMI:
			write_console(FONT_LOG, "FT8 is semi-automatic.\nClick on the callsign to start the QSO\n");
			ft8_mode = FT8_SEMI;
			break;
		case FT8_AUTO:
			write_console(FONT_LOG, "FT8 is automatic.\nIt will call CQ and QSO with the first reply.\n");
			ft8_mode = FT8_AUTO;
			break;
	}
}

int sbitx_ft8_encode(char *message, int32_t freq,  float *signal, bool is_ft4);

void ft8_queue_tx(){
	char buff[1000];
	//timestamp the packets for display log
	time_t	rawtime = time_sbitx();
	struct tm *t = gmtime(&rawtime);

  sprintf(buff, "%02d%02d%02d 99 +00 %04d ~ %s\n", t->tm_hour, t->tm_min, t->tm_sec, ft8_pitch, ft8_tx_text);
	write_console(FONT_FT8_TX, buff);

	printf("ft8 encoding : [%s]\n", ft8_tx_text);
	ft8_tx_nsamples = sbitx_ft8_encode(ft8_tx_text, ft8_pitch, ft8_tx_buff, false); 
	ft8_tx_buff_index = 0;
}

void ft8_tx(char *message, int freq){
	char cmd[200], buff[1000];
	FILE	*pf;
	time_t	rawtime = time_sbitx();
	struct tm *t = gmtime(&rawtime);

	for (int i = 0; i < strlen(message); i++)
		message[i] = toupper(message[i]);
	strcpy(ft8_tx_text, message);

	ft8_pitch = freq;
  sprintf(buff, "%02d%02d%02d 99 +00 %04d ~ %s\n", t->tm_hour, t->tm_min, t->tm_sec, ft8_pitch, ft8_tx_text);
	write_console(FONT_FT8_QUEUED, buff);
//	ft8_queue_tx(message, freq);

	//also set the times of transmission
	char str_tx1st[10], str_repeat[10];
	get_field_value_by_label("FT8_TX1ST", str_tx1st);
	get_field_value_by_label("FT8_REPEAT", str_repeat);
	if (!strcmp(str_tx1st, "ON"))
		ft8_tx1st = 1;
	else
		ft8_tx1st = 0;

	//no repeat for '73'
	int msg_length = strlen(message);
	if (msg_length > 3 && !strcmp(message + msg_length - 3, " 73")){
		printf("Sending a single 73 [%s]\n", message);
		ft8_repeat = 1;
	} 
	else
		ft8_repeat = atoi(str_repeat);
}

int sbitx_ft8_decode(float *signal, int num_samples, bool is_ft8);

void *ft8_thread_function(void *ptr){
	FILE *pf;
	char buff[1000], mycallsign_upper[20]; //there are many ways to crash sbitx, bufferoverflow of callsigns is 1

	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);

		if (!ft8_do_decode)
			continue;

		sbitx_ft8_decode(ft8_rx_buffer, ft8_rx_buff_index, true);
		//let the next batch begin
		ft8_do_decode = 0;
		ft8_rx_buff_index = 0;
	}
}

// the ft8 sampling is at 12000, the incoming samples are at
// 96000 samples/sec
void ft8_rx(int32_t *samples, int count){

	int decimation_ratio = 96000/12000;

	//if there is an overflow, then reset to the begining
	if (ft8_rx_buff_index + (count/decimation_ratio) >= FT8_MAX_BUFF){
		ft8_rx_buff_index = 0;		
	}

	//down convert to 12000 Hz sampling rate
	for (int i = 0; i < count; i += decimation_ratio)
		//ft8_rx_buff[ft8_rx_buff_index++] = samples[i];
		ft8_rx_buffer[ft8_rx_buff_index++] = samples[i] / 200000000.0f;

	int now = time_sbitx();
	if (now != wallclock)	
		wallclock = now;
	else 
		return;

	if (wallclock % 15)
		return;

	//we should have atleast 12 seconds of samples to decode
	if (ft8_rx_buff_index >= 12 * 12000)
		ft8_do_decode = 1;

}

void ft8_poll(int seconds, int tx_is_on){

	if (tx_is_on){
		//tx off should nto abort repeats, when called from here
		int ft8_repeat_save = ft8_repeat;
		if (ft8_tx_nsamples == 0){
			tx_off();
			ft8_repeat = ft8_repeat_save;
		}
		return;
	}

	if (seconds != 45 && seconds != 15 && seconds != 0 && seconds != 30)
		return;	

	if (ft8_repeat > 0){
		if (((seconds == 45 || seconds == 15) && !ft8_tx1st)
		|| (seconds == 0 || seconds == 30) && ft8_tx1st){
			printf("Ft8 transmiting with tx1st:%d at %d seconds\n", ft8_tx1st, seconds);
			tx_on(TX_SOFT);
			ft8_queue_tx();
			ft8_repeat--;
		}
	} 
}
/*******************************************************
**********           CW routines                 *******
********************************************************/

struct morse {
	char c;
	char *code;
};

struct morse morse_table[] = {
	{'~', " "}, //dummy, a null character
	{' ', " "},
	{'a', ".-"},
	{'b', "-..."},
	{'c', "-.-."},
	{'d', "-.."},
	{'e', "."},
	{'f', "..-."},
	{'g', "--."},
	{'h', "...."},
	{'i', ".."},
	{'j', ".---"},
	{'k', "-.-"},
	{'l', ".-.."},
	{'m', "--"},
	{'n', "-."},
	{'o', "---"},
	{'p', ".--."},
	{'q', "--.-"},
	{'r', ".-."},
	{'s', "..."},
	{'t', "-"},
	{'u', "..-"},
	{'v', "...-"},
	{'w', ".--"},
	{'x', "-..-"},
	{'y', "-.--"},
	{'z', "--.."},
	{'1', ".----"},
	{'2', "..---"},
	{'3', "...--"},
	{'4', "....-"},
	{'5', "....."},
	{'6', "_...."},
	{'7', "--..."},
	{'8', "---.."},
	{'9', "----."},
	{'0', "-----"},
	{'.', ".-.-.-"},
	{',', "--..--"},
	{'?', "..--.."},
	{'/', "-..-."},
	{' ', " "},
	{'[', ".-.-."},
	{']', ".-..."},
	{'+', ".-.-."},
	{'&', "-...-"},
	{'\'', "--..--"},
};


#define FLOAT_SCALE (1073741824.0)

static int cw_key_state = 0;
static int cw_period;
static struct vfo cw_tone, cw_env;
static int keydown_count=0;			//counts down pause afer a keydown is finished
static int keyup_count = 0;			//counts down how long a key is held down
static float cw_envelope = 1;		//used to shape the envelope
static int cw_tx_until = 0;			//delay switching to rx, expect more txing
static int data_tx_until = 0;

static char *symbol_next = NULL;
//char paddle_next = 0;
pthread_t iambic_thread;
char iambic_symbol[4];
char cw_symbol_prev = ' ';

/*
	HOW MORSE SENDING WORKS
	The routine cw_get_sample() is called for each audio sample being being
	transmitted. It has to be quick, without loops and with no device I/O.
	The cw_get_sample() is also called from the thread that does DSP, so stalling
	it is dangerous.

	The modem_poll is called about 10 to 20 times a second from the 'user interface'
	thread. The key is physically read from the GPIO by calling key_poll() through
	modem_poll and the value is stored as cw_key_state.

	The cw_get_sample just reads this cw_key_state instead of polling the GPIO.
	The logic of the cw keyer is taken from Richard Chapman, KC41FB's paper
	in QEX of September, 2009.

	the cw_read_key() routine returns the next dash/dot/space/, etc to be sent
	the word 'symbol' is used to denote a dot, dash, a gaps that are dot, dash or
	word long. These are defined in sdr.h (they shouldn't be) 
	
*/


static uint8_t cw_current_symbol = CW_IDLE;
static uint8_t cw_next_symbol = CW_IDLE;
static uint8_t cw_last_symbol = CW_IDLE;
static uint8_t cw_mode = CW_STRAIGHT;

#define CW_MAX_SYMBOLS 12
char cw_key_letter[CW_MAX_SYMBOLS];

//the of morse code needs to translate into CW_DOT, CW_DASH, etc
uint8_t cw_get_next_symbol(){

	if (!symbol_next)
		return CW_IDLE;
	
	uint8_t s = *symbol_next++;

	switch(s){
		case '.': 
			return CW_DOT;
		case '-': 
			return  CW_DASH;
		case 0:
			symbol_next = NULL; //we are at the end of the string
			return CW_DASH_DELAY;
		case '/': 
			return CW_DASH_DELAY;
		case ' ': 
			return  CW_WORD_DELAY;
	}
	return CW_IDLE;
}


// cw_read_key() routine is called 96000 times a second
//it can't poll gpio lines or text input, those are done in modem_poll()
//and we only read the status from the variable updated by modem_poll()

int cw_read_key(){
	char c;

	//preferance to the keyer activity
	if (cw_key_state != CW_IDLE) {
		//return cw_key_state;
		cw_key_state = key_poll();
		return cw_key_state;
	}

	if (cw_current_symbol != CW_IDLE)
		return CW_IDLE;

	//we are still sending the previously typed character..
	if (symbol_next){
		uint8_t s = cw_get_next_symbol();
		return s;
	}

	//return if a symbol is being transmitted
	if (bytes_available == 0)
		return CW_IDLE;

	get_tx_data_byte(&c);

	symbol_next = morse_table->code; // point to the first symbol, by default

	for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
		if (morse_table[i].c == tolower(c))
			symbol_next = morse_table[i].code;
	if (symbol_next)
		return cw_get_next_symbol(); 
	else
		return CW_IDLE;
}

float cw_get_sample_new(){
	float sample = 0;

	// for now, updatw time and cw pitch
	if (!keydown_count && !keyup_count){
		millis_now = millis();
		if (cw_tone.freq_hz != get_pitch())
			(&cw_tone, get_pitch(), 0);
	}

	uint8_t symbol_now = cw_read_key();

	switch(cw_current_symbol){
	case CW_IDLE:		//this is the start case 
		if (symbol_now == CW_DOWN){
			keydown_count = 2000; //add a few samples, to debounce 
			keyup_count = 0;
			cw_current_symbol = CW_DOWN;
		}
		else if (symbol_now & CW_DOT){
			keydown_count = cw_period;
			keyup_count = cw_period;
			cw_current_symbol = CW_DOT;
			cw_last_symbol = CW_IDLE;
		}
		else if (symbol_now & CW_DASH){
			keydown_count = cw_period * 3;
			keyup_count = cw_period;
			cw_current_symbol = CW_DASH;
			cw_last_symbol = CW_IDLE;
		}
		else if (symbol_now & CW_DASH_DELAY){
			keydown_count = 0;
			keyup_count = cw_period * 3;
			cw_current_symbol = CW_DOT_DELAY;
		}
		else if (symbol_now & CW_WORD_DELAY){
			keydown_count = 0;
			keyup_count = cw_period * 6;
			cw_current_symbol = CW_DOT_DELAY;
		}
		//else just continue in CW_IDLE
		break;
	case CW_DOWN:		//the straight key
		if (symbol_now == CW_DOWN){ //continue, keep up the good work
			keydown_count = 2000;
			keyup_count = 0;
		}
		else{ // ok, break it up
			keydown_count = 0;
			keyup_count = 0;
			cw_current_symbol = CW_IDLE;//go back to idle
		}
		break;
	case CW_DOT:
		if ((symbol_now & CW_DASH) && cw_next_symbol == CW_IDLE){
			cw_next_symbol = CW_DASH;	
		}
		if (keydown_count == 0){
			keyup_count = cw_period;
			cw_last_symbol = CW_DOT;
			cw_current_symbol = CW_DOT_DELAY;
		}
		break;
	case CW_DASH:
		if ((symbol_now & CW_DOT) && cw_next_symbol == CW_IDLE){
			cw_next_symbol = CW_DOT;	
		}
		if (keydown_count == 0){
			keyup_count = cw_period;
			cw_last_symbol = CW_DASH;
			cw_current_symbol = CW_DOT_DELAY;
		}
		break;
	case CW_DASH_DELAY:
	case CW_WORD_DELAY:
	case CW_DOT_DELAY:
		if (keyup_count == 0){
			cw_current_symbol = cw_next_symbol;
			if (cw_current_symbol == CW_DOT){
				keydown_count = cw_period;
				keyup_count = cw_period;
			}
			if (cw_current_symbol == CW_DASH){
				keydown_count = cw_period * 3;
				keyup_count = cw_period;
			}
			cw_last_symbol = CW_DOT_DELAY;
			cw_next_symbol = CW_IDLE;
		}
		if (cw_mode == CW_IAMBICB){
			if (cw_next_symbol == CW_IDLE && cw_last_symbol == CW_DOT && (symbol_now & CW_DASH)){
				cw_next_symbol = CW_DASH;
			}
			if (cw_next_symbol == CW_IDLE && cw_last_symbol == CW_DASH && (symbol_now & CW_DOT)){
				cw_next_symbol = CW_DASH;
			}
		}
		break;
	}

	// shape the cw keying
	if (keydown_count  > 0){
		if(cw_envelope < 0.999)
			cw_envelope = ((vfo_read(&cw_env)/FLOAT_SCALE) + 1)/2; 
			keydown_count--;
	}
	else { //keydown_count is zero
		if(cw_envelope > 0.001)
			cw_envelope = ((vfo_read(&cw_env)/FLOAT_SCALE) + 1)/2; 
		if (keyup_count > 0)
			keyup_count--;
	}

	sample = (vfo_read(&cw_tone)/FLOAT_SCALE) * cw_envelope;

	if (keyup_count > 0 || keydown_count > 0){
		cw_tx_until = millis_now + get_cw_delay(); 
	}
	return sample / 8;
}

void cw_init(){
	//init cw with some reasonable values
	vfo_start(&cw_env, 50, 49044); //start in the third quardrant, 270 degree
	vfo_start(&cw_tone, 700, 0);
	cw_period = 9600; 		// At 96ksps, 0.1sec = 1 dot at 12wpm
	cw_key_letter[0] = 0;
	keydown_count = 0;
	keyup_count = 0;
	cw_envelope = 0;
}


/* ---- Base64 Encoding/Decoding Table --- */
char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* decodeblock - decode 4 '6-bit' characters into 3 8-bit binary bytes */
void decodeblock(unsigned char in[], char *clrstr) {
  unsigned char out[4];
  out[0] = in[0] << 2 | in[1] >> 4;
  out[1] = in[1] << 4 | in[2] >> 2;
  out[2] = in[2] << 6 | in[3] >> 0;
  out[3] = '\0';
  strncat(clrstr, out, sizeof(out));
}

void b64_decode(char *b64src, char *clrdst) {
  int c, phase, i;
  unsigned char in[4];
  char *p;

  clrdst[0] = '\0';
  phase = 0; i=0;
  while(b64src[i]) {
    c = (int) b64src[i];
    if(c == '=') {
      decodeblock(in, clrdst); 
      break;
    }
    p = strchr(b64, c);
    if(p) {
      in[phase] = p - b64;
      phase = (phase + 1) % 4;
      if(phase == 0) {
        decodeblock(in, clrdst);
        in[0]=in[1]=in[2]=in[3]=0;
      }
    }
    i++;
  }
}

/* encodeblock - encode 3 8-bit binary bytes as 4 '6-bit' characters */
void encodeblock( unsigned char in[], char b64str[], int len ) {
    unsigned char out[5];
    out[0] = b64[ in[0] >> 2 ];
    out[1] = b64[ ((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4) ];
    out[2] = (unsigned char) (len > 1 ? b64[ ((in[1] & 0x0f) << 2) |
             ((in[2] & 0xc0) >> 6) ] : '=');
    out[3] = (unsigned char) (len > 2 ? b64[ in[2] & 0x3f ] : '=');
    out[4] = '\0';
    strncat(b64str, out, sizeof(out));
}

/* encode - base64 encode a stream, adding padding if needed */
void b64_encode(char *clrstr, char *b64dst) {
  unsigned char in[3];
  int i, len = 0;
  int j = 0;

  b64dst[0] = '\0';
  while(clrstr[j]) {
    len = 0;
    for(i=0; i<3; i++) {
     in[i] = (unsigned char) clrstr[j];
     if(clrstr[j]) {
        len++; j++;
      }
      else in[i] = 0;
    }
    if( len ) {
      encodeblock( in, b64dst, len );
    }
  }
}

/*******************************************************
**********           FLDIGI xml                  *******
********************************************************/
/*
An almost trivial xml, just enough to work fldigi
*/

char fldigi_mode[100];
long fldigi_retry_at = 0;
/*
An almost trivial xml, just enough to work fldigi
*/

int fldigi_call_i(char *action, int param, char *result){
  char buffer[10000], q[10000], xml[1000];
  struct sockaddr_in serverAddr;
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(7362);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  
	
	int fldigi_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fldigi_socket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
		//puts("unable to connect to the flidigi\n");        
		close(fldigi_socket);
		return -1;
   }

	sprintf(xml, 
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<methodCall><methodName>%s</methodName>\n"
"<params>\n<param><value><i4>%d</i4></value></param> </params></methodCall>\n",
		action, param);

	sprintf(q, 
		"POST / HTTP/1.1\n"
		"Host: 127.0.0.1:7362\n"
		"User-Agent: sbitx/v0.01\n" 
		"Accept:\n" 
		"Content-Length: %d\n"
		"Content-Type: application/x-www-form-urlencoded\n\n"
		"%s\n",
		(int)strlen(xml), xml);
 
  if (send(fldigi_socket, q, strlen(q), 0) < 0) {
		puts("Unable to request fldigi");
		close(fldigi_socket);
		return -1;
  } 
	char buff[10000]; //large, large
	int e = recv(fldigi_socket, buff, sizeof(buff), 0);
	if(e < 0) {
		puts("Unable to recv from fldigi");
		close(fldigi_socket);
		return -1;
  }
	buff[e] = 0;
	result[0] = 0;

	//now check if we got the data in base64
	char *p = strstr(buff, "<base64>");
	if (p){
		p += strlen("<base64"); //skip past the tag
		char *r =  strchr(p, '<'); //find the end
		if (r){
			*r = 0; //terminate the base64 at the end tag
			int len =  (strlen(p) * 6)/8;
			b64_decode(p, result);
			result[len] = 0;
		}
	}
	else
		strcpy(result, buff);
  close(fldigi_socket);

	return 0;
}

int fldigi_call(char *action, char *param, char *result){
  char buffer[10000], q[10000], xml[1000];
  struct sockaddr_in serverAddr;
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(7362);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  
	
	int fldigi_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fldigi_socket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
		//puts("unable to connect to the flidigi\n");        
		close(fldigi_socket);
		return -1;
   }

	sprintf(xml, 
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<methodCall><methodName>%s</methodName>\n"
"<params>\n<param><value><string>%s</string></value></param> </params></methodCall>\n",
		action, param);

	sprintf(q, 
		"POST / HTTP/1.1\n"
		"Host: 127.0.0.1:7362\n"
		"User-Agent: sbitx/v0.01\n" 
		"Accept:\n" 
		"Content-Length: %d\n"
		"Content-Type: application/x-www-form-urlencoded\n\n"
		"%s\n",
		(int)strlen(xml), xml);
 
  if (send(fldigi_socket, q, strlen(q), 0) < 0) {
		puts("Unable to request fldigi");
		close(fldigi_socket);
		return -1;
  } 
	char buff[10000]; //large, large
	int e = recv(fldigi_socket, buff, sizeof(buff), 0);
	if(e < 0) {
		puts("Unable to recv from fldigi");
		close(fldigi_socket);
		return -1;
  }
	buff[e] = 0;
	result[0] = 0;

	//now check if we got the data in base64
	char *p = strstr(buff, "<base64>");
	if (p){
		p += strlen("<base64>"); //skip past the tag
		char *r =  strchr(p, '<'); //find the end
		if (r){
			*r = 0; //terminate the base64 at the end tag
			int len =  (strlen(p) * 6)/8;
			b64_decode(p, result);
			result[len] = 0;
		}
	}
	//maybe it is not base64 encoded
	else if (p = strstr(buff, "<value>")){
		p += strlen("<value>");
		char *r = strchr(p, '<');
		if (r){
			*r = 0;
			strcpy(result, p);
		} 
	}
	else
		strcpy(result, buff);
  close(fldigi_socket);

	return 0;
}


/*******************************************************
**********      Modem dispatch routines          *******
********************************************************/
int fldigi_in_tx = 0;
static int rx_poll_count = 0;
static int sps, deci, s_timer ;


void fldigi_read(){
	char buffer[10000];

	//poll only every 250msec
	if (fldigi_retry_at > millis())
		return;

	if(!fldigi_call("rx.get_data", "", buffer)){		
		if (strlen(buffer)){
				if (buffer[0] != '<' )
					buffer[1] = 0;
				write_console(FONT_FLDIGI_RX, buffer);
//				printf("fldigi rx.get_data{%s}\n", buffer);
		}
	}
	fldigi_retry_at = millis() + 250;
}

void fldigi_set_mode(char *mode){
	char buffer[1000];
	if (strcmp(fldigi_mode, mode)){
		if(fldigi_call("modem.set_by_name", mode, buffer) == 0){
			fldigi_retry_at = millis() + 2000;
			strcpy(fldigi_mode, mode);
		}
	}
}

void fldigi_tx_more_data(){
	char c;
	if (get_tx_data_byte(&c)){
		char buff[10], resp[1000];
		buff[0] = c;
		buff[1] = 0;
		fldigi_call("text.add_tx", buff, resp);
		write_console(FONT_FLDIGI_TX, buff);
	}
}
	
void modem_set_pitch(int pitch){
	char response[1000];

	fldigi_call_i("modem.set_carrier", pitch, response);
//		puts("fldigi modem.set_carrier error");
}


int last_pitch = 0;
void modem_rx(int mode, int32_t *samples, int count){
	int i, j, k, l;
	int32_t *s;
	FILE *pf;
	char buff[10000];

	if (get_pitch() != last_pitch  
		&& (mode == MODE_CW || mode == MODE_CWR || MODE_RTTY || MODE_PSK31))
		modem_set_pitch(get_pitch());

	s = samples;
	switch(mode){
	case MODE_FT8:
		ft8_rx(samples, count);
		break;
	case MODE_RTTY:
		fldigi_set_mode("RTTY");
		fldigi_read();
		break;
	case MODE_PSK31:
		fldigi_set_mode("BPSK31");
		fldigi_read();
		break;
	case MODE_CW:
	case MODE_CWR:
		fldigi_set_mode("CW");
		modem_set_pitch(get_pitch());
		fldigi_read();
		break;
	}
}

void modem_init(){
	// init the ft8
	ft8_rx_buff_index = 0;
	ft8_tx_buff_index = 0;
	ft8_tx_nsamples = 0;
	pthread_create( &ft8_thread, NULL, ft8_thread_function, (void*)NULL);
//	pthread_create( &iambic_thread, NULL, iambic_thread_function, (void*)NULL);
	cw_init();
	strcpy(fldigi_mode, "");

	//for now, launch fldigi in the background, if not already running
	int e = system("pidof -x fldigi > /dev/null");
	if (e == 256)
		system("fldigi -i &");

}


//this called routinely to check if we should start/stop the transmitting
//each mode has its peculiarities, like the ft8 will start only on 15th second boundary
//psk31 will transmit a few spaces after the last character, etc.

void modem_poll(int mode){
	int tx_is_on = is_in_tx();
	time_t t;
	char buffer[10000];

	millis_now = millis();
	bytes_available = get_tx_data_length();

	if (current_mode != mode){
		//flush out the past decodes
		current_mode = mode;
		int l;
		do{
			fldigi_call("rx.get_data", "", buffer);	
			l = strlen(buffer);
		}while(l > 0);

		//clear the text buffer	
		abort_tx();

		if (current_mode == MODE_FT8)
			macro_load("ft8", NULL);
		else if (current_mode == MODE_RTTY || current_mode == MODE_PSK31 ||
			MODE_CWR || MODE_CW){
			macro_load("cw1", NULL);	
			modem_set_pitch(get_pitch());
		}

		if (current_mode == MODE_CW || current_mode == MODE_CWR)
			cw_init();
	}

	switch(mode){
	case MODE_FT8:
		t = time_sbitx();
		ft8_poll(t % 60, tx_is_on);
	break;
	case MODE_CW:
	case MODE_CWR:	
		cw_key_state = key_poll();
		if (!tx_is_on && (bytes_available || cw_key_state || (symbol_next && *symbol_next)) > 0){
			tx_on(TX_SOFT);
			millis_now = millis();
			cw_tx_until = get_cw_delay() + millis_now;
			cw_mode = get_cw_input_method();
		}
		else if (tx_is_on && cw_tx_until < millis_now){
				tx_off();
		}
		//printf("%d cw current %d\n", __LINE__, cw_current_symbol); 
	break;

	case MODE_RTTY:
	case MODE_PSK31:
		fldigi_call("main.get_trx_state", "", buffer);
		//we will let the keyboard decide this
		if (tx_is_on && !fldigi_in_tx){
			if (!fldigi_call("main.tx", "", buffer)){
				fldigi_in_tx = 1;	
				sound_input(1);
			}
			else
				puts("*fldigi tx failed");
		}	
		//switch to rx if the sbitx is set to manual or the fldigi has gone back to rx 
		else if ((tx_is_on && !strcmp(buffer, "RX")) || (!tx_is_on && fldigi_in_tx)){
			if (!fldigi_call("main.rx", "", buffer)){
				fldigi_in_tx = 0;
				tx_off();
				sound_input(0);
			}
			else
				puts("*fldigi rx failed");
		}
		if (tx_is_on && bytes_available > 0)
			fldigi_tx_more_data();	
		else 
			fldigi_read();		

	break; 
	}
}

float modem_next_sample(int mode){
	float sample=0;

	switch(mode){
		// the ft8 samples are generated at 12ksps, we need to feed the 
		// sdr with 96 ksps (eight times as much)
	case MODE_FT8: 
		if (ft8_tx_buff_index/8 < ft8_tx_nsamples){
			sample = ft8_tx_buff[ft8_tx_buff_index/8]/7;
			ft8_tx_buff_index++;
		}
		else //stop transmitting ft8 
			ft8_tx_nsamples = 0;
		break;
	case MODE_CW:
	case MODE_CWR:
		sample = cw_get_sample_new();
		cw_period = (12 *9600)/ get_wpm(); 		//as dot = 1.2/wpm
		break;
	}
	return sample;
}


void modem_abort(){
	ft8_tx_nsamples = 0;
	ft8_repeat = 0;
}
