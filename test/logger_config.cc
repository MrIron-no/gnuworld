/**
 * logger_config.cc
 * Unit test for libgnuworld/LogConfig.h and the configuration half of
 * libgnuworld/LogManager.h: what a logging.conf parses into, that a file with
 * any problem at all is rejected as a whole and named in the errors, that a
 * configuration is built before it is applied, and that what the code attached
 * survives every reload.  It links libgnuworld alone.
 * Runs under "make check".
 *
 * The loggers of the registry are permanent and process-wide, so every case
 * here uses names of its own.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include "LogConfig.h"
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

/* ------------------------------------------------------------------ *
 * A scratch directory of our own, gone again when the test ends
 * ------------------------------------------------------------------ */

std::string scratchDir;

void removeScratchDir() {
    if (scratchDir.empty())
        return;

    DIR* const dir = opendir(scratchDir.c_str());
    if (nullptr != dir) {
        for (const struct dirent* entry = readdir(dir); nullptr != entry; entry = readdir(dir)) {
            if (0 == std::strcmp(entry->d_name, ".") || 0 == std::strcmp(entry->d_name, ".."))
                continue;
            const std::string victim = scratchDir + '/' + entry->d_name;
            ::unlink(victim.c_str());
        }
        closedir(dir);
    }

    ::rmdir(scratchDir.c_str());
    scratchDir.clear();
}

/// Creates the scratch directory; returns false when the system will not have it
bool makeScratchDir() {
    const char* const tmp = std::getenv("TMPDIR");
    std::string pattern = (nullptr != tmp && '\0' != tmp[0]) ? tmp : "/tmp";
    pattern += "/gnuworld_logconfig_XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');

    if (nullptr == mkdtemp(buffer.data()))
        return false;

    scratchDir = buffer.data();
    std::atexit(removeScratchDir);

    return true;
}

std::string scratchPath(const std::string& name) { return scratchDir + '/' + name; }

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/// Writes a configuration file of this content and gives back its path
std::string writeConf(const std::string& name, const std::string& body) {
    const std::string path = scratchPath(name);
    std::ofstream out(path, std::ios::out | std::ios::trunc);

    out << body;
    out.close();

    return path;
}

/// The whole content of a file, empty when it is not there
std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;

    out << in.rdbuf();

    return out.str();
}

/// The lines of a file, without the empty one a trailing newline leaves
std::vector<std::string> readFileLines(const std::string& path) {
    std::vector<std::string> lines;
    std::string current;

    for (const char c : readWholeFile(path)) {
        if ('\n' == c) {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }

    if (!current.empty())
        lines.push_back(current);

    return lines;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return std::string::npos != haystack.find(needle);
}

/// Whether any of the errors names this key
bool anyErrorNames(const std::vector<std::string>& errors, const std::string& key) {
    for (const std::string& error : errors)
        if (contains(error, key))
            return true;

    return false;
}

/// One record on this logger, at this level, through the ordinary call path
void emit(Logger* logger, Verbosity level, const std::string& message) {
    logger->writeFunc(level, "void anon::emit()", "{}", message);
}

/// The spec of this sink id, or nothing
const SinkSpec* findSink(const LogConfig& config, const std::string& id) {
    for (const SinkSpec& spec : config.sinks)
        if (spec.id == id)
            return &spec;

    return nullptr;
}

/// The spec of this logger name, or nothing
const LoggerSpec* findLogger(const LogConfig& config, const std::string& name) {
    for (const LoggerSpec& spec : config.loggers)
        if (spec.name == name)
            return &spec;

    return nullptr;
}

/* ------------------------------------------------------------------ *
 * Parsing a whole file
 * ------------------------------------------------------------------ */

/**
 * The file of the example: two sinks of different kinds, four loggers, one of
 * them the root and one of them not additive.  Every field of every struct is
 * what the file said, and the two lists are in a stable order.
 */
void testParseFull() {
    const std::string logPath = scratchPath("full.log");
    const std::string path =
        writeConf("full.conf", "# The sinks\n"
                               "\n"
                               "sink.main.type = file\n"
                               "sink.main.path = " +
                                   logPath +
                                   "\n"
                                   "sink.main.format = json\n"
                                   "\n"
                                   "sink.console.type = console\n"
                                   "sink.console.colour = no\n"
                                   "sink.console.level = WARN\n"
                                   "\n"
                                   "# The loggers\n"
                                   "logger.root = INFO, main, console\n"
                                   "logger.cfg.a = DEBUG, main\n"
                                   "logger.cfg.a.sql = OFF\n"
                                   "logger.dronescan.spam.action = INFO, main\n"
                                   "additivity.dronescan.spam.action = no\n");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(path, config, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    // Sorted by id, so that two reads of the file give the same thing
    CHECK(2 == config.sinks.size());
    if (2 == config.sinks.size()) {
        CHECK_EQ(config.sinks[0].id, "console");
        CHECK_EQ(config.sinks[1].id, "main");
    }

    const SinkSpec* const main = findSink(config, "main");
    CHECK(nullptr != main);
    if (nullptr != main) {
        CHECK_EQ(main->type, "file");
        CHECK_EQ(main->path, logPath);
        CHECK(main->json);
        CHECK(TRACE == main->level);
        CHECK(main->highlight);
        CHECK_EQ(main->channel, "");
    }

    const SinkSpec* const console = findSink(config, "console");
    CHECK(nullptr != console);
    if (nullptr != console) {
        CHECK_EQ(console->type, "console");
        CHECK(WARN == console->level);
        CHECK(ConsoleSink::Colour::No == console->colour);
        CHECK(console->highlight);
        CHECK_EQ(console->path, "");
    }

    // Sorted by name, the root first because its name is empty
    CHECK(4 == config.loggers.size());
    if (4 == config.loggers.size()) {
        CHECK_EQ(config.loggers[0].name, "");
        CHECK_EQ(config.loggers[1].name, "cfg.a");
        CHECK_EQ(config.loggers[2].name, "cfg.a.sql");
        CHECK_EQ(config.loggers[3].name, "dronescan.spam.action");
    }

    const LoggerSpec* const root = findLogger(config, "");
    CHECK(nullptr != root);
    if (nullptr != root) {
        CHECK(root->level && INFO == *root->level);
        CHECK(2 == root->sinks.size());
        if (2 == root->sinks.size()) {
            CHECK_EQ(root->sinks[0], "main");
            CHECK_EQ(root->sinks[1], "console");
        }
        CHECK(!root->additive.has_value());
    }

    const LoggerSpec* const a = findLogger(config, "cfg.a");
    CHECK(nullptr != a);
    if (nullptr != a) {
        CHECK(a->level && DEBUG == *a->level);
        CHECK(1 == a->sinks.size());
        CHECK(!a->additive.has_value());
    }

    const LoggerSpec* const sql = findLogger(config, "cfg.a.sql");
    CHECK(nullptr != sql);
    if (nullptr != sql) {
        CHECK(sql->level && OFF == *sql->level);
        CHECK(sql->sinks.empty());
    }

    const LoggerSpec* const action = findLogger(config, "dronescan.spam.action");
    CHECK(nullptr != action);
    if (nullptr != action) {
        CHECK(action->level && INFO == *action->level);
        CHECK(1 == action->sinks.size());
        if (1 == action->sinks.size())
            CHECK_EQ(action->sinks[0], "main");
        CHECK(action->additive.has_value() && false == *action->additive);
    }
}

/**
 * Whitespace around a level and around every sink id is not part of them, the
 * name of a setting and the value of a format or of a yes/no are read whatever
 * their case, and the name of a logger keeps the case it was written in.
 */
void testWhitespaceAndCase() {
    const std::string path = writeConf("loose.conf", "sink.main.type = file\n"
                                                     "sink.main.path = " +
                                                         scratchPath("loose.log") +
                                                         "\n"
                                                         "sink.main.FORMAT = TEXT\n"
                                                         "sink.console.type = CONSOLE\n"
                                                         "logger.x =  debug ,main , console \n"
                                                         "additivity.x = Off\n");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(path, config, errors));

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    const SinkSpec* const main = findSink(config, "main");
    CHECK(nullptr != main);
    if (nullptr != main)
        CHECK(!main->json);

    const LoggerSpec* const x = findLogger(config, "x");
    CHECK(nullptr != x);
    if (nullptr != x) {
        CHECK(x->level && DEBUG == *x->level);
        CHECK(2 == x->sinks.size());
        if (2 == x->sinks.size()) {
            CHECK_EQ(x->sinks[0], "main");
            CHECK_EQ(x->sinks[1], "console");
        }
        CHECK(x->additive.has_value() && false == *x->additive);
    }
}

/* ------------------------------------------------------------------ *
 * Every kind of error: the whole file goes, and the key is named
 * ------------------------------------------------------------------ */

/**
 * Writes this configuration, parses it, and checks that it was rejected as a
 * whole with an error naming this key.
 */
void expectError(const std::string& name, const std::string& body, const std::string& key) {
    const std::string path = writeConf(name, body);

    LogConfig config;
    std::vector<std::string> errors;

    // Something is in there already, to show that a rejected file leaves nothing
    config.sinks.push_back(SinkSpec());
    config.loggers.push_back(LoggerSpec());

    const bool parsed = parseLogConfig(path, config, errors);

    if (parsed) {
        ++failures;
        std::cerr << __FILE__ << ": " << name << ": expected a rejection, got a configuration\n";
        return;
    }

    CHECK(!errors.empty());
    CHECK(config.sinks.empty());
    CHECK(config.loggers.empty());

    if (!anyErrorNames(errors, key)) {
        ++failures;
        std::cerr << __FILE__ << ": " << name << ": no error names [" << key << "]\n";
        for (const std::string& error : errors)
            std::cerr << "    error: " << error << '\n';
    }
}

void testErrors() {
    const std::string file = scratchPath("errors.log");

    // An unknown key prefix
    expectError("e-prefix.conf", "nonsense.x = 1\n", "nonsense.x");

    // An unknown setting of a sink, and a key that is not a setting at all
    expectError("e-setting.conf", "sink.main.type = console\nsink.main.wibble = 1\n",
                "sink.main.wibble");
    expectError("e-nosetting.conf", "sink.main = console\n", "sink.main");

    // A sink id with a character that is not one of the allowed ones
    expectError("e-id.conf", "sink.ba+d.type = console\n", "sink.ba+d.type");

    // A sink with settings but no type at all, and a type that is empty
    expectError("e-notype.conf", "sink.orphan.path = " + file + "\n", "sink.orphan");
    expectError("e-emptytype.conf", "sink.et.type =\n", "sink.et.type");

    // A level, a yes/no, a colour and a format that are not one
    expectError("e-level.conf", "sink.main.type = console\nsink.main.level = LOUD\n",
                "sink.main.level");
    expectError("e-yesno.conf", "sink.main.type = console\nsink.main.highlight = maybe\n",
                "sink.main.highlight");
    expectError("e-colour.conf", "sink.main.type = console\nsink.main.colour = plaid\n",
                "sink.main.colour");
    expectError("e-format.conf",
                "sink.main.type = file\nsink.main.path = " + file + "\nsink.main.format = yaml\n",
                "sink.main.format");

    // SQL is a leftover category, not a level anyone may configure
    expectError("e-sql.conf", "sink.main.type = console\nsink.main.level = SQL\n",
                "sink.main.level");

    // A file sink without a path, an irc sink without a channel
    expectError("e-nopath.conf", "sink.f.type = file\n", "sink.f.path");
    expectError("e-nochannel.conf", "sink.i.type = irc\n", "sink.i.channel");

    // A logger line whose level is missing, and one whose level is not a level
    expectError("e-nolevel.conf", "logger.x = , main\n", "logger.x");
    expectError("e-badlevel.conf", "logger.x = LOUD\n", "logger.x");

    // A sink nothing defines, and a sink listed twice on one line
    expectError("e-unknownsink.conf", "logger.x = INFO, nosuch\n", "logger.x");
    expectError("e-twice.conf", "sink.m.type = console\nlogger.x = INFO, m, m\n", "logger.x");

    // The same logger, the same additivity, the same setting, said twice
    expectError("e-duplogger.conf", "logger.x = INFO\nlogger.x = DEBUG\n", "logger.x");
    expectError("e-dupadd.conf", "additivity.x = yes\nadditivity.x = no\n", "additivity.x");
    expectError("e-dupsetting.conf", "sink.m.type = console\nsink.m.type = file\n", "sink.m.type");

    // A logger line with no name at all
    expectError("e-noname.conf", "logger. = INFO\n", "logger.");
    expectError("e-noaddname.conf", "additivity. = yes\n", "additivity.");
}

/**
 * A file that is not there is an error like any other, reported as such: the
 * process is still running afterwards, which is the whole point.
 */
void testMissingFile() {
    LogConfig config;
    std::vector<std::string> errors;

    const std::string path = scratchPath("no-such-file.conf");

    CHECK(!parseLogConfig(path, config, errors));
    CHECK(config.sinks.empty());
    CHECK(config.loggers.empty());
    CHECK(1 == errors.size());

    if (!errors.empty()) {
        CHECK(contains(errors[0], "cannot open"));
        CHECK(contains(errors[0], path));
    }
}

/* ------------------------------------------------------------------ *
 * Applying a configuration
 * ------------------------------------------------------------------ */

/**
 * A configured sink receives the records of the logger it was configured on and
 * of every logger below it, and the configuration remembers which loggers it
 * was told about: the ones with a level line, not the ones an additivity line
 * merely mentions.
 */
void testConfigureRoutes() {
    const std::string logPath = scratchPath("routed.log");
    const std::string path = writeConf("routed.conf", "sink.f2.type = file\n"
                                                      "sink.f2.path = " +
                                                          logPath +
                                                          "\n"
                                                          "logger.cfg2.a = DEBUG, f2\n"
                                                          "additivity.cfg2.only = no\n");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(path, config, errors));
    CHECK(LogManager::configure(config, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    emit(LogManager::get("cfg2.a.b"), INFO, "routed");

    const std::vector<std::string> lines = readFileLines(logPath);

    CHECK(1 == lines.size());
    if (1 == lines.size()) {
        CHECK(contains(lines[0], "\"logger\":\"cfg2.a.b\""));
        CHECK(contains(lines[0], "\"message\":\"routed\""));
    }

    // A level line is what makes a logger configured
    CHECK(LogManager::isConfigured("cfg2.a"));
    CHECK(!LogManager::isConfigured("cfg2.a.b"));
    CHECK(!LogManager::isConfigured("cfg2"));

    // An additivity line alone is not a configuration of the logger
    CHECK(!LogManager::isConfigured("cfg2.only"));
    CHECK(!LogManager::get("cfg2.only")->isAdditive());
}

/**
 * Reading a configuration again takes away everything the one before it gave the
 * loggers - its sinks, its levels, its additivity - and takes away nothing the
 * code attached.
 */
void testReloadReplacesConfigurationOnly() {
    const std::string firstLog = scratchPath("reload-first.log");
    const std::string secondLog = scratchPath("reload-second.log");

    const std::shared_ptr<CaptureSink> fromCode = std::make_shared<CaptureSink>();
    LogManager::get("cfg6.a")->addSink(fromCode, TRACE);

    LogConfig first;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("reload-first.conf", "sink.r1.type = file\n"
                                                        "sink.r1.path = " +
                                                            firstLog +
                                                            "\n"
                                                            "logger.cfg6.a = DEBUG, r1\n"
                                                            "additivity.cfg6.a = no\n"),
                         first, errors));
    CHECK(LogManager::configure(first, errors));

    CHECK(DEBUG == LogManager::get("cfg6.a")->effectiveLevel());
    CHECK(!LogManager::get("cfg6.a")->isAdditive());

    emit(LogManager::get("cfg6.a"), DEBUG, "first");

    CHECK(1 == readFileLines(firstLog).size());
    CHECK(1 == fromCode->size());

    LogConfig second;

    CHECK(parseLogConfig(writeConf("reload-second.conf", "sink.r2.type = file\n"
                                                         "sink.r2.path = " +
                                                             secondLog +
                                                             "\n"
                                                             "logger.cfg6.other = INFO, r2\n"),
                         second, errors));
    CHECK(LogManager::configure(second, errors));

    // The level, the sink and the additivity of the first configuration are gone
    CHECK(INFO == LogManager::get("cfg6.a")->effectiveLevel());
    CHECK(LogManager::get("cfg6.a")->isAdditive());
    CHECK(!LogManager::isConfigured("cfg6.a"));

    emit(LogManager::get("cfg6.a"), INFO, "second");

    CHECK(1 == readFileLines(firstLog).size());
    CHECK(2 == fromCode->size());

    LogManager::get("cfg6.a")->removeSink(fromCode);
}

/**
 * A logger the code made non-additive stays that way through a configuration
 * that says nothing about it; an additivity line overrides the code either way,
 * and dropping that line gives the code's answer back.
 */
void testConfigAdditivityOverridesCode() {
    Logger* const logger = LogManager::get("cfg3.code");

    logger->setAdditive(false);
    CHECK(!logger->isAdditive());

    LogConfig quiet;
    std::vector<std::string> errors;

    CHECK(
        parseLogConfig(writeConf("add-none.conf", "additivity.cfg3.other = no\n"), quiet, errors));
    CHECK(LogManager::configure(quiet, errors));

    // The configuration said nothing about this one, so the code still speaks
    CHECK(!logger->isAdditive());

    LogConfig loud;

    CHECK(parseLogConfig(writeConf("add-yes.conf", "additivity.cfg3.code = yes\n"), loud, errors));
    CHECK(LogManager::configure(loud, errors));

    CHECK(logger->isAdditive());

    // And with the line gone the code's answer is the answer again
    CHECK(LogManager::configure(quiet, errors));
    CHECK(!logger->isAdditive());

    logger->setAdditive(true);
}

/**
 * An additivity the file asked for is not just something to read back: it is
 * where the walk up the hierarchy stops.
 */
void testConfigAdditivityStopsDispatch() {
    const std::shared_ptr<CaptureSink> atTop = std::make_shared<CaptureSink>();
    LogManager::get("cfg12")->addSink(atTop, TRACE);

    LogConfig quiet;
    LogConfig loud;
    std::vector<std::string> errors;

    CHECK(
        parseLogConfig(writeConf("stop-yes.conf", "additivity.cfg12.leaf = no\n"), quiet, errors));
    CHECK(parseLogConfig(writeConf("stop-no.conf", "logger.cfg12.leaf = INFO\n"), loud, errors));

    CHECK(LogManager::configure(quiet, errors));

    emit(LogManager::get("cfg12.leaf"), INFO, "kept below");
    CHECK(0 == atTop->size());

    // With the line gone the records walk up to the ancestor's sink again
    CHECK(LogManager::configure(loud, errors));

    emit(LogManager::get("cfg12.leaf"), INFO, "up again");
    CHECK(1 == atTop->size());

    LogManager::get("cfg12")->removeSink(atTop);
}

/**
 * Nothing changes until every sink of the configuration exists.  A type nothing
 * has registered is a reason to change nothing; so is a file that will not open.
 */
void testBuildBeforeSwap() {
    const std::string keepLog = scratchPath("keep.log");

    LogConfig working;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("keep.conf", "sink.g1.type = file\n"
                                                "sink.g1.path = " +
                                                    keepLog +
                                                    "\n"
                                                    "logger.cfg4.a = INFO, g1\n"),
                         working, errors));
    CHECK(LogManager::configure(working, errors));

    emit(LogManager::get("cfg4.a"), INFO, "before");
    CHECK(1 == readFileLines(keepLog).size());

    // The same, with a second sink of a kind nothing knows yet
    LogConfig withIrc;

    CHECK(parseLogConfig(writeConf("with-irc.conf", "sink.g1.type = file\n"
                                                    "sink.g1.path = " +
                                                        keepLog +
                                                        "\n"
                                                        "sink.g2.type = irc\n"
                                                        "sink.g2.channel = #coder-com\n"
                                                        "logger.cfg4.a = INFO, g1, g2\n"),
                         withIrc, errors));

    CHECK(!LogManager::configure(withIrc, errors));
    CHECK(!errors.empty());
    CHECK(anyErrorNames(errors, "sink.g2.type"));

    // And the configuration that was in force is still in force, whole
    emit(LogManager::get("cfg4.a"), INFO, "still here");
    CHECK(2 == readFileLines(keepLog).size());

    // A factory that cannot make its sink is the same kind of refusal
    LogConfig badPath;

    CHECK(
        parseLogConfig(writeConf("bad-path.conf", "sink.g3.type = file\n"
                                                  "sink.g3.path = /no-such-directory-here/out.log\n"
                                                  "logger.cfg4.a = INFO, g3\n"),
                       badPath, errors));

    CHECK(!LogManager::configure(badPath, errors));
    CHECK(anyErrorNames(errors, "sink.g3"));

    emit(LogManager::get("cfg4.a"), INFO, "and still");
    CHECK(3 == readFileLines(keepLog).size());

    // With the kind of sink registered, the very same configuration is applied
    const std::shared_ptr<CaptureSink> channel = std::make_shared<CaptureSink>();

    LogManager::registerSinkType(
        "irc", [channel](const SinkSpec& spec, std::string& error) -> std::shared_ptr<LogSink> {
            if (spec.channel.empty()) {
                error = "no channel";
                return nullptr;
            }

            return channel;
        });

    CHECK(LogManager::configure(withIrc, errors));
    CHECK(errors.empty());

    emit(LogManager::get("cfg4.a"), INFO, "to the channel");

    CHECK(1 == channel->size());
    CHECK(4 == readFileLines(keepLog).size());
}

/**
 * A kind of sink is looked up without regard to the case of its name, a kind
 * registered again replaces the one that was there, and a sink no logger sends
 * anything to is built and then simply dropped.
 */
void testSinkTypeRegistry() {
    const std::shared_ptr<CaptureSink> first = std::make_shared<CaptureSink>();
    const std::shared_ptr<CaptureSink> second = std::make_shared<CaptureSink>();

    LogManager::registerSinkType(
        "PUSH",
        [first](const SinkSpec&, std::string&) -> std::shared_ptr<LogSink> { return first; });

    LogConfig config;
    std::vector<std::string> errors;

    // "Push" in the file, "PUSH" in the registration: the same kind of sink
    CHECK(parseLogConfig(writeConf("push.conf", "sink.p.type = Push\n"
                                                "logger.cfg10.a = INFO, p\n"),
                         config, errors));
    CHECK(LogManager::configure(config, errors));

    emit(LogManager::get("cfg10.a"), INFO, "pushed");

    CHECK(1 == first->size());
    CHECK(0 == second->size());

    // The same kind registered again is the kind that is used from then on
    LogManager::registerSinkType(
        "push",
        [second](const SinkSpec&, std::string&) -> std::shared_ptr<LogSink> { return second; });

    CHECK(LogManager::configure(config, errors));

    emit(LogManager::get("cfg10.a"), INFO, "pushed again");

    CHECK(1 == first->size());
    CHECK(1 == second->size());

    // A sink nothing logs to is made and dropped, and is no reason to refuse
    LogConfig unused;

    CHECK(parseLogConfig(writeConf("unused.conf", "sink.p.type = push\n"
                                                  "sink.spare.type = file\n"
                                                  "sink.spare.path = " +
                                                      scratchPath("spare.log") +
                                                      "\n"
                                                      "logger.cfg10.a = INFO, p\n"),
                         unused, errors));
    CHECK(LogManager::configure(unused, errors));
    CHECK(errors.empty());

    emit(LogManager::get("cfg10.a"), INFO, "still only push");

    CHECK(2 == second->size());
    CHECK(0 == readFileLines(scratchPath("spare.log")).size());
}

/**
 * The root answers to both of its names here too: a "logger.root" line
 * configures the logger whose name is empty.
 */
void testRootIsConfigured() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("rootline.conf", "logger.root = INFO\n"), config, errors));
    CHECK(LogManager::configure(config, errors));

    CHECK(LogManager::isConfigured("root"));
    CHECK(LogManager::isConfigured(""));

    // And with the line gone it is not configured any more
    LogConfig empty;

    CHECK(parseLogConfig(writeConf("noroot.conf", "logger.cfg11.a = INFO\n"), empty, errors));
    CHECK(LogManager::configure(empty, errors));

    CHECK(!LogManager::isConfigured("root"));
    CHECK(!LogManager::isConfigured(""));
}

/**
 * A sink takes its own threshold from the file: a logger may be as verbose as it
 * likes, a sink that asked for warnings hears warnings.
 */
void testSinkThreshold() {
    const std::string logPath = scratchPath("threshold.log");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("threshold.conf", "sink.w.type = file\n"
                                                     "sink.w.level = WARN\n"
                                                     "sink.w.path = " +
                                                         logPath +
                                                         "\n"
                                                         "logger.cfg5.a = DEBUG, w\n"),
                         config, errors));
    CHECK(LogManager::configure(config, errors));

    emit(LogManager::get("cfg5.a"), INFO, "chatter");
    CHECK(0 == readFileLines(logPath).size());

    emit(LogManager::get("cfg5.a"), WARN, "trouble");
    CHECK(1 == readFileLines(logPath).size());
}

/**
 * A file that cannot be applied is reported, once per problem, on "core.config",
 * and changes nothing at all.
 */
void testLoadFileReportsAndKeeps() {
    const std::string keepLog = scratchPath("loadfile.log");

    LogConfig working;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("loadfile.conf", "sink.h1.type = file\n"
                                                    "sink.h1.path = " +
                                                        keepLog +
                                                        "\n"
                                                        "logger.cfg7.a = INFO, h1\n"),
                         working, errors));
    CHECK(LogManager::configure(working, errors));

    emit(LogManager::get("cfg7.a"), INFO, "before");
    CHECK(1 == readFileLines(keepLog).size());

    // The sink that hears what loadFile has to say about the file
    const std::shared_ptr<CaptureSink> reports = std::make_shared<CaptureSink>();
    LogManager::get("core.config")->addSink(reports, TRACE);

    const std::string broken = writeConf("broken.conf", "nonsense.one = 1\n"
                                                        "nonsense.two = 2\n");

    CHECK(!LogManager::loadFile(broken));

    CHECK(2 == reports->size());
    for (const LogRecord& record : reports->records) {
        CHECK(ERROR == record.level);
        CHECK_EQ(record.logger, "core.config");
        CHECK(0 == record.message.find("logging.conf: "));
    }

    // Nothing of the configuration in force was touched
    emit(LogManager::get("cfg7.a"), INFO, "after");
    CHECK(2 == readFileLines(keepLog).size());

    // A file that is there and is good says nothing at all
    reports->clear();
    CHECK(LogManager::loadFile(writeConf("good.conf", "logger.cfg7.a = INFO\n")));
    CHECK(0 == reports->size());

    LogManager::get("core.config")->removeSink(reports);
}

/**
 * The console sink the root starts with is the one nobody configured; the first
 * configuration that is applied takes it away, so that a record does not appear
 * twice once a file says where records go.
 */
void testBootstrapConsoleGoes() {
    // main() took it off already: this case is about a configuration doing so
    LogManager::root()->addSink(LogManager::bootstrapConsoleSink(), TRACE);

    std::ostringstream captured;
    std::streambuf* const saved = std::cout.rdbuf(captured.rdbuf());

    emit(LogManager::root(), INFO, "on the console");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("console.conf", "logger.cfg8.a = INFO\n"), config, errors));
    CHECK(LogManager::configure(config, errors));

    const std::string beforeConfigure = captured.str();

    emit(LogManager::root(), INFO, "and now nowhere");

    const std::string afterConfigure = captured.str();

    std::cout.rdbuf(saved);

    CHECK(contains(beforeConfigure, "on the console"));
    CHECK_EQ(afterConfigure, beforeConfigure);
}

/**
 * The log file of a process that has no logging.conf: one text sink, attached
 * once however often it is asked for, and left alone by every configuration -
 * it belongs to the code, not to a file.
 */
void testBootstrapFile() {
    const std::string logPath = scratchPath("bootstrap.log");

    LogManager::bootstrapFile(logPath);
    LogManager::bootstrapFile(logPath);

    emit(LogManager::root(), INFO, "bootstrapped");

    const std::vector<std::string> first = readFileLines(logPath);

    CHECK(1 == first.size());
    if (1 == first.size()) {
        // Human-readable columns, not JSON
        CHECK('{' != first[0][0]);
        CHECK(contains(first[0], "bootstrapped"));
        CHECK(contains(first[0], "root"));
    }

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("bootstrap.conf", "logger.cfg9.a = INFO\n"), config, errors));
    CHECK(LogManager::configure(config, errors));

    emit(LogManager::root(), INFO, "still bootstrapped");

    CHECK(2 == readFileLines(logPath).size());
}

/**
 * Four threads logging while the configuration is replaced two hundred times.
 * Nothing here is about what arrives where: it is about the locks, and it fails
 * by timing out rather than by hanging "make check" for ever.
 */
void concurrencyBody() {
    const std::string oneLog = scratchPath("race-one.log");
    const std::string twoLog = scratchPath("race-two.log");

    LogConfig one;
    LogConfig two;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("race-one.conf", "sink.k1.type = file\n"
                                                    "sink.k1.path = " +
                                                        oneLog +
                                                        "\n"
                                                        "logger.race = DEBUG, k1\n"
                                                        "logger.race.b = TRACE, k1\n"),
                         one, errors));
    CHECK(parseLogConfig(writeConf("race-two.conf", "sink.k2.type = file\n"
                                                    "sink.k2.path = " +
                                                        twoLog +
                                                        "\n"
                                                        "logger.race = INFO, k2\n"
                                                        "additivity.race.b = no\n"),
                         two, errors));

    std::atomic<bool> stop(false);
    std::vector<std::thread> threads;

    /* The threads log for as long as the configurations are being replaced, and
     * pause between records so that a great deal of overlap costs a small file */
    for (int which = 0; which < 4; ++which)
        threads.emplace_back([&stop, which]() {
            Logger* const logger = LogManager::get(
                (0 == which % 2) ? std::string("race.b") : "race.c" + std::to_string(which));

            for (int round = 0; round < 100000 && !stop.load(); ++round) {
                emit(logger, INFO, "racing");
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

    for (int round = 0; round < 100; ++round) {
        std::vector<std::string> roundErrors;

        CHECK(LogManager::configure(one, roundErrors));
        CHECK(LogManager::configure(two, roundErrors));
    }

    stop.store(true);

    for (std::thread& thread : threads)
        thread.join();
}

void testConcurrency() {
    std::atomic<bool> done(false);
    std::thread runner([&done]() {
        concurrencyBody();
        done.store(true);
    });

    // A deadlock must fail this test, not hang "make check" for ever
    for (int tenth = 0; tenth < 1200 && !done.load(); ++tenth)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!done.load()) {
        std::cerr << __FILE__ << ": the configuration race did not finish: deadlock?\n";
        std::cerr.flush();
        std::cout.flush();
        std::_Exit(1);
    }

    runner.join();
}

} // namespace

int main() {
    // The console sink the registry starts with would print every record of
    // this test; what the sinks are given is what the test is about
    LogManager::root()->removeSink(LogManager::bootstrapConsoleSink());

    if (!makeScratchDir()) {
        std::cerr << "logger_config: cannot create a scratch directory\n";
        return 1;
    }

    testParseFull();
    testWhitespaceAndCase();
    testErrors();
    testMissingFile();
    testConfigureRoutes();
    testReloadReplacesConfigurationOnly();
    testConfigAdditivityOverridesCode();
    testConfigAdditivityStopsDispatch();
    testBuildBeforeSwap();
    testSinkTypeRegistry();
    testRootIsConfigured();
    testSinkThreshold();
    testLoadFileReportsAndKeeps();
    testBootstrapConsoleGoes();

    /* The bootstrap log file is attached to the root and stays there, so the
     * race below - which logs a great deal - goes first */
    testConcurrency();
    testBootstrapFile();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_config: all checks passed\n";
    return 0;
}
