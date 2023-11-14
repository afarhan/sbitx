// Based on https://mongoose.ws/tutorials/websocket-server/

#include "mongoose.h"
#include "webserver.h"
#include <pthread.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <wiringPi.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "logbook.h"

static const char *s_listen_on = "ws://0.0.0.0:8080";
static char s_web_root[1000];
static char session_cookie[100];
static struct mg_mgr mgr;  // Event manager

static void web_respond(struct mg_connection *c, char *message){
	mg_ws_send(c, message, strlen(message), WEBSOCKET_OP_TEXT);
}

static void get_console(struct mg_connection *c){
	char buff[2100];
	
	int n = web_get_console(buff, 2000);
	if (!n)
		return;
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
}

static void get_updates(struct mg_connection *c, int all){
	//send the settings of all the fields to the client
	char buff[2000];
	int i = 0;

	get_console(c);

	while(1){
		int update = remote_update_field(i, buff);
		// return of -1 indicates the eof fields
		if (update == -1)
			return;
	//send the status anyway
		if (all || update )
			mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT); 
		i++;
	}
}

static void do_login(struct mg_connection *c, char *key){

	char passkey[20];
	get_field_value("#passkey", passkey);

	//look for key only on non-local ip addresses
	if ((!key || strcmp(passkey, key)) && (c->rem.ip != 16777343)){
		web_respond(c, "login error");
		c->is_draining = 1;
		printf("passkey didn't match. Closing socket\n");
		return;
	}
	
	sprintf(session_cookie, "%x", rand());
	char response[100];
	sprintf(response, "login %s", session_cookie);
	web_respond(c, response);	
	get_updates(c, 1);
}

static int16_t remote_samples[10000]; //the max samples are set by the queue lenght in modems.c

static void get_spectrum(struct mg_connection *c){
	char buff[3000];
	web_get_spectrum(buff);
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
	get_updates(c, 0);
}

static void get_audio(struct mg_connection *c){
	char buff[3000];
	web_get_spectrum(buff);
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
	get_updates(c, 0);

	int count = remote_audio_output(remote_samples);		
	if (count > 0)
		mg_ws_send(c, remote_samples, count * sizeof(int16_t), WEBSOCKET_OP_BINARY);
}

static void get_logs(struct mg_connection *c, char *args){
	char logbook_path[200];
	char row_response[1000], row[1000];
	char query[100];
	int	row_id;

	query[0] = 0;
	row_id = atoi(strtok(args, " "));
	logbook_query(strtok(NULL, " \t\n"), row_id, logbook_path);
	FILE *pf = fopen(logbook_path, "r");
	if (!pf)
		return;
	while(fgets(row, sizeof(row), pf)){
		sprintf(row_response, "QSO %s", row);
		web_respond(c, row_response); 
	}
	fclose(pf);
}

void get_macros_list(struct mg_connection *c){
	char macros_list[2000], out[3000];
	macro_list(macros_list);
	sprintf(out, "macros_list %s", macros_list);
	web_respond(c, out);
}

void get_macro_labels(struct mg_connection *c){
	char key_list[2000], out[3000];
	macro_get_keys(key_list);
	sprintf(out, "macro_labels %s", key_list);
	web_respond(c, out);
}

char request[200];
int request_index = 0;

static void web_despatcher(struct mg_connection *c, struct mg_ws_message *wm){
	if (wm->data.len > 99)
		return;

	strncpy(request, wm->data.ptr, wm->data.len);	
	request[wm->data.len] = 0;
	//handle the 'no-cookie' situation
	char *cookie = NULL;
	char *field = NULL;
	char *value = NULL;

	cookie = strtok(request, "\n");
	field = strtok(NULL, "=");
	value = strtok(NULL, "\n");

	if (field == NULL || cookie == NULL){
		printf("Invalid request on websocket\n");
		web_respond(c, "quit Invalid request on websocket");
		c->is_draining = 1;
	}
	else if (strlen(field) > 100 || strlen(field) <  2 || strlen(cookie) > 40 || strlen(cookie) < 4){
		printf("Ill formed request on websocket\n");
		web_respond(c, "quit Illformed request");
		c->is_draining = 1;
	}
	else if (!strcmp(field, "login")){
		printf("trying login with passkey : [%s]\n", value);
		do_login(c, value);
	}
	else if (cookie == NULL || strcmp(cookie, session_cookie)){
		web_respond(c, "quit expired");
		printf("Cookie not found, closing socket %s vs %s\n", cookie, session_cookie);
		c->is_draining = 1;
	}
	else if (!strcmp(field, "spectrum"))
		get_spectrum(c);
	else if (!strcmp(field, "audio"))
		get_audio(c);
	else if (!strcmp(field, "logbook"))
		get_logs(c, value);
	else if (!strcmp(field, "macros_list"))
		get_macros_list(c);
	else if (!strcmp(field, "refresh"))
		get_updates(c, 1);
	else{
		char buff[1200];
		if (value)
			sprintf(buff, "%s %s", field, value);
		else
			strcpy(buff, field);
		remote_execute(buff);
		get_updates(c, 0);
	}
}

// This RESTful server implements the following endpoints:
//   /websocket - upgrade to Websocket, and implement websocket echo server
//   /rest - respond with JSON string {"result": 123}
//   any other URI serves static files from s_web_root
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
	} else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE){
//		if (ev == MG_EV_ERROR)
//			printf("closing with MG_EV_ERROR : ");
//		if (ev = MG_EV_CLOSE)
//			printf("closing with MG_EV_CLOSE : ");
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/websocket")) {
      // Upgrade to websocket. From now on, a connection is a full-duplex
      // Websocket connection, which will receive MG_EV_WS_MSG events.
      mg_ws_upgrade(c, hm, NULL);
    } else if (mg_http_match_uri(hm, "/rest")) {
      // Serve REST response
      mg_http_reply(c, 200, "", "{\"result\": %d}\n", 123);
    } else {
      // Serve static files
      struct mg_http_serve_opts opts = {.root_dir = s_web_root};
      mg_http_serve_dir(c, ev_data, &opts);
    }
  } else if (ev == MG_EV_WS_MSG) {
    // Got websocket frame. Received data is wm->data
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
//		printf("ws request,  client to %x:%d\n", c->rem.ip, c->rem.port);
    web_despatcher(c, wm);
  }
  (void) fn_data;
}

void *webserver_thread_function(void *server){
  mg_mgr_init(&mgr);  // Initialise event manager
  mg_http_listen(&mgr, s_listen_on, fn, NULL);  // Create HTTP listener
  for (;;) mg_mgr_poll(&mgr, 1000);             // Infinite event loop
	printf("exiting webserver thread\n");
}

void webserver_stop(){
  mg_mgr_free(&mgr);
}

static pthread_t webserver_thread;

void webserver_start(){
	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(s_web_root, path);
	strcat(s_web_root, "/sbitx/web");

	logbook_open();
 	pthread_create( &webserver_thread, NULL, webserver_thread_function, 
		(void*)NULL);
}
