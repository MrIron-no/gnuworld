/**
 * LogRecord.h
 * The typed record the logging system passes from a log statement to its
 * sinks, and the names of the verbosity levels.
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

#ifndef __LOGRECORD_H
#define __LOGRECORD_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gnuworld {

/**
 * Verbosity levels for the logging system.
 * Higher numbers are more verbose: a record of level v passes a threshold t
 * when v <= t.  A database statement is not a level of its own but a DEBUG
 * record of the logger "<module>.sql".
 */
enum Verbosity {
    OFF = 0,   // Nothing at all
    FATAL = 1, // Critical errors, the process exits right after
    ERROR = 2, // An operation failed
    WARN = 3,  // A protocol or state anomaly that is survived
    INFO = 4,  // Lifecycle messages
    DEBUG = 5, // Debug information for development
    TRACE = 6  // Most verbose - per message, per line, per loop iteration
};

/**
 * A single value carried by a log record.  Values are typed so that the JSON
 * formatter can quote strings only, and the text formatters can render every
 * type the same way.
 */
using LogValue =
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string>;

/**
 * One key/value pair of a record.  A display-only field is available to the
 * message template but is not emitted in JSON.
 */
struct LogField {
    std::string key;
    LogValue value;
    bool displayOnly = false;
};

/**
 * What an extractor makes of an object: the form shown in the rendered
 * sentence, and the sub-fields emitted in JSON as <key>_<sub>.
 */
struct LogObject {
    std::string display;
    std::vector<std::pair<std::string, LogValue>> fields;
};

/**
 * A [begin,end) byte range of a rendered message that came from a
 * substitution, so that sinks can highlight it.
 */
struct LogSpan {
    std::size_t begin;
    std::size_t end;
};

/**
 * Everything a sink is given about one log statement.
 */
struct LogRecord {
    std::chrono::system_clock::time_point time;
    Verbosity level;
    std::string logger;
    std::string function;
    std::string message;
    std::vector<LogSpan> spans;
    std::vector<LogField> context;
    std::vector<LogField> fields;

    /**
     * True for a record that must not leave this machine, such as an SQL
     * statement: the logger gives it to no sink that says leavesTheHost(),
     * whatever logging.conf routes where and at whatever level.
     */
    bool localOnly = false;
};

/**
 * Renders a value the way templates and the text formatters show it.
 * A null value is "(null)", a bool "true"/"false", a string itself.
 */
inline std::string logValueToString(const LogValue& value) {
    struct Visitor {
        std::string operator()(std::monostate) const { return "(null)"; }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(std::int64_t i) const { return std::to_string(i); }
        std::string operator()(std::uint64_t u) const { return std::to_string(u); }
        std::string operator()(double d) const { return std::format("{}", d); }
        std::string operator()(const std::string& s) const { return s; }
    };

    return std::visit(Visitor(), value);
}

/**
 * The name of a level as it appears in a JSON record.
 */
inline const char* levelName(Verbosity level) {
    switch (level) {
    case FATAL:
        return "FATAL";
    case ERROR:
        return "ERROR";
    case WARN:
        return "WARNING";
    case INFO:
        return "INFO";
    case DEBUG:
        return "DEBUG";
    case TRACE:
        return "TRACE";
    case OFF:
        return "OFF";
    }

    return "OFF";
}

/**
 * The name of a level padded to the five characters of the text column.
 */
inline const char* levelColumn(Verbosity level) {
    switch (level) {
    case FATAL:
        return "FATAL";
    case ERROR:
        return "ERROR";
    case WARN:
        return "WARN ";
    case INFO:
        return "INFO ";
    case DEBUG:
        return "DEBUG";
    case TRACE:
        return "TRACE";
    default:
        return "     ";
    }
}

/**
 * The short tag a level gets on IRC.
 */
inline const char* levelTag(Verbosity level) {
    switch (level) {
    case FATAL:
        return "[F]";
    case ERROR:
        return "[E]";
    case WARN:
        return "[W]";
    case INFO:
        return "[I]";
    case DEBUG:
        return "[D]";
    case TRACE:
        return "[T]";
    default:
        return "[ ]";
    }
}

/**
 * Parses a level name from a configuration file, case-insensitively.
 * Both WARN and WARNING are accepted.
 * Returns false and leaves the level untouched when the name is not one.
 */
inline bool parseLevel(const std::string& name, Verbosity& level) {
    std::string upper;
    upper.reserve(name.size());
    for (const char c : name)
        upper += static_cast<char>((c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c);

    if ("OFF" == upper)
        level = OFF;
    else if ("FATAL" == upper)
        level = FATAL;
    else if ("ERROR" == upper)
        level = ERROR;
    else if ("WARN" == upper || "WARNING" == upper)
        level = WARN;
    else if ("INFO" == upper)
        level = INFO;
    else if ("DEBUG" == upper)
        level = DEBUG;
    else if ("TRACE" == upper)
        level = TRACE;
    else
        return false;

    return true;
}

} // namespace gnuworld

#endif // __LOGRECORD_H
