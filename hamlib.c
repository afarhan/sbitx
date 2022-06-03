#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <complex.h>
#include <math.h>
#include <fcntl.h>
#include <complex.h>
#include <fftw3.h>
#include "sdr.h"
#include "sdr_ui.h"

static int welcome_socket = -1, data_socket = -1;
#define MAX_DATA 1000
char incoming_data[MAX_DATA];
int incoming_ptr;

void  hamlib_tx(int tx_on);

//copied from gqrx on github
static char dump_state_response[] =
        /* rigctl protocol version */
        "0\n"
        /* rigctl model */
        "2\n"
        /* ITU region */
        "1\n"
        /* RX/TX frequency ranges
         * start, end, modes, low_power, high_power, vfo, ant
         *  start/end - Start/End frequency [Hz]
         *  modes - Bit field of RIG_MODE's (AM|AMS|CW|CWR|USB|LSB|FM|WFM)
         *  low_power/high_power - Lower/Higher RF power in mW,
         *                         -1 for no power (ie. rx list)
         *  vfo - VFO list equipped with this range (RIG_VFO_A)
         *  ant - Antenna list equipped with this range, 0 means all
         *  FIXME: limits can be gets from receiver::get_rf_range()
         */
        "100000 30000000 0x2ef -1 -1 0x1 0x0\n"
        /* End of RX frequency ranges. */
        "0 0 0 0 0 0 0\n"
        /* End of TX frequency ranges. The Gqrx is receiver only. */
        "0 0 0 0 0 0 0\n"
        /* Tuning steps: modes, tuning_step */
        "0xef 1\n"
        "0xef 0\n"
        /* End of tuning steps */
        "0 0\n"
        /* Filter sizes: modes, width
         * FIXME: filter can be gets from filter_preset_table
         */
        "0x82 500\n"    /* CW | CWR normal */
        "0x82 200\n"    /* CW | CWR narrow */
        "0x82 2000\n"   /* CW | CWR wide */
        "0x221 5000\n"  /* AM | AMS | FM narrow */
        "0x0c 2700\n"   /* SSB normal */
        /* End of filter sizes  */
        "0 0\n"
        /* max_rit  */
        "0\n"
        /* max_xit */
        "0\n"
        /* max_ifshift */
        "0\n"
        /* Announces (bit field list) */
        "0\n" /* RIG_ANN_NONE */
        /* Preamp list in dB, 0 terminated */
        "0\n"
        /* Attenuator list in dB, 0 terminated */
        "0\n"
        /* Bit field list of get functions */
        "0\n" /* RIG_FUNC_NONE */
        /* Bit field list of set functions */
        "0\n" /* RIG_FUNC_NONE */
        /* Bit field list of get level */
        "0x40000020\n" /* RIG_LEVEL_SQL | RIG_LEVEL_STRENGTH */
        /* Bit field list of set level */
        "0x20\n"       /* RIG_LEVEL_SQL */
        /* Bit field list of get parm */
        "0\n" /* RIG_PARM_NONE */
        /* Bit field list of set parm */
        "0\n" /* RIG_PARM_NONE */;

int check_cmd(char *cmd, char *token){
  if (strstr(cmd, token) == cmd)
    return 1;
  else
    return 0;
}

void send_response(char *response){
  send(data_socket, response, strlen(response), 0);
	printf(" %s]\n", response); 
}

int in_tx = 0;
void send_freq(){
  char response[20];
  sprintf(response, "%d\n", get_freq());
  send_response(response);
}

void hamlib_set_freq(char *f){
	long freq;
	char cmd[50];
  if (!strncmp(f, "VFO", 3))
    freq = atoi(f+5);
  else
    freq = atoi(f);
  send_response("RPRT 0\n");
	sprintf(cmd, "r1:freq=%ld", freq);
	do_cmd(cmd);
	//set_freq(freq);
}
	

void tx_control(int s){
	//printf("tx_control(%d)\n", s);
  if (s == 1){
    in_tx = 1;
    hamlib_tx(in_tx);
  }
  if (s == 0){
    in_tx = 0;
    hamlib_tx(in_tx);
  }
  if (s != -1)
    send_response("RPRT 0\n");
  if (in_tx == 1)
    send_response("1\n");
  else
    send_response("0\n");
}

void interpret_command(char *cmd){
 
  if (check_cmd(cmd, "\\chk_vfo"))
    send_response("CHKVFO 1\n"); 
  else if (check_cmd(cmd, "\\dump_state"))
    send_response(dump_state_response);
  else if (check_cmd(cmd, "V"))
    send_response("VFOA\n");
  else if (check_cmd(cmd, "v"))
    send_response("VFOA\n");
	else if (!strcmp(cmd, "m VFOA"))
		send_response("USB\n3000\n");
  else if (!strncmp(cmd, "m VFOA", 6))
    send_response("USB\n3000\n");
  else if (check_cmd(cmd, "f"))
		send_freq();
  else if (check_cmd(cmd, "F"))
    hamlib_set_freq(cmd + 2);
  else if (cmd[0] == 'T'){
		if (strchr(cmd, '0'))
			tx_control(0); //if there is a zero in it, we are to rx
		else
			tx_control(1); //this is a shaky way to do it, who has the time to parse?
/*    if (!strcmp(cmd, "T 0") || !strcmp(cmd, "T VFOA 0"))
      tx_control(0);
    else if (!strcmp(cmd, "T 1") || !strcmp(cmd, "T VFOA 1"))
      tx_control(1);
*/
  }
  else if (check_cmd(cmd, "s"))
    send_response("0\n");
  else if (check_cmd(cmd, "t"))
    tx_control(-1);
  else if (check_cmd(cmd, "q")){
    close(data_socket);
    data_socket = -1;
  }
	else 
		printf("Hamlib: Unrecognized command [%s] '%c'\n", cmd, cmd[0]);
}

void hamlib_handler(char *data, int len){

	printf("<<<hamlib cmd %s =>", data);
  for (int i = 0; i < len; i++){
    if (data[i] == '\n'){
      incoming_data[incoming_ptr] = 0;
      incoming_ptr = 0;
      interpret_command(incoming_data);
    }
    else if (incoming_ptr < MAX_DATA){
      incoming_data[incoming_ptr] = data[i]; 
      incoming_ptr++;
    }
  }
}

void hamlib_start(){
  char buffer[MAX_DATA];
  struct sockaddr_in serverAddr;
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;

  welcome_socket = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(4532);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  

  /*---- Bind the address struct to the socket ----*/
  bind(welcome_socket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));

  /*---- Listen on the socket, with 5 max connection requests queued ----*/
  if(listen(welcome_socket,5)!=0)
    printf("hamlib listen() Error\n");
  incoming_ptr = 0;
}

void hamlib_slice(){
  struct sockaddr_storage server_storage;
  socklen_t addr_size;
  int e, len;
  char buffer[1024];

  if (data_socket == -1){
    addr_size = sizeof server_storage;
    e = accept(welcome_socket, (struct sockaddr *) &server_storage, &addr_size);
    if (e == -1)
      return;
    puts("Accepted connection\n");
    incoming_ptr = 0;
    data_socket = e;
    fcntl(data_socket, F_SETFL, fcntl(data_socket, F_GETFL) | O_NONBLOCK);
  }
  else { 
    len = recv(data_socket, buffer, sizeof(buffer), 0);
    if (len >= 0){
      buffer[len] = 0;
      hamlib_handler(buffer, len);
    }
    else {
      //e = errno();
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      //for other errors, just close the socket
			puts("Hamlib connection dropped. Restarting to listen ..."); 
      close(data_socket);
      data_socket = -1;      
    }
  } 
}
/*
int main(){
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;
  char buffer[1024];
  int len;

  puts("Starting server\n");
  hamlib_start();
  puts("Server started\n");
  while (1){
    hamlib_slice();
    usleep(1000);
  }
  return 0;
}
*/

