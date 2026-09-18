/**
 * numbers.cc
 * Unit test for parseNumber(), in libgnuworld/misc.h, and the safe accessors
 * of xParameters.
 * Runs under "make check".
 */

#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <string>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "misc.h"
#include "xparameters.h"

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

void testParseNumber() {
    CHECK(parseNumber<int>("0") == 0);
    CHECK(parseNumber<int>("42") == 42);
    CHECK(parseNumber<int>("-42") == -42);
    CHECK(parseNumber<std::time_t>("1789637405") == std::time_t{1789637405});
    CHECK(parseNumber<unsigned>("007") == 7U);

    // What atoi() would have called 0, or something else that is wrong
    CHECK(!parseNumber<int>(""));
    CHECK(!parseNumber<int>("stray"));
    CHECK(!parseNumber<int>("12abc")); // atoi: 12
    CHECK(!parseNumber<int>(" 12"));   // atoi: 12
    CHECK(!parseNumber<int>("12 "));
    CHECK(!parseNumber<int>("+12"));
    CHECK(!parseNumber<int>("1.5"));
    CHECK(!parseNumber<int>("0x10"));
    CHECK(!parseNumber<int>("-"));
    CHECK(!parseNumber<int>(std::string_view("12\0", 3)));

    // An unsigned type takes no sign
    CHECK(!parseNumber<unsigned>("-1"));

    // It has to fit
    CHECK(parseNumber<std::int32_t>("2147483647") == std::numeric_limits<std::int32_t>::max());
    CHECK(!parseNumber<std::int32_t>("2147483648"));
    CHECK(!parseNumber<std::int32_t>("99999999999999999999"));
    CHECK(parseNumber<std::uint8_t>("255") == std::uint8_t{255});
    CHECK(!parseNumber<std::uint8_t>("256"));
}

/// True if asking for params[pos] kills the process with SIGABRT.  Asked in
/// a child, so that the test survives it.
bool abortsOn(const xParameters& params, xParameters::size_type pos) {
    const pid_t child = ::fork();
    if (0 == child) {
        // The message about the line goes to elog, which is not open here
        ::close(STDERR_FILENO);
        const char* value = params[pos];
        ::_exit(value != nullptr ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFSIGNALED(status) && SIGABRT == WTERMSIG(status);
}

void testParameters() {
    char source[] = "AB";
    char channel[] = "#chan";
    char timestamp[] = "1789637405";
    char stray[] = "stray";

    xParameters params;
    params << source << channel << timestamp << stray;

    CHECK(params.size() == 4);
    CHECK(params.has(3) && !params.has(4));
    CHECK(params.view(1) == "#chan");
    CHECK(parseNumber<std::time_t>(params.view(2)) == std::time_t{1789637405});
    CHECK(!parseNumber<std::time_t>(params.view(3))); // there, but not a number
    CHECK(!parseNumber<std::time_t>(params.view(9))); // not there

    // A parameter that may be absent is asked for with has() or view()
    CHECK(params.view(4).empty());
    CHECK(params.assemble(9).empty());
    CHECK(params.assemble(2) == "1789637405 stray");

    const xParameters none;
    CHECK(none.view(0).empty() && none.assemble(0).empty());

    // operator[] past the end is a line shorter than the protocol has it,
    // or a handler that did not count: the process aborts
    CHECK(abortsOn(params, 4));
    CHECK(abortsOn(params, 1000));
    CHECK(abortsOn(none, 0));
    CHECK(!abortsOn(params, 3));
}

} // namespace

int main() {
    testParseNumber();
    testParameters();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "numbers: all checks passed\n";
    return 0;
}
