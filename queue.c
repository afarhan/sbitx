#include <stdlib.h>
#include <string.h>
#include "queue.h"
/**
 * Audio sampling queues for playback and recording
 */


void q_init(struct Queue *p, int length){
  p->head = 0;
  p->tail = 0;
  p->stall = 1;
	p->underflow = 0;
	p->overflow = 0;
	p->max_q = length;
	p->data = malloc(length+1);
	memset(p->data, 0, p->max_q+1);
}

int q_length(struct Queue *p){
  if ( p->head >= p->tail)
    return p->head - p->tail;
  else
    return ((p->head + p->max_q) - p->tail);
}

void q_write(struct Queue *p, int w){

  if (p->head + 1 == p->tail || p->tail == 0 && p->head == p->max_q-1){
    p->overflow++;
    return;
  }

  p->data[p->head++] = w;
  if (p->head > p->max_q){
    p->head = 0;
  }
}

int q_read(struct Queue *p){
 int data;

  if (p->tail == p->head){
    p->underflow++;
    return (int)0;
  }
    
  data = p->data[p->tail++];
  if (p->tail > p->max_q)
    p->tail = 0;

  return data;
}

