/**
 * pgsqlDB.h
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
 * $Id: pgsqlDB.h,v 1.3 2007/08/28 16:09:59 dan_karrels Exp $
 */

#ifndef __PGSQLDB_H
#define __PGSQLDB_H "$Id: pgsqlDB.h,v 1.3 2007/08/28 16:09:59 dan_karrels Exp $"

#include <sys/types.h>

#include <string>
#include <exception>

#include "libpq-fe.h"
#include "gnuworldDB.h"
#include "client.h"
#include "logger.h"

/**
 * Reports the statement that just failed, as an ERROR record of the "<module>.sql"
 * logger of whoever owns the handle, with the message of the database and the
 * statement itself as fields.  The name of the calling function comes from the
 * compiler, so that a call site needs to say nothing but which handle it is.
 *
 * Nothing here is a macro of the logging system: it works in every translation
 * unit of every module, whether that module named a logger of its own or not.
 */
#define SQL_ERROR(db) (db)->logError(__PRETTY_FUNCTION__)

/// The name this macro had while it was the logger's own
#define LOGSQL_ERROR(db) SQL_ERROR(db)

namespace gnuworld {

class pgsqlDB : public gnuworldDB {
  protected:
    xClient* bot;
    PGconn* theDB;
    PGresult* lastResult;

    /**
     * The logger the statements of this handle go to: "<module>.sql" of the
     * module that owns it, which logs errors and nothing else unless it is asked
     * for more.  It belongs to the registry and outlives this handle.
     */
    Logger* sqlLog;

    /// The statement Exec() was last given, which a failure reports
    std::string lastQuery;

    /**
     * Whether that statement may be shown: a caller that asked Exec() not to
     * log a statement - one that carries a secret, say - does not want to read
     * it in the record of its failure either.
     */
    bool lastQueryLoggable = true;

  public:
    pgsqlDB(xClient* bot, const std::string& dbHost, const unsigned short int dbPort,
            const std::string& dbName, const std::string& userName, const std::string& password);
    pgsqlDB(xClient* bot, const std::string& connectInfo);
    virtual ~pgsqlDB();

    /**
     * Executes one statement.
     *
     * The base class gnuworldDB calls the second parameter "returnData"; here it
     * has never had anything to do with the data that comes back, which is
     * fetched with countTuples() and GetValue() either way.  All it decides is
     * whether the statement itself is logged, which is what it is called here.
     */
    virtual bool Exec(const std::string&, bool logQuery = true);
    virtual bool Exec(const std::stringstream&, bool logQuery = true);
    virtual bool isConnected() const;

    /**
     * Reports the failure of the last statement, from the function named here.
     * Call sites use SQL_ERROR(db) rather than this method itself.
     */
    void logError(const char* func);

    virtual bool PutLine(const std::string&);
    virtual bool StartCopyIn(const std::string&);
    virtual bool StopCopyIn();

    virtual unsigned int countTuples() const;
    virtual unsigned int affectedRows() const;
    virtual const std::string ErrorMessage() const;

    // tuple number, field number (row,col)
    virtual const std::string GetValue(unsigned int, unsigned int) const;
    virtual const std::string GetValue(unsigned int row, const std::string& colName) const;
};

} // namespace gnuworld

#endif // __PGSQLDB_H
