#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]){
	char output[200];

	puts("shelling to ft_decode ..");
	FILE *pf = popen("./decode_ft8 test.wav", "r");
	if (!pf){
		puts("coudn't decode");
		exit(-1);
	}
	while(fgets(output, sizeof(output), pf)) 
		printf("[%s]\n", output);
	pclose(pf);
	return 0;
}
