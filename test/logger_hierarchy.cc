/**
 * logger_hierarchy.cc
 * Unit test for libgnuworld/LogManager.h: the process-wide registry, the dotted
 * hierarchy of loggers, the precedence of their levels, additive dispatch and
 * the module name a library file stands for.  It links libgnuworld alone.
 * Runs under "make check".
 */

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "LogManager.h"
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

/// Reports the value as well as the failure, for the string comparisons
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

/// A sink that counts how often it was asked to reopen itself
class CountingSink : public CaptureSink {
  public:
    void reopen() override { ++reopens; }

    std::size_t reopens = 0;
};

/// One record on this logger, at this level, through the ordinary call path
void emit(Logger* logger, Verbosity level, const std::string& message) {
    logger->writeFunc(level, "void anon::emit()", "{}", message);
}

/// A record to hand straight to Logger::log(), asking the logger nothing first
LogRecord recordOf(Verbosity level, const std::string& message) {
    LogRecord record{};

    record.level = level;
    record.message = message;

    return record;
}

/* ------------------------------------------------------------------ *
 * The registry: names, ancestors and identity
 * ------------------------------------------------------------------ */

/**
 * get() of a dotted name creates every logger on the way to it, wires each to
 * its parent, and hands out the same pointer for the same name ever after.
 */
void testTree() {
    Logger* const c = LogManager::get("a.b.c");

    CHECK(nullptr != c);
    CHECK_EQ(c->getName(), "a.b.c");

    // The ancestors exist because c does, and get() finds those very loggers
    Logger* const b = c->getParent();
    Logger* const a = nullptr == b ? nullptr : b->getParent();

    CHECK(b == LogManager::get("a.b"));
    CHECK(a == LogManager::get("a"));

    if (nullptr != b)
        CHECK_EQ(b->getName(), "a.b");
    if (nullptr != a)
        CHECK_EQ(a->getName(), "a");

    // Every name hangs under the root, and the root under nothing
    CHECK(LogManager::get("a")->getParent() == LogManager::root());
    CHECK(nullptr == LogManager::root()->getParent());
    CHECK(LogManager::root()->getName().empty());

    // The root answers to all three of its names
    CHECK(LogManager::root() == LogManager::get(""));
    CHECK(LogManager::root() == LogManager::get("root"));

    // Repeated get(), and child(), are the same logger
    CHECK(c == LogManager::get("a.b.c"));
    CHECK(c == LogManager::get("a.b")->child("c"));
    CHECK(LogManager::get("a") == LogManager::root()->child("a"));

    // An empty segment is not a segment
    CHECK(LogManager::get("x.y") == LogManager::get("x..y"));
    CHECK(LogManager::get("x") == LogManager::get(".x"));
    CHECK(LogManager::get("x") == LogManager::get("x."));
    CHECK(LogManager::root() == LogManager::get("."));
}

/* ------------------------------------------------------------------ *
 * Levels
 * ------------------------------------------------------------------ */

/**
 * A logger nobody has said anything about logs at INFO, the level of the root;
 * a level on an ancestor is what its descendants log at.
 */
void testInheritance() {
    Logger* const c = LogManager::get("inh.b.c");

    CHECK(INFO == c->effectiveLevel());
    CHECK(c->shouldLog(INFO));
    CHECK(!c->shouldLog(DEBUG));

    LogManager::get("inh")->setLevel(DEBUG);

    CHECK(DEBUG == c->effectiveLevel());
    CHECK(DEBUG == LogManager::get("inh.b")->effectiveLevel());

    // And a record of a level the ancestor allows is built and dispatched
    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    c->addSink(sink, TRACE);

    emit(c, DEBUG, "seen");
    emit(c, TRACE, "unseen");

    CHECK(1 == sink->records.size());
    if (1 == sink->records.size())
        CHECK_EQ(sink->records[0].message, "seen");

    c->removeSink(sink);
}

/**
 * The precedence list: a configured level beats a legacy one, a legacy one
 * beats the default the code asked for, and all three beat what the parent
 * says.  The code default is only ever set once.
 */
void testCodeDefault() {
    LogManager::get("cd")->setLevel(DEBUG);

    Logger* const sql = LogManager::get("cd")->child("sql", ERROR);

    CHECK(sql == LogManager::get("cd.sql"));

    // The code default of this logger, not the level of its parent
    CHECK(ERROR == sql->effectiveLevel());

    // A second code default is not a code default
    sql->setCodeDefault(TRACE);
    CHECK(ERROR == sql->effectiveLevel());

    // Configuration beats it, and clearing configuration gives it back
    sql->setConfigLevel(TRACE);
    CHECK(TRACE == sql->effectiveLevel());
    sql->setConfigLevel(std::nullopt);
    CHECK(ERROR == sql->effectiveLevel());

    // The legacy module keys sit between the two
    sql->setLegacyLevel(WARN);
    CHECK(WARN == sql->effectiveLevel());

    sql->setConfigLevel(TRACE);
    CHECK(TRACE == sql->effectiveLevel());

    sql->setConfigLevel(std::nullopt);
    CHECK(WARN == sql->effectiveLevel());

    // With nothing of its own left, the parent speaks again
    sql->setLegacyLevel(std::nullopt);
    CHECK(ERROR == sql->effectiveLevel());
}

/**
 * The level is the logger's own business and not only the call site's: a record
 * handed straight to log(), which nothing asked shouldLog() about, is dropped
 * unless the effective level admits it.  A record of level OFF is never emitted
 * at all, whatever the logger logs at.
 */
void testLogFiltersByEffectiveLevel() {
    Logger* const logger = LogManager::get("filter");

    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    logger->addSink(sink, TRACE);

    logger->setLevel(INFO);
    logger->log(recordOf(DEBUG, "too verbose"));

    CHECK(0 == sink->size());

    logger->setLevel(DEBUG);
    logger->log(recordOf(DEBUG, "wanted"));

    CHECK(1 == sink->size());
    if (1 == sink->size())
        CHECK_EQ(sink->records[0].message, "wanted");

    // Nothing is logged at OFF, not even on a logger whose level is OFF
    logger->setLevel(OFF);
    logger->log(recordOf(OFF, "nothing"));
    logger->setLevel(TRACE);
    logger->log(recordOf(OFF, "still nothing"));

    CHECK(1 == sink->size());

    logger->setConfigLevel(std::nullopt);
    logger->removeSink(sink);
}

/**
 * The logger a dbHandle logs its statements to: a child asked for with a code
 * default of its own is quieter than the module it belongs to, its records reach
 * the module's sinks by additivity, and the legacy keys of a module may make it
 * speak of every statement again -- until the configuration file says otherwise.
 */
void testSqlChildLevel() {
    Logger* const module = LogManager::get("dbmod");
    Logger* const sql = module->child("sql", ERROR);

    module->setLevel(DEBUG);

    const std::shared_ptr<CaptureSink> onModule = std::make_shared<CaptureSink>();
    module->addSink(onModule, TRACE);

    // The code default of the child, not the level of its parent
    CHECK(ERROR == sql->effectiveLevel());
    CHECK(sql->isAdditive());

    sql->log(recordOf(DEBUG, "select 1"));
    CHECK(0 == onModule->size());

    // What went wrong is heard, on the sinks of the module the child hangs under
    sql->log(recordOf(ERROR, "SQL Error: no such table"));

    CHECK(1 == onModule->size());
    if (1 == onModule->size()) {
        CHECK_EQ(onModule->records[0].logger, "dbmod.sql");
        CHECK_EQ(onModule->records[0].message, "SQL Error: no such table");
    }

    // log_sql = yes of a module's own configuration file
    sql->setLegacyLevel(DEBUG);
    CHECK(DEBUG == sql->effectiveLevel());

    sql->log(recordOf(DEBUG, "select 2"));
    CHECK(2 == onModule->size());

    // And a logger.<module>.sql line beats what that file asked for
    sql->setConfigLevel(ERROR);
    CHECK(ERROR == sql->effectiveLevel());

    sql->log(recordOf(DEBUG, "select 3"));
    CHECK(2 == onModule->size());

    sql->setConfigLevel(std::nullopt);
    sql->setLegacyLevel(std::nullopt);
    module->setConfigLevel(std::nullopt);
    module->removeSink(onModule);
}

/**
 * What a child inherits is the effective level of its parent, whichever of the
 * parent's own levels that came from: a code default is not itself inherited,
 * but the level it gives the parent is.
 */
void testInheritEffectiveLevel() {
    LogManager::get("ce")->setCodeDefault(ERROR);

    CHECK(ERROR == LogManager::get("ce")->effectiveLevel());
    CHECK(ERROR == LogManager::get("ce.x")->effectiveLevel());
    CHECK(ERROR == LogManager::get("ce.x.y")->effectiveLevel());
}

/* ------------------------------------------------------------------ *
 * Dispatch along the path
 * ------------------------------------------------------------------ */

/**
 * A record goes to the sinks of the logger it was logged on, then of its
 * parent, and so on up to the root; each sink sees it once, and every record
 * says which logger it was logged on.  A logger that is not additive is where
 * the walk stops.
 */
void testAdditiveDispatch() {
    Logger* const q = LogManager::get("p.q");
    Logger* const p = LogManager::get("p");
    Logger* const top = LogManager::root();

    const std::shared_ptr<CaptureSink> onQ = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> onP = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> onRoot = std::make_shared<CaptureSink>();

    q->addSink(onQ, TRACE);
    p->addSink(onP, TRACE);
    top->addSink(onRoot, TRACE);

    CHECK(p->isAdditive());
    CHECK(q->isAdditive());

    emit(q, INFO, "upwards");

    CHECK(1 == onQ->records.size());
    CHECK(1 == onP->records.size());
    CHECK(1 == onRoot->records.size());

    // The record carries the name of the logger it was logged on, not of the
    // logger whose sink is reading it
    if (1 == onRoot->records.size()) {
        CHECK_EQ(onRoot->records[0].logger, "p.q");
        CHECK_EQ(onRoot->records[0].message, "upwards");
    }

    // A logger that keeps its records to itself and its descendants
    p->setAdditive(false);
    CHECK(!p->isAdditive());

    emit(q, INFO, "stops at p");

    CHECK(2 == onQ->records.size());
    CHECK(2 == onP->records.size());
    CHECK(1 == onRoot->records.size());

    p->setAdditive(true);

    // One sink object on two loggers of the path still hears the record once
    const std::shared_ptr<CaptureSink> shared = std::make_shared<CaptureSink>();
    q->addSink(shared, TRACE);
    p->addSink(shared, TRACE);

    emit(q, INFO, "once");

    CHECK(1 == shared->records.size());

    q->removeSink(shared);
    p->removeSink(shared);
    q->removeSink(onQ);
    p->removeSink(onP);
    top->removeSink(onRoot);
}

/**
 * Every sink on the path keeps its own threshold: being an ancestor's sink is
 * no reason to see more than it asked for.
 */
void testThresholdsAlongThePath() {
    Logger* const child = LogManager::get("th.child");
    Logger* const parent = LogManager::get("th");

    const std::shared_ptr<CaptureSink> loud = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> quiet = std::make_shared<CaptureSink>();

    child->addSink(loud, TRACE);
    parent->addSink(quiet, WARN);

    emit(child, INFO, "chatter");

    CHECK(1 == loud->records.size());
    CHECK(0 == quiet->records.size());

    emit(child, ERROR, "trouble");

    CHECK(2 == loud->records.size());
    CHECK(1 == quiet->records.size());

    child->removeSink(loud);
    parent->removeSink(quiet);
}

/**
 * One sink attached twice along the path with two thresholds hears a record if
 * either of its attachments lets it through, and hears it once: the dispatch
 * keeps the most permissive of the thresholds of a sink it already has.  A
 * logger that is not additive is left with the threshold it gave the sink
 * itself, which is the only attachment the record still passes by.
 */
void testSharedSinkKeepsTheLoosestThreshold() {
    Logger* const child = LogManager::get("merge.child");
    Logger* const parent = LogManager::get("merge");

    child->setLevel(TRACE);

    const std::shared_ptr<CaptureSink> shared = std::make_shared<CaptureSink>();

    child->addSink(shared, ERROR);
    parent->addSink(shared, TRACE);

    // The child asked for errors only; the parent's attachment is what lets
    // this one through, and the sink still hears it a single time
    emit(child, INFO, "through the parent");

    CHECK(1 == shared->records.size());
    if (1 == shared->records.size())
        CHECK_EQ(shared->records[0].message, "through the parent");

    child->setAdditive(false);

    emit(child, INFO, "nowhere");
    CHECK(1 == shared->records.size());

    emit(child, ERROR, "trouble");
    CHECK(2 == shared->records.size());
    if (2 == shared->records.size())
        CHECK_EQ(shared->records[1].message, "trouble");

    child->setAdditive(true);
    child->setConfigLevel(std::nullopt);
    child->removeSink(shared);
    parent->removeSink(shared);
}

/**
 * The sinks a configuration file asked for and the sinks the code attached are
 * two lists: reloading the configuration drops the first and leaves the second.
 */
void testConfigAndCodeSinks() {
    Logger* const logger = LogManager::get("cfg");

    const std::shared_ptr<CaptureSink> fromConfig = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> fromCode = std::make_shared<CaptureSink>();

    logger->addConfigSink(fromConfig, TRACE);
    logger->addSink(fromCode, TRACE);

    emit(logger, INFO, "both");

    CHECK(1 == fromConfig->records.size());
    CHECK(1 == fromCode->records.size());

    // A configured sink has a threshold of its own like any other
    logger->setSinkThreshold(fromConfig, ERROR);
    emit(logger, INFO, "code only");

    CHECK(1 == fromConfig->records.size());
    CHECK(2 == fromCode->records.size());

    logger->clearConfigSinks();
    emit(logger, ERROR, "code again");

    CHECK(1 == fromConfig->records.size());
    CHECK(3 == fromCode->records.size());

    logger->removeSink(fromCode);
}

/* ------------------------------------------------------------------ *
 * Module names, the module being loaded, rotation, the name column
 * ------------------------------------------------------------------ */

/**
 * The name of the logger a module logs to is what is left of its library file
 * name once the directory, the "lib" and the suffixes are gone.
 */
void testModuleNameFromLibrary() {
    CHECK_EQ(LogManager::moduleNameFromLibrary("libcservice.la"), "cservice");
    CHECK_EQ(LogManager::moduleNameFromLibrary("libchanfix.la"), "chanfix");
    CHECK_EQ(LogManager::moduleNameFromLibrary("/x/y/libdronescan.so.0.0.0"), "dronescan");
    CHECK_EQ(LogManager::moduleNameFromLibrary("cservice"), "cservice");
    CHECK_EQ(LogManager::moduleNameFromLibrary("lib.la"), "");
}

/**
 * The name of the module that is being loaded is handed over once: whoever
 * takes it leaves nothing behind for the next module.
 */
void testLoadingModule() {
    LogManager::setLoadingModule("m");

    CHECK_EQ(LogManager::takeLoadingModule(), "m");
    CHECK_EQ(LogManager::takeLoadingModule(), "");
}

/**
 * The name column of the text sinks is as wide as the longest logger name, and
 * never wider than twenty characters.
 */
void testNameWidth() {
    const std::string longName("n234567890123");

    LogManager::get(longName);

    CHECK(LogSinks::nameWidth() >= longName.size());

    LogManager::get(std::string("w23456789012345678901234567890"));

    CHECK(20 == LogSinks::nameWidth());
}

/**
 * Rotation asks every sink of every logger to reopen itself, and a sink that
 * two loggers share is asked once.
 */
void testReopenAll() {
    const std::shared_ptr<CountingSink> sink = std::make_shared<CountingSink>();

    LogManager::get("ro.a")->addSink(sink, TRACE);
    LogManager::get("ro.b")->addSink(sink, TRACE);

    LogManager::reopenAll();

    CHECK(1 == sink->reopens);

    LogManager::get("ro.a")->removeSink(sink);
    LogManager::get("ro.b")->removeSink(sink);
}

/**
 * Eight threads asking for the same fifty loggers at once get the same fifty
 * loggers, and every record they log arrives.
 */
void testThreadSafety() {
    const std::size_t threadCount = 8;
    const std::size_t nameCount = 50;

    const std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    LogManager::get("mt")->addSink(sink, TRACE);

    std::vector<std::vector<Logger*>> seen(threadCount);
    std::vector<std::thread> threads;

    for (std::size_t thread = 0; thread < threadCount; ++thread)
        threads.emplace_back([&seen, thread, nameCount]() {
            for (std::size_t which = 0; which < nameCount; ++which) {
                Logger* const logger = LogManager::get("mt.n" + std::to_string(which));

                seen[thread].push_back(logger);
                emit(logger, INFO, "racing");
            }
        });

    for (std::thread& thread : threads)
        thread.join();

    for (std::size_t thread = 1; thread < threadCount; ++thread)
        CHECK(seen[thread] == seen[0]);

    CHECK(nameCount == seen[0].size());
    CHECK(threadCount * nameCount == sink->size());

    LogManager::get("mt")->removeSink(sink);
}

} // namespace

int main() {
    // The console sink the registry starts with would print every record of
    // this test; the test is about what the sinks are given, not about that
    LogManager::root()->removeSink(LogManager::bootstrapConsoleSink());

    testTree();
    testInheritance();
    testCodeDefault();
    testLogFiltersByEffectiveLevel();
    testSqlChildLevel();
    testInheritEffectiveLevel();
    testAdditiveDispatch();
    testThresholdsAlongThePath();
    testSharedSinkKeepsTheLoosestThreshold();
    testConfigAndCodeSinks();
    testModuleNameFromLibrary();
    testLoadingModule();
    testNameWidth();
    testReopenAll();
    testThreadSafety();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_hierarchy: all checks passed\n";
    return 0;
}
