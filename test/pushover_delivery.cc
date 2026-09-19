/**
 * pushover_delivery.cc
 * What a pushover sink really puts on the wire, against a local HTTP endpoint
 * that records every POST it is given: one request per user key, not one field
 * of the request coming from the log record, the loop guard, the rate limit, a
 * failing endpoint, two sinks sending at once, and destroying a sink with
 * requests still queued.  It links libnotifier, libgnuworld and libcurl, and is
 * built only where libcurl is (see test/Makefile.am, COND_LIBCURL).
 * Runs under "make check", where test/docker/full-deps/run.sh is what gives it
 * an endpoint; without one it says so and passes.
 *
 * The endpoint's URL and the file it records the requests in come from the
 * environment:
 *
 *   PUSHOVER_TEST_URL       http://127.0.0.1:<port>/1/messages.json
 *   PUSHOVER_TEST_CAPTURE   the file it appends one request per line to
 *
 * and two paths of that endpoint are special: /fail answers 500, /slow answers
 * a success after half a second.
 *
 * A line of that file holds the request twice: the raw body, and then the form
 * fields decoded out of it, name and value alternating, tab separated and
 * escaped (see test/docker/full-deps/endpoint.py, which writes it, and
 * parsePost() below, which reads it).  Both are needed - what the sentence has
 * to arrive as is the DECODED message, and that no sentence can add a field is
 * about the RAW body.
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
#include <utility>
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

/// One request the endpoint saw: the body as it arrived, and its form fields
struct Post {
    std::string raw;

    /// Name and value of every field, in the order sent, repetitions and all
    std::vector<std::pair<std::string, std::string>> fields;

    /// How many fields of this name the request carried
    std::size_t count(const std::string& name) const {
        std::size_t seen = 0;

        for (const std::pair<std::string, std::string>& field : fields)
            if (field.first == name)
                ++seen;

        return seen;
    }

    /// The first value of this field, or "" where the request had none
    std::string one(const std::string& name) const {
        for (const std::pair<std::string, std::string>& field : fields)
            if (field.first == name)
                return field.second;

        return std::string();
    }
};

/// Undoes endpoint.py's _escape(): the four it escapes and nothing else
std::string unescape(const std::string& text) {
    std::string plain;

    plain.reserve(text.size());

    for (std::string::size_type at = 0; at < text.size(); ++at) {
        if ('\\' != text[at] || at + 1 == text.size()) {
            plain.push_back(text[at]);
            continue;
        }

        switch (text[++at]) {
        case 'n':
            plain.push_back('\n');
            break;
        case 'r':
            plain.push_back('\r');
            break;
        case 't':
            plain.push_back('\t');
            break;
        default:
            plain.push_back(text[at]); // "\\" and anything else, as it stands
            break;
        }
    }

    return plain;
}

/**
 * One line of the capture file as a request: the raw body, then the decoded
 * fields with name and value alternating.
 */
Post parsePost(const std::string& line) {
    std::vector<std::string> pieces;
    std::string::size_type at = 0;

    for (;;) {
        const std::string::size_type tab = line.find('\t', at);

        pieces.push_back(std::string::npos == tab ? line.substr(at) : line.substr(at, tab - at));

        if (std::string::npos == tab)
            break;

        at = tab + 1;
    }

    Post post;

    post.raw = unescape(pieces[0]);

    for (std::size_t on = 1; on + 1 < pieces.size(); on += 2)
        post.fields.push_back(std::make_pair(unescape(pieces[on]), unescape(pieces[on + 1])));

    return post;
}

std::vector<Post> captured() {
    std::vector<Post> posts;
    std::ifstream in(capturePath.c_str());
    std::string line;

    while (std::getline(in, line))
        if (!line.empty())
            posts.push_back(parsePost(line));

    return posts;
}

void forgetCaptured() { std::ofstream out(capturePath.c_str(), std::ios::out | std::ios::trunc); }

/// Whatever was posted once at least count requests are there, or time is up
std::vector<Post> capturedAtLeast(std::size_t count, int milliseconds = 5000) {
    for (int waited = 0; waited < milliseconds; waited += 20) {
        const std::vector<Post> posts = captured();

        if (posts.size() >= count)
            return posts;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return captured();
}

/// Whatever was posted after long enough for anything on its way to arrive
std::vector<Post> capturedAfterAWhile(int milliseconds = 1500) {
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

/**
 * What a notification of one of those records begins with: the function the
 * record was logged from, as the logger keeps it - the argument list is trimmed
 * off a function name on its way into a record - and the "> " the sink puts
 * between that and the sentence at every level but INFO.
 */
const std::string functionPrefix("void anon::emit> ");

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

    const std::vector<Post> posts = capturedAtLeast(2);

    CHECK_EQ(posts.size(), std::size_t(2));

    bool sawUserOne = false;
    bool sawUserTwo = false;

    for (const Post& post : posts) {
        /* The DECODED fields, which is what the service reads: the raw body is
         * percent-encoded, so "[deliver.a] WARNING" is not in it as such */
        CHECK_EQ(post.one("token"), token);
        CHECK_EQ(post.one("title"), std::string("[deliver.a] WARNING"));
        CHECK_EQ(post.one("message"), functionPrefix + "the disk is on fire");

        sawUserOne = sawUserOne || "user-one" == post.one("user");
        sawUserTwo = sawUserTwo || "user-two" == post.one("user");
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

/**
 * NOT ONE FIELD OF THE REQUEST COMES FROM THE LOG RECORD.
 *
 * A log sentence carries whatever a remote IRC server or an IRC user put into
 * it, and a channel name may legally hold "&", "=", "%", "#" and "+".  A
 * sentence about a channel called "#x&user=ATTACKERKEY&priority=2&html=1" is
 * therefore a sentence that, written into the body as it stands, would add a
 * second "user" to the request - a page sent with this deployment's own token to
 * somebody else's account - a second "priority", and an "html" nobody asked for.
 *
 * So: exactly one "user", and it is the configured key; exactly one "priority";
 * no "html" at all; and the decoded message equal to the sentence byte for byte,
 * the "%", the "+" and the "=" included.
 */
void testNoSentenceCanAddAField() {
    CHECK(apply("sink.page.type = pushover\n"
                "sink.page.token = " +
                token +
                "\n"
                "sink.page.userkey = user-one\n"
                "sink.page.url = " +
                urlFor("") +
                "\n"
                "sink.page.level = WARN\n"
                "sink.page.rate = 100/min\n"
                "logger.root = TRACE, page\n"));

    forgetCaptured();

    const std::string sentence(
        "Failed to add channel: #x&user=ATTACKERKEY&priority=2&html=1 100% = done + more");

    emit("deliver.e", WARN, sentence);

    const std::vector<Post> posts = capturedAtLeast(1);

    CHECK_EQ(posts.size(), std::size_t(1));

    if (posts.empty())
        return;

    const Post& post = posts.front();

    CHECK_EQ(post.count("user"), std::size_t(1));
    CHECK_EQ(post.one("user"), std::string("user-one"));
    CHECK_EQ(post.count("priority"), std::size_t(1));
    CHECK_EQ(post.one("priority"), std::string("0"));
    CHECK_EQ(post.count("html"), std::size_t(0));
    CHECK_EQ(post.count("token"), std::size_t(1));
    CHECK_EQ(post.count("message"), std::size_t(1));

    // The whole sentence, behind the function it was logged from, unaltered
    CHECK_EQ(post.one("message"), functionPrefix + sentence);

    // And nothing of it reached the wire as a field separator or a field name
    CHECK(!contains(post.raw, "&user=ATTACKERKEY"));
    CHECK(!contains(post.raw, "&html=1"));
    CHECK(!contains(post.raw, "&priority=2"));

    /* The word itself is of course still in there, inside the one "message"
     * field where it belongs: what it may not be is a field of its own */
    CHECK(contains(post.raw, "ATTACKERKEY"));
}

/**
 * A record rendered over two lines arrives as two lines: the newline is the one
 * control character a notification keeps, and it travels as %0A rather than
 * being dropped or ending the body.
 */
void testATwoLineMessage() {
    forgetCaptured();

    emit("deliver.e", WARN, "the first line\nthe second line");

    const std::vector<Post> posts = capturedAtLeast(1);

    CHECK_EQ(posts.size(), std::size_t(1));

    if (posts.empty())
        return;

    CHECK_EQ(posts.front().one("message"), functionPrefix + "the first line\nthe second line");

    // Encoded, and so neither a raw newline in the body nor a lost one
    CHECK(contains(posts.front().raw, "%0A"));
    CHECK(!contains(posts.front().raw, "\n"));
}

/**
 * The title is escaped like every other field.  A logger name is code-defined
 * and holds no "&" in this tree, which is exactly why it is worth asserting:
 * the escaping is of every field of the request and not of the message alone.
 */
void testTheTitleIsEscapedToo() {
    forgetCaptured();

    emit("deliver&x=1.f", WARN, "a title made of a logger name");

    const std::vector<Post> posts = capturedAtLeast(1);

    CHECK_EQ(posts.size(), std::size_t(1));

    if (posts.empty())
        return;

    const Post& post = posts.front();

    CHECK_EQ(post.one("title"), std::string("[deliver&x=1.f] WARNING"));
    CHECK_EQ(post.count("x"), std::size_t(0));
    CHECK_EQ(post.count("title"), std::size_t(1));
    CHECK(!contains(post.raw, "&x=1"));
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
 * TWO PUSHOVER SINKS, BOTH SENDING AT ONCE.
 *
 * Each pushover sink has a worker thread of its own, so two of them are two
 * threads in libcurl: curl_global_init() is documented as not thread-safe, and
 * it is called once for the process, from the constructor, on the thread that
 * builds the sinks - which is this one, inside apply() - so that no worker can
 * reach libcurl before it has been initialised.
 *
 * What this asserts is the outcome: every page of both sinks arrives, and each
 * with its own user key.  A race in the initialisation is not deterministic, so
 * test/docker/full-deps/build-and-test runs this program twenty times.
 */
void testTwoSinksAtOnce() {
    CHECK(apply("sink.pa.type = pushover\n"
                "sink.pa.token = " +
                token +
                "\n"
                "sink.pa.userkey = user-one\n"
                "sink.pa.url = " +
                urlFor("") +
                "\n"
                "sink.pa.level = WARN\n"
                "sink.pa.rate = 100/min\n"
                "sink.pb.type = pushover\n"
                "sink.pb.token = " +
                token +
                "\n"
                "sink.pb.userkey = user-two\n"
                "sink.pb.url = " +
                urlFor("") +
                "\n"
                "sink.pb.level = WARN\n"
                "sink.pb.rate = 100/min\n"
                "logger.conc.a = TRACE, pa\n"
                "logger.conc.b = TRACE, pb\n"));

    forgetCaptured();

    const int each = 10;

    std::thread first([each]() {
        for (int at = 0; at < each; ++at)
            emit("conc.a", WARN, "from the first sink " + std::to_string(at));
    });

    std::thread second([each]() {
        for (int at = 0; at < each; ++at)
            emit("conc.b", WARN, "from the second sink " + std::to_string(at));
    });

    first.join();
    second.join();

    const std::vector<Post> posts = capturedAtLeast(2 * each, 10000);

    CHECK_EQ(posts.size(), std::size_t(2 * each));

    // One user key per sink, so which sink sent which is in the request
    std::size_t fromFirst = 0;
    std::size_t fromSecond = 0;

    for (const Post& post : posts) {
        if ("user-one" == post.one("user"))
            ++fromFirst;

        if ("user-two" == post.one("user"))
            ++fromSecond;
    }

    CHECK_EQ(fromFirst, std::size_t(each));
    CHECK_EQ(fromSecond, std::size_t(each));
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
    testNoSentenceCanAddAField();
    testATwoLineMessage();
    testTheTitleIsEscapedToo();
    testTheRateLimit();
    testAFailingEndpoint();
    testTwoSinksAtOnce();
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

    for (const Post& post : captured())
        tokenWasSent = tokenWasSent || token == post.one("token");

    ::unlink(configPath.c_str());

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "pushover_delivery: all checks passed"
              << (tokenWasSent ? "" : " (nothing was left in the capture at the end)") << '\n';

    return 0;
}
