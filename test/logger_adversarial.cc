/**
 * logger_adversarial.cc
 * Adversarial and integration-adjacent tests for the logging system, written
 * from the specification rather than from the implementation:
 * hostile field values and templates through the full render/format pipeline,
 * the coloured name column's exact boundaries, a null pointer of a type
 * nobody registered, and the logging system under concurrent use. It links
 * libgnuworld alone. Runs under "make check".
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ELog.h"
#include "LogConfig.h"
#include "LogFormat.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogRender.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"

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
 * A minimal strict JSON validator, written fresh for this test: objects,
 * strings (the seven named escapes and \uXXXX with exactly four hex digits),
 * numbers, true/false/null.  A raw byte below 0x20, a raw 0x7f, and any byte
 * sequence that is not valid UTF-8 inside a string are all rejected.  This is
 * deliberately independent of the parser test/logger_format.cc writes for the
 * same purpose, so that a mistake shared by both formatter and validator is
 * still caught by at least one of the two test programs.
 * ------------------------------------------------------------------ */

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

class StrictJson {
  public:
    explicit StrictJson(std::string_view text) : text(text) {}

    /// Parses one complete value at the top level; collects the keys of a
    /// top-level object, in the order they appear, into *keys when given.
    bool parse(std::vector<std::string>* keys = nullptr) {
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
            if (nullptr != keys)
                keys->push_back(key);

            skipWhite();
            if (pos >= text.size() || ':' != text[pos])
                return false;
            ++pos;

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

    static std::size_t utf8Length(unsigned char lead) {
        if (lead >= 0xc2 && lead <= 0xdf)
            return 2;
        if (lead >= 0xe0 && lead <= 0xef)
            return 3;
        if (lead >= 0xf0 && lead <= 0xf4)
            return 4;
        return 0;
    }

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
                return false; // a raw control byte: not allowed inside a string

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
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
            return false;
        if ('0' == text[pos]) {
            ++pos;
        } else {
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                ++pos;
        }
        if (pos < text.size() && '.' == text[pos]) {
            ++pos;
            if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
                return false;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                ++pos;
        }
        if (pos < text.size() && ('e' == text[pos] || 'E' == text[pos])) {
            ++pos;
            if (pos < text.size() && ('+' == text[pos] || '-' == text[pos]))
                ++pos;
            if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
                return false;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                ++pos;
        }
        return pos > start;
    }
};

bool validJson(const std::string& text) { return StrictJson(text).parse(); }

/// True when byte is valid UTF-8 as a whole string (used to sanity-check that
/// a text/IRC line, once its own control bytes are accounted for, is still
/// legible - not required by the spec for those two, only asserted for JSON).
bool contains(const std::string& haystack, const std::string& needle) {
    return std::string::npos != haystack.find(needle);
}

std::size_t lineCount(const std::string& text) {
    if (text.empty())
        return 0;
    std::size_t n = 1;
    for (const char c : text)
        if ('\n' == c)
            ++n;
    return n;
}

/// The value of the first field with this key, formatted the way logValueToString does
const LogField* findField(const std::vector<LogField>& fields, const std::string& key) {
    for (const LogField& field : fields)
        if (field.key == key)
            return &field;
    return nullptr;
}

/// Attaches sink, runs action, detaches it, and returns exactly the records
/// action caused (the sink is expected to have been empty before).
std::vector<LogRecord> captureRecords(Logger* logger, const std::shared_ptr<CaptureSink>& sink,
                                      const std::function<void()>& action) {
    sink->clear();
    logger->addSink(sink, TRACE);
    action();
    logger->removeSink(sink);
    return sink->records;
}

/* ------------------------------------------------------------------ *
 * Hostile field values, through the full pipeline: render, then all three
 * formats.  A value is never re-expanded as a template (the renderer scans
 * the template exactly once, so whatever a field contributes is copied in
 * verbatim); the JSON line always validates strictly; text carries no raw
 * control byte other than an actual newline; every IRC line carries no
 * control byte other than the formatter's own \002/\003, and no '\r'/'\n'.
 * ------------------------------------------------------------------ */

struct HostileValue {
    const char* name;
    std::string value;
};

std::vector<HostileValue> hostileValues() {
    return {
        {"empty-placeholder-braces", "{}"},
        {"named-placeholder-braces", "{x}"},
        {"open-brace-pair", "{{"},
        {"close-brace-pair", "}}"},
        {"printf-style", "%s%n"},
        {"newline", "\n"},
        {"crlf", "\r\n"},
        {"csi-clear-screen", "\x1b[2J"},
        {"irc-bold-colour", std::string("\x02") + "bold" + "\x03" + "04red"},
        {"long-10000", std::string(10000, 'Q')},
        {"invalid-utf8-ff-fe", std::string("\xff\xfe", 2)},
        {"invalid-utf8-truncated", std::string("\xe2\x82", 2)},
        {"embedded-nul", std::string("a\0b", 3)},
        {"del-byte", std::string("a\x7f"
                                 "b")},
    };
}

/// One record built the way a real call site would: a template with literal
/// text around the placeholder, so that only the substituted span is hostile.
LogRecord recordWithValue(Logger* logger, const std::shared_ptr<CaptureSink>& sink,
                          const std::string& value) {
    const std::vector<LogRecord> records = captureRecords(logger, sink, [&]() {
        logger->createMessage(TRACE, "Adv::hostileValue", "pre[{value}]post")
            .with("value", value)
            .log();
    });

    if (1 != records.size()) {
        ++failures;
        std::cerr << __FILE__ << ':' << __LINE__ << ": failed: expected exactly one record, got "
                  << records.size() << '\n';
        return LogRecord{};
    }

    return records.front();
}

void testHostileFieldValues() {
    Logger* const logger = LogManager::get("adv.hostile");
    logger->setLevel(TRACE);
    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();

    for (const HostileValue& hostile : hostileValues()) {
        const LogRecord record = recordWithValue(logger, sink, hostile.value);

        // The rendered message carries the value exactly once, literally: it
        // is never rescanned for template syntax of its own
        CHECK(contains(record.message, "pre[") && contains(record.message, "]post"));

        const LogField* const field = findField(record.fields, "value");
        CHECK(nullptr != field);
        if (nullptr != field)
            CHECK(std::holds_alternative<std::string>(field->value) &&
                  std::get<std::string>(field->value) == hostile.value);

        // JSON: always strictly valid, whatever the byte content of the field
        const std::string json = formatJson(record);
        if (!validJson(json)) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: invalid JSON for case '"
                      << hostile.name << "': " << json << '\n';
        }

        // Text: no raw C0 control byte other than an actual '\n', no 0x7f,
        // whatever the field contained
        const TextStyle style{false, false, false, 12};
        const std::string text = formatText(record, style);
        for (const unsigned char c : text) {
            if (c < 0x20 && '\n' != c) {
                ++failures;
                std::cerr << __FILE__ << ':' << __LINE__ << ": failed: raw control byte 0x"
                          << std::hex << static_cast<int>(c) << std::dec << " in text for case '"
                          << hostile.name << "'\n";
                break;
            }
            if (0x7f == c) {
                ++failures;
                std::cerr << __FILE__ << ':' << __LINE__
                          << ": failed: raw DEL byte in text for case '" << hostile.name << "'\n";
                break;
            }
        }

        // IRC: split into lines, format each, and check every one of them
        for (const std::pair<std::string, std::vector<LogSpan>>& line : splitLines(record)) {
            const std::string notice = formatIrcLine(record, line.first, line.second, true);

            CHECK(notice.find('\r') == std::string::npos);
            CHECK(notice.find('\n') == std::string::npos);

            for (const unsigned char c : notice) {
                if (c < 0x20 && 0x02 != c && 0x03 != c) {
                    ++failures;
                    std::cerr << __FILE__ << ':' << __LINE__ << ": failed: stray control byte 0x"
                              << std::hex << static_cast<int>(c) << std::dec
                              << " on an IRC line for case '" << hostile.name << "'\n";
                    break;
                }
                if (0x7f == c) {
                    ++failures;
                    std::cerr << __FILE__ << ':' << __LINE__
                              << ": failed: DEL byte on an IRC line for case '" << hostile.name
                              << "'\n";
                    break;
                }
            }
        }
    }
}

/**
 * A '\n' in the message makes a continuation line; a control character in the
 * logger name or in the function - even a real newline - never does, because
 * those two columns are escaped as single-line fields before they are ever
 * placed in the line.
 */
void testControlCharsNeverAddALine() {
    LogRecord record;
    record.level = TRACE;
    record.logger = std::string("adv.dirty\nname");
    record.function = std::string("Adv::dirty\nfunction");
    record.message = "one\ntwo\nthree";

    const std::string text = formatText(record, TextStyle{false, false, false, 20});

    // Two real newlines in the message: three lines, no more
    CHECK(3 == lineCount(text));
    CHECK(contains(text, "\\x0a"));

    // The IRC side: a record with a dirty name still gives one notice per
    // real line of the message, and the name in the brackets has no '\n' of
    // its own
    LogRecord record2 = record;
    record2.message = "single line";
    for (const std::pair<std::string, std::vector<LogSpan>>& line : splitLines(record2)) {
        const std::string notice = formatIrcLine(record2, line.first, line.second, true);
        CHECK(notice.find('\n') == std::string::npos);
        CHECK(notice.find("[adv.dirtyname]") != std::string::npos);
    }
}

/* ------------------------------------------------------------------ *
 * Hostile templates
 * ------------------------------------------------------------------ */

void testDanglingSpecColon() {
    // "{:" with nothing to close it is an unmatched '{': copied verbatim,
    // the same as a lone '{' - not the same code path as a well-formed
    // "{:spec}" whose spec std::format rejects
    const RenderResult result =
        renderTemplate("abc{:", std::vector<LogArg>{makeLogArg(1)}, std::vector<LogField>());
    CHECK_EQ(result.text, "abc{:");
    // The lone '{' before it is not a substitution, so there is no span
    CHECK(result.spans.empty());
}

void test5000Placeholders() {
    std::string tmpl;
    std::vector<LogArg> args;
    tmpl.reserve(5000 * 3);
    args.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        tmpl += "{}.";
        args.push_back(makeLogArg(i));
    }

    const auto start = std::chrono::steady_clock::now();
    const RenderResult result = renderTemplate(tmpl, args, std::vector<LogField>());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(5000 == result.spans.size());
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 2);

    // Spot-check the first, a middle, and the last substitution
    CHECK(result.text.rfind("0.", 0) == 0);
    CHECK(contains(result.text, ".2500."));
    CHECK(result.text.size() > 0 && '.' == result.text.back());
}

/**
 * "{:999999999}" and "{:>999999999}": a width so large that honouring it
 * literally means allocating close to a gigabyte for a single field value.
 * Run in a child process with its address space capped well below that, so
 * that if the implementation really does try to honour the width, we get a
 * fast, safe std::bad_alloc instead of the parent process actually using a
 * gigabyte of memory: the spec asks for the same finding either way, "must
 * not allocate gigabytes or crash".
 *
 * Exit codes: 0 the width was rejected or clamped (a bounded string, or the
 * placeholder left verbatim, as renderTemplate's format_error handler would
 * leave it); 1 an exception other than the format_error that renderTemplate
 * itself guards against reached the caller uncaught; 4 the call returned
 * successfully but with output in the hundreds of megabytes - "not a crash",
 * but still not what "bounded" was supposed to mean.
 */
int hugeWidthChild() {
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = 256 * 1024 * 1024; // 256 MiB address space
    setrlimit(RLIMIT_AS, &limit);
    alarm(5); // a hang is a failure too, not a hang of "make check"

    try {
        const std::vector<LogArg> argsA{makeLogArg(42)};
        const RenderResult a = renderTemplate("{:999999999}", argsA, std::vector<LogField>());

        const std::vector<LogArg> argsB{makeLogArg(42)};
        const RenderResult b = renderTemplate("{:>999999999}", argsB, std::vector<LogField>());

        const bool aBounded = a.text.size() < 10'000'000 || a.text == "{:999999999}";
        const bool bBounded = b.text.size() < 10'000'000 || b.text == "{:>999999999}";

        return (aBounded && bBounded) ? 0 : 4;
    } catch (...) {
        return 1;
    }
}

void testHugeWidthDoesNotBlowUp() {
    const pid_t child = fork();

    if (0 == child) {
        ::_exit(hugeWidthChild());
    }

    CHECK(child > 0);
    if (child <= 0)
        return;

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    CHECK(waited == child);

    if (WIFSIGNALED(status)) {
        ++failures;
        std::cerr << __FILE__ << ':' << __LINE__
                  << ": KNOWN_DEFECT: \"{:999999999}\" / \"{:>999999999}\" crashed the process "
                     "with signal "
                  << WTERMSIG(status)
                  << " instead of rendering bounded output or leaving the placeholder verbatim. "
                     "Suspected cause: libgnuworld/LogRender.cc renderTemplate() only catches "
                     "std::format_error around the positional-argument substitute() call; a "
                     "std::bad_alloc/std::length_error from std::vformat honouring an enormous "
                     "width is not a format_error and is not guarded against.\n";
        return;
    }

    CHECK(WIFEXITED(status));
    if (!WIFEXITED(status))
        return;

    const int code = WEXITSTATUS(status);
    if (0 == code)
        return; // bounded or left verbatim, as the spec asks

    ++failures;
    std::cerr << __FILE__ << ':' << __LINE__
              << ": KNOWN_DEFECT: \"{:999999999}\"/\"{:>999999999}\" did not render bounded "
                 "output ("
              << (1 == code ? "an uncaught exception escaped renderTemplate()"
                            : "the rendered value was hundreds of megabytes long")
              << "). Suspected cause: libgnuworld/LogRender.cc renderTemplate(), the "
                 "substitute(arg(spec)) call at its positional-argument branch, which only "
                 "catches std::format_error and lets std::vformat honour the width of a hostile "
                 "spec literally. A single crafted field value can force an allocation of "
                 "roughly a gigabyte.\n";
}

/* ------------------------------------------------------------------ *
 * The coloured name column's exact boundaries.  Independently derived from
 * the spec's colour rules, not copied from LogFormat.cc: palette
 * {36,35,34,32,33,96} indexed by the byte sum of the first segment of the
 * ORIGINAL (untruncated) name mod 6; the dim part is whatever printed text
 * lies at or after the first '.' of the original name; the whole column,
 * the '~' included, is dim when that '.' was cut away.
 * ------------------------------------------------------------------ */

int expectedPaletteColour(const std::string& name) {
    static const int palette[6] = {36, 35, 34, 32, 33, 96};
    unsigned int sum = 0;
    for (const char c : name.substr(0, name.find('.')))
        sum += static_cast<unsigned char>(c);
    return palette[sum % 6];
}

/// What the printed (possibly truncated) column looks like, computed the way
/// the spec describes truncation, independent of LogFormat.cc's own helper
std::string expectedPrinted(const std::string& name, std::size_t width) {
    if (0 == width || name.size() <= width)
        return name;
    return "~" + name.substr(name.size() - (width - 1));
}

/// Builds the whole coloured column exactly as the spec's rules would, given
/// the printed (already truncated) text and where in it the dim part starts
std::string expectedColouredColumn(int colour, const std::string& printed, std::size_t dimAt) {
    std::string out;
    const std::string normal = printed.substr(0, dimAt);
    const std::string dim = (dimAt >= printed.size()) ? std::string() : printed.substr(dimAt);

    if (!normal.empty())
        out += "\x1b[" + std::to_string(colour) + "m" + normal;
    if (!dim.empty())
        out += "\x1b[2;" + std::to_string(colour) + "m" + dim;
    out += "\x1b[0m";
    return out;
}

/**
 * Boundary 1: the first '.' of the original name is the FIRST visible
 * character right after the '~' of a truncated column - the earliest
 * position the dim part can start at without being cut away entirely.
 */
void testDimStartRightAfterTilde() {
    // 9 characters, '.', 10 characters: 20 characters total
    const std::string name = "123456789.abcdefghij";
    CHECK(20 == name.size());
    const std::size_t width = 12;

    const std::string printed = expectedPrinted(name, width);
    CHECK_EQ(printed, "~.abcdefghij"); // '.' is printed[1], right after '~'

    const int colour = expectedPaletteColour(name);
    const std::string expected = expectedColouredColumn(colour, printed, 1);

    const LogRecord record = LogRecord{{}, TRACE, name, "", "m", {}, {}, {}};
    const std::string line = formatText(record, TextStyle{false, true, true, width});
    CHECK(contains(line, expected));
}

/**
 * Boundary 2: the first '.' of the original name is the LAST character the
 * cut takes away - one position earlier than boundary 1, so that the '~'
 * itself is dim along with everything the column shows.
 */
void testDimStartWholeColumnDim() {
    // 8 characters, '.', 11 characters: 20 characters total; the cut for
    // width 12 keeps the last 11, so the '.' at index 8 is the last one gone
    const std::string name = "12345678.abcdefghijk";
    CHECK(20 == name.size());
    const std::size_t width = 12;

    const std::string printed = expectedPrinted(name, width);
    CHECK_EQ(printed, "~abcdefghijk"); // the '.' did not survive the cut at all

    const int colour = expectedPaletteColour(name);
    const std::string expected = expectedColouredColumn(colour, printed, 0); // whole column dim

    const LogRecord record = LogRecord{{}, TRACE, name, "", "m", {}, {}, {}};
    const std::string line = formatText(record, TextStyle{false, true, true, width});
    CHECK(contains(line, expected));
    // Nothing of it is in the bright, non-dim colour
    CHECK(!contains(line, "\x1b[" + std::to_string(colour) + "m~"));
}

/**
 * A name that is short enough raw, but only exceeds the name column once its
 * own control bytes are escaped to "\xNN": 9 visible characters plus 3
 * control bytes is 12 raw characters (not over a width-12 column), but 9 + 3
 * * 4 = 21 escaped characters is - truncation, and the colouring that goes
 * with it, must be computed on the ESCAPED name, which is what the column
 * actually shows and what its width is measured against.
 */
void testTruncationAfterControlByteEscaping() {
    const std::string rawName = std::string("abcdefghi") + '\x01' + '\x02' + '\x03';
    CHECK(12 == rawName.size());

    const std::string escapedName = "abcdefghi\\x01\\x02\\x03"; // 9 + 4*3 = 21 characters
    CHECK(21 == escapedName.size());

    const std::size_t width = 12;
    CHECK(rawName.size() <= width);    // not over width before escaping
    CHECK(escapedName.size() > width); // over width after escaping

    const std::string printed = expectedPrinted(escapedName, width);
    CHECK(printed.size() == width);
    CHECK('~' == printed.front());

    const LogRecord record = LogRecord{{}, TRACE, rawName, "", "m", {}, {}, {}};
    const std::string plain = formatText(record, TextStyle{false, false, false, width});
    CHECK(contains(plain, printed));

    // No '.' in this name at all, so nothing about it is ever dim
    const std::string coloured = formatText(record, TextStyle{false, true, true, width});
    CHECK(!contains(coloured, "\x1b[2;"));
}

/* ------------------------------------------------------------------ *
 * A null pointer of a type nobody registered an extractor for
 * ------------------------------------------------------------------ */

/// A type this test never calls Logger::registerExtractor<Nobody> for
struct Nobody {
    int x;
};

void testNullPointerWithoutExtractor() {
    Logger* const logger = LogManager::get("adv.nullptr");
    logger->setLevel(TRACE);
    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();

    Nobody* const nobody = nullptr;

    const std::vector<LogRecord> records = captureRecords(logger, sink, [&]() {
        logger->createMessage(TRACE, "Adv::nullptr", "who is {who}").with("who", nobody).log();
    });

    CHECK(1 == records.size());
    if (records.empty())
        return;

    const LogRecord& record = records.front();

    // The sentence says "(null)", exactly as it does for a registered type
    CHECK_EQ(record.message, "who is (null)");

    // Unlike a registered type's null pointer - which emits a display entry
    // plus a JSON-null field under the bare key - an unregistered type has
    // no display/sub-field split at all: "(null)" is the field's own string
    // value, not a null.  json.h's own doc comment on with() calls this out
    // ("any other pointer is its own address"); (null) is what a value of
    // such a type is when the address itself does not apply.
    const LogField* const who = findField(record.fields, "who");
    CHECK(nullptr != who);
    if (nullptr != who) {
        CHECK(!who->displayOnly);
        CHECK(std::holds_alternative<std::string>(who->value));
        if (std::holds_alternative<std::string>(who->value))
            CHECK_EQ(std::get<std::string>(who->value), "(null)");
    }

    const std::string json = formatJson(record);
    CHECK(validJson(json));
    CHECK(contains(json, "\"who\":\"(null)\""));
}

/* ------------------------------------------------------------------ *
 * Concurrency: 8 threads logging through a small hierarchy while the
 * configuration is repeatedly swapped underneath them, into one FileSink the
 * code attached (so it survives every configure()); the file itself is
 * re-read afterwards and every line must parse, with none lost or torn.
 * ------------------------------------------------------------------ */

std::string makeTempPath() {
    char path[] = "/tmp/logger_adversarial_XXXXXX";
    const int fd = mkstemp(path);
    if (-1 == fd) {
        ++failures;
        std::cerr << __FILE__ << ':' << __LINE__ << ": failed: mkstemp: " << std::strerror(errno)
                  << '\n';
        return std::string();
    }
    close(fd);
    return std::string(path);
}

void concurrencyBody() {
    const std::string path = makeTempPath();
    if (path.empty())
        return;

    Logger* const root = LogManager::get("adv");
    LogManager::setCodeDefault("adv", TRACE);

    const std::shared_ptr<FileSink> sink = std::make_shared<FileSink>(path, /*json=*/true);
    CHECK(sink->isOpen());
    root->addSink(sink, TRACE);

    const char* const names[3] = {"adv.a", "adv.a.b", "adv.c"};
    for (const char* const name : names)
        LogManager::get(name);

    // Two valid configurations, neither of which names "adv" or its sink:
    // the code-attached sink and the additive path to it survive every one
    // of the 200 swaps below by construction, which is exactly the point.
    LogConfig confDebug;
    LoggerSpec specDebug;
    specDebug.name = "adv.a";
    specDebug.level = DEBUG;
    confDebug.loggers.push_back(specDebug);

    LogConfig confTrace = confDebug;
    confTrace.loggers[0].level = TRACE;

    constexpr int perThread = 2000;
    constexpr int threadCount = 8;
    std::atomic<std::size_t> logged(0);
    std::vector<std::thread> workers;

    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([&, t]() {
            Logger* const logger = LogManager::get(names[t % 3]);
            for (int i = 0; i < perThread; ++i) {
                logger->writeFunc(DEBUG, "Adv::concurrency", "t{} i{}", t, i);
                logged.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::string> errors;
    for (int round = 0; round < 200; ++round)
        CHECK(LogManager::configure((0 == round % 2) ? confDebug : confTrace, errors));

    for (std::thread& worker : workers)
        worker.join();

    root->removeSink(sink);

    const std::size_t total = logged.load();
    CHECK(total == static_cast<std::size_t>(perThread * threadCount));

    std::ifstream file(path);
    CHECK(file.is_open());

    std::size_t lines = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty())
            continue;
        ++lines;
        if (!validJson(line)) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__
                      << ": failed: torn or invalid JSON line in the concurrency file: [" << line
                      << "]\n";
        }
    }
    file.close();
    std::remove(path.c_str());

    CHECK(lines == total);
}

void testConcurrentLoggingAndConfigure() {
    std::atomic<bool> done(false);
    std::thread runner([&done]() {
        concurrencyBody();
        done.store(true);
    });

    for (int tenth = 0; tenth < 100 && !done.load(); ++tenth)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!done.load()) {
        std::cerr << __FILE__ << ": the concurrency case did not finish: deadlock?\n";
        std::cerr.flush();
        std::cout.flush();
        std::_Exit(1);
    }

    runner.join();
}

/* ------------------------------------------------------------------ *
 * elog under fire: hostile content, four threads, exact count, no
 * interleaving.  Distinct from test/elog_shim.cc's own thread test, which
 * uses plain "t1-<n>" content to prove the per-thread buffer never mixes two
 * threads' output; this asks the same question of content designed to look
 * like it could confuse a naive implementation - embedded newlines, unicode,
 * NUL, control bytes - none of which are template syntax to elog, which never
 * parses what it is given.
 * ------------------------------------------------------------------ */

std::string hostilePayload(int thread, int n) {
    static const std::vector<std::string> junk = {
        "\n embedded newline",       "\x1b[31mred\x1b[0m", "caf\xc3\xa9",
        std::string("nul\0here", 8), "\r\ncrlf",           "%s%d%n"};
    std::ostringstream out;
    out << "adv-elog-" << thread << '-' << n << '-';
    // Streamed as a std::string, not a const char*, so an embedded NUL is
    // carried by length like every other byte, not read as a terminator
    out << junk[n % junk.size()];
    return out.str();
}

void testElogUnderFire() {
    Logger* const legacy = LogManager::get("legacy");
    legacy->setLevel(TRACE);
    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    legacy->addSink(sink, TRACE);

    constexpr int perThread = 1000;
    constexpr int threadCount = 4;

    const auto writer = [](int thread) {
        for (int n = 0; n < perThread; ++n)
            elog << hostilePayload(thread, n) << std::endl;
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t)
        threads.emplace_back(writer, t);
    for (std::thread& thread : threads)
        thread.join();

    legacy->removeSink(sink);

    CHECK(static_cast<std::size_t>(perThread * threadCount) == sink->records.size());

    // Every expected line appears exactly once: nothing lost, nothing
    // merged, nothing torn in half by another thread's write
    std::multiset<std::string> expected;
    for (int t = 0; t < threadCount; ++t)
        for (int n = 0; n < perThread; ++n) {
            std::ostringstream out;
            out << hostilePayload(t, n);
            // elog's buffer is a std::ostringstream: an embedded NUL is kept
            // by std::string's length, not truncated at the NUL, exactly like
            // every other field value in this logging system
            expected.insert(out.str());
        }

    std::multiset<std::string> actual;
    for (const LogRecord& record : sink->records) {
        CHECK(DEBUG == record.level); // every elog line is DEBUG, always
        actual.insert(record.message);
    }

    CHECK(expected == actual);
}

} // namespace

int main() {
    // The bootstrap console sink would print every record any of these
    // cases logs; what a sink is actually given is what the test is about
    LogManager::root()->removeSink(LogManager::bootstrapConsoleSink());

    testHostileFieldValues();
    testControlCharsNeverAddALine();
    testDanglingSpecColon();
    test5000Placeholders();
    testHugeWidthDoesNotBlowUp();
    testDimStartRightAfterTilde();
    testDimStartWholeColumnDim();
    testTruncationAfterControlByteEscaping();
    testNullPointerWithoutExtractor();
    testConcurrentLoggingAndConfigure();
    testElogUnderFire();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "logger_adversarial: all checks passed\n";
    return 0;
}
