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
#include "modem_ft8.h"
#include "modem_cw.h"

typedef float float32_t;

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
	Fldigi: We use fldigi as a proxy for all the modems that it implements


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

*/


static int current_mode = -1;
static unsigned long millis_now = 0;

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
	struct timeval timeout;

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(7362);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  
	
	int fldigi_socket = socket(AF_INET, SOCK_STREAM, 0);
/*	timeout.tv_sec = 0;
	timeout.tv_usec = 1000;
	setsockopt(fldigi_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fldigi_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
*/
	*result = 0; //start with a null string so it is returned if nothing is read

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
//		puts("Unable to recv from fldigi");
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
	struct timeval timeout;

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(7362);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  
	*result = 0;
	
	int fldigi_socket = socket(AF_INET, SOCK_STREAM, 0);
/*	timeout.tv_sec = 0;
	timeout.tv_usec = 500;
	setsockopt(fldigi_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fldigi_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
*/
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
//		puts("Unable to recv from fldigi");
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

static int fldigi_tx_stop(){	
	char buffer[10000];

	if (!fldigi_call("main.rx", "", buffer)){
		fldigi_in_tx = 0;
		sound_input(0);
		return 0;
	}
	else
		return -1;
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
		cw_rx(samples, count);
		break;
	}
}

void modem_init(){
	// init the ft8
	cw_init();
	ft8_init();
	strcpy(fldigi_mode, "");

/*
	//for now, launch fldigi in the background, if not already running
	int e = system("pidof -x fldigi > /dev/null");
	if (e == 256)
		system("fldigi -i 2>/dev/null &");
*/
}


//this called routinely to check if we should start/stop the transmitting
//each mode has its peculiarities, like the ft8 will start only on 15th second boundary
//psk31 will transmit a few spaces after the last character, etc.

void modem_poll(int mode){
	int tx_is_on = is_in_tx();
	time_t t;
	char buffer[10000];

	millis_now = millis();
	int bytes_available = get_tx_data_length();

	if (current_mode != mode){
		//flush out the past decodes
		current_mode = mode;
		int l;
		do{
			int e = fldigi_call("rx.get_data", "", buffer);	
			l = strlen(buffer);
		}while(l > 0);

		//clear the text buffer	
		abort_tx();

		if (current_mode == MODE_FT8)
			macro_load("FT8", NULL);
		else if (current_mode == MODE_RTTY || current_mode == MODE_PSK31 ||
			MODE_CWR || MODE_CW){
			macro_load("CW1", NULL);	
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
		cw_poll(bytes_available, tx_is_on);
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
			if (fldigi_tx_stop() == -1)
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
			sample = ft8_next_sample();
		break;
	case MODE_CW:
	case MODE_CWR:
		sample = cw_tx_get_sample();
		break;
	}
	return sample;
}


void modem_abort(){
	char c;	

	//flush the buffer
	while(get_tx_data_byte(&c))
		NULL;	

	switch(current_mode){
	case MODE_FT8:
		ft8_abort();
		break;
	case MODE_RTTY:
	case MODE_PSK31:
		fldigi_tx_stop();
		break;
	case MODE_CW:
	case MODE_CWR:
		cw_abort();
		break;
	}
}
