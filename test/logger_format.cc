/**
 * logger_format.cc
 * Unit test for the JSON, text and IRC formatters in libgnuworld/LogFormat.h.
 * Runs under "make check".
 */

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "LogFormat.h"
#include "LogRecord.h"
#include "LogRender.h"
#include "misc.h"

using namespace gnuworld;

namespace {

int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n';                \
        }                                                                                          \
    } while (0)

/// Reports the value as well as the failure, for the long string comparisons
#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const std::string got_((actual));                                                          \
        const std::string want_((expected));                                                       \
        if (got_ != want_) {                                                                       \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #actual "\n  got  [" << got_  \
                      << "]\n  want [" << want_ << "]\n";                                          \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ *
 * A minimal strict JSON validator.
 * Objects, arrays, strings (with the seven named escapes and \uXXXX
 * with exactly four hex digits), numbers, true, false and null.  A raw
 * byte below 0x20, a raw 0x7f and any byte sequence that is not valid
 * UTF-8 inside a string are rejected.
 * ------------------------------------------------------------------ */

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

class JsonParser {
  public:
    explicit JsonParser(std::string_view text) : text(text) {}

    /// Parses one complete value; collects the keys of a top-level object
    bool parse(std::vector<std::string>* keys) {
        skipWhite();
        if (!parseValue(keys))
            return false;
        skipWhite();
        return pos == text.size();
    }

  private:
    std::string_view text;
    std::size_t pos = 0;

    void skipWhite() {
        while (pos < text.size() &&
               (' ' == text[pos] || '\t' == text[pos] || '\n' == text[pos] || '\r' == text[pos]))
            ++pos;
    }

    bool parseValue(std::vector<std::string>* keys) {
        if (pos >= text.size())
            return false;

        switch (text[pos]) {
        case '{':
            return parseObject(keys);
        case '[':
            return parseArray();
        case '"': {
            std::string ignored;
            return parseString(ignored);
        }
        case 't':
            return parseLiteral("true");
        case 'f':
            return parseLiteral("false");
        case 'n':
            return parseLiteral("null");
        default:
            return parseNumber();
        }
    }

    bool parseLiteral(std::string_view word) {
        if (text.substr(pos, word.size()) != word)
            return false;
        pos += word.size();
        return true;
    }

    bool parseObject(std::vector<std::string>* keys) {
        ++pos; // '{'
        skipWhite();
        if (pos < text.size() && '}' == text[pos]) {
            ++pos;
            return true;
        }

        for (;;) {
            skipWhite();
            if (pos >= text.size() || '"' != text[pos])
                return false;

            std::string key;
            if (!parseString(key))
                return false;
            if (keys != nullptr)
                keys->push_back(key);

            skipWhite();
            if (pos >= text.size() || ':' != text[pos])
                return false;
            ++pos;

            skipWhite();
            if (!parseValue(nullptr)) // only the top-level keys are collected
                return false;

            skipWhite();
            if (pos >= text.size())
                return false;
            if (',' == text[pos]) {
                ++pos;
                continue;
            }
            if ('}' == text[pos]) {
                ++pos;
                return true;
            }
            return false;
        }
    }

    bool parseArray() {
        ++pos; // '['
        skipWhite();
        if (pos < text.size() && ']' == text[pos]) {
            ++pos;
            return true;
        }

        for (;;) {
            skipWhite();
            if (!parseValue(nullptr))
                return false;
            skipWhite();
            if (pos >= text.size())
                return false;
            if (',' == text[pos]) {
                ++pos;
                continue;
            }
            if (']' == text[pos]) {
                ++pos;
                return true;
            }
            return false;
        }
    }

    /// The length a UTF-8 sequence with this lead byte has, or 0
    static std::size_t utf8Length(unsigned char lead) {
        if (lead >= 0xc2 && lead <= 0xdf)
            return 2;
        if (lead >= 0xe0 && lead <= 0xef)
            return 3;
        if (lead >= 0xf0 && lead <= 0xf4)
            return 4;
        return 0;
    }

    /// True when the len bytes at pos are a valid UTF-8 sequence
    bool utf8SequenceValid(std::size_t len) const {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        unsigned char lowest = 0x80;
        unsigned char highest = 0xbf;

        if (0xe0 == lead)
            lowest = 0xa0;
        else if (0xed == lead)
            highest = 0x9f;
        else if (0xf0 == lead)
            lowest = 0x90;
        else if (0xf4 == lead)
            highest = 0x8f;

        for (std::size_t i = 1; i < len; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[pos + i]);
            const unsigned char low = (1 == i) ? lowest : 0x80;
            const unsigned char high = (1 == i) ? highest : 0xbf;
            if (c < low || c > high)
                return false;
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (pos >= text.size() || '"' != text[pos])
            return false;
        ++pos;

        while (pos < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[pos]);

            if ('"' == c) {
                ++pos;
                return true;
            }

            if ('\\' == c) {
                ++pos;
                if (pos >= text.size())
                    return false;
                const char escape = text[pos];
                if ('u' == escape) {
                    ++pos;
                    for (int i = 0; i < 4; ++i) {
                        if (pos >= text.size() || !isHexDigit(text[pos]))
                            return false;
                        ++pos;
                    }
                    continue;
                }
                if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
                    return false;
                ++pos;
                continue;
            }

            if (c < 0x20 || 0x7f == c)
                return false; // a raw control byte

            if (c < 0x80) {
                out += static_cast<char>(c);
                ++pos;
                continue;
            }

            const std::size_t len = utf8Length(c);
            if (0 == len || pos + len > text.size() || !utf8SequenceValid(len))
                return false;
            out.append(text.substr(pos, len));
            pos += len;
        }

        return false; // unterminated
    }

    bool parseNumber() {
        const std::size_t start = pos;
        if (pos < text.size() && '-' == text[pos])
            ++pos;

        if (pos >= text.size())
            return false;
        if ('0' == text[pos])
            ++pos;
        else if (isDigit(text[pos]))
            while (pos < text.size() && isDigit(text[pos]))
                ++pos;
        else
            return false;

        if (pos < text.size() && '.' == text[pos]) {
            ++pos;
            if (pos >= text.size() || !isDigit(text[pos]))
                return false;
            while (pos < text.size() && isDigit(text[pos]))
                ++pos;
        }

        if (pos < text.size() && ('e' == text[pos] || 'E' == text[pos])) {
            ++pos;
            if (pos < text.size() && ('+' == text[pos] || '-' == text[pos]))
                ++pos;
            if (pos >= text.size() || !isDigit(text[pos]))
                return false;
            while (pos < text.size() && isDigit(text[pos]))
                ++pos;
        }

        return pos > start;
    }
};

bool validJson(const std::string& text) { return JsonParser(text).parse(nullptr); }

std::vector<std::string> jsonKeys(const std::string& text) {
    std::vector<std::string> keys;
    if (!JsonParser(text).parse(&keys))
        return std::vector<std::string>();
    return keys;
}

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/// 'd' matches one digit, every other character matches itself
bool shapeMatches(std::string_view text, std::string_view shape) {
    if (text.size() != shape.size())
        return false;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if ('d' == shape[i]) {
            if (!isDigit(text[i]))
                return false;
        } else if (text[i] != shape[i]) {
            return false;
        }
    }
    return true;
}

LogField field(const std::string& key, const LogValue& value, bool displayOnly = false) {
    return LogField{key, value, displayOnly};
}

/// The text with every SGR sequence a formatter writes removed: ESC '[', then
/// digits and semicolons, then 'm'.  Any ESC byte left over came from a field.
std::string stripSgr(std::string_view text) {
    std::string out;

    for (std::size_t i = 0; i < text.size();) {
        if ('\x1b' == text[i] && i + 1 < text.size() && '[' == text[i + 1]) {
            std::size_t end = i + 2;
            while (end < text.size() && (isDigit(text[end]) || ';' == text[end]))
                ++end;
            if (end < text.size() && 'm' == text[end]) {
                i = end + 1;
                continue;
            }
        }
        out += text[i];
        ++i;
    }

    return out;
}

/// How many lines a piece of text has
std::size_t lineCount(std::string_view text) {
    std::size_t lines = 1;
    for (const char c : text)
        if ('\n' == c)
            ++lines;

    return lines;
}

/// 2026-09-18 12:34:56.789 local time, so that the time column is known
std::chrono::system_clock::time_point fixedTime() {
    std::tm broken{};
    broken.tm_year = 2026 - 1900;
    broken.tm_mon = 8;
    broken.tm_mday = 18;
    broken.tm_hour = 12;
    broken.tm_min = 34;
    broken.tm_sec = 56;
    broken.tm_isdst = -1;

    const std::time_t when = std::mktime(&broken);

    return std::chrono::system_clock::from_time_t(when) + std::chrono::milliseconds(789);
}

LogRecord makeRecord(Verbosity level, const std::string& logger, const std::string& function,
                     const std::string& message) {
    LogRecord record;
    record.time = fixedTime();
    record.level = level;
    record.logger = logger;
    record.function = function;
    record.message = message;
    return record;
}

/// A record whose message and spans come from the real renderer
LogRecord rendered(Verbosity level, const std::string& logger, const std::string& function,
                   std::string_view tmpl, const std::vector<LogField>& fields) {
    const RenderResult result = renderTemplate(tmpl, {}, fields);
    LogRecord record = makeRecord(level, logger, function, result.text);
    record.spans = result.spans;
    record.fields = fields;
    return record;
}

const std::size_t shortTime = 12; // HH:MM:SS.mmm
const std::size_t longTime = 23;  // YYYY-MM-DD HH:MM:SS.mmm

TextStyle plainStyle(std::size_t nameWidth, bool fullDate = false) {
    return TextStyle{fullDate, false, false, nameWidth};
}

/* ------------------------------------------------------------------ *
 * JSON
 * ------------------------------------------------------------------ */

void testJsonTypesAndOrder() {
    LogRecord record = makeRecord(WARN, "cservice", "msg_B::parseBurstUsers", "could not add");
    record.context.push_back(field("bot", std::string("cservice")));
    record.fields = {field("ip", std::string("192.168.1.1")),
                     field("id", std::int64_t{12345}),
                     field("ok", true),
                     field("name", std::string("7up")),
                     field("quote", std::string("a\"b\\c\n\x01")),
                     field("none", LogValue{})};

    const std::string json = formatJson(record);

    CHECK(validJson(json));
    CHECK(json.find('\n') == std::string::npos); // one line, no terminator

    const std::vector<std::string> keys = jsonKeys(json);
    const std::vector<std::string> expected{"timestamp", "level", "logger", "function",
                                            "bot",       "ip",    "id",     "ok",
                                            "name",      "quote", "none",   "message"};
    CHECK(keys == expected);

    // The timestamp is the ISO 8601 UTC stamp of misc.h
    CHECK(shapeMatches(std::string_view(json).substr(14, 20), "dddd-dd-ddTdd:dd:ddZ"));

    CHECK(json.find("\"level\":\"WARNING\"") != std::string::npos);
    CHECK(json.find("\"logger\":\"cservice\"") != std::string::npos);
    CHECK(json.find("\"function\":\"msg_B::parseBurstUsers\"") != std::string::npos);
    CHECK(json.find("\"bot\":\"cservice\"") != std::string::npos);

    // Strings are quoted, numbers are not
    CHECK(json.find("\"ip\":\"192.168.1.1\"") != std::string::npos);
    CHECK(json.find("\"name\":\"7up\"") != std::string::npos);
    CHECK(json.find("\"id\":12345") != std::string::npos);
    CHECK(json.find("\"ok\":true") != std::string::npos);
    CHECK(json.find("\"none\":null") != std::string::npos);
    CHECK(json.find("\"quote\":\"a\\\"b\\\\c\\n\\u0001\"") != std::string::npos);
    CHECK(json.find("\"message\":\"could not add\"") != std::string::npos);
}

void testJsonNoFunction() {
    LogRecord record = makeRecord(INFO, "core", "", "burst complete");
    const std::string json = formatJson(record);

    CHECK(validJson(json));
    CHECK(json.find("\"function\"") == std::string::npos);

    const std::vector<std::string> keys = jsonKeys(json);
    const std::vector<std::string> expected{"timestamp", "level", "logger", "message"};
    CHECK(keys == expected);
}

void testJsonDisplayOnly() {
    LogRecord record = makeRecord(DEBUG, "cservice", "", "x");
    record.context.push_back(field("hidden", std::string("ctx"), true));
    record.fields = {field("client", std::string("MrIron"), true),
                     field("client_ip", std::string("10.0.0.1"))};

    const std::string json = formatJson(record);

    CHECK(validJson(json));
    CHECK(json.find("\"client\":") == std::string::npos);
    CHECK(json.find("\"hidden\":") == std::string::npos);
    CHECK(json.find("\"client_ip\":\"10.0.0.1\"") != std::string::npos);
}

void testJsonNumbers() {
    LogRecord record = makeRecord(TRACE, "core", "", "numbers");
    record.fields = {field("nan", std::numeric_limits<double>::quiet_NaN()),
                     field("inf", std::numeric_limits<double>::infinity()),
                     field("ninf", -std::numeric_limits<double>::infinity()),
                     field("real", 1.5),
                     field("big", std::uint64_t{18446744073709551615ULL}),
                     field("neg", std::int64_t{-7})};

    const std::string json = formatJson(record);

    CHECK(validJson(json));
    CHECK(json.find("\"nan\":null") != std::string::npos);
    CHECK(json.find("\"inf\":null") != std::string::npos);
    CHECK(json.find("\"ninf\":null") != std::string::npos);
    CHECK(json.find("\"real\":1.5") != std::string::npos);
    CHECK(json.find("\"big\":18446744073709551615") != std::string::npos);
    CHECK(json.find("\"neg\":-7") != std::string::npos);
}

void testJsonInvalidUtf8() {
    LogRecord record = makeRecord(ERROR, "cservice", "", "bad byte");
    record.fields = {field("raw", std::string("\xff")),
                     field("utf8", std::string("na\xc3\xafve \xe2\x82\xac")),
                     field("cut", std::string("\xe2\x82"))};

    const std::string json = formatJson(record);

    CHECK(validJson(json));
    // An invalid byte becomes U+FFFD, a valid sequence passes through
    CHECK(json.find("\"raw\":\"\xef\xbf\xbd\"") != std::string::npos);
    CHECK(json.find("\"utf8\":\"na\xc3\xafve \xe2\x82\xac\"") != std::string::npos);
    CHECK(json.find("\"cut\":\"\xef\xbf\xbd\xef\xbf\xbd\"") != std::string::npos);
}

void testEscapeJsonString() {
    CHECK_EQ(escapeJsonString("\x01"), "\\u0001");
    CHECK_EQ(escapeJsonString("\x1f"), "\\u001f");
    CHECK_EQ(escapeJsonString("\x1b"), "\\u001b");
    CHECK_EQ(escapeJsonString("\x7f"), "\\u007f");
    CHECK_EQ(escapeJsonString("\002\003"), "\\u0002\\u0003");
    CHECK_EQ(escapeJsonString("a\"b\\c"), "a\\\"b\\\\c");
    CHECK_EQ(escapeJsonString("\b\f\n\r\t"), "\\b\\f\\n\\r\\t");
    CHECK_EQ(escapeJsonString("plain"), "plain");

    // Still valid JSON once wrapped in quotes
    CHECK(validJson("\"" + escapeJsonString(std::string("\x01\x1b\x7f\xff")) + "\""));
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

void testTextInfoAndWarn() {
    const LogRecord info = makeRecord(INFO, "cservice", "msg_B::parseBurstUsers", "burst complete");
    const std::string infoLine = formatText(info, plainStyle(12));

    CHECK(shapeMatches(infoLine.substr(0, shortTime), "dd:dd:dd.ddd"));
    // The function suffix is left out at INFO
    CHECK_EQ(infoLine.substr(shortTime), "  INFO   cservice      burst complete");

    const LogRecord warn =
        makeRecord(WARN, "cservice", "msg_B::parseBurstUsers", "unknown mode +Z");
    const std::string warnLine = formatText(warn, plainStyle(12));

    CHECK(shapeMatches(warnLine.substr(0, shortTime), "dd:dd:dd.ddd"));
    CHECK_EQ(warnLine.substr(shortTime),
             "  WARN   cservice      unknown mode +Z  (msg_B::parseBurstUsers)");

    // No trailing newline
    CHECK(warnLine.back() == ')');
}

void testTextFullDateAndRoot() {
    const LogRecord record = makeRecord(ERROR, "", "", "root speaks");
    const std::string line = formatText(record, plainStyle(12, true));

    CHECK(shapeMatches(line.substr(0, longTime), "dddd-dd-dd dd:dd:dd.ddd"));
    CHECK_EQ(line.substr(longTime), "  ERROR  root          root speaks");
}

void testTextNameWidth() {
    const std::string name = "abcdefghijklmnopqrstuvwxyz1234"; // 30 characters
    CHECK(name.size() == 30);

    const LogRecord record = makeRecord(TRACE, name, "", "long name");
    const std::string line = formatText(record, plainStyle(20));

    CHECK_EQ(line.substr(shortTime), "  TRACE  ~lmnopqrstuvwxyz1234  long name");

    // Exactly nameWidth characters in the name column
    const std::string shown = line.substr(shortTime + 2 + 5 + 2, 20);
    CHECK(shown.size() == 20);
    CHECK(shown[0] == '~');
    CHECK(shown.substr(1) == name.substr(name.size() - 19));
}

void testTextMultiline() {
    const LogRecord record = makeRecord(DEBUG, "cservice", "x::y", "first line\nsecond line");
    const std::string line = formatText(record, plainStyle(12));

    // Message column: 12 (time) + 2 + 5 (level) + 2 + 12 (name) + 2
    const std::string indent(35, ' ');
    CHECK_EQ(line.substr(shortTime),
             "  DEBUG  cservice      first line\n" + indent + "second line  (x::y)");
}

void testTextControlCharacters() {
    const LogRecord record =
        rendered(WARN, "cservice", "", "value {v}", {field("v", std::string("\x1b[31m"))});
    const std::string line = formatText(record, plainStyle(12));

    CHECK_EQ(line.substr(shortTime), "  WARN   cservice      value \\x1b[31m");
    CHECK(line.find('\x1b') == std::string::npos);

    // Every other C0 character and 0x7f, but a newline still breaks the line
    const LogRecord others = makeRecord(TRACE, "core", "",
                                        std::string("a\002b\003c\rd\x7f"
                                                    "e\tf"));
    const std::string line2 = formatText(others, plainStyle(12));
    CHECK_EQ(line2.substr(shortTime), "  TRACE  core          a\\x02b\\x03c\\x0dd\\x7fe\\x09f");
}

/* ------------------------------------------------------------------ *
 * Colour
 * ------------------------------------------------------------------ */

void testTextColour() {
    const LogRecord record = rendered(WARN, "cservice", "msg_B::parseBurstUsers",
                                      "mode {mode} unknown", {field("mode", std::string("+Z"))});

    const TextStyle style{false, true, true, 12};
    const std::string line = formatText(record, style);

    CHECK_EQ(line.substr(shortTime), "  \x1b[33mWARN \x1b[0m  \x1b[36mcservice\x1b[0m    "
                                     "  mode \x1b[1m+Z\x1b[22m unknown"
                                     "  \x1b[2m(msg_B::parseBurstUsers)\x1b[0m");

    // Highlight off: the value is not bold, everything else is unchanged
    const TextStyle noHighlight{false, true, false, 12};
    const std::string plain = formatText(record, noHighlight);
    CHECK(plain.find("\x1b[1m") == std::string::npos);
    CHECK(plain.find("\x1b[22m") == std::string::npos);
    CHECK(plain.find("mode +Z unknown") != std::string::npos);
    CHECK(plain.find("\x1b[33mWARN \x1b[0m") != std::string::npos);

    // Colour off: not one ESC byte
    const std::string bare = formatText(record, plainStyle(12));
    CHECK(bare.find('\x1b') == std::string::npos);
}

void testTextColourLevels() {
    const struct {
        Verbosity level;
        const char* expected;
    } cases[] = {{FATAL, "\x1b[1;31mFATAL\x1b[0m"}, {ERROR, "\x1b[31mERROR\x1b[0m"},
                 {WARN, "\x1b[33mWARN \x1b[0m"},    {INFO, "\x1b[32mINFO \x1b[0m"},
                 {DEBUG, "\x1b[2mDEBUG\x1b[0m"},    {TRACE, "\x1b[2mTRACE\x1b[0m"}};

    for (const auto& one : cases) {
        const LogRecord record = makeRecord(one.level, "cservice", "", "m");
        const std::string line = formatText(record, TextStyle{false, true, true, 12});
        CHECK(line.substr(shortTime + 2, std::string(one.expected).size()) == one.expected);
    }
}

void testTextColourName() {
    // "cservice" hashes to palette entry 0 (36); the part from the first '.'
    // on is dim
    const LogRecord sub = makeRecord(DEBUG, "cservice.sql", "", "m");
    const std::string line = formatText(sub, TextStyle{false, true, true, 12});
    CHECK(line.find("\x1b[36mcservice\x1b[2;36m.sql\x1b[0m  m") != std::string::npos);

    // The root prints as root, and hashes as the segment "root" (34)
    const LogRecord root = makeRecord(DEBUG, "", "", "m");
    const std::string rootLine = formatText(root, TextStyle{false, true, true, 12});
    CHECK(rootLine.find("\x1b[34mroot\x1b[0m        ") != std::string::npos);
}

/// A module and its sub-loggers share one hue, whatever the column cut away:
/// the palette index is taken from the first segment of the name itself, not
/// of the string that is printed
void testTextColourNameTruncated() {
    const TextStyle style{false, true, true, 20};

    // "dronescan" hashes to palette entry 3 (32) and has no '.', so none of it
    // is dim
    const LogRecord module = makeRecord(DEBUG, "dronescan", "", "m");
    const std::string moduleLine = formatText(module, style);
    CHECK(moduleLine.find("\x1b[32mdronescan\x1b[0m           ") != std::string::npos);
    CHECK(moduleLine.find("\x1b[2;") == std::string::npos);

    // The sub-logger is 34 characters, so the column shows its last 19 behind
    // a '~'.  The first '.' of the name was cut away, so the whole column is
    // dim -- and the hue is still the module's own.
    const std::string sub = "dronescan.spam.action.verylongname";
    CHECK(sub.size() == 34);

    const LogRecord subRecord = makeRecord(DEBUG, sub, "", "m");
    const std::string subLine = formatText(subRecord, style);
    CHECK(subLine.find("\x1b[2;32m~action.verylongname\x1b[0m  m") != std::string::npos);
    // Nothing is in the bright colour: there is no normal part at all
    CHECK(subLine.find("\x1b[32m~") == std::string::npos);

    // A first '.' that survives the cut still splits the column: everything
    // from it on is dim, the '~' and the characters before it are not
    const LogRecord kept = makeRecord(DEBUG, "cservice.sqlx1", "", "m");
    const std::string keptLine = formatText(kept, TextStyle{false, true, true, 13});
    CHECK(keptLine.find("\x1b[36m~ervice\x1b[2;36m.sqlx1\x1b[0m  m") != std::string::npos);

    // A truncated name without any '.' has nothing dim
    const LogRecord flat = makeRecord(TRACE, "abcdefghijklmnopqrstuvwxyz1234", "", "m");
    const std::string flatLine = formatText(flat, style);
    CHECK(flatLine.find("\x1b[35m~lmnopqrstuvwxyz1234\x1b[0m  m") != std::string::npos);
    CHECK(flatLine.find("\x1b[2;") == std::string::npos);
}

/// A name that carries a colour sequence of its own, and a function that
/// carries a bare ESC and a newline.  The ESC is kept away from the "bar" by
/// the concatenation: "\x1bb" would read as one hex escape.
const std::string dirtyLogger = "cser\x1b[31mvice";
const std::string dirtyFunction = std::string("Foo::\x1b") + "bar\n";

/// The logger name and the function are cleaned exactly like the message: a
/// control byte in either of them cannot reach the terminal
void testTextFieldControlCharacters() {
    const LogRecord record = makeRecord(WARN, dirtyLogger, dirtyFunction, "first\nsecond");

    const std::string bare = formatText(record, plainStyle(20));
    CHECK(bare.find('\x1b') == std::string::npos);
    CHECK(bare.find("cser\\x1b[31mvice") != std::string::npos);
    CHECK(bare.find("(Foo::\\x1bbar\\x0a)") != std::string::npos);
    CHECK(lineCount(bare) == lineCount(record.message));

    // With colour on, the only ESC bytes left are the formatter's own
    const std::string painted = formatText(record, TextStyle{false, true, true, 20});
    CHECK(stripSgr(painted).find('\x1b') == std::string::npos);
    CHECK(painted.find("cser\\x1b[31mvice") != std::string::npos);
    CHECK(painted.find("Foo::\\x1bbar\\x0a") != std::string::npos);
    CHECK(lineCount(painted) == lineCount(record.message));
}

/// The IRC notice drops the control bytes of the name and of the function, so
/// that neither can forge the sink's own colour or bold codes
void testIrcFieldControlCharacters() {
    const LogRecord record = makeRecord(WARN, dirtyLogger, dirtyFunction, "watch out");
    const std::string line = formatIrcLine(record, "watch out", {}, false);

    CHECK_EQ(line, "\00307[cser[31mvice] [W] Foo::bar> watch out\003");

    for (const char c : line) {
        const unsigned char byte = static_cast<unsigned char>(c);
        CHECK(byte >= 0x20 || '\002' == c || '\003' == c);
        CHECK(0x7f != byte);
    }

    // A function that is nothing but control bytes adds no prefix
    const LogRecord noFunction = makeRecord(ERROR, "core", "\x1b\002\x7f", "gone");
    CHECK_EQ(formatIrcLine(noFunction, "gone", {}, false), "\00304[core] [E] gone\003");
}

/* ------------------------------------------------------------------ *
 * splitLines and IRC
 * ------------------------------------------------------------------ */

void testSplitLines() {
    // An empty line between two others is dropped
    const LogRecord record = makeRecord(INFO, "core", "", "a\n\nb");
    const auto lines = splitLines(record);
    CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        CHECK(lines[0].first == "a");
        CHECK(lines[1].first == "b");
    }

    // Control characters are removed and the spans follow the text
    const LogRecord dirty = rendered(ERROR, "cservice", "", "nick {nick} banned",
                                     {field("nick", std::string("a\002\003\rb"))});
    const auto dirtyLines = splitLines(dirty);
    CHECK(dirtyLines.size() == 1);
    if (dirtyLines.size() == 1) {
        CHECK_EQ(dirtyLines[0].first, "nick ab banned");
        CHECK(dirtyLines[0].second.size() == 1);
        if (dirtyLines[0].second.size() == 1) {
            const LogSpan& span = dirtyLines[0].second[0];
            CHECK_EQ(dirtyLines[0].first.substr(span.begin, span.end - span.begin), "ab");
        }
    }

    // A span crossing a line break is clipped to each line
    LogRecord crossing = makeRecord(INFO, "core", "", "one\ntwo");
    crossing.spans.push_back(LogSpan{0, 7});
    const auto crossed = splitLines(crossing);
    CHECK(crossed.size() == 2);
    if (crossed.size() == 2) {
        CHECK(crossed[0].second.size() == 1);
        CHECK(crossed[1].second.size() == 1);
        if (crossed[0].second.size() == 1 && crossed[1].second.size() == 1) {
            CHECK(crossed[0].second[0].begin == 0 && crossed[0].second[0].end == 3);
            CHECK(crossed[1].second[0].begin == 0 && crossed[1].second[0].end == 3);
        }
    }

    // A message that is nothing but control characters has no lines at all
    const LogRecord empty = makeRecord(INFO, "core", "", "\002\003\n\r");
    CHECK(splitLines(empty).empty());
}

void testIrcLine() {
    const LogRecord error = rendered(ERROR, "cservice", "cservice::parseMode",
                                     "mode {mode} unknown", {field("mode", std::string("+Z"))});
    const auto lines = splitLines(error);
    CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return;

    const std::string bold = formatIrcLine(error, lines[0].first, lines[0].second, true);
    CHECK_EQ(bold, "\00304[cservice] [E] cservice::parseMode> "
                   "mode \002+Z\002 unknown\003");
    CHECK(bold.rfind("\00304[cservice] [E] ", 0) == 0);
    CHECK(bold.back() == '\003');

    // No highlight: no bold byte
    const std::string quiet = formatIrcLine(error, lines[0].first, lines[0].second, false);
    CHECK_EQ(quiet, "\00304[cservice] [E] cservice::parseMode> mode +Z unknown\003");
    CHECK(quiet.find('\002') == std::string::npos);

    // WARN is orange, INFO has no colour, no reset and no function prefix
    const LogRecord warn = makeRecord(WARN, "cservice", "cservice::parseMode", "watch out");
    CHECK_EQ(formatIrcLine(warn, "watch out", {}, false),
             "\00307[cservice] [W] cservice::parseMode> watch out\003");

    const LogRecord info = makeRecord(INFO, "cservice", "cservice::parseMode", "all good");
    CHECK_EQ(formatIrcLine(info, "all good", {}, false), "[cservice] [I] all good");

    // The root prints as root, and an empty function adds no prefix
    const LogRecord root = makeRecord(DEBUG, "", "", "hello");
    CHECK_EQ(formatIrcLine(root, "hello", {}, false), "[root] [D] hello");
}

void testIrcMultipleLines() {
    const LogRecord record =
        rendered(WARN, "core", "", "{a}\n{b}",
                 {field("a", std::string("one")), field("b", std::string("two"))});
    const auto lines = splitLines(record);
    CHECK(lines.size() == 2);
    if (lines.size() != 2)
        return;

    CHECK_EQ(formatIrcLine(record, lines[0].first, lines[0].second, true),
             "\00307[core] [W] \002one\002\003");
    CHECK_EQ(formatIrcLine(record, lines[1].first, lines[1].second, true),
             "\00307[core] [W] \002two\002\003");
}

void testParseFunction() {
    CHECK_EQ(
        parseFunction("void gnuworld::msg_B::parseBurstUsers(gnuworld::Channel*, const char*)"),
        "msg_B::parseBurstUsers");
    CHECK_EQ(parseFunction("int main(int, char**)"), "int main");
    CHECK_EQ(parseFunction("void foo()"), "void foo");
}

} // namespace

int main() {
    testJsonTypesAndOrder();
    testJsonNoFunction();
    testJsonDisplayOnly();
    testJsonNumbers();
    testJsonInvalidUtf8();
    testEscapeJsonString();
    testTextInfoAndWarn();
    testTextFullDateAndRoot();
    testTextNameWidth();
    testTextMultiline();
    testTextControlCharacters();
    testTextColour();
    testTextColourLevels();
    testTextColourName();
    testTextColourNameTruncated();
    testTextFieldControlCharacters();
    testSplitLines();
    testIrcLine();
    testIrcMultipleLines();
    testIrcFieldControlCharacters();
    testParseFunction();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_format: all checks passed\n";
    return 0;
}
