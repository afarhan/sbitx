#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_KEY (64)
#define MAX_VALUE (256)

struct record {
	char key[MAX_KEY];
	char value[MAX_VALUE];
	struct record *next;
};

static struct record *record_list = NULL;

struct record *get_record(char *key){
	for (struct record *r = record_list; r; r = r->next)
		if (!strcmp(r->key, key))
			return r;
	return NULL;
}

char *record_get_string(char *key, char *default_value){
	struct record *r = get_record(key);
	if (r)
		return r->value;
	else
		return default_value;
}

int record_get_integer(char *key, int default_value){
	struct record *r = get_record(key);
	
	if (!r)
		return default_value;
	else
		return atoi(r->value);
}

void record_update_integer(char *key, int value){
	struct record *r = get_record(key);
	if (!r)
			r = (struct record *)malloc(sizeof(struct record));
	strcpy(r->key, key);
	sprintf(r->value, "%ld", value);
}

void update_record_string(char *key, char *value){
	struct record *r = get_record(key);
	if (!r)
			r = (struct record *)malloc(sizeof(struct record));
	strcpy(r->key, key);
	strcpy(r->value, value);
}

void load_records(){
	char b[1000];
	struct record *r;

	FILE *pf = fopen("sbitx.rc", "r");
	if (!pf)
		return;

	while (fgets(b, sizeof(b), pf)){
		if (b[0] == '#')
			continue;
		char *p = strtok(b, "=");
		char *q = strtok(NULL, "\n");
		if(!p || !q)
			continue;
		printf("Read <%s> as \"%s\"\n", p, q);
		r = malloc(sizeof(struct record));
		strcpy(r->key, p);
		strcpy(r->value, q);
		r->next = record_list;
		record_list = r; 
	}
	fclose(pf);	
}

void dump_records(){
	for (struct record *r = record_list; r; r = r->next)
		printf("[%s] = <%s>\n", r->key, r->value);
}

void save_records(){
	FILE *pf;
	pf = fopen("sbitx.rc", "w");
	if (!pf){
		puts("*Error writing sbitx.rc\n");
		return;
	}
	for (struct record *r = record_list; r; r = r->next)
		fprintf(pf, "%s =%s\n", r->key, r->value);
	fclose(pf);	
}

int main(int argc, char **argv){
	load_records();
	printf("mic is %d\n", record_get_integer("mic_gain", 70));
	printf("mode is %s\n", record_get_string("mode", "USB")); 
}
