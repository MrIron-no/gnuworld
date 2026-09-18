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
 * The legacy per-module verbosities are a level of the logger as well as a
 * threshold of a sink: what the three keys ask for together is what the logger
 * logs, which is what they asked for before a logger had a hierarchy to inherit
 * a level from.  A slot that is not installed has no say, and what it was asked
 * for in the meantime is applied when it arrives.
 */
/**
 * The logger this one replaces built a record if any notifier wanted it.  So
 * while a module logs the legacy way, a sink it attached itself -- pushover,
 * say -- raises the level to what it asks for, and stops doing so when it goes.
 */
void testLegacyLevelHearsTheModulesOwnSinks() {
    Logger* const logger = LogManager::get("legacy.notifier");

    const std::shared_ptr<CaptureSink> file = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> notifier = std::make_shared<CaptureSink>();

    // Without a legacy slot a sink of the module's own changes no level
    logger->addSink(notifier, DEBUG);
    CHECK(INFO == logger->effectiveLevel());

    logger->addSink(file, TRACE);
    logger->setLegacyFileSink(file);
    logger->setLogVerbosity(4);

    // The file asks for INFO, the notifier for DEBUG: DEBUG it is
    CHECK(DEBUG == logger->effectiveLevel());

    emit(logger, DEBUG, "for the notifier");
    CHECK(1 == notifier->size());
    CHECK(0 == file->size());

    logger->setSinkThreshold(notifier, WARN);
    CHECK(INFO == logger->effectiveLevel());

    logger->setSinkThreshold(notifier, TRACE);
    CHECK(TRACE == logger->effectiveLevel());

    logger->removeSink(notifier);
    CHECK(INFO == logger->effectiveLevel());

    logger->removeSink(file);
    logger->resetLegacyState();
    CHECK(INFO == logger->effectiveLevel());
}

void testLegacyVerbosityLevel() {
    Logger* const logger = LogManager::get("legacy.mod");

    // With no slot installed the logger has nothing of its own to say: the
    // level is the root's, and a debug record is not built at all
    CHECK(INFO == logger->effectiveLevel());
    CHECK(!logger->shouldLog(DEBUG));

    const std::shared_ptr<CaptureSink> file = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> console = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> irc = std::make_shared<CaptureSink>();

    // The file slot arrives with the default log verbosity, which is TRACE
    logger->addSink(file, TRACE);
    logger->setLegacyFileSink(file);

    CHECK(TRACE == logger->effectiveLevel());

    emit(logger, DEBUG, "kept");
    CHECK(1 == file->size());

    // What the only slot installed asks for is what the logger logs
    logger->setLogVerbosity(4);

    CHECK(INFO == logger->effectiveLevel());

    emit(logger, DEBUG, "dropped");
    CHECK(1 == file->size());

    // A verbosity out of the range there is asks for the most there is
    logger->setLogVerbosity(99);
    CHECK(TRACE == logger->effectiveLevel());
    logger->setLogVerbosity(4);
    CHECK(INFO == logger->effectiveLevel());

    /* The most of what the installed slots ask for: the console wants debug
     * records, the file still only informational ones, and the threshold of the
     * file sink is what keeps this one from it */
    logger->addSink(console, TRACE);
    logger->setLegacyConsoleSink(console);
    logger->setConsoleVerbosity(5);

    CHECK(DEBUG == logger->effectiveLevel());

    emit(logger, DEBUG, "the console only");
    CHECK(1 == file->size());
    CHECK(1 == console->size());

    // A channel verbosity with no channel sink is remembered, not applied: the
    // slot that arrives later is what makes it a level
    logger->setChanVerbosity(6);
    CHECK(DEBUG == logger->effectiveLevel());

    logger->addSink(irc, INFO);
    logger->setLegacyIrcSink(irc);
    CHECK(TRACE == logger->effectiveLevel());

    // The configuration file beats every one of them, and says nothing again
    logger->setConfigLevel(WARN);
    CHECK(WARN == logger->effectiveLevel());
    logger->setConfigLevel(std::nullopt);
    CHECK(TRACE == logger->effectiveLevel());

    // The channel is asked for less, so the console is the loudest left, and
    // taking the console sink away leaves the file and the channel
    logger->setChanVerbosity(4);
    CHECK(DEBUG == logger->effectiveLevel());

    logger->removeSink(console);
    CHECK(INFO == logger->effectiveLevel());

    /* An unloaded module leaves nothing of itself on the logger, which is the
     * registry's and which the next instance of the module finds */
    logger->setChannel("#log");
    logger->setLogSQL(true);
    logger->setConsoleSQL(true);

    CHECK_EQ(logger->getChannel(), "#log");

    logger->removeSink(file);
    logger->removeSink(irc);
    logger->resetLegacyState();

    CHECK(INFO == logger->effectiveLevel());
    CHECK_EQ(logger->getChannel(), "");

    // A file slot installed again starts from the default verbosity, and the
    // SQL keys are off as they are at the outset
    const std::shared_ptr<CaptureSink> second = std::make_shared<CaptureSink>();

    logger->addSink(second, TRACE);
    logger->setLegacyFileSink(second);

    CHECK(TRACE == logger->effectiveLevel());

    logger->write(SQL, std::string("select 1"));
    CHECK(0 == second->size());

    emit(logger, TRACE, "verbose again");
    CHECK(1 == second->size());

    logger->removeSink(second);
    logger->resetLegacyState();
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
    testLegacyVerbosityLevel();
    testLegacyLevelHearsTheModulesOwnSinks();
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
