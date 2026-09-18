#!/bin/bash
# A database for mod.cservice, as doc/README.cservice sets one up: the schema,
# the languages, the help and "Admin", level 1000 on "*" and #coder-com, whose
# password is the one doc/cservice.addme.sql documents.
set -e

psql=( psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --no-password )

"${psql[@]}" --dbname postgres -c 'CREATE DATABASE cservice'

# The translations are Latin-1, which psql converts for a UTF-8 database
export PGCLIENTENCODING=LATIN1

for sql_file in cservice.sql cservice.languages.sql cservice.translations.sql \
                cservice.help.sql cservice.config.sql cservice.addme.sql; do
  "${psql[@]}" --dbname cservice -q -f "/gnuworld-doc/${sql_file}" > /dev/null
done

unset PGCLIENTENCODING

# Admin may log in from where the harness's clients are, which is loopback;
# without a matching row, access on "*" is refused.
"${psql[@]}" --dbname cservice <<-'EOSQL'
	INSERT INTO ip_restrict (user_id, added_by, added, type, expiry, value, description)
	VALUES (1, 1, EXTRACT(EPOCH FROM NOW())::int, 1, 0, '127.0.0.0/8', 'gnuworld test harness');
EOSQL
