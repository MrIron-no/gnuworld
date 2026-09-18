#!/bin/bash
# Databases for mod.nickserv, mod.dronescan and mod.openchanfix, from the
# schemas the repository ships.
set -e

psql=( psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --no-password )
export PGCLIENTENCODING=LATIN1

load() {  # load <database> <file>...
  local db="$1"; shift
  "${psql[@]}" --dbname postgres -c "CREATE DATABASE ${db}"
  for sql_file in "$@"; do
    "${psql[@]}" --dbname "$db" -q -f "$sql_file" > /dev/null
  done
}

load nickserv  /gnuworld-doc/nickserv/nickserv.sql
load dronescan /gnuworld-doc/dronescan.sql
load chanfix   /gnuworld-openchanfix-doc/chanfix.sql \
               /gnuworld-openchanfix-doc/chanfix.languages.sql \
               /gnuworld-openchanfix-doc/chanfix.language.english.sql \
               /gnuworld-openchanfix-doc/chanfix.help.sql \
               /gnuworld-openchanfix-doc/chanfix.addme.sql
