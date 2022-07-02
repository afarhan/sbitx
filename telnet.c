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
#include <ctype.h>
#include <sys/time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "sdr_ui.h"

void telnet_open(char *server);
int telnet_write(char *text);
void telnet_close();

static int telnet_sock = -1;
static pthread_t telnet_thread;

long get_address(char *host)
{
	int i, dotcount=0;
	char *p = host;
	struct hostent		*pent;
	/* struct sockaddr_in	addr; */ /* see the note on portabilit at the end of the routine */

	/*try understanding if this is a valid ip address
	we are skipping the values of the octets specified here.
	for instance, this code will allow 952.0.320.567 through*/
	while (*p)
	{
		for (i = 0; i < 3; i++, p++)
			if (!isdigit(*p))
				break;
		if (*p != '.')
			break;
		p++;
		dotcount++;
	}

	/* three dots with upto three digits in before, between and after ? */
	if (dotcount == 3 && i > 0 && i <= 3)
		return inet_addr(host);

	/* try the system's own resolution mechanism for dns lookup:
	 required only for domain names.
	 inspite of what the rfc2543 :D Using SRV DNS Records recommends,
	 we are leaving it to the operating system to do the name caching.

	 this is an important implementational issue especially in the light
	 dynamic dns servers like dynip.com or dyndns.com where a dial
	 ip address is dynamically assigned a sub domain like farhan.dynip.com

	 although expensive, this is a must to allow OS to take
	 the decision to expire the DNS records as it deems fit.
	*/
	printf("Looking up %s\n", host);
	pent = gethostbyname(host);
	if (!pent){
		printf("Failed to resolve %s\n", host);	
		return 0;
	}

	/* PORTABILITY-ISSUE: replacing a costly memcpy call with a hack, may not work on 
	some systems.  
	memcpy(&addr.sin_addr, (pent->h_addr), pent->h_length);
	return addr.sin_addr.s_addr; */
	return *((long *)(pent->h_addr));
}

void *telnet_thread_function(void *server){
  struct sockaddr_storage serverStorage;
  socklen_t addr_size;
	struct sockaddr_in serverAddr;
	char buff[200], s_name[100];

	if (strlen(server) > sizeof(s_name) - 1)
		return NULL;

	strcpy(s_name, server); 
	char *host_name = strtok((char *)s_name, ":");
	char *port = strtok(NULL, "");

	if(!host_name){
		write_console(FONT_LOG, "Telnet : specify host and port\nex:'\topen dxc.g3lrs.org.uk:7300\n'");
		return NULL;
	}
	if(!port){
		write_console(FONT_LOG, "Telnet port is missing\n");
		return NULL;	
	}	

	if (telnet_sock >= 0)
		close(telnet_sock);


	sprintf(buff, "Finding %s:%d\n", server, port);
	write_console(FONT_LOG, buff);

  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(atoi(port));
  serverAddr.sin_addr.s_addr = get_address(server); 

	sprintf(buff, "Opening %s:%d\n", server, port);
	write_console(FONT_LOG, buff);

	telnet_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(telnet_sock, 
			(struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
		printf("Telnet: failed to connect to %s\n", server);
		write_console(FONT_LOG, "Failed to open telnet, check hostname and port?\n");
		close(telnet_sock);
		telnet_sock = -1;
		return NULL;
   }

	int e;
	while((e = recv(telnet_sock, buff, sizeof(buff), 0)) >= 0){
		if (e > 0){
			buff[e] = 0;
			
			//here we will try stripping too many spaces
			char *p, *q, buff2[201]; // bigger than the original buff
			int n_spaces = 0;
			int tab_space = 3;	
			p = buff; q = buff2;
			for (p = buff;*p;p++){
				if (*p != ' '){ 
					//add spaces for a shorter tab
					if (n_spaces > tab_space){
						int l = strlen(buff2);
						int next_tab = (l/tab_space + 1)* tab_space;
						for (int i = 0; i < next_tab - l; i++){
							*q++ = ' ';
							*q = 0;
						}
					}
					*q++ = *p;
					*q = 0;
					n_spaces = 0;
				}
				else { // *p is a space
					if (n_spaces < tab_space){
						*q++ = *p;
						*q = 0;
					}
					n_spaces++;
				}
			}
			*q = 0;	

//			printf("compressed [%s] to [%s]\n", buff, buff2);
			write_console(FONT_LOG, buff2);
		}
	}
	close(telnet_sock);
	telnet_sock = -1;	
}

int telnet_write(char *text){
	if (telnet_sock < 0){
		return -1;
	}
	char nl[] = "\n";
	send (telnet_sock, text, strlen(text), 0);
	send (telnet_sock, nl, strlen(nl), 0);
}

void telnet_close(){
	puts("Tenet socket is closing");
	close(telnet_sock);
	telnet_sock = 0;
}

char telnet_server_name[100];
void telnet_open(char *server){
	
	strcpy(telnet_server_name, server);
 	pthread_create( &telnet_thread, NULL, telnet_thread_function, 
		(void*)telnet_server_name);
}

/*
int main(int argc, char *argv[]){

	telnet_open(argv[1]);
	sleep(10);	
	telnet_write("vu2ese\n");
	sleep(20);
	telnet_close();
	sleep(10);
}
*/
