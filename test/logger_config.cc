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
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
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

/// How many of this sink's records are at this level
std::size_t countAt(const std::shared_ptr<CaptureSink>& capture, Verbosity level) {
    std::size_t at = 0;

    for (const LogRecord& record : capture->records)
        if (record.level == level)
            ++at;

    return at;
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

/// The same text with every line ending written the way a Windows editor writes it
std::string withCrLf(const std::string& body) {
    std::string out;

    for (const char c : body) {
        if ('\n' == c)
            out += '\r';

        out += c;
    }

    return out;
}

/// How many lines of text there are, counting one without a newline of its own
std::size_t countLines(const std::string& text) {
    std::size_t lines = 0;

    for (const char c : text)
        if ('\n' == c)
            ++lines;

    return (text.empty() || '\n' == text[text.size() - 1]) ? lines : lines + 1;
}

/// Every field of two configurations, so that a file's twin really is its twin
void checkSameConfig(const std::string& what, const LogConfig& left, const LogConfig& right) {
    if (left.sinks.size() != right.sinks.size() || left.loggers.size() != right.loggers.size()) {
        ++failures;
        std::cerr << __FILE__ << ": " << what << ": " << left.sinks.size() << '/'
                  << left.loggers.size() << " against " << right.sinks.size() << '/'
                  << right.loggers.size() << '\n';
        return;
    }

    for (std::size_t at = 0; at < left.sinks.size(); ++at) {
        const SinkSpec& a = left.sinks[at];
        const SinkSpec& b = right.sinks[at];

        CHECK_EQ(a.id, b.id);
        CHECK_EQ(a.type, b.type);
        CHECK_EQ(a.path, b.path);
        CHECK_EQ(a.channel, b.channel);
        CHECK(a.json == b.json);
        CHECK(a.level == b.level);
        CHECK(a.colour == b.colour);
        CHECK(a.highlight == b.highlight);
    }

    for (std::size_t at = 0; at < left.loggers.size(); ++at) {
        const LoggerSpec& a = left.loggers[at];
        const LoggerSpec& b = right.loggers[at];

        CHECK_EQ(a.name, b.name);
        CHECK(a.level == b.level);
        CHECK(a.additive == b.additive);
        CHECK(a.sinks == b.sinks);
    }
}

/// The sink that counts what it is given, for the case about the swap
class CountingSink : public LogSink {
  public:
    void emit(const LogRecord&) override { count.fetch_add(1, std::memory_order_relaxed); }

    std::atomic<std::size_t> count{0};
};

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
 * The file that ships in bin/, which is what an installation starts from and
 * the only documentation of this syntax there is: it parses with no complaint
 * at all, and says what it says it says.  A mistake in the example - a key that
 * was renamed, a level that no longer exists - fails here rather than on the
 * first installation that copies it.
 */
void testExampleFile() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(LOGGING_EXAMPLE_CONF, config, errors));

    for (const std::string& error : errors)
        std::cerr << "  " << LOGGING_EXAMPLE_CONF << ": " << error << '\n';

    CHECK(errors.empty());

    // The five sinks it has, sorted by id, and nothing commented out
    CHECK(5 == config.sinks.size());
    if (5 == config.sinks.size()) {
        CHECK_EQ(config.sinks[0].id, "console");
        CHECK_EQ(config.sinks[1].id, "cservice");
        CHECK_EQ(config.sinks[2].id, "debugchan");
        CHECK_EQ(config.sinks[3].id, "debuglog");
        CHECK_EQ(config.sinks[4].id, "main");
    }

    const SinkSpec* const console = findSink(config, "console");
    CHECK(nullptr != console);
    if (nullptr != console)
        CHECK_EQ(console->type, "console");

    // The file the -d and -D switches are about is the human-readable one
    const SinkSpec* const debuglog = findSink(config, "debuglog");
    CHECK(nullptr != debuglog);
    if (nullptr != debuglog) {
        CHECK_EQ(debuglog->type, "file");
        CHECK_EQ(debuglog->path, "debug.log");
        CHECK(!debuglog->json);
    }

    const SinkSpec* const main = findSink(config, "main");
    CHECK(nullptr != main);
    if (nullptr != main) {
        CHECK_EQ(main->type, "file");
        CHECK_EQ(main->path, "gnuworld.log");
        CHECK(main->json);
    }

    // The root, at INFO, writing to all three of them
    const LoggerSpec* const root = findLogger(config, "");
    CHECK(nullptr != root);
    if (nullptr != root) {
        CHECK(root->level && INFO == *root->level);
        CHECK(3 == root->sinks.size());
        if (3 == root->sinks.size()) {
            CHECK_EQ(root->sinks[0], "debuglog");
            CHECK_EQ(root->sinks[1], "console");
            CHECK_EQ(root->sinks[2], "main");
        }
    }

    const LoggerSpec* const legacy = findLogger(config, "legacy");
    CHECK(nullptr != legacy);
    if (nullptr != legacy)
        CHECK(legacy->level && DEBUG == *legacy->level);

    const LoggerSpec* const core = findLogger(config, "core");
    CHECK(nullptr != core);
    if (nullptr != core)
        CHECK(core->level && INFO == *core->level);

    /* The cservice section, which is active and not an example: its own JSON
     * file, the debug channel and the console, and nothing of it walking up to
     * the sinks of the root */
    const SinkSpec* const cservice = findSink(config, "cservice");
    CHECK(nullptr != cservice);
    if (nullptr != cservice) {
        CHECK_EQ(cservice->type, "file");
        CHECK_EQ(cservice->path, "cservice.log");
        CHECK(cservice->json);
    }

    const SinkSpec* const debugchan = findSink(config, "debugchan");
    CHECK(nullptr != debugchan);
    if (nullptr != debugchan) {
        CHECK_EQ(debugchan->type, "irc");
        CHECK_EQ(debugchan->channel, "#coder-com");

        // At INFO, so that a DEBUG record - a SQL statement - never reaches it
        CHECK(INFO == debugchan->level);
    }

    const LoggerSpec* const cserviceLogger = findLogger(config, "cservice");
    CHECK(nullptr != cserviceLogger);
    if (nullptr != cserviceLogger) {
        CHECK(cserviceLogger->level && DEBUG == *cserviceLogger->level);
        CHECK(cserviceLogger->additive && !*cserviceLogger->additive);
        CHECK(3 == cserviceLogger->sinks.size());
        if (3 == cserviceLogger->sinks.size()) {
            CHECK_EQ(cserviceLogger->sinks[0], "cservice");
            CHECK_EQ(cserviceLogger->sinks[1], "debugchan");
            CHECK_EQ(cserviceLogger->sinks[2], "console");
        }
    }

    // The command log, to the file alone: its sentence is noise on a channel
    const LoggerSpec* const commands = findLogger(config, "cservice.commands");
    CHECK(nullptr != commands);
    if (nullptr != commands) {
        CHECK(commands->level && INFO == *commands->level);
        CHECK(commands->additive && !*commands->additive);
        CHECK(1 == commands->sinks.size());
        if (1 == commands->sinks.size())
            CHECK_EQ(commands->sinks[0], "cservice");
    }

    // And the statements, which the file ships commented out
    CHECK(nullptr == findLogger(config, "cservice.sql"));
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

/**
 * A file saved on Windows is the same file: the '\r' of every line ending is
 * not part of a level, of a sink id or of a yes, and a byte order mark in front
 * of the first key is not part of that key either.
 */
void testLineEndingsAndByteOrderMark() {
    const std::string logPath = scratchPath("crlf.log");
    /* The first line is a key line, because that is the line a byte order mark
     * lands on below; a comment of its own comes after it */
    const std::string body = "sink.main.type = file\n"
                             "# Written on Windows\n"
                             "sink.main.path = " +
                             logPath +
                             "\n"
                             "sink.main.format = text\n"
                             "sink.main.level = WARN\n"
                             "sink.con.type = console\n"
                             "sink.con.colour = no\n"
                             "logger.root = INFO, main\n"
                             "logger.crlf.a = DEBUG, main, con\n"
                             "additivity.crlf.a = no\n";

    LogConfig unix;
    LogConfig windows;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("crlf-unix.conf", body), unix, errors));

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    CHECK(parseLogConfig(writeConf("crlf-windows.conf", withCrLf(body)), windows, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    checkSameConfig("crlf", unix, windows);

    // And what the two of them say, so that this is not two wrongs agreeing
    const SinkSpec* const main = findSink(windows, "main");
    CHECK(nullptr != main);
    if (nullptr != main) {
        CHECK_EQ(main->path, logPath);
        CHECK(!main->json);
        CHECK(WARN == main->level);
    }

    const SinkSpec* const console = findSink(windows, "con");
    CHECK(nullptr != console);
    if (nullptr != console)
        CHECK(ConsoleSink::Colour::No == console->colour);

    const LoggerSpec* const a = findLogger(windows, "crlf.a");
    CHECK(nullptr != a);
    if (nullptr != a) {
        CHECK(a->level && DEBUG == *a->level);
        CHECK(2 == a->sinks.size());
        if (2 == a->sinks.size()) {
            CHECK_EQ(a->sinks[0], "main");
            CHECK_EQ(a->sinks[1], "con");
        }
        CHECK(a->additive.has_value() && false == *a->additive);
    }

    /* A byte order mark in front of the first key is dropped, so the file is
     * the file it looks like */
    LogConfig marked;

    CHECK(parseLogConfig(writeConf("bom.conf", "\xEF\xBB\xBF" + withCrLf(body)), marked, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    checkSameConfig("bom", unix, marked);

    /* A mark in front of a comment is one EConfig itself stumbles over, and is
     * then the one thing the errors talk about */
    LogConfig marked_comment;

    CHECK(!parseLogConfig(writeConf("bom-comment.conf", "\xEF\xBB\xBF# a comment\n" + body),
                          marked_comment, errors));
    CHECK(1 == errors.size());

    if (1 == errors.size()) {
        CHECK(contains(errors[0], "byte order mark"));
        CHECK(contains(errors[0], "save it without one"));
    }
}

/**
 * Three things the second look at the parser found: a byte order mark must not
 * hide the line that is really wrong, a sink a logger line asks for is quoted
 * like everything else, and the registry spells the root the way the file does.
 */
void testSecondLook() {
    LogConfig config;
    std::vector<std::string> errors;

    // The mark is there, but the third line is what is wrong: say so
    const std::string marked = std::string("\xEF\xBB\xBF") +
                               "sink.a.type = console\nlogger.x = INFO, a\nnot a config line\n";

    CHECK(!parseLogConfig(writeConf("bom-typo.conf", marked), config, errors));
    CHECK(1 == errors.size());
    CHECK(anyErrorNames(errors, "not a config line"));
    CHECK(anyErrorNames(errors, "line 3"));
    CHECK(!anyErrorNames(errors, "byte order mark"));

    // A mark in front of a comment, and nothing else wrong: the mark is the news
    errors.clear();
    CHECK(!parseLogConfig(writeConf("bom-comment.conf", std::string("\xEF\xBB\xBF") +
                                                            "# a comment\nsink.a.type = console\n"),
                          config, errors));
    CHECK(1 == errors.size());
    CHECK(anyErrorNames(errors, "byte order mark"));

    // An unknown sink is quoted with its control characters written out
    errors.clear();
    CHECK(!parseLogConfig(writeConf("unknown-sink.conf", std::string("logger.x = INFO, ba\x01"
                                                                     "d\n")),
                          config, errors));
    CHECK(anyErrorNames(errors, "unknown sink"));
    CHECK(anyErrorNames(errors, "\\x01"));

    for (const std::string& error : errors)
        for (const char c : error) {
            const unsigned char byte = static_cast<unsigned char>(c);

            CHECK(byte >= 0x20 && 0x7f != byte);
        }

    // The root, however it is spelt, for the registry as for the file
    CHECK(LogManager::normaliseName("root.").empty());
    CHECK(LogManager::normaliseName(".root").empty());
    CHECK(LogManager::normaliseName("..").empty());
    CHECK("a.root" == LogManager::normaliseName("a.root"));
    CHECK("ROOT" == LogManager::normaliseName("ROOT"));
    CHECK(LogManager::root() == LogManager::get("root."));
    CHECK(LogManager::root() == LogManager::get(".root"));

    errors.clear();
    CHECK(parseLogConfig(writeConf("root-dot.conf", "logger.root. = DEBUG\n"), config, errors));
    CHECK(1 == config.loggers.size() && config.loggers[0].name.empty());

    errors.clear();
    CHECK(
        !parseLogConfig(writeConf("root-twice.conf", "logger.root = INFO\nlogger.root. = DEBUG\n"),
                        config, errors));

    errors.clear();
    CHECK(!parseLogConfig(writeConf("dots.conf", "logger... = INFO\n"), config, errors));
    CHECK(anyErrorNames(errors, "empty logger name"));
}

/**
 * An error quotes what the file said, and what the file said may be anything at
 * all: a control character of a value is written as an escape, never passed on
 * into a log line, a terminal or a channel.
 */
void testControlCharactersInErrors() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(!parseLogConfig(writeConf("control.conf", std::string("logger.x = wa\x01rn\n")), config,
                          errors));
    CHECK(!errors.empty());

    bool escaped = false;

    for (const std::string& error : errors) {
        if (contains(error, "\\x01"))
            escaped = true;

        for (const char c : error) {
            const unsigned char byte = static_cast<unsigned char>(c);

            if (byte < 0x20 || 0x7f == byte) {
                ++failures;
                std::cerr << __FILE__ << ": a raw control character in [" << error << "]\n";
                break;
            }
        }
    }

    if (!escaped) {
        ++failures;
        std::cerr << __FILE__ << ": no error writes the control character as \\x01\n";
        for (const std::string& error : errors)
            std::cerr << "    error: " << error << '\n';
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

    /* A line EConfig itself refuses, which is the whole file gone: the line it
     * stumbled over is named, whether what is missing is the value or the key */
    expectError("e-novalue.conf", "sink.m.type = console\nsink.m.level =\n", "line 2");
    expectError("e-nokey.conf", "sink.m.type = console\n= console\n", "line 2");
    expectError("e-equals.conf", "===\n", "line 1");
}

/* ------------------------------------------------------------------ *
 * A file that is not a configuration but an attack on the process
 * ------------------------------------------------------------------ */

/**
 * A name of tens of thousands of segments used to be a name the registry
 * walked down, creating a logger for every prefix of it: memory the square of
 * what the file cost to write, and a std::bad_alloc on its way out of a
 * SIGHUP.  A name has a length and a depth now, and anything beyond either is
 * an ordinary parse error - which is what a broken file has always been.
 */
void testMonstrousNames() {
    const std::string longName(300, 'a');

    expectError("m-longname.conf", "logger." + longName + " = INFO\n", "logger.");
    expectError("m-longadd.conf", "additivity." + longName + " = yes\n", "additivity.");

    // What a message says about such a key is a message, not the key again
    {
        LogConfig config;
        std::vector<std::string> errors;

        CHECK(!parseLogConfig(writeConf("m-longname2.conf", "logger." + longName + " = INFO\n"),
                              config, errors));
        CHECK(1 == errors.size());

        for (const std::string& error : errors)
            CHECK(error.size() < 200);
    }

    // Sixteen segments is a hierarchy; seventeen is somebody trying it on
    std::string deep = "s1";
    for (int at = 2; at <= 16; ++at)
        deep += ".s" + std::to_string(at);

    {
        LogConfig config;
        std::vector<std::string> errors;

        CHECK(parseLogConfig(writeConf("m-deep16.conf", "logger." + deep + " = INFO\n"), config,
                             errors));
        CHECK(errors.empty());
        CHECK(nullptr != findLogger(config, deep));
    }

    expectError("m-deep17.conf", "logger." + deep + ".s17 = INFO\n", "logger.");

    // The empty segments are not segments, here as everywhere else
    {
        LogConfig config;
        std::vector<std::string> errors;

        CHECK(parseLogConfig(
            writeConf("m-dots.conf", "logger." + std::string(60, '.') + deep + " = INFO\n"), config,
            errors));
        CHECK(errors.empty());
    }

    // And a sink id nobody would type
    expectError("m-longsink.conf", "sink." + std::string(65, 'b') + ".type = console\n", "sink.");

    {
        LogConfig config;
        std::vector<std::string> errors;

        CHECK(parseLogConfig(
            writeConf("m-sink64.conf", "sink." + std::string(64, 'b') + ".type = console\n"),
            config, errors));
        CHECK(errors.empty());
    }
}

/**
 * A line naming a thousand sinks used to be a thousand errors, each of them a
 * string: the report of what is wrong with a file is capped, and says how much
 * of it is not being shown.
 */
void testErrorsAreCapped() {
    std::string line = "logger.capped = INFO";

    for (int at = 0; at < 1000; ++at)
        line += ", nosuch" + std::to_string(at);

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(!parseLogConfig(writeConf("m-capped.conf", line + "\n"), config, errors));
    CHECK(errors.size() <= 20);
    CHECK(!errors.empty());

    if (errors.empty())
        return;

    CHECK(contains(errors.back(), "more"));
    CHECK(contains(errors.back(), "981"));
}

/**
 * None of it stops the process or takes its logging away: loadFile() says no,
 * says why on core.config, and what was in force is still in force.
 */
void testMonstrousFilesKeepTheConfiguration() {
    const std::string keepLog = scratchPath("monstrous.log");

    LogConfig working;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("m-working.conf", "sink.m1.type = file\n"
                                                     "sink.m1.path = " +
                                                         keepLog +
                                                         "\n"
                                                         "logger.cfg20.a = INFO, m1\n"),
                         working, errors));
    CHECK(LogManager::configure(working, errors));

    emit(LogManager::get("cfg20.a"), INFO, "before");
    CHECK(1 == readFileLines(keepLog).size());

    const std::shared_ptr<CaptureSink> reports = std::make_shared<CaptureSink>();
    LogManager::get("core.config")->addSink(reports, TRACE);

    std::string deep = "d1";
    for (int at = 2; at <= 17; ++at)
        deep += ".d" + std::to_string(at);

    std::string capped = "logger.cfg20.b = INFO";
    for (int at = 0; at < 1000; ++at)
        capped += ", nosuch" + std::to_string(at);

    const std::string files[4] = {
        writeConf("m-l1.conf", "logger." + std::string(300, 'a') + " = INFO\n"),
        writeConf("m-l2.conf", "logger." + deep + " = INFO\n"),
        writeConf("m-l3.conf", "sink." + std::string(65, 'b') + ".type = console\n"),
        writeConf("m-l4.conf", capped + "\n")};

    std::size_t written = 1;

    for (const std::string& path : files) {
        reports->clear();

        CHECK(!LogManager::loadFile(path));
        CHECK(!reports->records.empty());
        CHECK(reports->size() <= 20);

        // The routing of the configuration in force is untouched
        ++written;
        emit(LogManager::get("cfg20.a"), INFO, "still here");
        CHECK(written == readFileLines(keepLog).size());
    }

    LogManager::get("core.config")->removeSink(reports);
}

/**
 * The name of a logger line is the name the registry knows, and it is that name
 * before the file is looked at for duplicates: "logger.x" and "logger..x" are
 * two lines about one logger, which is an error, and a name that is nothing but
 * empty segments is no name at all.  The case of a name is part of it.
 */
void testLoggerNames() {
    LogConfig config;
    std::vector<std::string> errors;

    // The empty segments are not segments, on either kind of line
    CHECK(parseLogConfig(writeConf("names.conf", "logger.a..b = INFO\n"
                                                 "additivity.x. = no\n"),
                         config, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    const LoggerSpec* const b = findLogger(config, "a.b");
    CHECK(nullptr != b);
    if (nullptr != b)
        CHECK(b->level && INFO == *b->level);

    const LoggerSpec* const x = findLogger(config, "x");
    CHECK(nullptr != x);
    if (nullptr != x) {
        CHECK(!x->level.has_value());
        CHECK(x->additive.has_value() && false == *x->additive);
    }

    // The root is written "root"; "ROOT" is a logger of that name and no more
    LogConfig roots;

    CHECK(parseLogConfig(writeConf("roots.conf", "logger.root = INFO\n"
                                                 "logger.ROOT = DEBUG\n"),
                         roots, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';

    CHECK(2 == roots.loggers.size());

    const LoggerSpec* const theRoot = findLogger(roots, "");
    CHECK(nullptr != theRoot);
    if (nullptr != theRoot)
        CHECK(theRoot->level && INFO == *theRoot->level);

    const LoggerSpec* const shouted = findLogger(roots, "ROOT");
    CHECK(nullptr != shouted);
    if (nullptr != shouted)
        CHECK(shouted->level && DEBUG == *shouted->level);

    // Two lines about one logger, however they are spelled
    expectError("names-dup.conf", "logger.x = INFO\nlogger..x = DEBUG\n", "logger.");
    expectError("names-dupadd.conf", "additivity.x = yes\nadditivity..x. = no\n", "additivity.");

    // And a name that is nothing but the dots
    expectError("names-empty.conf", "logger.. = INFO\n", "empty logger name");
    expectError("names-emptyadd.conf", "additivity... = yes\n", "empty logger name");
}

/**
 * Two file sinks on one file would be two streams appending to it, each with a
 * lock of its own: a record of one could land in the middle of a record of the
 * other.  The file is compared as it was written, and nothing more.
 */
void testDuplicateFileSinkPaths() {
    const std::string logPath = scratchPath("shared.log");

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(!parseLogConfig(writeConf("shared.conf", "sink.a.type = file\n"
                                                   "sink.a.path = " +
                                                       logPath +
                                                       "\n"
                                                       "sink.b.type = file\n"
                                                       "sink.b.path = " +
                                                       logPath + "\n"),
                          config, errors));

    bool named = false;

    for (const std::string& error : errors)
        if (contains(error, "sink.a.path") && contains(error, "sink.b.path"))
            named = true;

    if (!named) {
        ++failures;
        std::cerr << __FILE__ << ": no error names both sink.a.path and sink.b.path\n";
        for (const std::string& error : errors)
            std::cerr << "    error: " << error << '\n';
    }

    /* Two spellings of one file are two files here: resolving a path is the
     * business of whoever opens it, not of the parser */
    LogConfig spelled;

    CHECK(parseLogConfig(writeConf("spelled.conf", "sink.a.type = file\n"
                                                   "sink.a.path = " +
                                                       logPath +
                                                       "\n"
                                                       "sink.b.type = file\n"
                                                       "sink.b.path = ./" +
                                                       logPath + "\n"),
                         spelled, errors));
    CHECK(errors.empty());

    for (const std::string& error : errors)
        std::cerr << "  unexpected error: " << error << '\n';
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

/* ------------------------------------------------------------------ *
 * The settings of a kind of sink the parser does not know
 * ------------------------------------------------------------------ */

/// A recognisable token, which no error message may ever quote
const std::string secretToken("SECRET-aTokenNobodyMayQuote-0123456789");

/// The value of this option of this sink, empty when there is none
std::string option(const LogConfig& config, const std::string& id, const std::string& key) {
    const SinkSpec* const spec = findSink(config, id);

    if (nullptr == spec)
        return std::string();

    const std::map<std::string, std::string>::const_iterator known = spec->options.find(key);

    return spec->options.end() == known ? std::string() : known->second;
}

/**
 * A type the parser knows nothing about carries settings the parser knows
 * nothing about: they are collected, keyed in lower case and trimmed, for
 * whatever factory the type is registered by to make sense of.  The common
 * settings are still the parser's own and are not among them.
 */
void testOptionsOfAnUnknownType() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("options.conf", "sink.p.type = pushover\n"
                                                   "sink.p.TOKEN =   " +
                                                       secretToken +
                                                       "   \n"
                                                       "sink.p.userkey = key1, key2\n"
                                                       "sink.p.rate = 5/min\n"
                                                       "sink.p.url = http://127.0.0.1:1/x\n"
                                                       "sink.p.level = WARN\n"
                                                       "sink.p.highlight = no\n"
                                                       "logger.cfg20.a = INFO, p\n"),
                         config, errors));

    for (const std::string& error : errors)
        std::cerr << "  options.conf: " << error << '\n';

    CHECK(errors.empty());

    const SinkSpec* const spec = findSink(config, "p");

    CHECK(nullptr != spec);

    if (nullptr != spec) {
        CHECK_EQ(spec->type, "pushover");

        // The common settings are read as they are for every other kind
        CHECK(WARN == spec->level);
        CHECK(!spec->highlight);

        // And are not among the options the factory is handed
        CHECK(spec->options.end() == spec->options.find("level"));
        CHECK(spec->options.end() == spec->options.find("highlight"));
        CHECK(spec->options.end() == spec->options.find("type"));

        CHECK(4 == spec->options.size());
    }

    // The key is lower-cased, the value trimmed and otherwise left alone
    CHECK_EQ(option(config, "p", "token"), secretToken);
    CHECK_EQ(option(config, "p", "userkey"), "key1, key2");
    CHECK_EQ(option(config, "p", "rate"), "5/min");
    CHECK_EQ(option(config, "p", "url"), "http://127.0.0.1:1/x");
}

/**
 * The kinds of sink the parser does know are unchanged: a setting none of them
 * has is the parse error it always was, and the whole file goes with it.
 */
void testOptionsAreNotForBuiltInTypes() {
    const char* const builtIn[] = {"file", "console", "irc"};

    for (const char* const type : builtIn) {
        LogConfig config;
        std::vector<std::string> errors;

        const std::string body = std::string("sink.b.type = ") + type +
                                 "\n"
                                 "sink.b.path = " +
                                 scratchPath("built-in.log") +
                                 "\n"
                                 "sink.b.channel = #chan\n"
                                 "sink.b.token = " +
                                 secretToken +
                                 "\n"
                                 "sink.b.userkey = key1\n"
                                 "sink.b.url = http://127.0.0.1:1/x\n";

        CHECK(!parseLogConfig(writeConf("builtin-options.conf", body), config, errors));
        CHECK(anyErrorNames(errors, "sink.b.token"));
        CHECK(anyErrorNames(errors, "sink.b.userkey"));
        CHECK(anyErrorNames(errors, "sink.b.url"));

        // Whatever is said about them, the token itself is not in it
        for (const std::string& error : errors)
            CHECK(!contains(error, secretToken));
    }
}

/// One option written twice is the duplicate it would be for any other setting
void testDuplicateOptionKey() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(!parseLogConfig(writeConf("dup-option.conf", "sink.p.type = pushover\n"
                                                       "sink.p.token = " +
                                                           secretToken +
                                                           "\n"
                                                           "sink.p.TOKEN = " +
                                                           secretToken +
                                                           "-other\n"
                                                           "logger.cfg20.b = INFO, p\n"),
                          config, errors));

    CHECK(anyErrorNames(errors, "given twice"));

    for (const std::string& error : errors)
        CHECK(!contains(error, secretToken));
}

/**
 * Nothing about an option's VALUE is ever quoted back: a value may be a token,
 * and an error message about one is read by whoever can read the log, which is
 * not always whoever may hold the token.
 */
void testNoErrorQuotesAnOptionValue() {
    const std::string paths[] = {
        // The type comes after the options, which is where they are collected
        "sink.p.token = " + secretToken +
            "\nsink.p.type = pushover\nlogger.cfg20.c = nonsense, p\n",
        /* A type nothing registered parses: it is the factory's business, not
         * this parser's, and what it says about the token is asserted where
         * that error is made (see testUnregisteredTypeChangesNothing) */
        // No type at all: the sink is the error, the options are beside it
        "sink.p.token = " + secretToken + "\nsink.p.userkey = " + secretToken + "\n",
        // A sink id that is not one, carrying a token
        "sink.not a sink.token = " + secretToken + "\n",
        // A built-in kind, for which the option is an unknown setting
        "sink.p.type = console\nsink.p.token = " + secretToken + "\n",
        // The whole file is refused for a reason elsewhere, options and all
        "sink.p.type = pushover\nsink.p.token = " + secretToken +
            "\nsink.f.type = file\nlogger.cfg20.c = INFO, nosuchsink\n",
    };

    for (const std::string& body : paths) {
        LogConfig config;
        std::vector<std::string> errors;

        CHECK(!parseLogConfig(writeConf("secret.conf", body), config, errors));
        CHECK(!errors.empty());

        for (const std::string& error : errors)
            if (contains(error, secretToken)) {
                ++failures;
                std::cerr << __FILE__ << ':' << __LINE__
                          << ": failed: an error quoted the token: " << error << '\n';
            }
    }
}

/**
 * "rate" is a setting of an irc sink and of no other built-in kind, and what it
 * says has to be a rate.
 */
void testIrcRateSetting() {
    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("irc-rate.conf", "sink.c.type = irc\n"
                                                    "sink.c.channel = #chan\n"
                                                    "sink.c.rate = 5/min\n"
                                                    "logger.cfg21.a = INFO, c\n"),
                         config, errors));
    CHECK(errors.empty());

    const SinkSpec* const spec = findSink(config, "c");

    CHECK(nullptr != spec);

    if (nullptr != spec) {
        CHECK_EQ(spec->rate, "5/min");

        // A setting of the parser's own, not one handed to a factory
        CHECK(spec->options.empty());
    }

    // Written in any case and with any spacing, like every other value
    LogConfig spaced;

    CHECK(parseLogConfig(writeConf("irc-rate-spaced.conf", "sink.c.type = IRC\n"
                                                           "sink.c.channel = #chan\n"
                                                           "sink.c.RATE = 2 / Hour\n"),
                         spaced, errors));
    CHECK_EQ(findSink(spaced, "c")->rate, "2 / Hour");

    // A rate that is not one is a parse error naming the key
    const char* const refused[] = {"0/min", "10/day", "nonsense", "10"};

    for (const char* const value : refused) {
        LogConfig bad;

        CHECK(!parseLogConfig(writeConf("irc-rate-bad.conf", std::string("sink.c.type = irc\n"
                                                                         "sink.c.channel = #chan\n"
                                                                         "sink.c.rate = ") +
                                                                 value + "\n"),
                              bad, errors));
        CHECK(anyErrorNames(errors, "sink.c.rate"));
    }

    // And it is an irc setting: a file or a console sink has no rate
    const char* const others[] = {"file", "console"};

    for (const char* const type : others) {
        LogConfig bad;

        CHECK(!parseLogConfig(writeConf("other-rate.conf", std::string("sink.c.type = ") + type +
                                                               "\n"
                                                               "sink.c.path = " +
                                                               scratchPath("other-rate.log") +
                                                               "\n"
                                                               "sink.c.rate = 5/min\n"),
                              bad, errors));
        CHECK(anyErrorNames(errors, "sink.c.rate"));
    }

    // No rate at all is the default, and means no limit
    LogConfig none;

    CHECK(parseLogConfig(writeConf("no-rate.conf", "sink.c.type = irc\n"
                                                   "sink.c.channel = #chan\n"),
                         none, errors));
    CHECK(findSink(none, "c")->rate.empty());
}

/**
 * A configuration naming a kind of sink nothing registered - a pushover sink in
 * a build without one - is refused as a whole, names the type line, and changes
 * nothing about the logging in force.
 */
void testUnregisteredTypeChangesNothing() {
    const std::shared_ptr<CaptureSink> capture = std::make_shared<CaptureSink>();

    Logger* const logger = LogManager::get("cfg22.a");

    logger->addSink(capture, TRACE);

    LogConfig before;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("before-push.conf", "sink.f.type = file\n"
                                                       "sink.f.path = " +
                                                           scratchPath("cfg22.log") +
                                                           "\n"
                                                           "logger.cfg22.a = INFO, f\n"),
                         before, errors));
    CHECK(LogManager::configure(before, errors));

    emit(logger, INFO, "before");

    CHECK(1 == capture->size());
    CHECK(1 == readFileLines(scratchPath("cfg22.log")).size());

    // No factory is registered for this type in this program
    LogConfig config;

    CHECK(parseLogConfig(writeConf("push-nofactory.conf", "sink.page.type = pushover\n"
                                                          "sink.page.token = " +
                                                              secretToken +
                                                              "\n"
                                                              "sink.page.userkey = key1\n"
                                                              "logger.cfg22.a = INFO, page\n"),
                         config, errors));

    CHECK(!LogManager::configure(config, errors));
    CHECK(anyErrorNames(errors, "sink.page.type"));

    for (const std::string& error : errors)
        CHECK(!contains(error, secretToken));

    // And what was in force is still in force, sink for sink
    emit(logger, INFO, "after");

    CHECK(2 == capture->size());
    CHECK(2 == readFileLines(scratchPath("cfg22.log")).size());

    logger->removeSink(capture);
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
 * A KIND OF SINK SAYS WHAT LEVEL IT WANTS WHERE THE FILE SAYS NOTHING.
 *
 * LogSink::defaultThreshold() is TRACE, so a file sink with no "level" of its own
 * still hears every record its logger sends.  A kind of sink for which that is
 * nonsense - a pager - overrides it, and configure() attaches such a sink at that
 * level instead.  A level the FILE gave always wins, in either direction.
 */
void testASinkTypeSaysItsOwnDefaultLevel() {
    /// A kind of sink that wants warnings and above, like a pager
    class WarnSink : public LogSink {
      public:
        void emit(const LogRecord&) override { ++count; }

        Verbosity defaultThreshold() const override { return WARN; }

        std::size_t count = 0;
    };

    const std::shared_ptr<WarnSink> pager = std::make_shared<WarnSink>();

    LogManager::registerSinkType(
        "wants-warn",
        [pager](const SinkSpec&, std::string&) -> std::shared_ptr<LogSink> { return pager; });

    LogConfig config;
    std::vector<std::string> errors;

    // No "sink.p1.level" at all: the kind of sink is what decides
    CHECK(parseLogConfig(writeConf("defaultlevel.conf", "sink.p1.type = wants-warn\n"
                                                        "logger.cfg30.a = TRACE, p1\n"),
                         config, errors));
    CHECK(LogManager::configure(config, errors));

    emit(LogManager::get("cfg30.a"), INFO, "chatter, which a pager does not want");
    CHECK(0 == pager->count);

    emit(LogManager::get("cfg30.a"), WARN, "trouble, which it does");
    CHECK(1 == pager->count);

    // And a level the file DID give wins, even below the kind's own default
    LogConfig asked;

    CHECK(parseLogConfig(writeConf("askedlevel.conf", "sink.p1.type = wants-warn\n"
                                                      "sink.p1.level = DEBUG\n"
                                                      "logger.cfg30.a = TRACE, p1\n"),
                         asked, errors));
    CHECK(LogManager::configure(asked, errors));

    emit(LogManager::get("cfg30.a"), DEBUG, "which the file asked for");
    CHECK(2 == pager->count);

    // The kinds that say nothing are unchanged: a file sink with no level is TRACE
    const std::string logPath = scratchPath("defaultlevel.log");

    LogConfig plain;

    CHECK(parseLogConfig(writeConf("plainlevel.conf", "sink.f1.type = file\n"
                                                      "sink.f1.path = " +
                                                          logPath +
                                                          "\n"
                                                          "logger.cfg30.b = TRACE, f1\n"),
                         plain, errors));
    CHECK(LogManager::configure(plain, errors));

    emit(LogManager::get("cfg30.b"), TRACE, "the quietest record there is");
    CHECK(1 == readFileLines(logPath).size());
}

/**
 * A factory error that already names a key of this sink is quoted as it stands,
 * and one that names a DIFFERENT sink whose id merely begins with this one's is
 * not: the delimiter has to be there, or an id of "a" would swallow an error
 * about "sink.ab.token".
 */
void testAFactoryErrorNamingAnotherSink() {
    LogManager::registerSinkType(
        "names-own-key", [](const SinkSpec& spec, std::string& error) -> std::shared_ptr<LogSink> {
            error = "sink." + spec.id + ".token: a made-up complaint";

            return nullptr;
        });

    LogManager::registerSinkType(
        "names-another-sink", [](const SinkSpec&, std::string& error) -> std::shared_ptr<LogSink> {
            /* "sink.ab..." while this sink's id is "a": the text begins with
             * "sink.a" and is about something else entirely */
            error = "sink.ab.token: a complaint about another sink";

            return nullptr;
        });

    LogConfig own;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("ownkey.conf", "sink.a.type = names-own-key\n"
                                                  "logger.cfg31.a = INFO, a\n"),
                         own, errors));

    CHECK(!LogManager::configure(own, errors));
    CHECK(1 == errors.size());

    if (!errors.empty())
        CHECK_EQ(errors[0], std::string("sink.a.token: a made-up complaint"));

    // The other one is prefixed, because it is not about "sink.a." at all
    LogConfig other;

    CHECK(parseLogConfig(writeConf("othersink.conf", "sink.a.type = names-another-sink\n"
                                                     "logger.cfg31.a = INFO, a\n"),
                         other, errors));

    CHECK(!LogManager::configure(other, errors));
    CHECK(1 == errors.size());

    if (!errors.empty())
        CHECK_EQ(errors[0], std::string("sink.a: sink.ab.token: a complaint about another sink"));
}

/**
 * A FILE HOLDING A TOKEN AND READABLE BY SOMEBODY ELSE IS SAID ONCE, AT WARN.
 *
 * READABLE is the whole of it: 0640 warns, because a group can read the token;
 * 0600 says nothing, which is the normal case; and 0610 says nothing either -
 * a group that may write the file and not read it is not this problem, whatever
 * else it is.  A file with no token in it is nobody's business either way.
 */
void testASecretReadableByOthers() {
    const std::shared_ptr<CaptureSink> warnings = std::make_shared<CaptureSink>();

    LogManager::get("core.config")->addSink(warnings, TRACE);

    const std::string withToken(writeConf("secret-mode.conf", "sink.p.type = pushover\n"
                                                              "sink.p.token = " +
                                                                  secretToken +
                                                                  "\n"
                                                                  "sink.p.userkey = key1\n"));

    LogConfig config;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(withToken, config, errors));

    struct ModeCase {
        mode_t mode;
        bool warns;
    };

    const ModeCase cases[] = {{0640, true}, {0600, false}, {0610, false}, {0604, true}};

    for (const ModeCase& one : cases) {
        CHECK(0 == ::chmod(withToken.c_str(), one.mode));

        warnings->clear();

        LogManager::warnIfSecretsAreReadable(withToken, config);

        const std::size_t said = countAt(warnings, WARN);

        if (said != (one.warns ? std::size_t(1) : std::size_t(0))) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: mode 0" << std::oct << one.mode
                      << std::dec << " said " << said << " warning(s), wanted "
                      << (one.warns ? 1 : 0) << '\n';
        }

        // And whatever it said, it never said the token
        for (const LogRecord& record : warnings->records)
            CHECK(!contains(record.message, secretToken));
    }

    // A file with no token at all is nobody's business, whatever its mode
    const std::string noToken(writeConf("no-secret-mode.conf", "sink.c.type = console\n"));

    LogConfig plain;

    CHECK(parseLogConfig(noToken, plain, errors));
    CHECK(0 == ::chmod(noToken.c_str(), 0644));

    warnings->clear();

    LogManager::warnIfSecretsAreReadable(noToken, plain);

    CHECK(0 == warnings->size());

    LogManager::get("core.config")->removeSink(warnings);
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

/**
 * A record logged while a configuration is being applied sees the logger as it
 * was or as it is to be, and never as neither of the two: one thread logs on
 * "atomic.x" without pause while the configuration is replaced two hundred
 * times, and every record it logged arrives, once, at one of the two sinks.
 */
void atomicityBody() {
    const std::shared_ptr<CountingSink> first = std::make_shared<CountingSink>();
    const std::shared_ptr<CountingSink> second = std::make_shared<CountingSink>();

    LogManager::registerSinkType(
        "count-one",
        [first](const SinkSpec&, std::string&) -> std::shared_ptr<LogSink> { return first; });
    LogManager::registerSinkType(
        "count-two",
        [second](const SinkSpec&, std::string&) -> std::shared_ptr<LogSink> { return second; });

    LogConfig one;
    LogConfig two;
    std::vector<std::string> errors;

    CHECK(parseLogConfig(writeConf("atomic-one.conf", "sink.a1.type = count-one\n"
                                                      "logger.atomic.x = DEBUG, a1\n"),
                         one, errors));
    CHECK(parseLogConfig(writeConf("atomic-two.conf", "sink.a2.type = count-two\n"
                                                      "logger.atomic.x = DEBUG, a2\n"),
                         two, errors));
    CHECK(errors.empty());

    // The first configuration is in force before the first record is logged
    CHECK(LogManager::configure(one, errors));

    std::atomic<bool> stop(false);
    std::atomic<std::size_t> logged(0);

    std::thread writer([&stop, &logged]() {
        Logger* const logger = LogManager::get("atomic.x");

        while (!stop.load(std::memory_order_relaxed)) {
            emit(logger, DEBUG, "atomic");
            logged.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The writer is logging before the first swap: on a busy machine it may
    // otherwise not have been scheduled at all by the time the last one is done
    while (0 == logged.load(std::memory_order_relaxed))
        std::this_thread::yield();

    for (int round = 0; round < 200; ++round) {
        std::vector<std::string> roundErrors;

        CHECK(LogManager::configure(two, roundErrors));
        CHECK(LogManager::configure(one, roundErrors));
    }

    stop.store(true);
    writer.join();

    const std::size_t attempted = logged.load();
    const std::size_t arrived = first->count.load() + second->count.load();

    CHECK(attempted > 0);

    if (arrived != attempted) {
        ++failures;
        std::cerr << __FILE__ << ": " << attempted << " record(s) logged, " << arrived
                  << " arrived (" << first->count.load() << " + " << second->count.load() << ")\n";
    }
}

void testSwapAtomicity() {
    std::atomic<bool> done(false);
    std::thread runner([&done]() {
        atomicityBody();
        done.store(true);
    });

    // As above: a lock that is never let go of fails this, it does not hang
    for (int tenth = 0; tenth < 1200 && !done.load(); ++tenth)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!done.load()) {
        std::cerr << __FILE__ << ": the atomic swap did not finish: deadlock?\n";
        std::cerr.flush();
        std::cout.flush();
        std::_Exit(1);
    }

    runner.join();
}

/**
 * A start-up log file that will not open says so, once, on stderr: a process
 * with no logging.conf would otherwise lose every record it ever logs without a
 * word.  The sink is attached all the same - a SIGHUP reopens it - and logging
 * through a dead sink is no trouble at all.
 */
void testBootstrapFileCannotOpen() {
    const std::string path = scratchPath("no/such/dir/x.log");

    std::ostringstream captured;
    std::streambuf* const saved = std::cerr.rdbuf(captured.rdbuf());

    LogManager::bootstrapFile(path);

    const std::string complained = captured.str();

    // The same path again is the same sink, and says nothing more
    LogManager::bootstrapFile(path);

    const std::string afterwards = captured.str();

    std::cerr.rdbuf(saved);

    CHECK(contains(complained, "cannot open log file"));
    CHECK(contains(complained, path));
    CHECK(1 == countLines(complained));
    CHECK_EQ(afterwards, complained);

    // And the dead sink is a sink like any other to whatever logs next
    emit(LogManager::root(), INFO, "through a dead sink");
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
    testExampleFile();
    testWhitespaceAndCase();
    testLineEndingsAndByteOrderMark();
    testControlCharactersInErrors();
    testSecondLook();
    testErrors();
    testMonstrousNames();
    testErrorsAreCapped();
    testMonstrousFilesKeepTheConfiguration();
    testLoggerNames();
    testDuplicateFileSinkPaths();
    testMissingFile();
    testConfigureRoutes();
    testReloadReplacesConfigurationOnly();
    testConfigAdditivityOverridesCode();
    testConfigAdditivityStopsDispatch();
    testBuildBeforeSwap();
    testOptionsOfAnUnknownType();
    testOptionsAreNotForBuiltInTypes();
    testDuplicateOptionKey();
    testNoErrorQuotesAnOptionValue();
    testIrcRateSetting();
    testUnregisteredTypeChangesNothing();
    testSinkTypeRegistry();
    testRootIsConfigured();
    testSinkThreshold();
    testASinkTypeSaysItsOwnDefaultLevel();
    testAFactoryErrorNamingAnotherSink();
    testASecretReadableByOthers();
    testLoadFileReportsAndKeeps();
    testBootstrapConsoleGoes();

    /* The bootstrap log files are attached to the root and stay there, so the
     * two cases below - which log a great deal - go first */
    testConcurrency();
    testSwapAtomicity();
    testBootstrapFile();
    testBootstrapFileCannotOpen();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_config: all checks passed\n";
    return 0;
}
