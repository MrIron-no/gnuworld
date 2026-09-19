/**
 * LogFormat.h
 * The formatters that turn a log record into the line a sink writes: one
 * JSON object, one human-readable text line, or one IRC notice.
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

#ifndef __LOGFORMAT_H
#define __LOGFORMAT_H

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "LogRecord.h"

namespace gnuworld {

/**
 * One record as a single JSON object, without a trailing newline.
 *
 * The keys are, in this order: "timestamp" (the current UTC stamp of
 * getCurrentTimestamp()), "level", "logger", "function" (only when it is not
 * empty), the record's context fields, the record's fields in insertion
 * order, and "message".  A field marked displayOnly is left out: it exists
 * for the message template, not for the machines reading this line.
 *
 * Values keep their type: null, true/false, decimal integers and doubles are
 * unquoted, strings are quoted and escaped.  A double that is NaN or
 * infinite is emitted as null, which is the nearest thing JSON has.
 */
std::string formatJson(const LogRecord&);

/**
 * How a text sink wants its lines: the console shows the time of day and may
 * use colour, a file shows the full date and never does.
 */
struct TextStyle {
    bool fullDate;         // YYYY-MM-DD HH:MM:SS.mmm instead of HH:MM:SS.mmm
    bool colour;           // ANSI colour, for a terminal
    bool highlight;        // substituted values in bold (needs colour)
    std::size_t nameWidth; // width of the logger name column
};

/**
 * One record as a human-readable line, without a trailing newline:
 *
 *   <time>  <LEVEL>  <name>  <message>[  (<function>)]
 *
 * Columns are separated by two spaces.  The time is local.  A logger name
 * longer than the column keeps its last nameWidth - 1 characters behind a
 * '~'; the root logger prints as "root".  The function is left out at INFO
 * and when it is empty, and follows the last line of the message.
 * Continuation lines of a multi-line message are indented to the message
 * column.
 *
 * Every C0 control character of the message other than the newline, 0x7f,
 * and each byte of a C1 control - U+0080 to U+009F, which a terminal reads as
 * an escape of its own - is written as "\xNN", so that a field value cannot
 * move the cursor or set a colour of its own.  Every other byte of 0x80 and
 * above is text and passes.  The name and the function are cleaned the same
 * way before they are cut, padded and coloured, the newline included: those
 * two columns hold one line each.
 *
 * In colour, the name takes its palette entry from the first segment of the
 * logger's own name, so that a module and all of its sub-loggers share a hue
 * however much the column had to cut away; the printed text at or after the
 * first '.' of that name is additionally dim.
 */
std::string formatText(const LogRecord&, const TextStyle&);

/**
 * The lines of a record's message as an IRC sink wants them: split at the
 * newlines, every control character removed - the C1 range U+0080 to U+009F
 * along with the C0 one - the record's spans re-based on each line (a span
 * crossing a line break is clipped to both), and the lines that are empty
 * once cleaned dropped.
 */
std::vector<std::pair<std::string, std::vector<LogSpan>>> splitLines(const LogRecord&);

/**
 * One line of a record as an IRC notice:
 *
 *   <col>[<name>] <tag> <func-prefix><line><reset>
 *
 * The colour is mIRC 04 for FATAL and ERROR, 07 for WARN and nothing else;
 * the reset is only written when a colour was.  The function prefix
 * "<function>> " is written at every level but INFO, and only when the
 * function is not empty.  When highlight is set, each span of the line is
 * wrapped in the bold control character.
 *
 * The line and its spans are what splitLines() returned, so the line carries
 * no control character of its own.  Every control character and 0x7f of the
 * name and of the function is removed here for the same reason; a function
 * that is empty once cleaned gets no prefix.
 */
std::string formatIrcLine(const LogRecord&, std::string_view line,
                          const std::vector<LogSpan>& lineSpans, bool highlight);

/**
 * The readable name of a function from __PRETTY_FUNCTION__: the return type,
 * the parameters and every namespace but the innermost are dropped, leaving
 * "Class::method".
 */
std::string parseFunction(std::string pretty);

} // namespace gnuworld

#endif // __LOGFORMAT_H
