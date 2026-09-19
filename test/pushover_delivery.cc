/**
 * pushover_delivery.cc
 * What a pushover sink really puts on the wire, against a local HTTP endpoint
 * that records every POST it is given: one request per user key, the loop guard,
 * the rate limit, a failing endpoint, and destroying a sink with requests still
 * queued.  It links libnotifier, libgnuworld and libcurl, and is built only
 * where libcurl is (see test/Makefile.am, COND_LIBCURL).
 * Runs under "make check", where test/docker/full-deps/run.sh is what gives it
 * an endpoint; without one it says so and passes.
 *
 * The endpoint's URL and the file it records the bodies in come from the
 * environment:
 *
 *   PUSHOVER_TEST_URL       http://127.0.0.1:<port>/1/messages.json
 *   PUSHOVER_TEST_CAPTURE   the file it appends one POST body per line to
 *
 * and two paths of that endpoint are special: /fail answers 500, /slow answers
 * a success after half a second.
 */

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "LogConfig.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"
#include "pushover.h"

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
        const auto got_ = (actual);                                                                \
        const auto want_ = (expected);                                                             \
        if (got_ != want_) {                                                                       \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #actual "\n  got  [" << got_  \
                      << "]\n  want [" << want_ << "]\n";                                          \
        }                                                                                          \
    } while (0)

/**
 * The token this test's sinks are built with.  It is recognisable on purpose:
 * it must be in every POST body and in no log record at all.
 */
const std::string token("SECRET-token-no-record-may-carry-0123456789");

std::string endpointUrl;
std::string capturePath;
std::string configPath;

bool contains(const std::string& haystack, const std::string& needle) {
    return std::string::npos != haystack.find(needle);
}

/// The URL of one of the endpoint's paths: "" the ordinary one, or /fail, /slow
std::string urlFor(const std::string& path) {
    if (path.empty())
        return endpointUrl;

    // Everything up to the third '/' of "http://host:port/..." is the endpoint
    const std::string::size_type slash = endpointUrl.find('/', endpointUrl.find("//") + 2);

    return (std::string::npos == slash ? endpointUrl : endpointUrl.substr(0, slash)) + path;
}

/* ------------------------------------------------------------------ *
 * The endpoint's record of what was posted
 * ------------------------------------------------------------------ */

std::vector<std::string> captured() {
    std::vector<std::string> lines;
    std::ifstream in(capturePath.c_str());
    std::string line;

    while (std::getline(in, line))
        if (!line.empty())
            lines.push_back(line);

    return lines;
}

void forgetCaptured() { std::ofstream out(capturePath.c_str(), std::ios::out | std::ios::trunc); }

/// Whatever was posted once at least count bodies are there, or time is up
std::vector<std::string> capturedAtLeast(std::size_t count, int milliseconds = 5000) {
    for (int waited = 0; waited < milliseconds; waited += 20) {
        const std::vector<std::string> lines = captured();

        if (lines.size() >= count)
            return lines;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return captured();
}

/// Whatever was posted after long enough for anything on its way to arrive
std::vector<std::string> capturedAfterAWhile(int milliseconds = 1500) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));

    return captured();
}

/* ------------------------------------------------------------------ *
 * The configuration, written and read like any other logging.conf
 * ------------------------------------------------------------------ */

/**
 * Applies a configuration through the whole of the real path: the file is
 * parsed by parseLogConfig() and the sinks are built by the factories
 * registerSinkType() knows, so what is exercised here is the sink a
 * logging.conf really gets and not one this test made by hand.
 */
bool apply(const std::string& body) {
    {
        std::ofstream out(configPath.c_str(), std::ios::out | std::ios::trunc);

        out << body;
    }

    LogConfig config;
    std::vector<std::string> errors;

    if (!parseLogConfig(configPath, config, errors)) {
        for (const std::string& error : errors)
            std::cerr << "  parse: " << error << '\n';

        return false;
    }

    if (!LogManager::configure(config, errors)) {
        for (const std::string& error : errors)
            std::cerr << "  configure: " << error << '\n';

        return false;
    }

    return true;
}

/// One record on this logger, through the ordinary call path
void emit(const std::string& logger, Verbosity level, const std::string& message) {
    LogManager::get(logger)->writeFunc(level, "void anon::emit()", "{}", message);
}

/// Everything this sink saw, as the sentence of each record
std::vector<std::string> messagesOf(const std::shared_ptr<CaptureSink>& capture) {
    std::vector<std::string> messages;

    for (const LogRecord& record : capture->records)
        messages.push_back(record.message);

    return messages;
}

/// How many of this sink's records are at this level
std::size_t countAt(const std::shared_ptr<CaptureSink>& capture, Verbosity level) {
    std::size_t at = 0;

    for (const LogRecord& record : capture->records)
        if (record.level == level)
            ++at;

    return at;
}

/* ------------------------------------------------------------------ *
 * The cases
 * ------------------------------------------------------------------ */

/**
 * A WARN record on an ordinary logger arrives as one POST per user key, with the
 * token, that user's key, the title the record makes and the sentence in it.
 *
 * The sink is on the ROOT, which is where a pager belongs and where the loop
 * guard below has to work.
 */
void testOnePostPerUserKey() {
    CHECK(apply("sink.page.type = pushover\n"
                "sink.page.token = " +
                token +
                "\n"
                "sink.page.userkey = user-one, user-two\n"
                "sink.page.url = " +
                urlFor("") +
                "\n"
                "sink.page.level = WARN\n"
                "sink.page.rate = 100/min\n"
                "logger.root = TRACE, page\n"));

    forgetCaptured();

    emit("deliver.a", WARN, "the disk is on fire");

    const std::vector<std::string> posts = capturedAtLeast(2);

    CHECK_EQ(posts.size(), std::size_t(2));

    bool sawUserOne = false;
    bool sawUserTwo = false;

    for (const std::string& post : posts) {
        CHECK(contains(post, "token=" + token));
        CHECK(contains(post, "title=[deliver.a] WARNING"));
        CHECK(contains(post, "the disk is on fire"));

        sawUserOne = sawUserOne || contains(post, "user=user-one");
        sawUserTwo = sawUserTwo || contains(post, "user=user-two");
    }

    CHECK(sawUserOne);
    CHECK(sawUserTwo);

    // A record the sink's own threshold refuses is not posted at all
    forgetCaptured();

    emit("deliver.a", INFO, "nothing worth waking anybody for");

    CHECK_EQ(capturedAfterAWhile().size(), std::size_t(0));
}

/**
 * The loop guard: a record of core.notifier, or of anything below it, is never
 * posted - which is what keeps a sink on the root from paging about its own
 * failure to page.
 */
void testTheLoopGuard() {
    forgetCaptured();

    emit("core.notifier", WARN, "a failure of the notifier itself");
    emit("core.notifier.deeper", FATAL, "and one below it");

    CHECK_EQ(capturedAfterAWhile().size(), std::size_t(0));

    // And the sink is still working: an ordinary record still goes out
    forgetCaptured();

    emit("deliver.a", WARN, "still paging");

    CHECK_EQ(capturedAtLeast(2).size(), std::size_t(2));
}

/// A rate of two a minute posts two of ten records, and no more
void testTheRateLimit() {
    CHECK(apply("sink.page.type = pushover\n"
                "sink.page.token = " +
                token +
                "\n"
                "sink.page.userkey = user-one\n"
                "sink.page.url = " +
                urlFor("") +
                "\n"
                "sink.page.level = WARN\n"
                "sink.page.rate = 2/min\n"
                "logger.root = TRACE, page\n"));

    forgetCaptured();

    for (int at = 0; at < 10; ++at)
        emit("deliver.b", WARN, "flood " + std::to_string(at));

    /* Exactly two: the bucket holds two and a minute's refill is far longer
     * than this test.  The notification that says how many were suppressed
     * needs that refill, so it is test_logger_ratelimit that covers the count */
    CHECK_EQ(capturedAtLeast(2).size(), std::size_t(2));
    CHECK_EQ(capturedAfterAWhile().size(), std::size_t(2));
}

/**
 * An endpoint that answers 500: the failures are logged on core.notifier, at
 * most one record for all of them, and nothing crashes.
 */
void testAFailingEndpoint() {
    const std::shared_ptr<CaptureSink> notifierRecords = std::make_shared<CaptureSink>();

    LogManager::get("core.notifier")->addSink(notifierRecords, TRACE);

    CHECK(apply("sink.page.type = pushover\n"
                "sink.page.token = " +
                token +
                "\n"
                "sink.page.userkey = user-one\n"
                "sink.page.url = " +
                urlFor("/fail") +
                "\n"
                "sink.page.level = WARN\n"
                "sink.page.rate = 100/min\n"
                "logger.root = TRACE, page\n"));

    notifierRecords->clear();

    for (int at = 0; at < 5; ++at)
        emit("deliver.c", WARN, "this one cannot be delivered " + std::to_string(at));

    // Long enough for five failures, and for a sixth record about them
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    CHECK_EQ(countAt(notifierRecords, ERROR), std::size_t(1));

    // And no record of the failure carries the token or the user key
    for (const std::string& message : messagesOf(notifierRecords)) {
        CHECK(!contains(message, token));
        CHECK(!contains(message, "user-one"));
    }

    LogManager::get("core.notifier")->removeSink(notifierRecords);
}

/**
 * Destroying a sink with requests still queued: the sink owns its worker, so
 * nothing of it can run afterwards - and what is queued is discarded rather than
 * sent, so this costs the one request that is already on its way and no more.
 *
 * The endpoint's /slow answers after half a second, so fifty queued requests
 * would be twenty-five seconds if the destructor waited for them.  The sink is
 * destroyed the way a rehash destroys one: a configuration that no longer names
 * it lets go of the last reference to it.
 */
void testDestroyingASinkWithWorkQueued() {
    CHECK(apply("sink.page.type = pushover\n"
                "sink.page.token = " +
                token +
                "\n"
                "sink.page.userkey = user-one\n"
                "sink.page.url = " +
                urlFor("/slow") +
                "\n"
                "sink.page.level = WARN\n"
                "sink.page.rate = 1000/min\n"
                "logger.root = TRACE, page\n"));

    forgetCaptured();

    for (int at = 0; at < 50; ++at)
        emit("deliver.d", WARN, "queued " + std::to_string(at));

    const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();

    // A configuration with no pushover sink in it: the one above is let go of
    // here, inside configure(), which is where its destructor runs
    CHECK(apply("sink.nul.type = console\n"
                "logger.root = OFF, nul\n"));

    const long elapsed = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - began)
                                               .count());

    if (elapsed > 5000) {
        ++failures;
        std::cerr << __FILE__ << ':' << __LINE__ << ": failed: destroying the sink took " << elapsed
                  << "ms\n";
    }

    // Whatever it did send it sent before it was destroyed, and nothing after
    const std::size_t sent = captured().size();

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    CHECK_EQ(captured().size(), sent);
}

} // namespace

int main() {
    const char* const url = std::getenv("PUSHOVER_TEST_URL");
    const char* const capture = std::getenv("PUSHOVER_TEST_CAPTURE");

    if (nullptr == url || '\0' == url[0] || nullptr == capture || '\0' == capture[0]) {
        std::cout << "pushover_delivery: no PUSHOVER_TEST_URL/PUSHOVER_TEST_CAPTURE, "
                     "nothing to deliver to (see test/docker/full-deps/run.sh)\n";

        return 0;
    }

    endpointUrl = url;
    capturePath = capture;

    std::ostringstream conf;
    conf << capturePath << ".logging.conf." << ::getpid();
    configPath = conf.str();

    PushoverClient::registerSinkType();

    /* Every record of the process, so that the token check at the end really is
     * over everything that was logged and not only over core.notifier */
    const std::shared_ptr<CaptureSink> everything = std::make_shared<CaptureSink>();

    LogManager::root()->addSink(everything, TRACE);

    testOnePostPerUserKey();
    testTheLoopGuard();
    testTheRateLimit();
    testAFailingEndpoint();
    testDestroyingASinkWithWorkQueued();

    // Not one record of this process may carry the token or a user key
    for (const std::string& message : messagesOf(everything)) {
        if (contains(message, token) || contains(message, "user-one") ||
            contains(message, "user-two")) {
            ++failures;
            std::cerr << __FILE__ << ": failed: a log record carried a secret: " << message << '\n';
        }
    }

    // And the endpoint did see it, or nothing above proved anything at all
    bool tokenWasSent = false;

    for (const std::string& post : captured())
        tokenWasSent = tokenWasSent || contains(post, token);

    ::unlink(configPath.c_str());

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "pushover_delivery: all checks passed"
              << (tokenWasSent ? "" : " (nothing was left in the capture at the end)") << '\n';

    return 0;
}
