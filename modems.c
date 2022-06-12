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
#include <arpa/inet.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"

typedef float float32_t;
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
		 The demodulators call write_log() to call the routines to display the decoded text.
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


static long modem_tx_timeout = 0;
//static int get_cw_input_method() = CW_STRAIGHT;
static int modem_pitch = 700;

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
pthread_t ft8_thread;

void ft8_tx(char *message, int freq){
	char cmd[200], buff[1000];
	FILE	*pf;

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
	sprintf(ft8_tx_text, ">%s\n", message);
	printf("ft8 ready to transmit with %d samples\n", ft8_tx_nsamples);
}

void *ft8_thread_function(void *ptr){
	FILE *pf;
	char buff[1000];

	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);

		if (!ft8_do_decode)
			continue;

//		puts("Decoding FT8 on cmd line");
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
		char time_str[20];
		time(&rawtime);
		struct tm *t = gmtime(&rawtime);
		sprintf(time_str, "%02d%02d%02d", t->tm_hour, t->tm_min, t->tm_sec);

		while(fgets(buff, sizeof(buff), pf)) {
			strncpy(buff, time_str, 6);
			write_log(FONT_LOG_RX, buff);
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
	{'~', ""}, //dummy, a null character
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
	{'A', ".-.-."},
	{'K', "-.--."}
};


#define FLOAT_SCALE (1073741824.0)

int cw_period;
static struct vfo cw_tone, cw_env;
static int keydown_count=0;			//counts down the pause afer a keydown is finished
static int keyup_count = 0;			//counts down to how long a key is held down
static float cw_envelope = 1;
static int cw_pitch = 700;

char cw_text[] = " cq cq dx de vu2ese A k";
char *symbol_next = NULL;
char paddle_next = 0;


//when symbol_next is NULL, it reads the next letter from the input
static char cw_get_next_kbd_symbol(){
	char c;

	if (!symbol_next){	
		symbol_next = morse_table->code; // point to the first symbol, by default
		get_tx_data_byte(&c);

		char b[2];
		b[0]= c;
		b[1] = 0;
		write_log(FONT_LOG_TX, b);

		for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
			if (morse_table[i].c == c)
				symbol_next = morse_table[i].code;
	}

	if (!*symbol_next){ 		//send the letter seperator
		symbol_next = NULL;
		return '/';
	}
	else
		return *symbol_next++;
}

#define CW_MAX_SYMBOLS 12
char cw_key_letter[CW_MAX_SYMBOLS];

//when symbol_next is NULL, it reads the next letter from the input
float cw_get_sample(){
	float sample = 0;
	static char last_symbol = 0;

	//start new symbol, if any
	if (!keydown_count && !keyup_count){

		if (get_cw_input_method() == CW_KBD || get_cw_input_method() == CW_IAMBIC){
			char c;

			if (get_cw_input_method() == CW_KBD)
				c = cw_get_next_kbd_symbol();
			else if (get_cw_input_method() == CW_IAMBIC){
				int i = key_poll();

				if (i == 0 && last_symbol == '/')
					c = ' ';	
				else if (i == 0)
					c = '/';
				else if (last_symbol == '-' && (i & CW_DOT))
					c = '.';
				else if (last_symbol == '.' && (i & CW_DASH))
					c = '-';
				else if (i & CW_DOT)
					c = '.';
				else if (i & CW_DASH)
					c = '-';
	
				//decode iambic letters	
				if (get_cw_input_method() == CW_IAMBIC){
					int len = strlen(cw_key_letter);
					if (len < CW_MAX_SYMBOLS-1 && (c == '.' || c == '-')){
						cw_key_letter[len++] = c;
						cw_key_letter[len] = 0;	
					}		
					else if (c == ' '){
					//	printf("SP\n");
						if (get_cw_input_method() == CW_IAMBIC)
							write_log(FONT_LOG_RX, " ");
						cw_key_letter[0] = 0;
					}
					else if (c == '/'){
						//search for the letter match in cw table and emit it
						for (int i = 0; i < sizeof(morse_table)/sizeof(struct morse); i++)
							if (!strcmp(morse_table[i].code, cw_key_letter)){
								char buff[2];
								buff[0] = morse_table[i].c;
								buff[1] = 0;
								write_log(FONT_LOG_RX, buff);
								//printf("[%c]\n", morse_table[i].c);
							}
						cw_key_letter[0] = 0;
					} 
				}//end of iambic decoding
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
		}
		else if (get_cw_input_method() == CW_STRAIGHT){
			if (key_poll()){
				keydown_count = 50; //add a few samples, to debounce 
				keyup_count = 0;
			}
			else {
				keydown_count = 0;
				keyup_count = 0;
			}
		}
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

	return sample / 8;
}

void cw_init(int freq, int wpm, int keyer){
	vfo_start(&cw_env, 50, 49044); //start in the third quardrant, 270 degree
	vfo_start(&cw_tone, freq, 0);
	cw_period = (12 *9600)/ wpm; 		//as dot = 1.2/wpm
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
		printf("decoded: [%s]\n", buffer);
		if (strlen(buffer))
			write_log(FONT_LOG_RX, buffer);
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

void modem_rx(int mode, int32_t *samples, int count){
	int i, j, k, l;
	int32_t *s;
	FILE *pf;
	char buff[10000];

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
	cw_init(cw_pitch, 12, CW_IAMBIC);
	strcpy(fldigi_mode, "");

	//for now, launch fldigi in the background, if not already running
	int e = system("pidof -x fldigi > /dev/null");
	if (e == 256)
		system("fldigi -i &");
}

int modem_center_freq(int mode){
	switch(mode){
		case MODE_FT8:
			return 3000;
		case MODE_RTTY:
			return 700;
		case MODE_PSK31:
			return 500; 
		default:
			return 0;
	}
}

//change this as per mode
int modem_set_pitch(int pitch){
	modem_pitch = pitch;
}

//change the settings as per the mode
int modem_get_pitch(int pitch){
}
//this called routinely to check if we should start/stop the transmitting
//each mode has its peculiarities, like the ft8 will start only on 15th second boundary
//psk31 will transmit a few spaces after the last character, etc.

void modem_poll(int mode){
	int bytes_available = get_tx_data_length();
	int tx_is_on = is_in_tx();
	int key_status;
	time_t now;
	char buffer[1000];

	switch(mode){
	case MODE_FT8:
		now = time(NULL);
		if (now % 15 == 0){
			if(ft8_tx_nsamples > 0 && !tx_is_on){
				tx_on();	
				write_log(FONT_LOG_TX, ft8_tx_text);
			}
			if (tx_is_on && ft8_tx_nsamples == 0)
				tx_off();
		}
	break;
	case MODE_CW:
	case MODE_CWR:
		key_status = key_poll();
		if (!tx_is_on && (bytes_available || key_status) > 0){
			if (!key_status)
				write_log(FONT_LOG, "\n<tx>\n");
			tx_on();
			cw_init(700, 12, CW_IAMBIC); 
			modem_tx_timeout = millis() + get_cw_delay();
		}
		if (tx_is_on){
			if (bytes_available > 0 || key_status){
				modem_tx_timeout = millis() + get_cw_delay();
			}
			else if (modem_tx_timeout < millis()){
				tx_off();
				if (!key_status)
					write_log(FONT_LOG, "\n<rx>\n");
			}
		}
	break;

	case MODE_RTTY:
	case MODE_PSK31:
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
			sample = ft8_tx_buff[ft8_tx_buff_index/8];
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

