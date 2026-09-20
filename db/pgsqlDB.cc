/**
 * pgsqlDB.cc
 * Author: Daniel Karrels (dan@karrels.com)
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 *
 * $Id: pgsqlDB.cc,v 1.5 2009/07/25 16:59:48 mrbean_ Exp $
 */

#include <sys/types.h>

#include <new>
#include <iostream>
#include <exception>
#include <sstream>
#include <string>

#include "libpq-fe.h"
#include "gnuworldDB.h"
#include "pgsqlDB.h"
#include "client.h"
#include "logger.h"

namespace gnuworld {
using std::cout;
using std::endl;
using std::ends;
using std::string;
using std::stringstream;

/**
 * One value of a connection string, quoted the way libpq documents it: in
 * single quotes, with a backslash before every backslash and single quote.
 * Without this a value holding a space (a password may) ends the key it
 * belongs to, and libpq's complaint about the rest of it names the fragment
 * - which is how a password reaches a log file.
 */
static string quoteConnValue(const string& value) {
    string quoted("'");

    for (const char c : value) {
        if ('\\' == c || '\'' == c)
            quoted += '\\';
        quoted += c;
    }

    return quoted + '\'';
}

pgsqlDB::pgsqlDB(xClient* _bot, const string& dbHost, const unsigned short int dbPort,
                 const string& dbName, const string& userName, const string& password)
    : gnuworldDB(dbHost, dbPort, dbName, userName, password), bot(_bot), theDB(0), lastResult(0),
      sqlLog(_bot->getLogger()->child("sql", ERROR)) {
    stringstream s;
    s << "host=" << quoteConnValue(dbHost) << " dbname=" << quoteConnValue(dbName)
      << " port=" << dbPort;

    if (!userName.empty()) {
        s << " user=" << quoteConnValue(userName);
    }
    if (!password.empty()) {
        s << " password=" << quoteConnValue(password);
    }
    s << ends;

    // Allow exception to be thrown
    theDB = PQconnectdb(s.str().c_str());
    if (0 == theDB) {
        cout << "pgsqlDB> Failed to allocate memory for db handle" << endl;
        throw std::exception();
    }
    if (!isConnected()) {
        cout << "pgsqlDB> Failed to connect to db: " << ErrorMessage() << endl;
        //	throw std::exception() ;
    }
}

pgsqlDB::pgsqlDB(xClient* _bot, const string& connectInfo)
    : bot(_bot), sqlLog(_bot->getLogger()->child("sql", ERROR)) {
    // TODO
    // Allow exception to be thrown
    lastResult = 0;
    theDB = PQconnectdb(connectInfo.c_str());
    if (0 == theDB) {
        cout << "pgsqlDB> Failed to allocate memory for db handle" << endl;
        throw std::exception();
    }
    if (!isConnected()) {
        cout << "pgsqlDB> Failed to connect to db: " << ErrorMessage() << endl;
        throw std::exception();
    }
}

pgsqlDB::~pgsqlDB() {
    if (theDB != 0) {
        PQfinish(theDB);
        theDB = 0;
    }
    if (lastResult != 0) {
        PQclear(lastResult);
        lastResult = 0;
    }
}

bool pgsqlDB::Exec(const string& theQuery, bool log, std::source_location where) {
    /* Log query. */
    if (log)
        LOG_MSG_TO(sqlLog, DEBUG, "{query}").with("query", theQuery).log();

    // It is necessary to manually deallocate the last result
    // to prevent memory leaks.
    if (lastResult != 0) {
        PQclear(lastResult);
        lastResult = 0;
    }
    lastResult = PQexec(theDB, theQuery.c_str());

    ExecStatusType status = PQresultStatus(lastResult);
    if (PGRES_COPY_IN == status)
        return true;
    if (PGRES_TUPLES_OK == status)
        return true;
    if (PGRES_COMMAND_OK == status)
        return true;

    /* The failure is the handle's own to report, whatever "log" says: no
     * caller has to remember to, and none of them can name the statement in
     * the record anyway.
     *
     * PostgreSQL's primary message and nothing else: the full text of
     * PQerrorMessage() carries a "LINE 1: <statement>" excerpt and a
     * "DETAIL: Key (...)=(...)", either of which would put back into the log
     * the literal values - a password hash, a TOTP secret, a SCRAM record -
     * that Exec(query, false) keeps out of it.  A connection-level failure
     * has no result to ask, and there the whole message is all there is. */
    const char* primary = PQresultErrorField(lastResult, PG_DIAG_MESSAGE_PRIMARY);

    sqlLog->createMessage(ERROR, where.function_name(), "SQL Error: {error}")
        .with("error", (0 == primary) ? ErrorMessage() : string(primary))
        .log();
    return false;
}

bool pgsqlDB::Exec(const stringstream& theQuery, bool retData, std::source_location where) {
    return Exec(theQuery.str(), retData, where);
}

bool pgsqlDB::StartCopyIn(const string& writeMe) { return Exec(writeMe); }

bool pgsqlDB::StopCopyIn() {
    if (0 == lastResult) {
        return false;
    }
    return (PQputCopyEnd(theDB, 0) != -1);
}

bool pgsqlDB::PutLine(const string& writeMe) {
    if (0 == lastResult) {
        return false;
    }
    return (PQputline(theDB, writeMe.c_str()) != -1);
}

unsigned int pgsqlDB::countTuples() const {
    if (0 == lastResult) {
        return 0;
    }
    return PQntuples(lastResult);
}

unsigned int pgsqlDB::affectedRows() const {
    if (0 == lastResult)
        return 0;

    return std::atoi(PQcmdTuples(lastResult));
}

const string pgsqlDB::ErrorMessage() const { return string(PQerrorMessage(theDB)); }

const string pgsqlDB::GetValue(unsigned int rowNumber, unsigned int columnNumber) const {
    if (0 == lastResult) {
        return string();
    }
    return PQgetvalue(lastResult, rowNumber, columnNumber);
}

const string pgsqlDB::GetValue(unsigned int rowNumber, const string& columnName) const {
    if (0 == lastResult) {
        return string("No result stored");
    }

    // Retrieve the column number for this name.
    const int columnNumber = PQfnumber(lastResult, columnName.c_str());
    if (-1 == columnNumber) {
        return (string("No such column: ") + columnName);
    }

    return PQgetvalue(lastResult, rowNumber, columnNumber);
}

bool pgsqlDB::isConnected() const { return (CONNECTION_OK == PQstatus(theDB)); }

} // namespace gnuworld
