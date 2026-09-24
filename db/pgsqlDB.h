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
#include <source_location>

#include "libpq-fe.h"
#include "gnuworldDB.h"
#include "client.h"
#include "logger.h"

namespace gnuworld {

class pgsqlDB : public gnuworldDB {
  protected:
    xClient* bot;
    PGconn* theDB;
    PGresult* lastResult;

    /// "<module>.sql": statements at DEBUG, failures at ERROR
    Logger* sqlLog;

  public:
    pgsqlDB(xClient* bot, const std::string& dbHost, const unsigned short int dbPort,
            const std::string& dbName, const std::string& userName, const std::string& password);
    pgsqlDB(xClient* bot, const std::string& connectInfo);
    virtual ~pgsqlDB();

    /**
     * A failed statement is an ERROR record of "<module>.sql" naming the
     * function that ran it: "where" is defaulted, so it is the call site's,
     * and needs no argument of its own.  The "log" flag governs the DEBUG
     * record of the statement, not the report of a failure.
     */
    virtual bool Exec(const std::string&, bool log = true,
                      std::source_location where = std::source_location::current());
    virtual bool Exec(const std::stringstream&, bool log = true,
                      std::source_location where = std::source_location::current());
    virtual bool isConnected() const;

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
