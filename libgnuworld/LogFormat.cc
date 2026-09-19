/**
 * LogFormat.cc
 * The JSON, text and IRC formatters of the logging system.
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "LogFormat.h"
#include "LogRecord.h"
#include "misc.h"

namespace gnuworld {

namespace {

/// The ANSI sequence that puts a terminal back the way it was
const char* const ansiReset = "\x1b[0m";

/// The colours a logger name is picked from
const int namePalette[6] = {36, 35, 34, 32, 33, 96};

/// The lower-case hexadecimal digits of an escaped control character
const char hexDigits[] = "0123456789abcdef";

/// The name a logger shows: the root logger has none of its own
std::string displayName(const std::string& logger) {
    return logger.empty() ? std::string("root") : logger;
}

/**
 * Whether the two bytes at this position are the UTF-8 encoding of a C1
 * control, U+0080 to U+009F.
 *
 * A terminal reads those as escapes of its own: U+009B is what ESC '[' means,
 * so a field value carrying it paints a terminal exactly as "\x1b[" would.
 * Every other byte of 0x80 and above is text, U+00A0 - the no-break space,
 * whose second byte is 0xa0 - included.
 */
bool isC1Pair(const std::string& text, std::size_t at) {
    if (at + 1 >= text.size() || 0xc2 != static_cast<unsigned char>(text[at]))
        return false;

    const unsigned char second = static_cast<unsigned char>(text[at + 1]);

    return second >= 0x80 && second <= 0x9f;
}

/// Appends one byte written as "\xNN"
void appendHexEscape(std::string& out, unsigned char byte) {
    out += "\\x";
    out += hexDigits[(byte >> 4) & 0x0f];
    out += hexDigits[byte & 0x0f];
}

/* ------------------------------------------------------------------ *
 * JSON
 * ------------------------------------------------------------------ */

/// Appends a quoted, escaped JSON string
void appendJsonString(std::string& out, const std::string& text) {
    out += '"';
    out += escapeJsonString(text);
    out += '"';
}

/// Appends a value with the JSON type it has: only strings are quoted
void appendJsonValue(std::string& out, const LogValue& value) {
    struct Visitor {
        std::string& out;

        void operator()(std::monostate) const { out += "null"; }
        void operator()(bool b) const { out += b ? "true" : "false"; }
        void operator()(std::int64_t i) const { out += std::to_string(i); }
        void operator()(std::uint64_t u) const { out += std::to_string(u); }
        void operator()(double d) const {
            // JSON has neither NaN nor infinity
            out += std::isfinite(d) ? std::format("{}", d) : "null";
        }
        void operator()(const std::string& s) const { appendJsonString(out, s); }
    };

    std::visit(Visitor{out}, value);
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/// The local time of a record: HH:MM:SS.mmm, with the date in front when the
/// sink asked for it
std::string formatTime(const std::chrono::system_clock::time_point& when, bool fullDate) {
    using namespace std::chrono;

    const auto secs = time_point_cast<seconds>(when);
    const auto ms = duration_cast<milliseconds>(when - secs).count();

    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm broken{};
    localtime_r(&tt, &broken);

    std::ostringstream out;
    out << std::put_time(&broken, fullDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << ms;

    return out.str();
}

/// A message with its control characters made printable, and the spans that
/// moved along with the text
struct EscapedText {
    std::string text;
    std::vector<LogSpan> spans;
};

/// Writes every C0 control character but the newline, 0x7f, and the two bytes
/// of a C1 control, as "\xNN", so that a field value cannot inject a terminal
/// escape.  Every other byte of 0x80 and above is left alone: the terminal's
/// encoding is not ours to guess.
EscapedText escapeControls(const std::string& message, const std::vector<LogSpan>& spans) {
    EscapedText result;
    result.text.reserve(message.size());

    // Where each byte of the message ended up, one entry past the end
    std::vector<std::size_t> offset(message.size() + 1, 0);

    for (std::size_t i = 0; i < message.size(); ++i) {
        offset[i] = result.text.size();

        const unsigned char c = static_cast<unsigned char>(message[i]);

        // Both bytes of the pair are escaped, and each keeps an offset of its
        // own, so that a span beginning or ending between them still lands
        if (isC1Pair(message, i)) {
            appendHexEscape(result.text, c);
            offset[i + 1] = result.text.size();
            appendHexEscape(result.text, static_cast<unsigned char>(message[i + 1]));
            ++i;
            continue;
        }

        if ('\n' == c || (c >= 0x20 && 0x7f != c)) {
            result.text += static_cast<char>(c);
            continue;
        }

        appendHexEscape(result.text, c);
    }
    offset[message.size()] = result.text.size();

    for (const LogSpan& span : spans) {
        const std::size_t begin = std::min(span.begin, message.size());
        const std::size_t end = std::min(std::max(span.end, begin), message.size());
        if (begin < end)
            result.spans.push_back(LogSpan{offset[begin], offset[end]});
    }

    return result;
}

/// One line of an escaped message, pointing into it, with the spans of that
/// line rebased on its own first byte
struct TextLine {
    std::string_view text;
    std::vector<LogSpan> spans;
};

/// Splits escaped text at its newlines, clipping each span to the lines it
/// runs through.  Empty lines are kept: the text formatters print them.
std::vector<TextLine> splitEscaped(const EscapedText& escaped) {
    std::vector<TextLine> lines;
    std::size_t start = 0;

    for (;;) {
        const std::size_t newline = escaped.text.find('\n', start);
        const std::size_t end = (std::string::npos == newline) ? escaped.text.size() : newline;

        TextLine line;
        line.text = std::string_view(escaped.text).substr(start, end - start);
        for (const LogSpan& span : escaped.spans) {
            const std::size_t begin = std::max(span.begin, start);
            const std::size_t stop = std::min(span.end, end);
            if (begin < stop)
                line.spans.push_back(LogSpan{begin - start, stop - start});
        }
        lines.push_back(std::move(line));

        if (std::string::npos == newline)
            break;
        start = newline + 1;
    }

    return lines;
}

/// The text with each span wrapped in open ... close.  Spans are clamped to
/// the text and merged where they overlap, so that the wrapping never nests;
/// two spans that merely touch stay two.
std::string wrapSpans(std::string_view text, const std::vector<LogSpan>& spans,
                      std::string_view open, std::string_view close) {
    std::vector<LogSpan> ranges;
    ranges.reserve(spans.size());
    for (const LogSpan& span : spans) {
        const std::size_t begin = std::min(span.begin, text.size());
        const std::size_t end = std::min(std::max(span.end, begin), text.size());
        if (begin < end)
            ranges.push_back(LogSpan{begin, end});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const LogSpan& a, const LogSpan& b) { return a.begin < b.begin; });

    std::vector<LogSpan> merged;
    merged.reserve(ranges.size());
    for (const LogSpan& range : ranges) {
        if (!merged.empty() && range.begin < merged.back().end)
            merged.back().end = std::max(merged.back().end, range.end);
        else
            merged.push_back(range);
    }

    std::string out;
    out.reserve(text.size() + merged.size() * (open.size() + close.size()));

    std::size_t pos = 0;
    for (const LogSpan& range : merged) {
        out.append(text.substr(pos, range.begin - pos));
        out.append(open);
        out.append(text.substr(range.begin, range.end - range.begin));
        out.append(close);
        pos = range.end;
    }
    out.append(text.substr(pos));

    return out;
}

/// Writes every C0 control character, the newline included, 0x7f, and the two
/// bytes of a C1 control, as "\xNN".  The name and the function are
/// single-line columns, so a newline has no meaning of its own there and is
/// escaped along with the rest.
std::string escapeField(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (isC1Pair(text, i)) {
            appendHexEscape(out, c);
            appendHexEscape(out, static_cast<unsigned char>(text[i + 1]));
            ++i;
            continue;
        }

        if (c >= 0x20 && 0x7f != c) {
            out += static_cast<char>(c);
            continue;
        }

        appendHexEscape(out, c);
    }

    return out;
}

/// The text with every C0 control character, 0x7f and C1 control dropped, so
/// that a name or a function cannot forge the colour and bold codes an IRC
/// sink writes
std::string stripControls(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (isC1Pair(text, i)) {
            ++i;
            continue;
        }

        if (c >= 0x20 && 0x7f != c)
            out += static_cast<char>(c);
    }

    return out;
}

/// The name as the column shows it: a name longer than the column keeps its
/// last width - 1 characters behind a '~'
std::string truncateName(const std::string& name, std::size_t width) {
    if (0 == width || name.size() <= width)
        return name;

    return "~" + name.substr(name.size() - (width - 1));
}

/// The colour of a name, from the bytes of the segment before its first '.'.
/// It is taken from the whole name, never from the truncated column, so that a
/// module and all of its sub-loggers share one hue.
int nameColour(const std::string& name) {
    unsigned int sum = 0;
    for (const char c : std::string_view(name).substr(0, name.find('.')))
        sum += static_cast<unsigned char>(c);

    return namePalette[sum % 6];
}

/// Where the dim part of the printed column begins, or npos when none of it is
/// dim.  Dim is whatever printed text lies at or after the first '.' of the
/// name itself: a name without a '.' has no dim part, and a truncation that
/// cut the first '.' away leaves the whole column -- the '~' included -- dim.
std::size_t dimStart(const std::string& name, const std::string& printed) {
    const std::size_t dot = name.find('.');
    if (std::string::npos == dot)
        return std::string::npos;
    if (printed.size() == name.size())
        return dot; // the column shows the name itself

    // printed is '~' followed by the last printed.size() - 1 characters
    const std::size_t cut = name.size() - (printed.size() - 1);

    return (dot < cut) ? 0 : 1 + (dot - cut);
}

/// The printed column in the colour of its module, the part that belongs to a
/// sub-logger additionally dim
std::string colourName(const std::string& name, const std::string& printed) {
    const int colour = nameColour(name);
    const std::size_t dim = dimStart(name, printed);

    const std::string_view normalPart = std::string_view(printed).substr(0, dim);
    const std::string_view dimPart =
        (std::string::npos == dim) ? std::string_view() : std::string_view(printed).substr(dim);

    std::string out;
    if (!normalPart.empty()) {
        out += std::format("\x1b[{}m", colour);
        out += normalPart;
    }
    if (!dimPart.empty()) {
        out += std::format("\x1b[2;{}m", colour);
        out += dimPart;
    }
    out += ansiReset;

    return out;
}

/// The colour a level is printed in, or "" for a level that has none
const char* levelColour(Verbosity level) {
    switch (level) {
    case FATAL:
        return "\x1b[1;31m";
    case ERROR:
        return "\x1b[31m";
    case WARN:
        return "\x1b[33m";
    case INFO:
        return "\x1b[32m";
    case DEBUG:
    case TRACE:
        return "\x1b[2m";
    default:
        return "";
    }
}

/// The mIRC colour a level is announced in, or "" for a level that has none
const char* ircColour(Verbosity level) {
    switch (level) {
    case FATAL:
    case ERROR:
        return "\00304";
    case WARN:
        return "\00307";
    default:
        return "";
    }
}

} // namespace

std::string formatJson(const LogRecord& record) {
    std::string out;
    out.reserve(record.message.size() + 128);
    out += '{';

    /// Appends "key":"value", with the comma that separates it from the one
    /// before
    const auto text = [&out](const std::string& key, const std::string& value) {
        if (out.size() > 1)
            out += ',';
        appendJsonString(out, key);
        out += ':';
        appendJsonString(out, value);
    };

    /// Appends "key":value, keeping the value's own JSON type
    const auto typed = [&out](const std::string& key, const LogValue& value) {
        if (out.size() > 1)
            out += ',';
        appendJsonString(out, key);
        out += ':';
        appendJsonValue(out, value);
    };

    text("timestamp", getCurrentTimestamp());
    text("level", levelName(record.level));
    text("logger", record.logger);
    if (!record.function.empty())
        text("function", record.function);

    // A display-only field is there for the message template; the reader of
    // this line has the fields it was made of
    for (const LogField& field : record.context)
        if (!field.displayOnly)
            typed(field.key, field.value);
    for (const LogField& field : record.fields)
        if (!field.displayOnly)
            typed(field.key, field.value);

    text("message", record.message);

    out += '}';

    return out;
}

std::string formatText(const LogRecord& record, const TextStyle& style) {
    const std::string time = formatTime(record.time, style.fullDate);
    const std::string level = levelColumn(record.level);

    // The name is cleaned before it is cut, padded, hashed and coloured: the
    // hue belongs to the logger, not to whatever fits in the column
    const std::string logger = escapeField(displayName(record.logger));
    const std::string name = truncateName(logger, style.nameWidth);
    const std::size_t nameColumn = std::max(name.size(), style.nameWidth);

    std::string out;
    out += time;
    out += "  ";

    const char* const paint = style.colour ? levelColour(record.level) : "";
    out += paint;
    out += level;
    if ('\0' != *paint)
        out += ansiReset;
    out += "  ";

    out += style.colour ? colourName(logger, name) : name;
    out.append(nameColumn - name.size(), ' '); // the padding follows the reset
    out += "  ";

    const std::size_t messageColumn = time.size() + 2 + level.size() + 2 + nameColumn + 2;

    // The function follows the last line of the message, and is left out at
    // INFO, where the sentence speaks for itself
    const std::string function = escapeField(record.function);

    std::string suffix;
    if (INFO != record.level && !function.empty()) {
        suffix = "  ";
        if (style.colour) {
            suffix += "\x1b[2m(";
            suffix += function;
            suffix += ')';
            suffix += ansiReset;
        } else {
            suffix += '(';
            suffix += function;
            suffix += ')';
        }
    }

    const EscapedText escaped = escapeControls(record.message, record.spans);
    const std::vector<TextLine> lines = splitEscaped(escaped);
    const bool bold = style.colour && style.highlight;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
            out.append(messageColumn, ' ');
        }
        if (bold)
            out += wrapSpans(lines[i].text, lines[i].spans, "\x1b[1m", "\x1b[22m");
        else
            out += lines[i].text;
    }

    out += suffix;

    return out;
}

std::vector<std::pair<std::string, std::vector<LogSpan>>> splitLines(const LogRecord& record) {
    const std::string& message = record.message;

    // Every line, with its control characters dropped, and for each byte of
    // the message the line it belongs to, where it landed in that line, and
    // whether it survived at all
    std::vector<std::string> lines(1);
    std::vector<std::size_t> lineOf(message.size(), 0);
    std::vector<std::size_t> posOf(message.size(), 0);
    std::vector<bool> kept(message.size(), false);

    std::size_t current = 0;
    for (std::size_t i = 0; i < message.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(message[i]);

        lineOf[i] = current;
        posOf[i] = lines[current].size();

        // Both bytes of a C1 control go, and neither is kept
        if (isC1Pair(message, i)) {
            lineOf[i + 1] = current;
            posOf[i + 1] = lines[current].size();
            ++i;
            continue;
        }

        if ('\n' == c) {
            lines.emplace_back();
            ++current;
            continue;
        }
        if (c < 0x20 || 0x7f == c)
            continue; // dropped, so that nothing of ours can be forged

        lines[current] += static_cast<char>(c);
        kept[i] = true;
    }

    // A span becomes one span per line it has surviving bytes in
    std::vector<std::vector<LogSpan>> spans(lines.size());
    for (const LogSpan& span : record.spans) {
        const std::size_t begin = std::min(span.begin, message.size());
        const std::size_t end = std::min(std::max(span.end, begin), message.size());

        bool open = false;
        std::size_t line = 0;
        std::size_t from = 0;
        std::size_t to = 0;

        for (std::size_t i = begin; i < end; ++i) {
            if (!kept[i])
                continue;
            if (!open || lineOf[i] != line) {
                if (open)
                    spans[line].push_back(LogSpan{from, to});
                open = true;
                line = lineOf[i];
                from = posOf[i];
            }
            to = posOf[i] + 1;
        }
        if (open)
            spans[line].push_back(LogSpan{from, to});
    }

    std::vector<std::pair<std::string, std::vector<LogSpan>>> result;
    result.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].empty()) // a line that was nothing but control characters
            result.emplace_back(std::move(lines[i]), std::move(spans[i]));

    return result;
}

std::string formatIrcLine(const LogRecord& record, std::string_view line,
                          const std::vector<LogSpan>& lineSpans, bool highlight) {
    const char* const colour = ircColour(record.level);

    // The name and the function are cleaned like the message was: nothing of
    // theirs may look like the colour or bold code this notice writes itself
    const std::string function = stripControls(record.function);

    std::string out;
    out.reserve(line.size() + 32);

    out += colour;
    out += '[';
    out += stripControls(displayName(record.logger));
    out += "] ";
    out += levelTag(record.level);
    out += ' ';

    if (INFO != record.level && !function.empty()) {
        out += function;
        out += "> ";
    }

    if (highlight)
        out += wrapSpans(line, lineSpans, "\002", "\002");
    else
        out += line;

    if ('\0' != *colour)
        out += '\003';

    return out;
}

/**
 * Parses __PRETTY_FUNCTION__ output to extract readable function names.
 * Removes template parameters and return types, keeping class::function format.
 * Handles both C++ member functions and standalone functions.
 */
std::string parseFunction(std::string pretty) {
    auto paren = pretty.find('(');
    if (paren != std::string::npos)
        pretty.erase(paren);

    auto lastColons = pretty.rfind("::");
    if (lastColons != std::string::npos) {
        // Find the second-to-last "::" to get class::function
        auto secondLastColons = pretty.rfind("::", lastColons - 1);
        if (secondLastColons != std::string::npos)
            return pretty.substr(secondLastColons + 2);
        else
            return pretty;
    }
    return pretty;
}

} // namespace gnuworld
