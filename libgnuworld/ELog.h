/**
 * ELog.h
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
 * $Id: ELog.h,v 1.8 2005/02/20 15:49:21 dan_karrels Exp $
 */

#ifndef __ELOG_H
#define __ELOG_H "$Id: ELog.h,v 1.8 2005/02/20 15:49:21 dan_karrels Exp $"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <time.h>

namespace gnuworld {

/**
 * DEPRECATED.  The front end the thousand "elog << ... << endl" statements of
 * this tree still write through, kept source compatible so that every one of
 * them compiles, and logs, unchanged.
 *
 * This class writes nothing itself any more.  A line is collected as it is
 * streamed and completed by std::endl, and every completed line becomes one
 * record of level DEBUG on the logger "legacy": where it is then written, and
 * whether it is written at all, is what logging.conf says about that logger, so
 * that "logger.legacy = INFO" silences elog altogether.  The line carries no
 * function and no fields, because nothing in the statement says what they
 * would be.
 *
 * New code does not use this class.  It logs through LOG and LOG_MSG, which
 * name the logger of the module they stand in and carry a level, a function and
 * typed fields; see doc/README.logger.md.
 */
class ELog {

  protected:
    typedef std::ostream& (*__E_omanip)(std::ostream&);
    typedef std::ostream& (*__E_manip)(std::ios&);

    /**
     * The stream setStream() was given, which nothing is written to: what a
     * stream used to mean, "these lines are seen on the terminal as well", is
     * the console sink of the logging system now, which setStream() turns on
     * or off to match.  Core decides its own std::cerr fallback by asking
     * ConsoleSink::enabled() directly; this member is kept only so that
     * getStream() still answers what was last passed in.
     */
    std::ostream* outStream;

    /// Whether openFile() has been called, which is what isOpen() answers
    bool fileOpened;

    /**
     * The line the calling thread has streamed so far, and nothing of any other
     * thread.  It is declared here, and defined in ELog.cc, so that this header
     * needs nothing of the logging system: it is included nearly everywhere.
     */
    static std::ostringstream& buffer();

    /// Hands the line the buffer holds to the logger, and empties the buffer
    static void emit();

  public:
    /**
     * Instantiate an instance of this class.  No log file is asked for, and no
     * output stream is specified.
     */
    ELog();

    /**
     * Instantiate an instance of this class, specifying the name of the file to
     * which to log messages, as openFile() does.
     */
    ELog(const std::string&);

    /**
     * Destroy an instance of this class.  Nothing is logged, and nothing is
     * closed: the sinks of the logging system outlive every instance of this.
     */
    virtual ~ELog();

    /**
     * Ask for a log file of this name, which the logging system gives the root
     * logger and appends to - it is never truncated, so that restarting the
     * process keeps the history.  Asking again for the same path adds no second
     * file and reopens the one there is, which is how a SIGHUP after logrotate
     * moved the file away lands the next line in a new one.
     *
     * This always succeeds: a file that will not open is reported by the
     * logging system, and the lines still reach every other sink.
     */
    bool openFile(const std::string& fileName);

    /**
     * Nothing to close: the log file belongs to the logging system, which keeps
     * it for as long as the process lives.
     */
    void closeFile();

    /**
     * Return true if a log file has been asked for, false otherwise.
     */
    inline bool isOpen() { return fileOpened; }

    /**
     * Use this method to specify which stream to which to log messages.
     * Specifying NULL turns the console off, and a stream turns it on: the
     * stream itself is not written to, the console sink is.
     */
    void setStream(std::ostream* newStream);

    /**
     * Get the stream to which to log messages.
     */
    inline std::ostream* getStream() { return outStream; }

    /*
     * Get local time in [hh:mm:ss] format
     */
    std::string getLocalTime();

    /**
     * Output the endl function.
     */
    ELog& operator<<(__E_omanip func);

    /**
     * Output the endl function.
     */
    ELog& operator<<(__E_manip func);

    /**
     * Output any other type supported by std::ostream.
     */
    template <typename T> ELog& operator<<(const T& var) {
        buffer() << var;
        return *this;
    }
};

#ifndef GNUWORLD_NO_ELOG

/// The global logging instance.
extern ELog elog;

#endif // GNUWORLD_NO_ELOG

} // namespace gnuworld

#endif // __ELOG_H
