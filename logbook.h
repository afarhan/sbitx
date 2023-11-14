
void logbook_open();
void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent, 
	char *rst_recv, char *exchange_recv);
int logbook_query(char *query, int from_id, char *result_file);
int logbook_count_dup(const char *callsign, int last_seconds);
