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

/* This is the file that defines the global elog, so here it is always declared,
 * whatever a translation unit that wants nothing to do with it has defined */
#undef GNUWORLD_NO_ELOG

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <ctime>

#include "ELog.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogSinks.h"
#include "logger.h"

namespace gnuworld {

using std::string;

// Instantiate the global instance
ELog elog;

namespace {

/**
 * The line of the calling thread, on the heap, and the small object that ends
 * its life with the thread.
 *
 * elog is streamed to from the worker threads, and - this is the awkward part -
 * from static destructors and atexit handlers, that is from a thread whose
 * thread_local objects may already have been destroyed.  A plain
 * "thread_local std::ostringstream" would then be used after its destructor
 * ran, which nothing would report and which nothing here can rule out: this is
 * a class a thousand statements write through, and the last of them runs on the
 * way out of the process.
 *
 * So the buffer lives on the heap, a thread_local pointer of a trivial type -
 * which has no destructor to run and no order to be destroyed in - finds it, and
 * a thread_local holder deletes it and forgets it when the thread ends.  Past
 * that point bufferGone says so, and the holder is never touched again: a line
 * streamed then is given a buffer nothing owns and nothing frees, one small leak
 * in a process that is exiting anyway, in exchange for a line that is logged
 * rather than a crash.
 */
thread_local std::ostringstream* threadBuffer = nullptr;

/// Whether the holder of this thread has already deleted the thread's buffer
thread_local bool bufferGone = false;

/// Deletes the buffer of its thread, and marks the thread as past that point
class BufferHolder {
  public:
    /// Hands the thread the buffer it will stream into
    BufferHolder() { threadBuffer = new std::ostringstream(); }

    ~BufferHolder() {
        std::ostringstream* const spent = threadBuffer;

        threadBuffer = nullptr;
        bufferGone = true;

        delete spent;
    }
};

/**
 * The logger every elog line becomes a record on, found once.
 *
 * DEBUG is the level elog always had, and the level it has unless logging.conf
 * says otherwise: the INFO the root logs at would drop every line of it.  The
 * pointer is a function-local static, so the lookup and the default happen once,
 * thread-safely, and nothing of this is destroyed on the way out of the process -
 * the registry of the loggers outlives every static destructor that may log.
 */
Logger* legacyLogger() {
    static Logger* const theLogger = []() {
        LogManager::setCodeDefault("legacy", DEBUG);

        return LogManager::get("legacy");
    }();

    return theLogger;
}

} // anonymous namespace

ELog::ELog() : outStream(0), fileOpened(false) {}

ELog::ELog(const string& fileName) : outStream(0), fileOpened(false) { openFile(fileName); }

ELog::~ELog() {}

std::ostringstream& ELog::buffer() {
    if (nullptr != threadBuffer)
        return *threadBuffer;

    if (!bufferGone) {
        // The first line of this thread: constructing the holder gives it a
        // buffer, and the holder will delete it when the thread ends
        static thread_local BufferHolder holder;

        if (nullptr != threadBuffer)
            return *threadBuffer;
    }

    // Past the end of this thread's thread_locals: a buffer nobody owns
    threadBuffer = new std::ostringstream();

    return *threadBuffer;
}

void ELog::emit() {
    std::ostringstream& text = buffer();
    string message = text.str();

    /* The line is taken out of the buffer before anything else may happen to
     * it: a line that is dropped must not turn up in front of the next one.
     * Only the text is emptied - a format state the caller set with std::hex
     * belongs to the thread until it sets it back, exactly as it would on a
     * real stream */
    text.str(string());

    /* An endl contributes no newline of its own, but a caller may have streamed
     * one, and the sinks end the line themselves */
    if (!message.empty() && '\n' == message[message.size() - 1])
        message.erase(message.size() - 1);

    if (message.empty())
        return;

    Logger* const legacy = legacyLogger();

    if (!legacy->shouldLog(DEBUG))
        return;

    LogRecord record;
    record.level = DEBUG;
    record.message = std::move(message);

    legacy->log(std::move(record));
}

bool ELog::openFile(const string& fileName) {
    /* The first call gives the root logger a log file at this path, which is
     * what the file always was.  A later call is core reopening the file on
     * SIGHUP, after logrotate moved it away: the path is attached already, and
     * what is wanted is every sink opening its path anew */
    const bool again = fileOpened;

    fileOpened = true;

    LogManager::bootstrapFile(fileName);

    if (again)
        LogManager::reopenAll();

    return true;
}

void ELog::closeFile() {}

void ELog::setStream(std::ostream* newStream) {
    outStream = newStream;

    ConsoleSink::setEnabled(nullptr != newStream);
}

ELog& ELog::operator<<(__E_omanip var) {
    /* The manipulator is compared as a function pointer, the way
     * Logger::LoggerStream does: std::endl completes the line, std::flush has
     * nothing left to flush, and anything else is a manipulator the buffer
     * itself should see */
    if (var == static_cast<__E_omanip>(std::endl)) {
        emit();

        return *this;
    }

    if (var == static_cast<__E_omanip>(std::flush))
        return *this;

    var(buffer());

    return *this;
}

ELog& ELog::operator<<(__E_manip var) {
    var(buffer());

    return *this;
}

std::string ELog::getLocalTime() {
    time_t theTime;
    time(&theTime); /* get current time; same as: theTime = time(NULL)  */
    struct tm* timeinfo = localtime(&theTime);
    char buffer[20] = {0};

    std::strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", timeinfo);
    return string("[" + string(buffer) + "] ");
}

} // namespace gnuworld
