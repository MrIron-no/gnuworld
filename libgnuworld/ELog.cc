/**
 * ELog.cc
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
 * $Id: ELog.cc,v 1.8 2005/02/20 15:49:21 dan_karrels Exp $
 */

#include	<iostream>
#include	<fstream>
#include	<sstream>
#include	<string>
#include	<ctime>
#include	"ELog.h"

namespace gnuworld
{

using std::stringstream ;
using std::string ;
using std::endl ;

// Instantiate the global instance
ELog	elog ;

ELog::ELog()
 : outStream( 0 ),
   logFile( false ),
   newline(true)
{}

ELog::ELog( const string& fileName )
 : outStream( 0 ),
   logFile( false ),
   newline(true)
{
openFile( fileName ) ;
}

ELog::~ELog()
{
if( logFile )
	{
 	closeFile() ;
	}
}

bool ELog::openFile( const string& fileName )
{
bool isAlreadyOpen = false;
if( isOpen() )
	{
	isAlreadyOpen = true;
	closeFile() ;
	}
logFile = true ;
if (isAlreadyOpen)
	outFile.open( fileName.c_str(), std::ios::out | std::ios::app ) ;
else
	outFile.open( fileName.c_str(), std::ios::out | std::ios::trunc ) ;

if( !isOpen() )
	{
	logFile = false ;
	}
return isOpen() ;
}

void ELog::closeFile()
{
if( isOpen() )
	{
	outFile << endl ;
	outFile.close() ;
	}
}

ELog& ELog::operator<<( __E_omanip var )
{
if( logFile )
	{
	outFile	<< var ;
	}
if( outStream ) *outStream << var ;
newline = true;
return *this ;
}

ELog& ELog::operator<<( __E_manip var )
{
if( logFile )
	{
	outFile	<< var ;
	}
if( outStream ) *outStream << var ;
newline = true;
return *this ;
}

std::string ELog::getLocalTime()
{
	  time_t theTime;
	  time(&theTime);  /* get current time; same as: theTime = time(NULL)  */
	  struct tm* timeinfo = localtime(&theTime);
	  char buffer[20] = {0};

	  std::strftime(buffer,20,"%Y-%m-%d %H:%M:%S",timeinfo);
	  return string("[" + string(buffer) + "] ");
}

} // namespace gnuworld
