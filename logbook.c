#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "logbook.h"

#include <sqlite3.h>

static int rc;
static sqlite3 *db;

/* writes the output to data/result_rows.txt */

void logbook_query(char *query, int from_id, char *result_file){
	sqlite3_stmt *stmt;
	char statement[200], param[2000];

	if (from_id == -1)
		from_id = 1000000000; //set it very high
	if (!query)
		sprintf(statement, "select * from logbook where id <= %d ORDER BY id DESC LIMIT 50", from_id);
	else
		sprintf(statement, "select * from logbook where (callsign_recv LIKE '%s%%' AND id <=%d) ORDER BY id DESC LIMIT 50", query, from_id);	

	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

	char output_path[200];	//dangerous, find the MAX_PATH and replace 200 with it
	sprintf(output_path, "%s/sbitx/data/result_rows.txt", getenv("HOME"));
	strcpy(result_file, output_path);
	
	FILE *pf = fopen(output_path, "w");

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, (const char *)sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			case (SQLITE_NULL):
				break;
			default:
				sprintf(param, "%d", sqlite3_column_type(stmt, i));
				break;
			}
			fprintf(pf, "%s|", param);
		}
		fprintf(pf, "\n");
	}
	sqlite3_finalize(stmt);
	fclose(pf);
}

void logbook_open(){
	char db_path[200];	//dangerous, find the MAX_PATH and replace 200 with it
	sprintf(db_path, "%s/sbitx/data/sbitx.db", getenv("HOME"));

	rc = sqlite3_open(db_path, &db);
	printf("sqlite3 opening returned %d\n", rc);
}

void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent, 
	char *rst_recv, char *exchange_recv){
	char statement[1000], *err_msg, date_str[36], time_str[10];
	char freq[12], log_freq[12], mode[10], mycallsign[10];

	time_t log_time = time_sbitx();
	struct tm *tmp = gmtime(&log_time);
	get_field_value("r1:freq", freq);
	get_field_value("r1:mode", mode);
	get_field_value("#mycallsign", mycallsign);

	sprintf(log_freq, "%d", atoi(freq)/1000);
	
	sprintf(date_str, "%04d-%02d-%02d", tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday);
	sprintf(time_str, "%02d%02d", tmp->tm_hour, tmp->tm_min);

	sprintf(statement,
		"INSERT INTO logbook (freq, mode, qso_date, qso_time, callsign_sent,"
		"rst_sent, exch_sent, callsign_recv, rst_recv, exch_recv) "
		"VALUES('%s', '%s', '%s', '%s',  '%s','%s','%s',  '%s','%s','%s');",
			log_freq, mode, date_str, time_str, mycallsign,
			 rst_sent, exchange_sent, contact_callsign, rst_recv, exchange_recv);

	sqlite3_exec(db, statement, 0,0, &err_msg);
}


void import_logs(char *filename){
	char entry_text[1000], statement[1000];
	char freq[10], mode[10], date_str[10], time_str[10], mycall[10], rst_sent[10],
	exchange_sent[10], contact_callsign[10], rst_recv[10], exchange_recv[10];

	FILE *pf = fopen(filename, "r");
	while(fgets(entry_text, sizeof(entry_text), pf)){
		char *p = strtok(entry_text, "\t ");
		strcpy(freq, p);
		strcpy(mode, strtok(NULL, "\t "));
		strcpy(date_str, strtok(NULL, "\t "));
		strcpy(time_str, strtok(NULL, "\t "));
		strcpy(mycall, strtok(NULL, "\t "));
		strcpy(rst_sent, strtok(NULL, "\t "));
		strcpy(exchange_sent, strtok(NULL, "\t "));
		strcpy(contact_callsign, strtok(NULL, "\t "));
		strcpy(rst_recv, strtok(NULL, "\t "));
		strcpy(exchange_recv, strtok(NULL, "\t\n"));
		sprintf(statement,
		"INSERT INTO logbook (freq, mode, qso_date, qso_time, callsign_sent,"
		"rst_sent, exch_sent, callsign_recv, rst_recv, exch_recv) "
		"VALUES('%s', '%s', '%s', '%s',  '%s','%s','%s',  '%s','%s','%s');",
			freq, mode, date_str, time_str,
			 mycall, rst_sent, exchange_sent,
			contact_callsign, rst_recv, exchange_recv);
			
		puts(statement);
	}
	fclose(pf);
}

/*
int main(int argc, char **argv){
	database_open();
	query_log_to_text_file(argv[1], 50);
}
*/
