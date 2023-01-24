// Based on https://mongoose.ws/tutorials/websocket-server/

#include "mongoose.h"
#include "webserver.h"
#include <pthread.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "sdr.h"

static const char *s_listen_on = "ws://0.0.0.0:8000";
static const char *s_web_root = "./web";
static struct mg_mgr mgr;  // Event manager

int set_field(char *id, char *value);
extern int spectrum_plot[];

static void respond(struct mg_connection *c, char *message){
	mg_ws_send(c, message, strlen(message), WEBSOCKET_OP_TEXT);
}

char request[200];
int request_index = 0;

static void spectrum_update(struct mg_connection *c){
	char spectrum_response[MAX_BINS + 20];

	strcpy(spectrum_response, "spectrum=");
	int k = strlen(spectrum_response);
	for (int i = 0; i < MAX_BINS; i++)
			spectrum_response[k++] = spectrum_plot[i] + ' ';
	spectrum_response[k] = 0;
	respond(c, spectrum_response);	
}

static void web_despatcher(struct mg_connection *c, struct mg_ws_message *wm){
	if (wm->data.len > 99)
		return;

	strncpy(request, wm->data.ptr, wm->data.len);	
	request[wm->data.len] = 0;
	char *field = strtok(request, "=");
	char *value = strtok(NULL, "\n");

	if (!strcmp(field, "login")){
		printf("trying login with passkey : [%s]\n", value); 
		respond(c, "ok login");	
	}
	else if (!strcmp(field, "spectrum"))
		spectrum_update(c);
	else{
//		printf("web setting[%s] to [%s]\n", field, value);
		set_field(field, value);
/*		for (int i = 0; i < strlen(p); p++){
			if (*p == '\n'){
				request[request_index] = 0;
				do_cmd(request);
			}
			else{
				request[request_index++] = *p;
				if(request_index >= sizeof(request))
					request_index = 0;
			}
		}*/
	}
}

// This RESTful server implements the following endpoints:
//   /websocket - upgrade to Websocket, and implement websocket echo server
//   /rest - respond with JSON string {"result": 123}
//   any other URI serves static files from s_web_root
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/websocket")) {
      // Upgrade to websocket. From now on, a connection is a full-duplex
      // Websocket connection, which will receive MG_EV_WS_MSG events.
      printf("connected to websocket!\n");
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
    // Got websocket frame. Received data is wm->data. Echo it back!
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    //mg_ws_send(c, wm->data.ptr, wm->data.len, WEBSOCKET_OP_TEXT);
    web_despatcher(c, wm);
	//	printf("ws: [%s]\n", wm->data.ptr);
  }
  (void) fn_data;
}

void *webserver_thread_function(void *server){
  mg_mgr_init(&mgr);  // Initialise event manager
  printf("Starting WS listener on %s/websocket\n", s_listen_on);
  mg_http_listen(&mgr, s_listen_on, fn, NULL);  // Create HTTP listener
  for (;;) mg_mgr_poll(&mgr, 1000);             // Infinite event loop
}

void webserver_stop(){
  mg_mgr_free(&mgr);
}

static pthread_t webserver_thread;

void webserver_start(){
 	pthread_create( &webserver_thread, NULL, webserver_thread_function, 
		(void*)NULL);
}
