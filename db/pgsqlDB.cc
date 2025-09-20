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

#include	<iostream>
#include	<exception>

#include	"ELog.h"
#include	"pgsqlDB.h"

namespace gnuworld
{
using std::endl ;
using std::ends ;
using std::string ;
using std::stringstream ;

pgsqlDB::pgsqlDB( xClient* _bot,
	const string& dbHost,
	const unsigned short int dbPort,
	const string& dbName,
	const string& userName,
	const string& password )
: gnuworldDB( dbHost, dbPort, dbName, userName, password ),
  bot( _bot ),
  connectInfo( "host=" + dbHost +
               " dbname=" + dbName +
               " port=" + std::to_string(dbPort) +
               (!userName.empty() ? " user=" + userName : "") +
               (!password.empty() ? " password=" + password : "" ) ),
  dbConn( std::make_unique<pqxx::connection>( connectInfo ) )
{
/* Save the conenctInfo string. */
try
	{
	if( !dbConn || !dbConn->is_open() )
		{
		elog	<< "pgsqlDB> Failed to connect to db: "
			<< ErrorMessage()
			<< endl ;
		}
	}
catch( const std::exception& e)
	{
	elog << "pgsqlDB> " << e.what() << endl;
	}
}

pgsqlDB::pgsqlDB( xClient* _bot, const string& connInfo )
	: bot(_bot), connectInfo( connInfo ), dbConn( std::make_unique<pqxx::connection>( connInfo ) )
{
if( !dbConn || !dbConn->is_open() )
	{
	elog	<< "pgsqlDB> Failed to connect to db: "
		<< ErrorMessage()
		<< endl ;
	}
}

pgsqlDB::~pgsqlDB()
{
}

bool pgsqlDB::Exec( const string& theQuery, bool log )
{
/* Log query. */
if (log)
    bot->getLogger()->write( SQL, theQuery ) ;

/* Check if the connection has been lost, and attempt to reconnect. */
if( !dbConn || !dbConn->is_open() )
	{
	elog << "pgsqlDB::Exec> Connection lost. Attempting to reconnect..." << std::endl ;
	if( !reconnect() )
		{
		elog << "pgsqlDB::Exec> Failed to reconnect to database." << std::endl ;
		return false ;
		}
	}

/* Attempt to execute the query. */
try
	{
	pqxx::work txn{ *dbConn } ;
	lastResult = std::make_unique<pqxx::result>( txn.exec( theQuery ) ) ;
	txn.commit() ;

	// Clear the error message.
	errorMsg.clear() ;

        return true ;
	}
catch( const pqxx::broken_connection& bc)
	{
	errorMsg = std::string("Database connection lost: ") + bc.what() ;
	elog << "pgsqlDB> " << errorMsg << endl ;
	return false ;
	}
catch( const pqxx::sql_error& sqlEx )
	{
	errorMsg = std::string("SQL Error: ") + sqlEx.what() ;
	elog << "pgsqlDB> " << errorMsg << endl ;
	return false ;
	}
catch( const std::exception& e )
	{
	errorMsg = std::string("General Error: ") + e.what() ;
	elog << "pgsqlDB> " << errorMsg << endl ;
	return false ;
	}
}

bool pgsqlDB::Exec( const stringstream& theQuery, bool log )
{
return Exec( theQuery.str(), log ) ;
}

unsigned int pgsqlDB::countTuples() const
{
return lastResult ? lastResult->size() : 0 ;
}

unsigned int pgsqlDB::affectedRows() const
{
if( !lastResult )
	return 0 ;

return lastResult->affected_rows() ;
}

const string pgsqlDB::ErrorMessage() const
{
if( !errorMsg.empty() )
	return errorMsg ;

if( !dbConn || !dbConn->is_open() )
	return "Connection not open" ;

return "No error" ;
}

const string pgsqlDB::GetValue( int rowNumber,
				int columnNumber ) const
{
if (!lastResult || rowNumber >= static_cast<int>( lastResult->size() )
	|| columnNumber >= static_cast<int>( lastResult->columns() ) )
	return "" ;

// Check if the field is NULL
const pqxx::field field = (*lastResult)[rowNumber][columnNumber];
if( field.is_null() )
	return "" ;

return field.as<std::string>() ;
}

const string pgsqlDB::GetValue( int rowNumber,
				const string& columnName ) const
{
if (!lastResult || rowNumber >= static_cast<int>( lastResult->size() ) )
	return "" ;

// Get column index from the column name
pqxx::row_size_type columnNumber = lastResult->column_number( columnName.c_str() ) ;

if( columnNumber == pqxx::row_size_type(-1) )
	return "" ;

// Check if the field is NULL
const pqxx::field field = (*lastResult)[rowNumber][columnNumber];
if( field.is_null() )
	return "" ;

return field.as<std::string>() ;
}

bool pgsqlDB::isConnected() const
{
if( !dbConn)
	return false ;
return dbConn->is_open() ;
}

bool pgsqlDB::reconnect()
{
try
	{
	if( !isConnected() )
		{
		elog	<< "pgsqlDB::reconnect()> Reconnecting to the database ... " << endl ;
		dbConn = std::make_unique< pqxx::connection >( connectInfo ) ;
		elog << "pgsqlDB::reconnect()> Successfully reconnected to the database." << endl ;
		errorMsg.clear() ;
		return true ;
		}
	}
catch( const std::exception& e )
	{
		errorMsg = std::string("Failed to reconnect to the database: ") + e.what() ;
		elog << errorMsg << endl ;
		return false;
	}

return false ;
}

} // namespace gnuworld
