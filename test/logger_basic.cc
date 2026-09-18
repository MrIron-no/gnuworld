/**
 * logger_basic.cc
 * Unit test for libgnuworld/logger.h: the Logger that lives in libgnuworld and
 * writes through sinks.  It links libgnuworld alone, which is the point of the
 * test: the logger no longer knows anything about the IRC core.
 * Runs under "make check".
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "LogRecord.h"
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
 * The logger the LOG macros write to: they expand to "logger->", so the
 * name is part of their contract and the test has to use it too.
 * ------------------------------------------------------------------ */

std::unique_ptr<Logger> logger;

/* ------------------------------------------------------------------ *
 * Two objects of our own, so that the extractor registry can be tested
 * without a single type of the IRC core
 * ------------------------------------------------------------------ */

struct FakeUser {
    std::string nick;
    std::int64_t id;
};

struct FakeChan {
    std::string name;
    std::string modes;
};

/// The token every extractor of this test is registered under
const int owner = 0;

/// How often an extractor has been asked for an object, for the lazy test
std::size_t extractorCalls = 0;

void registerFakes() {
    Logger::registerExtractor<FakeUser>(&owner, [](const FakeUser* user) {
        ++extractorCalls;

        LogObject object;
        object.display = user->nick;
        object.fields.emplace_back("x", user->id);

        return object;
    });

    Logger::registerExtractor<FakeChan>(&owner, [](const FakeChan* chan) {
        ++extractorCalls;

        LogObject object;
        object.display = chan->name;
        object.fields.emplace_back("y", chan->modes);

        return object;
    });
}

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/// The nth field with this key, counting from zero, or nullptr
const LogField* nthField(const std::vector<LogField>& fields, const std::string& key,
                         std::size_t which = 0) {
    for (const LogField& field : fields) {
        if (field.key != key)
            continue;
        if (0 == which)
            return &field;
        --which;
    }

    return nullptr;
}

/// The part of the message one span covers
std::string spanText(const LogRecord& record, std::size_t which) {
    if (which >= record.spans.size())
        return std::string();

    const LogSpan& span = record.spans[which];

    return record.message.substr(span.begin, span.end - span.begin);
}

/// A fresh logger with one capture sink on it, which the caller keeps
std::shared_ptr<CaptureSink> freshLogger(const std::string& name = "test") {
    logger = std::make_unique<Logger>(name);

    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    logger->addSink(sink);

    return sink;
}

/* ------------------------------------------------------------------ *
 * A sink that logs from inside its own emit(), and one that wants none
 * of what such a sink produces
 * ------------------------------------------------------------------ */

class ReentrantSink : public LogSink {
  public:
    void emit(const LogRecord&) override {
        if (busy)
            return;

        busy = true;
        logger->write(INFO, std::string("nested"));
        busy = false;

        ++emitted;
    }

    bool suppressOnReentry() const override { return true; }

    std::size_t emitted = 0;

  private:
    bool busy = false;
};

/// A capture sink that refuses re-entrant records, as the IRC sink does
class SuppressingCaptureSink : public CaptureSink {
  public:
    bool suppressOnReentry() const override { return true; }
};

/* ------------------------------------------------------------------ *
 * The tests
 * ------------------------------------------------------------------ */

/**
 * The whole of a structured record: the rendered sentence, its spans, the
 * display entries, the typed sub-fields, the level, the logger, the function
 * and the logger's context.
 */
void testStructuredRecord() {
    const std::shared_ptr<CaptureSink> sink = freshLogger("cservice");
    logger->setContext("bot", std::string("X"));

    registerFakes();

    FakeUser user{"A", 42};
    FakeChan chan{"#b", "+tn"};

    LOG_MSG(WARN, "{nick} failed to add to {chan}").with("nick", &user).with("chan", &chan).log();

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK_EQ(record.message, "A failed to add to #b");
    CHECK(WARN == record.level);
    CHECK_EQ(record.logger, "cservice");
    CHECK(!record.function.empty());

    CHECK(2 == record.spans.size());
    CHECK_EQ(spanText(record, 0), "A");
    CHECK_EQ(spanText(record, 1), "#b");

    // One display entry per object, for the template only
    const LogField* const nick = nthField(record.fields, "nick");
    CHECK(nullptr != nick);
    if (nullptr != nick) {
        CHECK(nick->displayOnly);
        CHECK_EQ(logValueToString(nick->value), "A");
    }

    const LogField* const chanField = nthField(record.fields, "chan");
    CHECK(nullptr != chanField);
    if (nullptr != chanField) {
        CHECK(chanField->displayOnly);
        CHECK_EQ(logValueToString(chanField->value), "#b");
    }

    // One <key>_<sub> field per sub-field, keeping the extractor's type
    const LogField* const nickX = nthField(record.fields, "nick_x");
    CHECK(nullptr != nickX);
    if (nullptr != nickX) {
        CHECK(!nickX->displayOnly);
        CHECK(std::holds_alternative<std::int64_t>(nickX->value));
        CHECK(42 == std::get<std::int64_t>(nickX->value));
    }

    const LogField* const chanY = nthField(record.fields, "chan_y");
    CHECK(nullptr != chanY);
    if (nullptr != chanY) {
        CHECK(!chanY->displayOnly);
        CHECK(std::holds_alternative<std::string>(chanY->value));
        CHECK_EQ(std::get<std::string>(chanY->value), "+tn");
    }

    // The logger's own context travels with every record it dispatches
    const LogField* const bot = nthField(record.context, "bot");
    CHECK(nullptr != bot);
    if (nullptr != bot)
        CHECK_EQ(logValueToString(bot->value), "X");
}

/**
 * A null object of a type that has an extractor: "(null)" in the sentence, and
 * a null-valued field under the bare key, so that JSON says "nick":null.
 */
void testNullObject() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    FakeUser* const nobody = nullptr;

    LOG_MSG(INFO, "{nick} is gone").with("nick", nobody).log();

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK_EQ(record.message, "(null) is gone");

    const LogField* const display = nthField(record.fields, "nick", 0);
    CHECK(nullptr != display);
    if (nullptr != display) {
        CHECK(display->displayOnly);
        CHECK_EQ(logValueToString(display->value), "(null)");
    }

    const LogField* const null = nthField(record.fields, "nick", 1);
    CHECK(nullptr != null);
    if (nullptr != null) {
        CHECK(!null->displayOnly);
        CHECK(std::holds_alternative<std::monostate>(null->value));
    }
}

/**
 * An extractor registered for FakeUser is found for a const FakeUser* too: the
 * constness of the pointee is not part of what the registry is keyed on.
 */
void testConstPointer() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    const FakeUser user{"A", 1};
    const FakeUser* const pointer = &user;

    LOG_MSG(INFO, "{nick}").with("nick", pointer).log();

    CHECK(1 == sink->records.size());
    if (1 == sink->records.size())
        CHECK_EQ(sink->records[0].message, "A");
}

/**
 * With the extractors of this owner gone, the same pointer type is just a
 * pointer again, and shows as one.
 */
void testFallbackWithoutExtractor() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    Logger::removeExtractors(&owner);

    FakeUser user{"A", 1};

    LOG_MSG(INFO, "{nick}").with("nick", &user).log();

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK(record.message.rfind("0x", 0) == 0);

    const LogField* const nick = nthField(record.fields, "nick");
    CHECK(nullptr != nick);
    if (nullptr != nick) {
        CHECK(!nick->displayOnly);
        CHECK(std::holds_alternative<std::string>(nick->value));
    }

    // The registry is process-wide: put them back for the tests that follow
    registerFakes();
}

/**
 * A record passes a sink's own threshold, not the logger's alone.
 */
void testSinkThresholds() {
    logger = std::make_unique<Logger>("test");

    const std::shared_ptr<CaptureSink> loud = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> quiet = std::make_shared<CaptureSink>();

    logger->addSink(loud, TRACE);
    logger->addSink(quiet, WARN);

    logger->write(INFO, std::string("chatter"));

    CHECK(1 == loud->records.size());
    CHECK(0 == quiet->records.size());

    logger->write(ERROR, std::string("trouble"));

    CHECK(2 == loud->records.size());
    CHECK(1 == quiet->records.size());

    // Raising a threshold lets what was filtered through
    logger->setSinkThreshold(quiet, TRACE);
    logger->write(INFO, std::string("chatter"));

    CHECK(2 == quiet->records.size());

    // And a sink that is gone hears nothing at all
    logger->removeSink(loud);
    logger->write(ERROR, std::string("trouble"));

    CHECK(3 == loud->records.size());
    CHECK(3 == quiet->records.size());
}

/**
 * A record logged from inside a sink's emit() reaches the sinks that put up
 * with it and no others, and nothing deadlocks on the way.
 */
void testReentrancy() {
    logger = std::make_unique<Logger>("test");

    const std::shared_ptr<CaptureSink> normal = std::make_shared<CaptureSink>();
    const std::shared_ptr<SuppressingCaptureSink> suppressing =
        std::make_shared<SuppressingCaptureSink>();
    const std::shared_ptr<ReentrantSink> trigger = std::make_shared<ReentrantSink>();

    logger->addSink(normal);
    logger->addSink(suppressing);
    logger->addSink(trigger);

    logger->write(INFO, std::string("outer"));

    CHECK(1 == trigger->emitted);

    // The outer record and the one the trigger logged
    CHECK(2 == normal->records.size());
    if (2 == normal->records.size()) {
        CHECK_EQ(normal->records[0].message, "outer");
        CHECK_EQ(normal->records[1].message, "nested");
    }

    // Only the outer one: the nested record is re-entrant
    CHECK(1 == suppressing->records.size());
    if (1 == suppressing->records.size())
        CHECK_EQ(suppressing->records[0].message, "outer");
}

/**
 * The stream API MigrationChecker uses: one record per std::endl.
 */
void testStreamApi() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    logger->write(INFO) << "a" << 1 << std::endl;

    CHECK(1 == sink->records.size());
    if (1 == sink->records.size()) {
        CHECK_EQ(sink->records[0].message, "a1");
        CHECK(INFO == sink->records[0].level);
    }
}

/**
 * The LOG macro: positional arguments with their format specs, and a span for
 * each of them.
 */
void testLogMacro() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    LOG(INFO, "x {} {:>3}", "y", 7);

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK_EQ(record.message, "x y   7");
    CHECK(2 == record.spans.size());
    CHECK_EQ(spanText(record, 0), "y");
    CHECK_EQ(spanText(record, 1), "  7");
    CHECK(!record.function.empty());
}

/**
 * A record the logger's level rules out costs nothing: no extractor runs and
 * no sink is troubled.
 */
void testLazyRecord() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();
    logger->setLevel(INFO);

    FakeUser user{"A", 1};

    const std::size_t before = extractorCalls;

    LOG_MSG(DEBUG, "{nick}").with("nick", &user).log();

    CHECK(before == extractorCalls);
    CHECK(0 == sink->records.size());

    // And the level it does allow still arrives
    LOG_MSG(INFO, "{nick}").with("nick", &user).log();

    CHECK(before + 1 == extractorCalls);
    CHECK(1 == sink->records.size());
}

/**
 * The compatibility routing of SQL records: the legacy file slot takes them
 * only with logSQL, the legacy console slot only with consoleSQL, and no other
 * sink ever does.
 */
void testLegacySqlRouting() {
    logger = std::make_unique<Logger>("test");

    const std::shared_ptr<CaptureSink> file = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> console = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> other = std::make_shared<CaptureSink>();

    logger->addSink(file, TRACE);
    logger->setLegacyFileSink(file);
    logger->addSink(console, TRACE);
    logger->setLegacyConsoleSink(console);
    logger->addSink(other, TRACE);

    logger->write(SQL, std::string("select 1"));

    CHECK(0 == file->records.size());
    CHECK(0 == console->records.size());
    CHECK(0 == other->records.size());

    logger->setLogSQL(true);
    logger->write(SQL, std::string("select 1"));

    CHECK(1 == file->records.size());
    CHECK(0 == console->records.size());
    CHECK(0 == other->records.size());

    logger->setConsoleSQL(true);
    logger->write(SQL, std::string("select 1"));

    CHECK(2 == file->records.size());
    CHECK(1 == console->records.size());
    CHECK(0 == other->records.size());

    // The level of an SQL record says so
    if (!file->records.empty())
        CHECK_EQ(levelName(file->records[0].level), "SQL");

    // An ordinary record is not affected by any of this
    logger->write(INFO, std::string("hello"));

    CHECK(1 == other->records.size());
}

/**
 * setContext replaces the value of a key it already has, rather than adding a
 * second entry under it.
 */
void testContextReplacement() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    logger->setContext("bot", std::string("first"));
    logger->setContext("bot", std::string("second"));

    logger->write(INFO, std::string("hello"));

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK(nullptr == nthField(record.context, "bot", 1));

    const LogField* const bot = nthField(record.context, "bot");
    CHECK(nullptr != bot);
    if (nullptr != bot)
        CHECK_EQ(logValueToString(bot->value), "second");
}

/**
 * The plain values of with(): the type of the field is the type of the value.
 */
void testTypedFields() {
    const std::shared_ptr<CaptureSink> sink = freshLogger();

    LOG_MSG(INFO, "{name} {count} {offset} {ratio} {ok}")
        .with("name", std::string("MrIron"))
        .with("count", 7u)
        .with("offset", -5)
        .with("ratio", 0.5)
        .with("ok", true)
        .log();

    CHECK(1 == sink->records.size());
    if (1 != sink->records.size())
        return;

    const LogRecord& record = sink->records[0];

    CHECK_EQ(record.message, "MrIron 7 -5 0.5 true");

    const LogField* const count = nthField(record.fields, "count");
    CHECK(nullptr != count && std::holds_alternative<std::uint64_t>(count->value));

    const LogField* const offset = nthField(record.fields, "offset");
    CHECK(nullptr != offset && std::holds_alternative<std::int64_t>(offset->value));

    const LogField* const ratio = nthField(record.fields, "ratio");
    CHECK(nullptr != ratio && std::holds_alternative<double>(ratio->value));

    const LogField* const ok = nthField(record.fields, "ok");
    CHECK(nullptr != ok && std::holds_alternative<bool>(ok->value));

    const LogField* const name = nthField(record.fields, "name");
    CHECK(nullptr != name && std::holds_alternative<std::string>(name->value));
}

} // namespace

int main() {
    testStructuredRecord();
    testNullObject();
    testConstPointer();
    testFallbackWithoutExtractor();
    testSinkThresholds();
    testReentrancy();
    testStreamApi();
    testLogMacro();
    testLazyRecord();
    testLegacySqlRouting();
    testContextReplacement();
    testTypedFields();

    Logger::removeExtractors(&owner);
    logger.reset();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_basic: all checks passed\n";
    return 0;
}
