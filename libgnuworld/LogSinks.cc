/**
 * LogSinks.cc
 * The file and console sinks of the logging system.
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

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "LogFormat.h"
#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"

namespace gnuworld {

namespace {

/**
 * What Colour::Auto settles on: colour is worth writing only when std::cout
 * still goes to a terminal, and only when the user has not asked for none.
 */
bool colourWanted() {
    if (0 == ::isatty(STDOUT_FILENO))
        return false;

    const char* const noColour = std::getenv("NO_COLOR");

    return nullptr == noColour || '\0' == noColour[0];
}

} // namespace

/* ------------------------------------------------------------------ *
 * The settings the text sinks share
 * ------------------------------------------------------------------ */

std::atomic<std::size_t> LogSinks::nameWidthValue(12);

void LogSinks::setNameWidth(std::size_t width) { nameWidthValue.store(width); }

std::size_t LogSinks::nameWidth() { return nameWidthValue.load(); }

/* ------------------------------------------------------------------ *
 * FileSink
 * ------------------------------------------------------------------ */

FileSink::FileSink(std::string path, bool json, bool fullDateText)
    : filePath(std::move(path)), json(json), fullDateText(fullDateText), file(-1) {
    const std::lock_guard<std::mutex> guard(lock);

    openLocked();
}

FileSink::~FileSink() {
    const std::lock_guard<std::mutex> guard(lock);

    closeLocked();
}

/**
 * The mode a log file this sink creates is given: its own user and its own
 * group, and nobody else.  A file that already exists keeps its own mode, as
 * ::open() leaves the mode of a file it did not create alone.
 */
void FileSink::openLocked() {
    file = ::open(filePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
}

void FileSink::closeLocked() {
    if (-1 == file)
        return;

    ::close(file);
    file = -1;
}

void FileSink::emit(const LogRecord& record) {
    // The whole line at once, so that O_APPEND lands it in one piece
    std::string line =
        json ? formatJson(record)
             : formatText(record, TextStyle{fullDateText, false, false, LogSinks::nameWidth()});

    line += '\n';

    const std::lock_guard<std::mutex> guard(lock);

    if (-1 == file)
        return;

    /* A write of a pipe or a file on a full disk may stop short, and a signal
     * may interrupt one before a byte of it is written.  Anything else is a
     * file this sink can do nothing more with, and says nothing about */
    std::string::size_type written = 0;

    while (written < line.size()) {
        const ssize_t count = ::write(file, line.data() + written, line.size() - written);

        if (count > 0) {
            written += static_cast<std::string::size_type>(count);
            continue;
        }

        if (-1 == count && EINTR == errno)
            continue;

        return;
    }
}

void FileSink::reopen() {
    const std::lock_guard<std::mutex> guard(lock);

    closeLocked();
    openLocked();
}

bool FileSink::isOpen() const {
    const std::lock_guard<std::mutex> guard(lock);

    return -1 != file;
}

/* ------------------------------------------------------------------ *
 * ConsoleSink
 * ------------------------------------------------------------------ */

std::atomic<bool> ConsoleSink::enabledFlag(true);

std::mutex& ConsoleSink::outputLock() {
    static std::mutex* const lock = new std::mutex();

    return *lock;
}

ConsoleSink::ConsoleSink(Colour colour, bool highlight)
    : colour(Colour::Auto == colour ? colourWanted() : Colour::Yes == colour),
      highlight(highlight) {}

void ConsoleSink::emit(const LogRecord& record) {
    if (!enabledFlag.load())
        return;

    // The console shows the time of day; the date is the log file's business
    const std::string line =
        formatText(record, TextStyle{false, colour, highlight, LogSinks::nameWidth()});

    const std::lock_guard<std::mutex> guard(outputLock());

    std::cout << line << '\n';
    std::cout.flush();
}

void ConsoleSink::setEnabled(bool enable) { enabledFlag.store(enable); }

bool ConsoleSink::enabled() { return enabledFlag.load(); }

} // namespace gnuworld
