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
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "LogManager.h"
#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"
#include "notifier.h"

using namespace gnuworld;

/* ------------------------------------------------------------------ *
 * The logger the LOG macros write to.  They resolve moduleLogger(), which
 * this declaration defines, so every macro of this file logs on "test"
 * however many loggers the file otherwise uses.
 * ------------------------------------------------------------------ */

GNUWORLD_MODULE_LOGGER("test");

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
 * The logger the cases that do not go through a macro write to.  The
 * loggers belong to the registry, which keeps each of them for the life of
 * the process, so every such case asks for one of its own.
 * ------------------------------------------------------------------ */

Logger* logger = nullptr;

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

/// A logger of this test's own, with one capture sink on it
std::shared_ptr<CaptureSink> freshLogger(const std::string& name) {
    logger = LogManager::get(name);

    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    logger->addSink(sink);

    return sink;
}

/**
 * A capture sink on the logger the macros write to.  That logger is the
 * registry's and outlives every case here, so the sink goes on for the one case
 * that reads it and comes off again however that case leaves: a macro of the
 * next one must see nothing of this one.
 */
class MacroSink {
  public:
    MacroSink() : sink(std::make_shared<CaptureSink>()) { moduleLogger()->addSink(sink); }

    ~MacroSink() { moduleLogger()->removeSink(sink); }

    MacroSink(const MacroSink&) = delete;
    MacroSink& operator=(const MacroSink&) = delete;

    /// Reads as the shared_ptr the cases held while they owned their logger
    CaptureSink* operator->() const { return sink.get(); }

  private:
    std::shared_ptr<CaptureSink> sink;
};

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
 * The notifier base class, which libnotifier's pushover and prometheus
 * clients are.  Only its header is used here - the test links libgnuworld
 * alone, and notifier.h names nothing else either.
 * ------------------------------------------------------------------ */

/// Records what the default notifier::emit() makes of a record
class RecordingNotifier : public notifier {
  public:
    struct Call {
        int level;
        std::string message;
    };

    bool sendMessage(int level, const std::string message) override {
        calls.push_back(Call{level, message});
        return true;
    }

    size_t getSuccessful() const override { return calls.size(); }

    size_t getErrors() const override { return 0; }

    std::vector<Call> calls;
};

/**
 * A notifier that logs from inside sendMessage(), as the pushover client does
 * when a send fails: on the very logger it is attached to, which is the worst
 * case there is.
 */
class SelfLoggingNotifier : public RecordingNotifier {
  public:
    bool sendMessage(int level, const std::string message) override {
        RecordingNotifier::sendMessage(level, message);
        logger->write(ERROR, std::string("send failed"));

        return true;
    }
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
    const MacroSink sink;
    moduleLogger()->setContext("bot", std::string("X"));

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
    CHECK_EQ(record.logger, "test");
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
    const MacroSink sink;

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
    const MacroSink sink;

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
    const MacroSink sink;

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
    logger = LogManager::get("basic.thresholds");

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
    logger = LogManager::get("basic.reentrancy");

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
 * What the notifier base class makes of a record: the level as the number the
 * two-argument sendMessage() takes, and the sentence behind the function it was
 * logged from - which a notification carries at every level but INFO, where the
 * function says nothing a human reading a push message wants.
 */
void testNotifierEmit() {
    RecordingNotifier sink;

    LogRecord warning;
    warning.level = WARN;
    warning.logger = "cservice";
    warning.function = "Class::fn";
    warning.message = "text";

    sink.emit(warning);

    CHECK(1 == sink.calls.size());
    if (1 == sink.calls.size()) {
        CHECK(3 == sink.calls[0].level);
        CHECK_EQ(sink.calls[0].message, "Class::fn> text");
    }

    LogRecord notice;
    notice.level = INFO;
    notice.logger = "cservice";
    notice.function = "Class::fn";
    notice.message = "text";

    sink.emit(notice);

    CHECK(2 == sink.calls.size());
    if (2 == sink.calls.size()) {
        CHECK(4 == sink.calls[1].level);
        CHECK_EQ(sink.calls[1].message, "text");
    }
}

/**
 * A notifier that logs when it fails to deliver cannot feed itself: the record
 * it logs from inside sendMessage() is re-entrant, and a notifier takes no
 * re-entrant record however it is attached.  The record is not lost for that -
 * an ordinary sink still gets it.
 */
void testNotifierDoesNotRecurse() {
    logger = LogManager::get("basic.notifier");

    const std::shared_ptr<CaptureSink> normal = std::make_shared<CaptureSink>();
    const std::shared_ptr<SelfLoggingNotifier> notified = std::make_shared<SelfLoggingNotifier>();

    logger->addSink(normal);
    logger->addSink(notified);

    logger->writeFunc(WARN, "Class::fn", std::string("text"));

    // Once for the record that was logged, and not again for its own failure
    CHECK(1 == notified->calls.size());
    if (1 == notified->calls.size()) {
        CHECK(3 == notified->calls[0].level);
        CHECK_EQ(notified->calls[0].message, "Class::fn> text");
    }

    // The failure is logged all the same, where a sink that takes re-entrant
    // records - the file, the console - reads it
    CHECK(2 == normal->records.size());
    if (2 == normal->records.size()) {
        CHECK_EQ(normal->records[0].message, "text");
        CHECK_EQ(normal->records[1].message, "send failed");
        CHECK(ERROR == normal->records[1].level);
    }

    logger->removeSink(notified);
    logger->removeSink(normal);
}

/**
 * The LOG macro: positional arguments with their format specs, and a span for
 * each of them.
 */
void testLogMacro() {
    const MacroSink sink;

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
    const MacroSink sink;
    moduleLogger()->setLevel(INFO);

    FakeUser user{"A", 1};

    const std::size_t before = extractorCalls;

    LOG_MSG(DEBUG, "{nick}").with("nick", &user).log();

    CHECK(before == extractorCalls);
    CHECK(0 == sink->records.size());

    // And the level it does allow still arrives
    LOG_MSG(INFO, "{nick}").with("nick", &user).log();

    CHECK(before + 1 == extractorCalls);
    CHECK(1 == sink->records.size());

    // The logger is the registry's: the next case finds it as it was
    moduleLogger()->setConfigLevel(std::nullopt);
}

/**
 * setContext replaces the value of a key it already has, rather than adding a
 * second entry under it.
 */
void testContextReplacement() {
    const std::shared_ptr<CaptureSink> sink = freshLogger("basic.context");

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
    const MacroSink sink;

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

/**
 * The macros find the logger themselves: LOG and LOG_MSG write to the one
 * GNUWORLD_MODULE_LOGGER named, and moduleLogger() is that logger and no other,
 * however often it is asked for.
 */
void testModuleLoggerAccessor() {
    const MacroSink sink;

    LOG(INFO, "x {}", 1);
    LOG_MSG(WARN, "{a}").with("a", 5).log();

    CHECK(2 == sink->records.size());
    if (2 != sink->records.size())
        return;

    CHECK_EQ(sink->records[0].message, "x 1");
    CHECK(INFO == sink->records[0].level);
    CHECK_EQ(sink->records[0].logger, "test");
    CHECK(!sink->records[0].function.empty());

    CHECK_EQ(sink->records[1].message, "5");
    CHECK(WARN == sink->records[1].level);
    CHECK_EQ(sink->records[1].logger, "test");

    // One logger, looked up once and the registry's own
    CHECK(moduleLogger() == moduleLogger());
    CHECK(moduleLogger() == LogManager::get("test"));
}

/**
 * LOG_TO and LOG_MSG_TO write to the logger they are handed rather than to the
 * module's: the record carries that logger's name, and walks up to the sinks of
 * its ancestors as the record of any additive logger does.
 */
void testLogToMacros() {
    const MacroSink sink;

    LOG_TO(LogManager::get("test.sub"), WARN, "y");
    LOG_MSG_TO(LogManager::get("test.sub"), ERROR, "{b}").with("b", true).log();

    CHECK(2 == sink->records.size());
    if (2 != sink->records.size())
        return;

    CHECK_EQ(sink->records[0].message, "y");
    CHECK(WARN == sink->records[0].level);
    CHECK_EQ(sink->records[0].logger, "test.sub");
    CHECK(!sink->records[0].function.empty());

    CHECK_EQ(sink->records[1].message, "true");
    CHECK(ERROR == sink->records[1].level);
    CHECK_EQ(sink->records[1].logger, "test.sub");
}

} // namespace

int main() {
    // Every logger of this test is a child of the root, whose console sink
    // would print each of their records; the test reads its own sinks
    LogManager::root()->removeSink(LogManager::bootstrapConsoleSink());

    testStructuredRecord();
    testNullObject();
    testConstPointer();
    testFallbackWithoutExtractor();
    testSinkThresholds();
    testReentrancy();
    testNotifierEmit();
    testNotifierDoesNotRecurse();
    testLogMacro();
    testLazyRecord();
    testContextReplacement();
    testTypedFields();
    testModuleLoggerAccessor();
    testLogToMacros();

    Logger::removeExtractors(&owner);
    logger = nullptr;

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_basic: all checks passed\n";
    return 0;
}
