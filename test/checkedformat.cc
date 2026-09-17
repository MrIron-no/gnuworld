/**
 * checkedformat.cc
 * Unit test for libgnuworld/CheckedFormat.h.  Runs under "make check".
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

#include "CheckedFormat.h"

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
    return std::format(fmt.format, std::forward<Args>(args)...);
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
// A printf string has no fields at all, which is what gives it away
static_assert(detail::formatArgumentCount("%s is not on %s (%d users)") == 0);

} // namespace

int main() {
    const std::string nick = "bob";

    CHECK(send("plain") == "plain");
    CHECK(send("{} is on {}", nick, "#chan") == "bob is on #chan");
    CHECK(send("{:04} {:#x} {:>5}|{:<5}|", 7, 255, "ab", "cd") == "0007 0xff    ab|cd   |");
    CHECK(send("{1} before {0}", "a", "b") == "b before a");
    CHECK(send("{{}} is literal, {} is not", 1) == "{} is literal, 1 is not");
    CHECK(send("100% sure, {}", nick) == "100% sure, bob");

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
