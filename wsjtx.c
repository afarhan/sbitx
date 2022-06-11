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

static int udp_socket = -1;

void write_log(char *line);

void wsjtx_send(char *response){
    send(udp_socket, response, strlen(response), 0);
}

void wsjtx_start(){
  struct sockaddr_in serverAddr;
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;

  udp_socket = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
  
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(2237);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  

  /*---- Bind the address struct to the socket ----*/
  int e = bind(udp_socket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
}

void wsjtx_slice(){
  struct sockaddr_in addr;
  int e, len;
  char buffer[1024];

	len = sizeof(buffer);
  e = recvfrom(udp_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&addr, &len);
	if (e < 0)
		return;

	int32_t *px;
	px = (int32_t *) buffer;

	int32_t magic, schema, id, l, i;
	int32_t hours, minutes, seconds, snr, delta_freq;
	char utf[1000], message[2000], mode[1000], unique[1000], log[1000];

	magic = ntohl(*px++);
	schema = ntohl(*px++);
	id = ntohl(*px++);
	
	//printf("\nMsg: %d\n", id);
	switch(id){
		case 0:
			//printf("Hearbeat!\n");
			break;
		case 1:
			//printf("Status\n");
			break;
		case 2:
			// count the bytes of the unique string
			l = ntohl(*px++);
			char *pt = (char *)px;
			for (i = 0; i < l; i++)
				unique[i] = *pt++;
			unique[i] = 0;
			//printf("Unique : %s\n", unique);
			//printf("New: %d\n", (int)(*pt));
			pt++;
			px = (int32_t *)pt;
			int32_t mds = ntohl(*px++);
			mds = mds/1000; //reduce to only seconds, drop millisecs
			seconds = mds % 60;
			hours = mds / 3600;
			minutes = (mds / 60) % 60;
			//printf("Time: %d:%d:%d\n", hours, minutes, seconds) ;
			snr = ntohl(*px++);
			//printf("SNR: %d\n", snr);
			//printf("delta time:%g\n", (double *)px);
			px++;px++;
			delta_freq = ntohl(*px++); 
			//printf("delta freq:%u\n", delta_freq);
			//mode
			l = ntohl(*px++);
			pt = (char *)px;
			//printf("mode(%d):", l);
			for (i = 0; i < l; i++)
				mode[i] = *pt++;
			mode[i] = 0;
			//printf("mode: %s\n", mode);
			//message
			px = (int32_t *)pt;			
			l = ntohl(*px++);
			pt = (char *)px;
			for (i = 0; i < l; i++)
				message[i] = *pt++;
			message[i] = 0;	
			//printf("msg: %s\n", message);
			sprintf(log, "%02d:%02d:%02d %03d %4d %s %s", hours, minutes, seconds, 
				snr, delta_freq, mode, message);
			//append_log(log);
			write_log(log);
			puts(log);
			break;
	}	
}

/*
int main(){

  puts("Starting udp\n");
  wsjtx_start();
  puts("Server started\n");
  while (1){
    wsjtx_slice();
    usleep(1000);
  }
  return 0;
}
*/


