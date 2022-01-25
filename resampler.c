#include <stdint.h>
#include <stdio.h>


void resample(int32_t *in, int in_count, int32_t *out, int out_count){
	int in_sample, out_sample;
	int i;
	int length = in_count * out_count;
	int sample;

	printf("Resampling from %d to %d\n", in_count, out_count);
	out_sample = 0;
	in_sample = 0;

	i = 0;
	while (out_sample < length){
		//printf("%d %d %d %d-%d/%d", 
		//	i, in_sample, out_sample, out_sample/out_count, 
		//	out_sample % out_count, out_count);
		int si = out_sample / out_count;
		int p = out_sample % out_count;
		int q = out_count - p;
		int v = ((in[si] * q) + (in[si+1] * p))/out_count;
		out[i] = v;
		//printf("in[%d]= %d, out=%d p=%d q=%d v = %d\n", 
		//	i, in[i], si, p, q, v);
		i++;
		in_sample += out_count;
		out_sample += in_count;
	}
}

int32_t in_samples[] = 
{0,10,20,30,40,50,60,50,40,30,20,10,0,-10,-20,-30,-40,-50,-60,-50,-40,-30,-20,-10,0,10,20,30,
40,50,60,50,30,20,10,0};
int32_t out_samples[100];

void main(int argc, char **argv){
	resample(in_samples, 20, out_samples, 24);
	for (int i = 0; i < 24; i++)
		printf("%d: %d : %d\n", i, in_samples[i], out_samples[i]);
}
