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

void ft8_tx(char *message, int freq){
	char cmd[200], buff[1000];
	FILE	*pf;

	for (int i = 0; i < strlen(message); i++)
		message[i] = toupper(message[i]);

	//timestamp the packets for display log
	time_t	rawtime = time_sbitx();
	struct tm *t = gmtime(&rawtime);

  sprintf(buff, "%02d%02d%02d 99 +00 %04d ~ %s\n", t->tm_hour, t->tm_min, t->tm_sec, get_cw_tx_pitch(), message);
	write_console(FONT_FT8_TX, buff);

	ft8_pitch = freq;

	ft8_tx_nsamples = sbitx_ft8_encode(message, ft8_pitch, ft8_tx_buff, false); 
	
	ft8_tx_buff_index = 0;
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

int cw_period;
static struct vfo cw_tone, cw_env;
static int keydown_count=0;			//counts down the pause afer a keydown is finished
static int keyup_count = 0;			//counts down to how long a key is held down
static float cw_envelope = 1;
static int cw_tx_until = 0;
static int data_tx_until = 0;

static char *symbol_next = NULL;
//char paddle_next = 0;
pthread_t iambic_thread;
char iambic_symbol[4];
char cw_symbol_prev = ' ';

void queue_cw_symbol(char c){
	iambic_symbol[0] = c;
	iambic_symbol[1] = 0;
	symbol_next = iambic_symbol;
	printf("%ld queued [%c]\n", millis(), c);
	cw_symbol_prev = c;
}

void *iambic_thread_function(void *ptr){
	int paddle;
	int paddle_prev = 0;
	char insert = 0;

	while(1){
		if (((current_mode != MODE_CW) && (current_mode != MODE_CWR))
			|| get_cw_input_method() != CW_IAMBIC){
			usleep(200000); //sleep longer
			continue;
		}
		else
			usleep(5000); // 5msec
	
		paddle = key_poll();

		//if we are starting afresh and the previous symbol has been swallowed up
		if (!keydown_count && keyup_count < cw_period/4 && symbol_next != iambic_symbol){
			if (insert){
				queue_cw_symbol(insert);
				insert = 0;
			}
			//if both are pressed
			else if ((paddle & CW_DOT) && (paddle & CW_DASH)){
				//alternate with previous symbol
				if (cw_symbol_prev == '-'){
					queue_cw_symbol('.');
printf("%d iambic\n", __LINE__);
				}
				else if (cw_symbol_prev == '.'){
					queue_cw_symbol('-');
printf("%d iambic\n", __LINE__);
				}
			}
			else if(paddle & CW_DOT){
				queue_cw_symbol('.');
printf("%d iambic\n", __LINE__);
			}
			else if (paddle & CW_DASH){
			queue_cw_symbol('-');
printf("%d iambic\n", __LINE__);
			}
			else if (cw_symbol_prev != '/' && cw_symbol_prev != ' '){
			queue_cw_symbol('/');
printf("%d iambic\n", __LINE__);
			}
/*			else if (cw_symbol_prev == '/'){
				queue_cw_symbol(' ');
printf("%d iambic\n", __LINE__);
			}
*/

		}
		// if the paddle went from one press to squeeze
		if (!insert && paddle == (CW_DASH | CW_DOT) && paddle_prev != paddle){
			if (!(paddle_prev & CW_DOT)){
				insert = '.';
printf("%d iambic\n", __LINE__);
			}
			if (!(paddle_prev & CW_DASH)){
				insert = '-';
printf("%d iambic\n", __LINE__);
			}
		}		
		paddle_prev = paddle;
			
	} // end of while loop
}

//when symbol_next is NULL, it reads the next letter from the input
static char cw_get_next_symbol(){
	char c;

	if (!symbol_next){	
		if(!get_tx_data_byte(&c)){
			return 0;
		}
		symbol_next = morse_table->code; // point to the first symbol, by default

		char b[2];
		b[0]= c;
		b[1] = 0;

		for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
			if (morse_table[i].c == tolower(c))
				symbol_next = morse_table[i].code;
	}

	if (!*symbol_next){ 		//send the letter seperator
		symbol_next = NULL;
		return '/';
	}
	printf("next_symbol [%c]\n", *symbol_next); 
	return *symbol_next++;
}


#define CW_MAX_SYMBOLS 12
char cw_key_letter[CW_MAX_SYMBOLS];

float cw_get_sample(){
	float sample = 0;
	static char last_symbol = 0;


	//start new symbol, if any
	if (!keydown_count && !keyup_count){

		millis_now = millis();

		if (cw_tone.freq_hz != get_pitch())
			vfo_start(&cw_tone, get_pitch(), 0);

		//first check if there is a character pending in ascii
		char c = cw_get_next_symbol(); //c holds dot, dash, space, word-space
		if(c){	
			printf("got [%c]\n", c);
			//set the cw speed
			cw_period = (12 *9600)/ get_wpm(); 		//as dot = 1.2/wpm

			switch(c){
			case '.':
				keydown_count = cw_period;
				keyup_count = cw_period;
				last_symbol = '.';
				break;
			case '-':
				keydown_count = cw_period * 3;
				keyup_count = cw_period;
				last_symbol = '-';
				break;
			case '/':
				keydown_count = 0;
				keyup_count = cw_period * 2; // 1 (passed) + 2 
				last_symbol = '/';
				break;
			case ' ':
				keydown_count = 0;
				keyup_count = cw_period * 2; //3 periods already passed 
				last_symbol = ' ';
				break;
			}
//			printf("%d: set timing with down %d, up %d\n", __LINE__, keydown_count, keyup_count);
		}
		else if (get_cw_input_method() == CW_STRAIGHT){
			if (key_poll()){
				keydown_count = 2000; //add a few samples, to debounce 
				keyup_count = 0;
			}
			else {
				keydown_count = 0;
				keyup_count = 0;
			}
		}
		
		//by now we have a dot/dash/etc to send 
		switch(c){
			case '.':
				keydown_count = cw_period;
				keyup_count = cw_period;
				last_symbol = '.';

				break;
			case '-':
				keydown_count = cw_period * 3;
				keyup_count = cw_period;
				last_symbol = '-';
				break;
			case '/':
				keydown_count = 0;
				keyup_count = cw_period * 2; // 1 (passed) + 2 
				last_symbol = '/';
				break;
			case ' ':
				keydown_count = 0;
				keyup_count = cw_period * 2; //3 periods already passed 
				last_symbol = ' ';
				break;
		}

		//decode iambic letters	
		int len = strlen(cw_key_letter);
		if (len < CW_MAX_SYMBOLS-1 && (c == '.' || c == '-')){
			cw_key_letter[len++] = c;
			cw_key_letter[len] = 0;	
		}		
		else if (last_symbol  == ' '){
			write_console(FONT_CW_TX, " ");
			cw_key_letter[0] = 0;
		}
		else if (last_symbol == '/'){
			//search for the letter match in cw table and emit it
			for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
				if (strlen(cw_key_letter) && !strcmp(morse_table[i].code, cw_key_letter)){
					char buff[2];
					buff[0] = morse_table[i].c;
					buff[1] = 0;
					write_console(FONT_CW_TX, buff);
				}
			cw_key_letter[0] = 0;
		} 

		//printf("%d:%d sending output '%c', last_symbol '%c', down %d, up %d\n", __LINE__, millis_now, 
		//	c, last_symbol, keydown_count, keyup_count);
	}
	
	if (keydown_count && cw_envelope < 0.999)
		cw_envelope = ((vfo_read(&cw_env)/FLOAT_SCALE) + 1)/2; 
	if (keydown_count == 0 && cw_envelope > 0.001)
		cw_envelope = ((vfo_read(&cw_env)/FLOAT_SCALE) + 1)/2; 

	if (keydown_count > 0)
		keydown_count--;	

	//start the keyup counter only after keydown is finished
	if (keyup_count > 0 && keydown_count == 0)
		keyup_count--;

	sample = (vfo_read(&cw_tone)/FLOAT_SCALE) * cw_envelope;

	if (cw_envelope >  0.01 && cw_tx_until < millis_now + keydown_count/96 + keyup_count/96){
		cw_tx_until = millis_now + get_cw_delay() + 
			keydown_count/96 + keyup_count/96;
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
	pthread_create( &iambic_thread, NULL, iambic_thread_function, (void*)NULL);
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
	int bytes_available = get_tx_data_length();
	int tx_is_on = is_in_tx();
	int key_status;
	time_t t;
	char buffer[10000];

	millis_now = millis();

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
		if ((t % 15) == 0){
			if(ft8_tx_nsamples > 0 && !tx_is_on){
				tx_on(TX_SOFT);	
			}
			if (tx_is_on && ft8_tx_nsamples == 0)
				tx_off();
		}
	break;
	case MODE_CW:
	case MODE_CWR:	
		key_status = key_poll();
		if (!tx_is_on && (bytes_available || key_status || (symbol_next && *symbol_next)) > 0){
			tx_on(TX_SOFT);
			millis_now = millis();
			cw_tx_until = get_cw_delay() + millis_now;
		}
		else if (tx_is_on && cw_tx_until < millis_now){
				tx_off();
		}
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
		sample = cw_get_sample();
		break;
	}
	return sample;
}


void modem_abort(){
	ft8_tx_nsamples = 0;
}
