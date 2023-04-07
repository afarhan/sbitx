create table logbook (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	mode TEXT,
	freq TEXT,
	qso_date TEXT,
	qso_time TEXT,
	callsign_sent TEXT,
	rst_sent TEXT,
	exch_sent TEXT DEFAULT "",
	callsign_recv TEXT,
	rst_recv TEXT,
	exch_recv TEXT DEFAULT "",
	tx_id	TEXT DEFAULT "",
	comments TEXT DEFAULT ""
);
