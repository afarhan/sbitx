#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int macro_load(char *filename);
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

int  macro_load(char *filename){
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
	return 0;
}
/*
void macro_get_var(char *var, char *s){
	*s = 0;

	if(!strcmp(var, "MYCALL"))
		strcpy(s, mycallsign);
	else if (!strcmp(var, "CALL"))
		strcpy(s, yourcallsign);
	else if (!strcmp(var, "SENTRST"))
		sprintf(s, "%d", contact_rst);
	else if (!strcmp(var, "GRID")){
		strcpy(s, mygrid);
	else if (!strcmp(var, "GRIDSQUARE")){
		strcpy(var, mygrid);
		var[4] = 0;
		strcpy(s, var);
	}
	else
		*s = 0;
}
*/

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
	else if (!strcmp(var, "EXCH"))
		sprintf(s, "%03d", serial);
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
			if(is_running)
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

void macro_label(int fn_key, char *label){
	*label = 0;

	//if running, take the first match, if S&P, take the last match
	for (int i = 0; i < MACRO_MAX; i++){	
		if (macro_table[i].fn_key == fn_key){
			strcpy(label, macro_table[i].label);
			if (is_running)
				break;
		}
	}
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
