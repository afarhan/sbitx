#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "sdr_ui.h"

int macro_exec(int key, char *dest);
void macro_get_var(char *var, char *s);


#define MACRO_MAX_LABEL (30)
#define MACRO_MAX_TEXT (200)
#define MACRO_MAX (25)

struct macro {
	int fn_key;
	char	label[MACRO_MAX_LABEL];
	char 	text[MACRO_MAX_TEXT];
};

static struct macro macro_table[MACRO_MAX];
static int serial = 1;
static char macro_v_str[10];
static char is_running = 0; 

void macro_get_keys(char *output){
	output[0] = 0;
	for (int i = 0; i < MACRO_MAX; i++){	
		if (macro_table[i].label[0] == 0)
				break;
		char key_name[10];
		sprintf(key_name, "|%d ", macro_table[i].fn_key);
		strcat(output, key_name);
		strcat(output, macro_table[i].label);	
	}
}

void macro_list(char *output){
	char full_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	char *home_path = getenv("HOME");
	strcpy(full_path, home_path);
	strcat(full_path, "/sbitx/web/");
	DIR *d = opendir(full_path);
  struct dirent *dir;

	if (!d){
		write_console(FONT_LOG, "\Error:data subdirectory is missing\n");
		return;
	}

	write_console(FONT_LOG, "\nAvailable macros:\n");

	if(output)
		output[0] = 0;	
  while ((dir = readdir(d)) != NULL) {
		char *p = dir->d_name;
		int len = strlen(p);
		if (p[len-3] == '.' && p[len-2] == 'm' && p[len-1] == 'c'){
			p[len-3] = 0;
			write_console(FONT_LOG, p);
			write_console(FONT_LOG, "\n");
     	printf("%s\n", dir->d_name);
			if (output){
				strcat(output, dir->d_name);
				strcat(output, "|");
			}
		}
	}
  closedir(d);
}


void macro_label(int fn_key, char *label){
	*label = 0;

	for (int i = 0; i < MACRO_MAX; i++){	
		if (macro_table[i].fn_key == fn_key){
			strcpy(label, macro_table[i].label);
			if (is_running)
				break;
		}
	}
}

int  macro_load(char *filename, char *output){
	char macro_line[255];
	char full_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	char *home_path = getenv("HOME");
	strcpy(full_path, home_path);
	strcat(full_path, "/sbitx/data/");
	strcat(full_path, filename);
	strcat(full_path, ".mc");
	FILE *pf = fopen(full_path, "r");

	if(!pf)
		return -1;

	memset(macro_table, 0, sizeof(macro_table));
	int i = 0; 
	while (i < MACRO_MAX){
		if (fgets(macro_line, sizeof(macro_line) - 1, pf) == NULL)
			break;

		char *p = macro_line;
		if (*p++ != 'F')
			continue;
		macro_table[i].fn_key = atoi(p);

		//now, skip to beyond the key identifier 
		while(*p != ' ' && *p)
			p++;
		
		//skip the following spaces to the start of button label
		while(*p == ' ' && *p)
			p++;

		int j = 0;
		for (j = 0; j < MACRO_MAX_LABEL; j++){
			if (*p  && *p != ',')
				macro_table[i].label[j] = *p++;
			else	
				break;		
		}
		macro_table[i].label[j] = 0;

		//expect a comma before the macro text
		if (*p != ','){
			//printf("Macro loading %s, Expected a comma before [%s]\n", full_path, p);
			continue;
		}
		
		strcpy(macro_table[i].text, ++p);
		i++;
	}
	fclose(pf);

	for (int i = 1; i <= 12; i++){
		char button[32], label[32];
		macro_label(i, label);
		sprintf(button, "#mf%d", i);
		set_field(button, label);
	}

	return 0;
}

static char *macro_expand_var(char *var, char *s){
	*s = 0;

	if (!strcmp(var, "RUN"))
		is_running = 1;
	else if (!strcmp(var, "S&P"))
		is_running = 0;
	else if(!strcmp(var, "MYCALL"))
		macro_get_var(var, s);
	else if (!strcmp(var, "CALL"))
		macro_get_var(var, s);
	else if (!strcmp(var, "SENTRSTCUT"))
		strcpy(s, "5NN");
	else if (!strcmp(var, "SENTRST"))
		macro_get_var(var, s);
	else if (!strcmp(var, "EXCH") || !strcmp(var, "EXCHANGE"))
		macro_get_var("EXCH", s);
	else if (!strcmp(var, "GRID"))
		macro_get_var(var, s);
	else if (!strcmp(var, "GRIDSQUARE"))
		macro_get_var(var, s);
	return s + strlen(s);	
}

static char *macro_expand_short_var(char v, char *dest){
	if(v == '*')
		return macro_expand_var("MYCALL", dest);
	else if (v == '!')
		return macro_expand_var("CALL", dest);
	else if (v == '#')
		return macro_expand_var("EXCH", dest);
	else if (v == '@')
		return macro_expand_var("FREQ", dest); 
	return dest;
}

int macro_exec(int key, char *dest){
	struct macro *m = NULL;

	//if you are running, take the first definition of the fn key,
	//else (you are s&p-ing, take the last definition

	int i;
	for(i = 0; i < MACRO_MAX; i++)
		if (macro_table[i].fn_key == key){
			m = macro_table + i; 
			//if(is_running)
				break;
		}

	if(!m)
		return -1;

	//now we have a candidate macro
	char macro_name[10];

	char *q = dest;
	char *p = m->text;
	char var[20];
	char in_var = 0;
	
	var[0] = 0;
	//we end at the string end, newline, etc.
	while (*p >= ' '){
		//the sequency of if-else-if is imp.
		if (*p == '!' || *p == '*' || *p == '@' || *p == '#'){
			q = macro_expand_short_var(*p++, q);		
		}
		else if (*p == '}'){
			q = macro_expand_var(var, q);
			var[0] = 0;
			p++;
			in_var = 0;
		}
		else if (in_var == 1){
			int l = strlen(var);	
			var[l++] = *p++;
			var[l++] = 0;	
		}
		else if (*p == '{'){
			in_var = 1;
			p++;
		}
		else{
			*q++ = *p++;
			*q = 0;
		} 
	}
	return 0;
}


/*
int main(int argc, char *argv[]){
	macro_load("CQWWCW");

	strcpy(mycallsign, "vu2ese");
	strcpy(yourcallsign, "ka7exm");
	strcpy(mygrid, "Mk97fg");
	char buff[100];

	macro_exec(atoi(argv[1]), atoi(argv[2]), buff);
	puts(buff);
}
*/
