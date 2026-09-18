/**
 * checkedformat.cc
 * Unit test for CheckedFormat, in libgnuworld/misc.h.  Runs under "make check".
 *
 * What matters most about CheckedFormat is what it refuses to compile, and a
 * test program cannot contain that.  These do not compile, as intended:
 *
 *     send("%s is not on %s", nick, chan);   // printf left behind: no fields, two arguments
 *     send("{} only", nick, chan);           // an argument no field uses
 *     send("{} {}", nick);                   // a field with no argument (std::format's own check)
 */

#include <iostream>
#include <string>
#include <utility>

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

/// The shape every network function has: Notice(), Message(), Write()...
template <typename... Args> std::string send(CheckedFormat<Args...> fmt, Args&&... args) {
    return formatMessage<Args...>(fmt, std::forward<Args>(args)...);
}

// How many arguments a format string consumes
static_assert(detail::formatArgumentCount("") == 0);
static_assert(detail::formatArgumentCount("no fields") == 0);
static_assert(detail::formatArgumentCount("{}") == 1);
static_assert(detail::formatArgumentCount("{} is on {} ({} users)") == 3);
static_assert(detail::formatArgumentCount("{{literal}} {}") == 1);
static_assert(detail::formatArgumentCount("{{}}") == 0);
static_assert(detail::formatArgumentCount("{:#x} {:>10} {:02}") == 3);
static_assert(detail::formatArgumentCount("{1} {0} {1}") == 2); // numbered: the highest plus one
static_assert(detail::formatArgumentCount("{:{}}") == 2);       // a dynamic width is an argument
static_assert(detail::formatArgumentCount("{0:{1}}") == 2);
// A printf string has no fields at all
static_assert(detail::formatArgumentCount("%s is not on %s (%d users)") == 0);

// How many arguments a printf format consumes
static_assert(detail::printfArgumentCount("") == 0);
static_assert(detail::printfArgumentCount("%s is not on %s (%d users)") == 3);
static_assert(detail::printfArgumentCount("%-12s %05u %#lx %llu %.2f %c %p") == 7);
static_assert(detail::printfArgumentCount("%*s %.*f") == 4); // a '*' is an argument too
static_assert(detail::printfArgumentCount("100%% of %s") == 1);
// A '%' in a text is no conversion
static_assert(detail::printfArgumentCount("100% sure, {}") == 0);
static_assert(detail::printfArgumentCount("{}% done") == 0);
static_assert(detail::printfArgumentCount("ends in %") == 0);
static_assert(detail::printfArgumentCount("{} is on {}") == 0);

} // namespace

int main() {
    const std::string nick = "bob";

    CHECK(send("plain") == "plain");
    CHECK(send("{} is on {}", nick, "#chan") == "bob is on #chan");
    CHECK(send("{:04} {:#x} {:>5}|{:<5}|", 7, 255, "ab", "cd") == "0007 0xff    ab|cd   |");
    CHECK(send("{1} before {0}", "a", "b") == "b before a");
    CHECK(send("{{}} is literal, {} is not", 1) == "{} is literal, 1 is not");
    CHECK(send("100% sure, {}", nick) == "100% sure, bob");

    // printf syntax, which the modules still write.  No va_list: a
    // std::string for a %s is a string.
    CHECK(send("%s is on %s", nick, "#chan") == "bob is on #chan");
    CHECK(send("%s is on %s", nick.c_str(), std::string_view("#chan")) == "bob is on #chan");
    CHECK(send("%-6s|%04d|%#x|%5.1f|%c|%lu", "ab", 7, 255, 2.25, 'x', 123456789UL) ==
          "ab    |0007|0xff|  2.2|x|123456789");
    CHECK(send("%*d|%.*s", 4, 7, 2, "abcdef") == "   7|ab");
    CHECK(send("100%% of %s", nick) == "100% of bob");
    CHECK(send("%u %d", static_cast<unsigned short>(65535), true) == "65535 1");

    // A format that is read at run time, as from the language tables of
    // cservice: printf, and not checked
    const std::string fromTable = "%s has %d channels";
    CHECK(send(fromTable.c_str(), nick, 3) == "bob has 3 channels");
    const char* noArguments = "100%% done";
    CHECK(send(noArguments) == "100% done");
    const std::string longText(5000, 'x');
    CHECK(send("%s!", longText).size() == 5001);

    // What the converter warns about: std::format does not print these as %d did
    CHECK(send("{}", true) == "true");
    CHECK(send("{}", 'x') == "x");
    CHECK(send("{}", static_cast<int>(true)) == "1");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "checkedformat: all checks passed\n";
    return 0;
}
