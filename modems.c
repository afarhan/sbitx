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
extern char mycallsign[];
extern char contact_callsign[];
extern char sent_rst[];
extern char received_rst[];
extern char sent_exchange[];
extern char received_exchange[];
extern char mygrid[];
extern char contact_grid[];

/*
	This file implements modems for :
	1. Fldigi: We use fldigi as a proxy for all the modems that it implements

	2. FT8 : uses the command line FT8 encoder/decoders from Karlis Goba

	3. CW : transmit only

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
 
	1. FT8 is not really a modem, we use a slightly modified version of the command line
	decode_ft8 and gen_ft8 programs written by Karlis Goba at https://github.com/kgoba/ft8_lib.

	2. FT8 handles short blocks of data every 15 seconds. On the 0, 15th, 30th and 45th second of 
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

/*******************************************************
**********                  FT8                  *******
********************************************************/


#define FT8_MAX_BUFF (12000 * 18) 
unsigned int wallclock = 0;
int32_t ft8_rx_buff[FT8_MAX_BUFF];
float ft8_tx_buff[FT8_MAX_BUFF];
char ft8_tx_text[128];
int ft8_rx_buff_index = 0;
int ft8_tx_buff_index = 0;
int	ft8_tx_nsamples = 0;
int ft8_do_decode = 0;
int	ft8_do_tx = 0;
int	ft8_mode = FT8_SEMI;
pthread_t ft8_thread;

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

void ft8_interpret(char *received, char *transmit){
	char first_word[100];
	char second_word[100];
	char third_word[100];
	char fourth_word[100];

	//reset the transmit buffer
	transmit[0]= 0;	

	//move past the prefixes	
	char *q, *p = received + 25;
	while (*p == ' ')
		p++;

	//read in four words, max
	q = first_word;
	for (int i =0; *p && isalnum(*p) && i < 99; i++)
		*q++ = *p++;
	*q = 0;

	while (*p == ' ')
		p++;

	q = second_word;
	for (int i =0; *p && isalnum(*p) && i < 99 && *p; i++)
		*q++ = *p++;
	*q = 0;

	while(*p == ' ')
		p++;

	q = third_word;
	for (int i =0; *p && (isalnum(*p) || *p == '+' || *p == '-') && i < 99 && *p; i++)
		*q++ = *p++;
	*q = 0;

	while(*p == ' ')
		p++;

	q = fourth_word;
	for (int i =0; *p && isalnum(*p) && i < 99 && *p; i++)
		*q++ = *p++;
	*q = 0;


	if (!strcmp(first_word, "CQ")){
		if (strlen(second_word) == 2 && strlen(fourth_word) > 0){
			strcpy(contact_callsign, third_word);
			strcpy(contact_grid, fourth_word);
		}
		else{
			strcpy(contact_callsign, second_word);
			strcpy(contact_grid, third_word);
		}

		char grid_square[10];
		strcpy(grid_square, mygrid);
		grid_square[4] = 0;
		received_rst[0] = 0;
		sprintf(transmit, "%s %s %s", contact_callsign, mycallsign, grid_square);
	}
	//this is a station that has replied/called me
	else if (!strcasecmp(first_word, mycallsign)){
		strcpy(contact_callsign, second_word);
		if (!strncmp(third_word, "R-", 2) || !strncmp(third_word, "R+", 2)){
			strcpy(received_rst, third_word + 1);
			sprintf(transmit, "%s %s RRR", contact_callsign, mycallsign);
			if (ft8_mode != 0)
				write_call_log();
		}
		else if (!strcmp(third_word, "RRR")){
			sprintf(transmit, "%s %s 73", contact_callsign, mycallsign);
			if(ft8_mode != 0)
				write_call_log();
		}
		else if (third_word[0] == '-' || third_word[0] == '+'){
			strcpy(received_rst, third_word);
			sprintf(transmit, "%s %s R-10", contact_callsign, mycallsign);
			strcpy(sent_rst, "-10");
		}
		else if (strlen(third_word) == 4){
			// this is a fresh call
			strcpy(contact_callsign, second_word);
			strcpy(contact_grid, third_word);
			sprintf(transmit, "%s %s -10", contact_callsign, mycallsign);
			strcpy(sent_rst, "-10");
			received_rst[0] = 0;
		}
	}
	else { //i have just picked a station in qso with someone else
		strcpy(contact_callsign, second_word);
		sprintf(transmit, "%s %s %s", contact_callsign, mycallsign, mygrid);
		received_rst[0] = 0;
		contact_grid[0] = 0;
	}
	redraw();
	update_log_ed();	
}

void ft8_tx(char *message, int freq){
	char cmd[200], buff[1000];
	FILE	*pf;


	for (int i = 0; i < strlen(message); i++)
		message[i] = toupper(message[i]);

	//timestamp the packets for display log
	time_t	rawtime;
	char time_str[20];
	time(&rawtime);
	struct tm *t = gmtime(&rawtime);
	sprintf(time_str, "%02d%02d%02d                   ", t->tm_hour, t->tm_min, t->tm_sec);
	write_console(FONT_LOG_TX, time_str);
	write_console(FONT_LOG_TX, message);
	write_console(FONT_LOG_TX, "\n");

	//printf("ft8 tx:[%s]\n", message);
	//generate the ft8 samples into a temporary file

	sprintf(cmd, "/home/pi/ft8_lib/gen_ft8 \"%s\" /tmp/ft_tx.wav %d", 
		message, freq);
	pf = popen(cmd, "r");
	while(fgets(buff, sizeof(buff), pf)) 
		puts(buff);
	fclose(pf);

	//read the samples into the tx buffer, set up the variables to trigger 
	//tx on the next 15th second boundary

	pf = fopen("/home/pi/sbitx/ft8tx_float.raw", "r");
	ft8_tx_nsamples = fread(ft8_tx_buff, sizeof(float), 180000, pf);
	fclose(pf);
	ft8_tx_buff_index = 0;
	//printf("ft8 ready to transmit with %d samples\n", ft8_tx_nsamples);
}

void *ft8_thread_function(void *ptr){
	FILE *pf;
	char buff[1000], mycallsign_upper[20]; //there are many ways to crash sbitx, bufferoverflow of callsigns is 1

	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);

		if (!ft8_do_decode)
			continue;

		//create a temporary file of the ft8 samples
		pf = fopen("/tmp/ftrx.raw", "w");
		fwrite(ft8_rx_buff, sizeof(ft8_rx_buff), 1, pf);
		fclose(pf);

		//let the next batch begin
		ft8_do_decode = 0;
		ft8_rx_buff_index = 0;
		
		//now launch the decoder
		pf = popen("/home/pi/ft8_lib/decode_ft8 /tmp/ftrx.raw", "r");

		//timestamp the packets
		time_t	rawtime;
		char time_str[20], response[100];
		time(&rawtime);
		struct tm *t = gmtime(&rawtime);
		sprintf(time_str, "%02d%02d%02d", t->tm_hour, t->tm_min, t->tm_sec);

		while(fgets(buff, sizeof(buff), pf)) {
			strncpy(buff, time_str, 6);
			write_console(FONT_LOG_RX, buff);

			int i;
			for (i = 0; i < strlen(mycallsign); i++)
				mycallsign_upper[i] = toupper(mycallsign[i]);
			mycallsign_upper[i] = 0;	

			//is this interesting?
			if (ft8_mode != FT8_MANUAL && strstr(buff, mycallsign_upper)){
				ft8_interpret(buff, response);
				if (ft8_mode && strlen(response))
					ft8_tx(response, get_pitch());
				else
					set_field("#text_in", response);
			}
		}
		fclose(pf);
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
		ft8_rx_buff[ft8_rx_buff_index++] = samples[i];

	int now = time(NULL);
	if (now != wallclock)	
		wallclock = now;
	else 
		return;

	// do nothing unless we are on a 15 second boundary
	if (wallclock % 15)
		return;

	//we should have atleast 12 seconds of samples to decode
	if (ft8_rx_buff_index >= 14 * 12000)
		ft8_do_decode = 1;

	ft8_rx_buff_index = 0;	
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
	{'=', "-...-"},
	{'\'', "--..--"},
};


#define FLOAT_SCALE (1073741824.0)

int cw_period;
static struct vfo cw_tone, cw_env;
static int keydown_count=0;			//counts down the pause afer a keydown is finished
static int keyup_count = 0;			//counts down to how long a key is held down
static float cw_envelope = 1;
static int cw_pitch = 700;
static int cw_tx_until = 0;
static int data_tx_until = 0;

char cw_text[] = " cq cq dx de vu2ese A k";
char *symbol_next = NULL;
char paddle_next = 0;


//when symbol_next is NULL, it reads the next letter from the input
static char cw_get_next_kbd_symbol(){
	char c;

	if (!symbol_next){	
		if(!get_tx_data_byte(&c)){
			return 0;
		}
		symbol_next = morse_table->code; // point to the first symbol, by default

		char b[2];
		b[0]= c;
		b[1] = 0;
//		write_console(FONT_LOG_TX, b);

		for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
			if (morse_table[i].c == tolower(c))
				symbol_next = morse_table[i].code;
	}

	if (!*symbol_next){ 		//send the letter seperator
		symbol_next = NULL;
//		printf("symbol_next set to NULL, returning / \n");
		return '/';
	}
	else{
//		printf("cw_kbd_read returning %c\n", *symbol_next);
		return *symbol_next++;
	}
}

#define CW_MAX_SYMBOLS 12
char cw_key_letter[CW_MAX_SYMBOLS];
static int symbol_memory = 0;


//when symbol_next is NULL, it reads the next letter from the input
float cw_get_sample(){
	float sample = 0;
	static char last_symbol = 0;


	//start new symbol, if any
	if (!keydown_count && !keyup_count){

		if (cw_tone.freq_hz != get_cw_tx_pitch())
			vfo_start(&cw_tone, get_cw_tx_pitch(), 0);

		if (get_cw_input_method() == CW_KBD || get_cw_input_method() == CW_IAMBIC){
			char c, key;
			cw_period = (12 *9600)/ get_wpm(); 		//as dot = 1.2/wpm

			//check if we have a symbol coming in from the keyboard
			c = cw_get_next_kbd_symbol();
			key = key_poll();

			if ((key || symbol_memory) && !c){
			
				if (last_symbol == '-' && (symbol_memory & CW_DOT)){
					c = '.';	
				}
				else if (last_symbol == '.' && (symbol_memory & CW_DASH)){
					c = '-'; 
				} 
				else if ((last_symbol == '/' || last_symbol == ' ') && (symbol_memory & CW_DOT)){
					c = '.';
				}
				else if ((last_symbol == '/' || last_symbol == ' ') && (symbol_memory & CW_DASH)){
					c = '-';
				}
				else if (key == 0 && last_symbol == '/'){
					c = ' ';	
				}
				else if (key == 0){
					c = '/';
				}
				else if (last_symbol == '-' && (key & CW_DOT)){
					c = '.';
				}
				else if (last_symbol == '.' && (key & CW_DASH)){
					c = '-';
				}
				else if (key & CW_DOT){
					c = '.';
				}
				else if (key & CW_DASH){
					c = '-';
				}

				symbol_memory = 0; //clean out the last symbol memory
			}
			else if (!c){ //no activty on the keyer and no symbol from the keyboard either
				if (last_symbol == '/'){
					 c = ' ';
				}
				else {
					c = '/';
				}
			}

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
				write_console(FONT_LOG_TX, " ");
				cw_key_letter[0] = 0;
			}
			else if (last_symbol == '/'){
				//search for the letter match in cw table and emit it
				for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
					if (strlen(cw_key_letter) && !strcmp(morse_table[i].code, cw_key_letter)){
						char buff[2];
						buff[0] = morse_table[i].c;
						buff[1] = 0;
						write_console(FONT_LOG_TX, buff);
					}
				cw_key_letter[0] = 0;
			} 
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

		//we are converrting the keyup and keydwon counts to millis
		if (keydown_count){
			cw_tx_until = millis() + ((keydown_count * 1000)/96000) + get_cw_delay();
		}
	}
	else if ((!(keydown_count + keyup_count) & 0xFF) && get_cw_input_method() == CW_STRAIGHT){
		if (key_poll())
			keydown_count += 1000;
	}
	//infrequently poll to see if the keyer has sent a new symbol while we were stil txing the last symbol
	else if ((keydown_count > 0 || keyup_count) > 0 && !((keyup_count + keydown_count) & 0xFF) 
			&& get_cw_input_method() == CW_IAMBIC){
		char key = key_poll();
		if (last_symbol == '-' && (key & CW_DOT))
				symbol_memory = CW_DOT;
		if (last_symbol == '.' && (key & CW_DASH))
			symbol_memory = CW_DASH;
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

//	printf("keydown %d keyup %d \n", keydown_count, keyup_count);
	sample = (vfo_read(&cw_tone)/FLOAT_SCALE) * cw_envelope;

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
		if (strlen(buffer))
			write_console(FONT_LOG_RX, buffer);
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
		write_console(FONT_LOG_TX, buff);
	}
}
	
void modem_set_pitch(int pitch){
	char response[1000];

	if(fldigi_call_i("modem.set_carrier", pitch, response))
		puts("fldigi modem.set_carrier error");
}

int last_pitch = 0;
void modem_rx(int mode, int32_t *samples, int count){
	int i, j, k, l;
	int32_t *s;
	FILE *pf;
	char buff[10000];

	if (get_pitch() !+ last_pitch)
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
	cw_init();
	strcpy(fldigi_mode, "");

	//for now, launch fldigi in the background, if not already running
	int e = system("pidof -x fldigi > /dev/null");
	if (e == 256)
		system("fldigi -i &");
}

void modem_abort(){
	ft8_tx_nsamples = 0;
}

//this called routinely to check if we should start/stop the transmitting
//each mode has its peculiarities, like the ft8 will start only on 15th second boundary
//psk31 will transmit a few spaces after the last character, etc.

void modem_poll(int mode){
	int bytes_available = get_tx_data_length();
	int tx_is_on = is_in_tx();
	int key_status;
	time_t now;
	char buffer[10000];

	if (current_mode != mode){
		//flush out the past decodes
		current_mode = mode;
		int l;
		do{
			fldigi_call("rx.get_data", "", buffer);	
			l = strlen(buffer);
		}while(l > 0);

		//clear the text buffer	
		clear_tx_text_buffer();

		if (current_mode == MODE_FT8)
			macro_load("ft8");
		else if (current_mode == MODE_RTTY || current_mode == MODE_PSK31 ||
			MODE_CWR || MODE_CW)
			macro_load("cw1");	
		modem_set_pitch(get_pitch());
	}

	switch(mode){
	case MODE_FT8:
		now = time(NULL);
		if (now % 15 == 0){
			if(ft8_tx_nsamples > 0 && !tx_is_on){
//				puts("Starting ft8 tx");
				tx_on();	
			}
			if (tx_is_on && ft8_tx_nsamples == 0)
				tx_off();
		}
	break;
	case MODE_CW:
	case MODE_CWR:
		key_status = key_poll();
		if (!tx_is_on && (bytes_available || key_status) > 0){
			puts("switching cw on");
			tx_on();
			cw_init();
			symbol_memory = key_status;
			cw_tx_until = millis() + get_cw_delay();; //at least for 200 msec to  begin with
		}
		else if (tx_is_on && cw_tx_until < millis()){
				tx_off();
		}
	break;

	case MODE_RTTY:
	case MODE_PSK31:
/*
		if (!tx_is_on && bytes_available && !fldigi_in_tx){
			if (!fldigi_call("main.tx", "", buffer)){
				tx_on();
				fldigi_in_tx = 1;	
				sound_input(1);
				data_tx_until = millis() + get_data_delay();
				printf("TX start %d:data_tx_until = %d vs millis = %d\n", __LINE__, data_tx_until, millis());
			}
			else
				write_console(FONT_LOG, "\n*fldigi failed to start RX\n");
		}
		else if (fldigi_in_tx && data_tx_until < millis()){
			printf("RX start %d:data_tx_until = %d vs millis = %d\n", __LINE__, data_tx_until, millis());
			if (!fldigi_call("main.rx", "", buffer)){
				fldigi_in_tx = 0;
				sound_input(0);
				tx_off();
			}
			else
				write_console(FONT_LOG, "\n*fldigi failed to start RX\n");
		}
		else if (fldigi_in_tx && bytes_available > 0){
			printf("tx %d:data_tx_until = %d vs millis = %d\n", __LINE__, data_tx_until, millis());
			fldigi_tx_more_data();
			data_tx_until = millis() + get_data_delay();
		}
		else if (fldigi_in_tx){
			printf("%d:data_tx_until = %d vs millis = %d\n", __LINE__, data_tx_until, millis());
			//see if we can time-out
			fldigi_call("tx.get_data", "", buffer);
			if (strlen(buffer) > 0){
					data_tx_until = millis() + get_data_delay();
					printf("### len = %d\n%s\n###\n", strlen(buffer), buffer); 
			}
		}
		else if (!tx_is_on)
			fldigi_read();
*/
		//we will let the keyboard decide this
		if (tx_is_on && !fldigi_in_tx){
			if (!fldigi_call("main.tx", "", buffer)){
				fldigi_in_tx = 1;	
				sound_input(1);
			}
			else
				puts("*fldigi tx failed");
		}	
		else if (!tx_is_on && fldigi_in_tx){
			if (!fldigi_call("main.rx", "", buffer)){
				fldigi_in_tx = 0;
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

