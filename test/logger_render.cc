/**
 * logger_render.cc
 * Unit test for the log record types in libgnuworld/LogRecord.h and the
 * single-pass template renderer in libgnuworld/LogRender.h.
 * Runs under "make check".
 */

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "LogRecord.h"
#include "LogRender.h"

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

/// True when the spans of a result are exactly the given [begin,end) pairs.
bool spansAre(const RenderResult& result,
              const std::vector<std::pair<std::size_t, std::size_t>>& expected) {
    if (result.spans.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (result.spans[i].begin != expected[i].first || result.spans[i].end != expected[i].second)
            return false;
    return true;
}

/// The substring of the rendered text that a span covers.
std::string spanText(const RenderResult& result, std::size_t index) {
    if (index >= result.spans.size())
        return std::string();
    const LogSpan& span = result.spans[index];
    if (span.begin > span.end || span.end > result.text.size())
        return std::string();
    return result.text.substr(span.begin, span.end - span.begin);
}

LogField field(const std::string& key, const LogValue& value) {
    return LogField{key, value, false};
}

void testPositional() {
    const std::vector<LogArg> args{makeLogArg(1), makeLogArg("x")};
    const RenderResult result = renderTemplate("a {} b {}", args, {});

    CHECK(result.text == "a 1 b x");
    CHECK(spansAre(result, {{2, 3}, {6, 7}}));
    CHECK(spanText(result, 0) == "1");
    CHECK(spanText(result, 1) == "x");
}

void testPositionalSpec() {
    const std::vector<LogArg> good{makeLogArg(42)};
    const RenderResult padded = renderTemplate("{:>5}", good, {});
    CHECK(padded.text == "   42");
    CHECK(spansAre(padded, {{0, 5}}));

    // A spec std::format cannot parse leaves the placeholder verbatim, and
    // still consumes the argument.
    const std::vector<LogArg> bad{makeLogArg(42), makeLogArg(7)};
    const RenderResult broken = renderTemplate("{:zz}", bad, {});
    CHECK(broken.text == "{:zz}");
    CHECK(broken.spans.empty());

    const RenderResult consumed = renderTemplate("{:zz}{}", bad, {});
    CHECK(consumed.text == "{:zz}7");
}

void testNamed() {
    const std::vector<LogField> fields{field("nick", std::string("MrIron")),
                                       field("chan", std::string("#gnuworld"))};
    const RenderResult result = renderTemplate("{nick} failed to add to {chan}", {}, fields);

    CHECK(result.text == "MrIron failed to add to #gnuworld");
    CHECK(result.spans.size() == 2);
    CHECK(spanText(result, 0) == "MrIron");
    CHECK(spanText(result, 1) == "#gnuworld");
    CHECK(spansAre(result, {{0, 6}, {24, 33}}));
}

void testNoRescanning() {
    // A substituted value that looks like a placeholder is not expanded again.
    const std::vector<LogField> fields{field("nick", std::string("{reason}")),
                                       field("reason", std::string("spam"))};
    const RenderResult result = renderTemplate("{nick}: {reason}", {}, fields);

    CHECK(result.text == "{reason}: spam");
    CHECK(spansAre(result, {{0, 8}, {10, 14}}));
}

void testRepeatedName() {
    const std::vector<LogField> fields{field("a", std::string("x"))};
    const RenderResult result = renderTemplate("{a} {a}", {}, fields);

    CHECK(result.text == "x x");
    CHECK(spansAre(result, {{0, 1}, {2, 3}}));
}

void testFirstFieldWins() {
    const std::vector<LogField> fields{field("a", std::string("first")),
                                       field("a", std::string("second"))};
    const RenderResult result = renderTemplate("{a}", {}, fields);
    CHECK(result.text == "first");
}

void testEscapes() {
    const RenderResult result = renderTemplate("{{}} {{nick}}", {}, {});
    CHECK(result.text == "{} {nick}");
    CHECK(result.spans.empty());
}

void testVerbatim() {
    const std::vector<LogField> fields{field("nick", std::string("MrIron"))};

    // An unknown name is copied verbatim
    const RenderResult unknown = renderTemplate("{nope}", {}, fields);
    CHECK(unknown.text == "{nope}");
    CHECK(unknown.spans.empty());

    // An exhausted positional is copied verbatim
    const RenderResult exhausted = renderTemplate("{} {}", {makeLogArg(1)}, {});
    CHECK(exhausted.text == "1 {}");
    CHECK(spansAre(exhausted, {{0, 1}}));

    const RenderResult noArgs = renderTemplate("{:>5}", {}, {});
    CHECK(noArgs.text == "{:>5}");

    // An unmatched '{' and a lone '}' are copied verbatim
    CHECK(renderTemplate("a { b", {}, {}).text == "a { b");
    CHECK(renderTemplate("{nick", {}, fields).text == "{nick");
    CHECK(renderTemplate("a } b", {}, {}).text == "a } b");
    CHECK(renderTemplate("}", {}, {}).text == "}");
    CHECK(renderTemplate("{", {}, {}).text == "{");

    // A name with a character outside [A-Za-z0-9_] is not a placeholder
    const RenderResult dotted = renderTemplate("{a.b}", {}, {field("a.b", std::string("v"))});
    CHECK(dotted.text == "{a.b}");
    CHECK(dotted.spans.empty());

    // An empty name is not a placeholder either
    CHECK(renderTemplate("{}", {}, {}).text == "{}");
}

void testTypedValues() {
    CHECK(logValueToString(LogValue{}) == "(null)");
    CHECK(logValueToString(LogValue{std::monostate{}}) == "(null)");
    CHECK(logValueToString(LogValue{true}) == "true");
    CHECK(logValueToString(LogValue{false}) == "false");
    CHECK(logValueToString(LogValue{std::int64_t{-5}}) == "-5");
    CHECK(logValueToString(LogValue{std::uint64_t{18446744073709551615ULL}}) ==
          "18446744073709551615");
    CHECK(logValueToString(LogValue{1.5}) == "1.5");
    CHECK(logValueToString(LogValue{std::string("plain")}) == "plain");

    const std::vector<LogField> fields{field("flag", true), field("count", std::int64_t{-5}),
                                       field("missing", LogValue{})};
    const RenderResult result = renderTemplate("{flag} {count} {missing}", {}, fields);
    CHECK(result.text == "true -5 (null)");
    CHECK(spansAre(result, {{0, 4}, {5, 7}, {8, 14}}));
}

void testMakeLogArgLifetime() {
    // A char array must be copied into the argument, not referenced
    std::vector<LogArg> args;
    {
        char buffer[] = "gone";
        args.push_back(makeLogArg(buffer));
    }
    CHECK(renderTemplate("{}", args, {}).text == "gone");

    // std::string and const char* likewise
    std::vector<LogArg> more;
    {
        const std::string owned("kept");
        more.push_back(makeLogArg(owned));
        more.push_back(makeLogArg(owned.c_str()));
    }
    CHECK(renderTemplate("{} {}", more, {}).text == "kept kept");
}

void testLevels() {
    CHECK(std::string(levelName(FATAL)) == "FATAL");
    CHECK(std::string(levelName(ERROR)) == "ERROR");
    CHECK(std::string(levelName(WARN)) == "WARNING");
    CHECK(std::string(levelName(INFO)) == "INFO");
    CHECK(std::string(levelName(DEBUG)) == "DEBUG");
    CHECK(std::string(levelName(TRACE)) == "TRACE");
    CHECK(std::string(levelName(SQL)) == "SQL");
    CHECK(std::string(levelName(OFF)) == "OFF");

    CHECK(std::string(levelColumn(FATAL)) == "FATAL");
    CHECK(std::string(levelColumn(ERROR)) == "ERROR");
    CHECK(std::string(levelColumn(WARN)) == "WARN ");
    CHECK(std::string(levelColumn(INFO)) == "INFO ");
    CHECK(std::string(levelColumn(DEBUG)) == "DEBUG");
    CHECK(std::string(levelColumn(TRACE)) == "TRACE");

    CHECK(std::string(levelTag(FATAL)) == "[F]");
    CHECK(std::string(levelTag(ERROR)) == "[E]");
    CHECK(std::string(levelTag(WARN)) == "[W]");
    CHECK(std::string(levelTag(INFO)) == "[I]");
    CHECK(std::string(levelTag(DEBUG)) == "[D]");
    CHECK(std::string(levelTag(TRACE)) == "[T]");
}

void testParseLevel() {
    Verbosity level = TRACE;

    CHECK(parseLevel("warn", level) && level == WARN);
    CHECK(parseLevel("WARNING", level) && level == WARN);
    CHECK(parseLevel("Off", level) && level == OFF);
    CHECK(parseLevel("FATAL", level) && level == FATAL);
    CHECK(parseLevel("error", level) && level == ERROR);
    CHECK(parseLevel("InFo", level) && level == INFO);
    CHECK(parseLevel("debug", level) && level == DEBUG);
    CHECK(parseLevel("TRACE", level) && level == TRACE);

    // SQL is not a configurable level, and neither is anything else
    level = INFO;
    CHECK(!parseLevel("SQL", level));
    CHECK(!parseLevel("bogus", level));
    CHECK(!parseLevel("", level));
    CHECK(!parseLevel("99", level));
    CHECK(level == INFO); // untouched on failure
}

} // namespace

int main() {
    testPositional();
    testPositionalSpec();
    testNamed();
    testNoRescanning();
    testRepeatedName();
    testFirstFieldWins();
    testEscapes();
    testVerbatim();
    testTypedValues();
    testMakeLogArgLifetime();
    testLevels();
    testParseLevel();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_render: all checks passed\n";
    return 0;
}
