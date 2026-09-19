/**
 * logger_ratelimit.cc
 * Unit test for libgnuworld/LogRateLimit.h: the token bucket a sink that talks
 * to the outside world holds, the count of what it refused, and the "<N>/min"
 * of a configuration file it is built from.  It links libgnuworld alone.
 * Runs under "make check".
 *
 * Every case that is about the refill injects the clock rather than sleeping: a
 * test that waits for a bucket to fill is a test that is slow when it passes
 * and flaky when the machine is busy.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "LogRateLimit.h"

using namespace gnuworld;

using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {

int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n';                \
        }                                                                                          \
    } while (0)

/// Reports the value as well as the failure, for the counts and the strings
#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const auto got_ = (actual);                                                                \
        const auto want_ = (expected);                                                             \
        if (got_ != want_) {                                                                       \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #actual "\n  got  [" << got_  \
                      << "]\n  want [" << want_ << "]\n";                                          \
        }                                                                                          \
    } while (0)

/// An arbitrary point of the steady clock, the origin of every injected time
const steady_clock::time_point origin = steady_clock::now();

/* ------------------------------------------------------------------ *
 * The bucket
 * ------------------------------------------------------------------ */

/// A fresh bucket is full: the first count records go through, the next do not
void testStartsFull() {
    LogRateLimit limit(3, seconds(60));

    CHECK(limit.admit(origin));
    CHECK(limit.admit(origin));
    CHECK(limit.admit(origin));

    // And nothing more at the same instant, however often it is asked
    for (int at = 0; at < 10; ++at)
        CHECK(!limit.admit(origin));
}

/// A bucket of one is the smallest there is, and behaves like any other
void testCapacityOfOne() {
    LogRateLimit limit(1, seconds(60));

    CHECK(limit.admit(origin));
    CHECK(!limit.admit(origin));

    // Half a window later there is still nothing to have
    CHECK(!limit.admit(origin + seconds(29)));

    // A whole window later there is exactly one
    CHECK(limit.admit(origin + seconds(60)));
    CHECK(!limit.admit(origin + seconds(60)));
}

/// The refill is continuous, not a window that empties all at once
void testContinuousRefill() {
    LogRateLimit limit(6, seconds(60));

    // Empty it
    for (int at = 0; at < 6; ++at)
        CHECK(limit.admit(origin));

    CHECK(!limit.admit(origin));

    // 6 per 60 seconds is one every ten: at nine seconds there is none yet
    CHECK(!limit.admit(origin + seconds(9)));

    // At ten there is one, and only one
    CHECK(limit.admit(origin + seconds(10)));
    CHECK(!limit.admit(origin + seconds(10)));

    // Thirty seconds in, three have accrued and one of them was taken
    CHECK(limit.admit(origin + seconds(30)));
    CHECK(limit.admit(origin + seconds(30)));
    CHECK(!limit.admit(origin + seconds(30)));
}

/// However long it stands idle, a bucket holds no more than its capacity
void testCapacityIsTheCeiling() {
    LogRateLimit limit(4, seconds(60));

    // An hour of silence, which would be 240 tokens if they accumulated
    const steady_clock::time_point later = origin + seconds(3600);

    for (int at = 0; at < 4; ++at)
        CHECK(limit.admit(later));

    CHECK(!limit.admit(later));
}

/// A clock that goes backwards neither fills the bucket nor empties it
void testTimeGoingBackwards() {
    LogRateLimit limit(2, seconds(60));

    CHECK(limit.admit(origin + seconds(600)));
    CHECK(limit.admit(origin + seconds(600)));
    CHECK(!limit.admit(origin + seconds(600)));

    // Asked about a moment before the last one, which cannot add anything
    CHECK(!limit.admit(origin));
    CHECK(!limit.admit(origin + seconds(600)));

    // And the refill still runs from the latest moment it was told about
    CHECK(limit.admit(origin + seconds(630)));
}

/* ------------------------------------------------------------------ *
 * What it refused
 * ------------------------------------------------------------------ */

void testSuppressedCountsAndResets() {
    LogRateLimit limit(2, seconds(60));

    CHECK_EQ(limit.takeSuppressed(), std::size_t(0));

    CHECK(limit.admit(origin));
    CHECK(limit.admit(origin));

    for (int at = 0; at < 5; ++at)
        CHECK(!limit.admit(origin));

    CHECK_EQ(limit.takeSuppressed(), std::size_t(5));

    // Taken means taken: the next caller hears about the refusals since
    CHECK_EQ(limit.takeSuppressed(), std::size_t(0));

    CHECK(!limit.admit(origin));
    CHECK(!limit.admit(origin));

    CHECK_EQ(limit.takeSuppressed(), std::size_t(2));
    CHECK_EQ(limit.takeSuppressed(), std::size_t(0));
}

/// An admitted record adds nothing to the count
void testAdmittedAreNotSuppressed() {
    LogRateLimit limit(3, seconds(60));

    CHECK(limit.admit(origin));
    CHECK(limit.admit(origin));
    CHECK(limit.admit(origin));

    CHECK_EQ(limit.takeSuppressed(), std::size_t(0));
}

/* ------------------------------------------------------------------ *
 * What the configuration file writes
 * ------------------------------------------------------------------ */

void testParse() {
    std::size_t count = 0;
    seconds per(0);

    CHECK(LogRateLimit::parse("10/min", count, per));
    CHECK_EQ(count, std::size_t(10));
    CHECK_EQ(per.count(), seconds(60).count());

    CHECK(LogRateLimit::parse("1/hour", count, per));
    CHECK_EQ(count, std::size_t(1));
    CHECK_EQ(per.count(), seconds(3600).count());

    // The spaces around the slash are optional, on either side
    CHECK(LogRateLimit::parse("5 / sec", count, per));
    CHECK_EQ(count, std::size_t(5));
    CHECK_EQ(per.count(), seconds(1).count());

    // The unit is read without regard to case, like every other value
    CHECK(LogRateLimit::parse("3/MIN", count, per));
    CHECK_EQ(count, std::size_t(3));
    CHECK_EQ(per.count(), seconds(60).count());

    CHECK(LogRateLimit::parse("  7/Hour  ", count, per));
    CHECK_EQ(count, std::size_t(7));
    CHECK_EQ(per.count(), seconds(3600).count());
}

void testParseRefuses() {
    std::size_t count = 99;
    seconds per(99);

    const char* const refused[] = {
        "0/min",  // a rate of nothing is not a rate
        "-1/min", // and neither is less than nothing
        "10",     // no unit at all
        "10/",    // nor here
        "/min",   // no count
        "10/day", // not a unit this understands
        "x/min",  // not a count
        "10/min/min",
        "10 20/min",
        "min/10",
        "",
        " ",
        "99999999999999999999/min", // more than a count can hold
        "1e3/min",                  // not digits
        "10.5/min",
        "+10/min",
    };

    for (const char* const text : refused) {
        std::size_t gotCount = 99;
        seconds gotPer(99);

        if (LogRateLimit::parse(text, gotCount, gotPer)) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: parse(\"" << text
                      << "\") was accepted\n";
        }
    }

    // A refused text leaves what it was given to fill in alone
    CHECK_EQ(count, std::size_t(99));
    CHECK_EQ(per.count(), seconds(99).count());
}

/// What a message says the limit is, which is the rate and not the file's text
void testDescribe() {
    CHECK_EQ(LogRateLimit(10, seconds(60)).describe(), std::string("10/min"));
    CHECK_EQ(LogRateLimit(1, seconds(3600)).describe(), std::string("1/hour"));
    CHECK_EQ(LogRateLimit(5, seconds(1)).describe(), std::string("5/sec"));

    // A period nothing in the file can ask for still describes itself
    CHECK_EQ(LogRateLimit(2, seconds(90)).describe(), std::string("2/90s"));
}

/* ------------------------------------------------------------------ *
 * Four threads at one bucket
 * ------------------------------------------------------------------ */

/**
 * The capacity is the capacity however many threads ask at once: every thread
 * is given the same instant, so nothing refills while they run and the number
 * admitted is exactly the capacity - not "about" it.
 */
void testThreads() {
    const std::size_t capacity = 100;

    LogRateLimit limit(capacity, seconds(60));

    std::atomic<std::size_t> admitted(0);
    std::vector<std::thread> threads;

    for (int at = 0; at < 4; ++at)
        threads.emplace_back([&limit, &admitted]() {
            for (int each = 0; each < 1000; ++each)
                if (limit.admit(origin))
                    ++admitted;
        });

    for (std::thread& thread : threads)
        thread.join();

    CHECK_EQ(admitted.load(), capacity);

    // And every one of the 4000 - 100 refusals was counted, exactly once
    CHECK_EQ(limit.takeSuppressed(), std::size_t(4 * 1000) - capacity);
}

} // namespace

int main() {
    testStartsFull();
    testCapacityOfOne();
    testContinuousRefill();
    testCapacityIsTheCeiling();
    testTimeGoingBackwards();
    testSuppressedCountsAndResets();
    testAdmittedAreNotSuppressed();
    testParse();
    testParseRefuses();
    testDescribe();
    testThreads();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_ratelimit: all checks passed\n";
    return 0;
}
