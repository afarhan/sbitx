struct Queue
{
  int id;
  int head;
  int tail;
  int  stall;
	int *data;
	unsigned int underflow;
	unsigned int overflow;
	unsigned int max_q;
};

void q_init(struct Queue *p, int32_t length);
int q_length(struct Queue *p);
int32_t q_read(struct Queue *p);
int q_write(struct Queue *p, int w);
void q_empty(struct Queue *p);
