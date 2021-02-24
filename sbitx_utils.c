#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEY 32
#define MAX_VALUE 256
#define MAX_LINE 1000
#define USE_STRING 0xC0FFEE

struct entry {
	char key[MAX_KEY];
	char value[MAX_VALUE];
	int	 value_int;
	struct entry *next;
};

struct entry *settings_head = NULL;

void config_update(char *key, char *value){
	struct entry *e = settings_head;

	if (strlen(key) >= MAX_KEY){
		printf("*Error:key of [%s] is too big\n", key);
		return;
	}

	if (strlen(value) >= MAX_VALUE){
		printf("*Error: value of key [%s] is too big\n", value);
		return;
	} 

	//search an existing key
	while(e){
		if (!strcmp(e->key, key))
			break;
		e = e->next;
	}
	if (!e){
		e = (struct entry *)malloc(sizeof(struct entry));
		if (!e){
			printf("*Error: Out of memory entering [%s] = %s", key, value); 
			return;
		}
		strcpy(e->key, key);
		e->next = settings_head;
		settings_head = e;
	}
	strcpy(e->value, value);
}	

void config_update_int(char *key, int v){
	char value[MAX_VALUE];
	sprintf(value, "%d", v);
	config_update(key, value);
}

int config_get(char *key, char *d){
	for (struct entry *e = settings_head; e; e = e->next)
		if (!strcmp(e->key, key)){
			strcpy(d, e->value);
			return 0;
		}
	return 1;
}

int config_get_int(char *key, int *d){
	char v[MAX_VALUE];

	if (config_get(key, v))
		return 1;
	*d = atoi(v);		
	return 0;
}

void config_load(){
	char buff [MAX_LINE];
	FILE *pf = fopen("sbitx.conf", "r");
	
	while (!fgets(buff, MAX_LINE, pf)){
		if (buff[0] == '#')
			continue;
		char *k = strtok(buff, "=");
		if(!k)
			continue;	
		char *v = strtok(NULL, "\n\r");	
		if (v)
			config_update(k, v);
	}
	fclose(pf);
}

void config_save(){
	FILE *pf = fopen("sbitx.conf", "w");
	fputs("#Settngs of sbitx\n"
	"#This file is maintained by sbitx\n"
	"#Don't edit it unless you know what you are doing\n", pf);

	for (struct entry *e = settings_head; e; e = e->next)
		fprintf(pf, "%s=%s\n", e->key, e->value);

	fclose(pf);
}

/*
void main(){
	config_update("r1:freq", "7100000");	
	config_save();
	int data;
	if (!config_get_int("r1:freq",&data))
		printf("value of r1:freq is [%d]\n", data);
	else
		puts("Not found!");
}
*/
