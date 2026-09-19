/**
 * LogConfig.h
 * What logging.conf says: the sinks it names and what each of them is, and the
 * loggers it configures with a level, a list of those sinks, and whether their
 * records also walk up the hierarchy.
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

#ifndef __LOGCONFIG_H
#define __LOGCONFIG_H

#include <optional>
#include <string>
#include <vector>

#include "LogRecord.h"
#include "LogSinks.h"

namespace gnuworld {

/**
 * One named destination of logging.conf, as the file describes it: the settings
 * of every kind of sink there is, whether this one uses them or not.
 *
 * The type is a name and not an enumeration because the kinds of sink that exist
 * are not libgnuworld's to know: core registers the one that writes to a
 * channel, a notifier registers its own, and the registry of LogManager is what
 * turns a type name into a sink.
 */
struct SinkSpec {
    std::string id, type, path, channel;
    bool json = true;
    Verbosity level = TRACE;
    ConsoleSink::Colour colour = ConsoleSink::Colour::Auto;
    bool highlight = true;
};

/**
 * One logger of logging.conf.  The level is the one the "logger.<name>" line
 * asked for and nothing when the file only said something about the logger's
 * additivity; the sinks are the ids listed on that line, in the order they were
 * written; additive is what an "additivity.<name>" line asked for and nothing
 * when the file did not mention it, in which case the code's own answer stands.
 *
 * The root is the empty name, however it was written in the file.
 */
struct LoggerSpec {
    std::string name;
    std::optional<Verbosity> level;
    std::vector<std::string> sinks;
    std::optional<bool> additive;
};

/**
 * A whole logging.conf: its sinks by id, its loggers by name, both in a stable
 * order so that two reads of the same file give the same thing.
 */
struct LogConfig {
    std::vector<SinkSpec> sinks;
    std::vector<LoggerSpec> loggers;
};

/**
 * Reads logging.conf.
 *
 * The file is accepted as a whole or not at all: every problem found is added to
 * errors as one sentence naming the key it is about, and a file with any problem
 * at all leaves out empty and returns false, so that a configuration is never
 * half applied.  A file that is not there is such a problem, not a crash: this
 * never terminates the process and never throws.
 *
 * The file is read the way every editor writes it: the '\r' of a Windows line
 * ending is not part of a value, and a byte order mark in front of the first key
 * is not part of that key.
 */
bool parseLogConfig(const std::string& fileName, LogConfig& out, std::vector<std::string>& errors);

/**
 * The text with every control character written as "\xNN".
 *
 * What an error message quotes came out of a file, and a file may hold anything:
 * an escape sequence, a carriage return, a colour code.  Escaping it is what
 * keeps a message about a configuration from painting a terminal, breaking a log
 * line in two or carrying a control character into a channel.
 */
std::string escapeControl(const std::string& text);

} // namespace gnuworld

#endif // __LOGCONFIG_H
