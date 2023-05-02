cd data 
rm user_settings.ini
rm sbitx.db
sqlite3 sbitx.db < create_db.sql
cd ..

