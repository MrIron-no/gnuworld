/**
 * LogSinks.h
 * The sinks that need nothing but the standard library: a file, the console,
 * and the vector of records the unit tests read.
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
 */

#ifndef __LOGSINKS_H
#define __LOGSINKS_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "LogRecord.h"
#include "LogSink.h"

namespace gnuworld {

/**
 * The settings the text sinks share.  The width of the logger name column is
 * one of them, because every sink has to agree on it for the columns to line
 * up; LogManager keeps it at the length of the longest registered logger name.
 */
class LogSinks {
  public:
    /// Sets the width of the logger name column of every text sink
    static void setNameWidth(std::size_t);

    /// The current width of the logger name column, 12 until it is set
    static std::size_t nameWidth();

  private:
    static std::atomic<std::size_t> nameWidthValue;
};

/**
 * A log file.  The file is opened for appending and never truncated, so that
 * restarting the process keeps the history; every record is one line, flushed
 * as it is written, so that a crash keeps what was logged before it.
 *
 * The lines are the JSON objects of formatJson() or the columns of
 * formatText(), never coloured: nothing reads a log file through a terminal.
 *
 * A log file holds addresses, accounts and the command lines of users, so a
 * file this sink creates is readable by its own user and its own group and by
 * nobody else - 0640, less whatever the umask takes away.  A file that is
 * already there keeps the mode it has: it is the operator's file, not ours.
 * The directory it sits in is the operator's business as well, and has to be
 * writable by this user alone.
 *
 * A file that cannot be opened is not an error the logging system reports to
 * anyone: emit() then does nothing at all and isOpen() is false.
 */
class FileSink : public LogSink {
  public:
    /**
     * Opens path for appending.  json picks the line format; fullDateText
     * asks the text format for the full date rather than the time of day.
     */
    FileSink(std::string path, bool json, bool fullDateText = true);

    ~FileSink() override;

    void emit(const LogRecord&) override;

    /// Closes the file and opens the same path again, for log rotation
    void reopen() override;

    /// True while the file is open for writing
    bool isOpen() const;

    /// The path the sink was given, which reopen() does not change
    const std::string& path() const { return filePath; }

  private:
    /// Opens filePath for appending, with the lock held
    void openLocked();

    /// Closes the file if it is open, with the lock held
    void closeLocked();

    std::string filePath;
    bool json;
    bool fullDateText;

    mutable std::mutex lock;

    /**
     * The file, or -1 while there is none.  It is a descriptor of its own
     * rather than a stream because a stream cannot say what mode to create a
     * file with, and because O_APPEND makes each write land whole even when
     * another process is appending to the same file.
     */
    int file;
};

/**
 * The terminal the process was started from.  Lines go to std::cout, as the
 * time of day rather than the full date, and may be coloured.
 *
 * All console sinks write under one lock, so that two of them configured at
 * different levels cannot interleave halves of their lines.
 */
class ConsoleSink : public LogSink {
  public:
    /**
     * Whether to colour the output: Auto asks the terminal, and is settled
     * once, in the constructor.
     */
    enum class Colour { Auto, Yes, No };

    /**
     * Auto means colour only when std::cout is a terminal and the environment
     * variable NO_COLOR is unset or empty.  highlight puts the substituted
     * values of a message in bold, and needs colour to show.
     */
    ConsoleSink(Colour colour, bool highlight);

    void emit(const LogRecord&) override;

    /**
     * Turns console logging off, or on again, for the whole process: that is
     * what the daemon does once it has detached from its terminal.  emit()
     * does nothing while it is off.
     */
    static void setEnabled(bool);

    /// True while the console is written to, which it is until it is not
    static bool enabled();

  private:
    bool colour;
    bool highlight;

    /**
     * Shared by every console sink: one line at a time on std::cout.  It is
     * allocated on first use and never destroyed, because a static destructor
     * elsewhere may still log while the process is on its way out.
     */
    static std::mutex& outputLock();
    static std::atomic<bool> enabledFlag;
};

/**
 * The sink that keeps the records it is given, and the reference example of
 * how to write a sink: take the lock, do the one thing, return.  The unit
 * tests attach one of these and read records afterwards.
 *
 * The records are copies, so the caller may reuse or destroy the record it
 * logged as soon as emit() returns.  Reading records while another thread is
 * still logging is the reader's problem, not the sink's; the tests join their
 * threads first.
 */
class CaptureSink : public LogSink {
  public:
    /// Everything this sink was given, oldest first
    std::vector<LogRecord> records;

    void emit(const LogRecord& record) override {
        const std::lock_guard<std::mutex> guard(lock);
        records.push_back(record);
    }

    /// Forgets every record kept so far
    void clear() {
        const std::lock_guard<std::mutex> guard(lock);
        records.clear();
    }

    /// How many records the sink is holding
    std::size_t size() const {
        const std::lock_guard<std::mutex> guard(lock);
        return records.size();
    }

  private:
    mutable std::mutex lock;
};

} // namespace gnuworld

#endif // __LOGSINKS_H
