/**
 * LogManager.cc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 *
 */
#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "LogConfig.h"
#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"

#include "LogManager.h"

namespace gnuworld {

using std::string;

namespace {

/// The longest a logger name column ever gets, however long the name is
const std::size_t maximumNameWidth = 20;

/// The width the name of the root takes, which it prints as
const std::size_t rootNameWidth = 4;

/**
 * The most errors a report about one configuration carries.  A configuration
 * built out of a file that names ten thousand sinks of a kind this build has
 * not got is ten thousand errors otherwise, and nobody reads past the first few
 * of them.  parseLogConfig() caps what it returns in the same way.
 */
const std::size_t maximumErrors = 20;

/// The errors, no more than maximumErrors of them, the last saying how many of
/// them are not being shown
void capErrors(std::vector<string>& errors) {
    if (errors.size() <= maximumErrors)
        return;

    const std::size_t hidden = errors.size() - (maximumErrors - 1);

    errors.resize(maximumErrors - 1);
    errors.push_back("\xE2\x80\xA6 and " + std::to_string(hidden) + " more");
}

/// A type name as the registry of the sink kinds keys it: without its case
string lowerType(const string& type) {
    string folded;
    folded.reserve(type.size());

    for (const char c : type)
        folded += static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);

    return folded;
}

/**
 * The kind of sink that writes lines to a file, which is the one every
 * configuration is likely to name.  A file that will not open is the reason the
 * whole configuration is refused: a log nobody can read is not a log.
 */
std::shared_ptr<LogSink> makeFileSink(const SinkSpec& spec, string& error) {
    const std::shared_ptr<FileSink> sink = std::make_shared<FileSink>(spec.path, spec.json);

    if (!sink->isOpen()) {
        error = "cannot open " + spec.path;

        return nullptr;
    }

    return sink;
}

/// The kind of sink that writes to the terminal the process was started from
std::shared_ptr<LogSink> makeConsoleSink(const SinkSpec& spec, string&) {
    return std::make_shared<ConsoleSink>(spec.colour, spec.highlight);
}

/**
 * What a configuration is to make of one logger: the level and the additivity it
 * asks for, and the sinks it sends the logger's records to, all of it worked out
 * before any logger is touched.  A logger no spec names gets one of these empty.
 */
struct LoggerChange {
    std::optional<Verbosity> level;
    std::optional<bool> additive;
    std::vector<std::pair<std::shared_ptr<LogSink>, Verbosity>> sinks;
};

} // namespace

/**
 * The registry: the loggers by name, the root among them under the empty name,
 * the console sink the root starts with, and the module that is being loaded.
 */
struct LogManager::State {
    std::mutex lock;
    std::map<string, Logger*> loggers;
    Logger* rootLogger = nullptr;
    std::shared_ptr<LogSink> bootstrapConsole;
    string loadingModule;

    /// The kinds of sink a configuration may name, by type name without its case
    std::map<string, SinkFactory> sinkFactories;

    /// The loggers the configuration in force has a "logger.<name>" line for
    std::set<string> configuredLoggers;

    /// The paths bootstrapFile() has already given the root a log file for
    std::set<string> bootstrapFiles;
};

/**
 * Allocates the registry with the root logger in it.  Called once, from the
 * function-local static of state(), and what it returns is never freed.
 */
LogManager::State* LogManager::createState() {
    State* const fresh = new State();

    fresh->rootLogger = new Logger(string(), nullptr);
    fresh->loggers[string()] = fresh->rootLogger;

    // Until a configuration file says where records go, they go to the console:
    // a message logged while the process starts up has to be seen somewhere
    fresh->bootstrapConsole = std::make_shared<ConsoleSink>(ConsoleSink::Colour::Auto, true);
    fresh->rootLogger->addSink(fresh->bootstrapConsole, TRACE);

    /* The two kinds of sink that need nothing but the standard library are known
     * from the outset.  A channel sink is core's to register, because only core
     * knows what a channel is, and a notifier registers its own */
    fresh->sinkFactories[string("file")] = &makeFileSink;
    fresh->sinkFactories[string("console")] = &makeConsoleSink;

    LogSinks::setNameWidth(rootNameWidth);

    return fresh;
}

/**
 * The one registry of the process.  It is allocated on first use and never
 * destroyed: a static destructor or an atexit handler may still log, and would
 * find a destroyed registry where this one is simply still there.
 */
LogManager::State& LogManager::state() {
    static State* const singleton = createState();

    return *singleton;
}

/**
 * The name without its empty segments, and with "root" for the root: "a..b",
 * ".a.b" and "a.b." all name the logger "a.b"; "", "root" and "root." the root.
 */
string LogManager::normaliseName(const string& name) {
    string normalised;
    string::size_type at = 0;

    while (at <= name.size()) {
        const string::size_type dot = name.find('.', at);
        const string::size_type end = string::npos == dot ? name.size() : dot;

        if (end > at) {
            if (!normalised.empty())
                normalised += '.';

            normalised.append(name, at, end - at);
        }

        if (string::npos == dot)
            break;

        at = dot + 1;
    }

    // Asked only once the empty segments are gone, so that "root." and ".root"
    // are the root as well: a name means one logger however it is spelt
    if ("root" == normalised)
        normalised.clear();

    return normalised;
}

/**
 * Tells the text sinks how wide their name column has to be: the longest name
 * there is, at most twenty characters, and never less than the four the root
 * prints as.
 */
void LogManager::updateNameWidthLocked(State& registry) {
    std::size_t longest = rootNameWidth;

    for (const std::pair<const string, Logger*>& entry : registry.loggers)
        longest = std::max(longest, entry.first.empty() ? rootNameWidth : entry.first.size());

    LogSinks::setNameWidth(std::min<std::size_t>(longest, maximumNameWidth));
}

/**
 * The logger of this already normalised name, with every ancestor of it that was
 * missing created on the way down.  The registry's lock is held.
 */
Logger* LogManager::getLocked(State& registry, const string& name) {
    const std::map<string, Logger*>::const_iterator known = registry.loggers.find(name);

    if (registry.loggers.end() != known)
        return known->second;

    Logger* parent = registry.rootLogger;
    string prefix;
    string::size_type at = 0;

    while (at < name.size()) {
        const string::size_type dot = name.find('.', at);
        const string::size_type end = string::npos == dot ? name.size() : dot;

        if (!prefix.empty())
            prefix += '.';

        prefix.append(name, at, end - at);

        std::map<string, Logger*>::iterator entry = registry.loggers.find(prefix);

        if (registry.loggers.end() == entry)
            entry = registry.loggers.emplace(prefix, new Logger(prefix, parent)).first;

        parent = entry->second;

        if (string::npos == dot)
            break;

        at = dot + 1;
    }

    updateNameWidthLocked(registry);

    return parent;
}

Logger* LogManager::get(const string& name) {
    State& registry = state();
    const string normalised = normaliseName(name);

    const std::lock_guard<std::mutex> guard(registry.lock);

    return getLocked(registry, normalised);
}

Logger* LogManager::root() {
    State& registry = state();

    const std::lock_guard<std::mutex> guard(registry.lock);

    return registry.rootLogger;
}

void LogManager::setCodeDefault(const string& name, Verbosity level) {
    // Outside the registry's lock: the logger takes its own
    get(name)->setCodeDefault(level);
}

std::string LogManager::moduleNameFromLibrary(const string& file) {
    const string::size_type slash = file.find_last_of('/');
    string name = string::npos == slash ? file : file.substr(slash + 1);

    if (0 == name.compare(0, 3, "lib"))
        name.erase(0, 3);

    const string::size_type dot = name.find('.');

    if (string::npos != dot)
        name.erase(dot);

    return name;
}

void LogManager::setLoadingModule(const string& name) {
    State& registry = state();

    const std::lock_guard<std::mutex> guard(registry.lock);

    registry.loadingModule = name;
}

std::string LogManager::takeLoadingModule() {
    State& registry = state();

    const std::lock_guard<std::mutex> guard(registry.lock);

    string name;
    name.swap(registry.loadingModule);

    return name;
}

/**
 * Asks every sink there is to reopen itself.
 *
 * The sinks are collected first and reopened afterwards: a sink is asked once
 * however many loggers hold it, and no lock of the registry or of a logger is
 * held while it closes and opens a file.
 */
void LogManager::reopenAll() {
    std::vector<std::shared_ptr<LogSink>> sinks;

    {
        State& registry = state();

        const std::lock_guard<std::mutex> guard(registry.lock);

        for (const std::pair<const string, Logger*>& entry : registry.loggers)
            entry.second->appendSinks(sinks);
    }

    std::vector<std::shared_ptr<LogSink>> distinct;

    for (const std::shared_ptr<LogSink>& sink : sinks) {
        bool seen = false;

        for (const std::shared_ptr<LogSink>& known : distinct)
            if (known == sink) {
                seen = true;
                break;
            }

        if (!seen)
            distinct.push_back(sink);
    }

    for (const std::shared_ptr<LogSink>& sink : distinct)
        sink->reopen();
}

std::shared_ptr<LogSink> LogManager::bootstrapConsoleSink() {
    State& registry = state();

    const std::lock_guard<std::mutex> guard(registry.lock);

    return registry.bootstrapConsole;
}

/**
 * Teaches the configuration a kind of sink.
 *
 * The factory that was there, if any, is handed out of the lock and let go of
 * afterwards: a factory may hold a sink of its own, and letting go of the last
 * reference to a sink closes a file, which is not something to do while the
 * registry is locked.
 */
void LogManager::registerSinkType(const string& type, SinkFactory factory) {
    SinkFactory replaced;

    {
        State& registry = state();

        const std::lock_guard<std::mutex> guard(registry.lock);

        SinkFactory& slot = registry.sinkFactories[lowerType(type)];

        replaced.swap(slot);
        slot = std::move(factory);
    }

    // And whatever the factory that was there held goes here, unlocked
    replaced = SinkFactory();
}

LogManager::SinkFactory LogManager::findSinkFactory(const string& type) {
    State& registry = state();

    const std::lock_guard<std::mutex> guard(registry.lock);

    const std::map<string, SinkFactory>::const_iterator known =
        registry.sinkFactories.find(lowerType(type));

    return registry.sinkFactories.end() == known ? SinkFactory() : known->second;
}

/**
 * Applies a whole configuration, or none of it.
 *
 * Every sink is built before anything is changed, because a sink is the part
 * that can fail: a type nothing has registered, a file that will not open.  If
 * any of them cannot be made, what was built is thrown away, the reasons are in
 * errors and the configuration in force is untouched.
 *
 * The swap that follows happens under the registry's lock, and every logger of
 * the process is given its new configuration - or none at all - in one go, under
 * its own mutex: a record logged meanwhile finds that logger as it was or as it
 * now is, and never with its level and its sinks taken away.  Across loggers
 * there is no such promise: a record walking up the hierarchy may meet a child
 * already on the new configuration and a parent still on the old one, which
 * costs it nothing.
 *
 * Nothing is called on a sink while the registry's lock is held: the sinks of
 * the configuration before this one are carried out of it and let go of
 * afterwards, where closing their files is no concern of anyone waiting for the
 * registry.
 */
bool LogManager::configure(const LogConfig& config, std::vector<string>& errors) {
    errors.clear();

    std::map<string, std::shared_ptr<LogSink>> built;
    std::map<string, Verbosity> thresholds;

    for (const SinkSpec& spec : config.sinks) {
        const SinkFactory factory = findSinkFactory(spec.type);

        if (nullptr == factory) {
            errors.push_back("sink." + spec.id + ".type: unknown sink type '" + spec.type + "'");
            continue;
        }

        string why;
        const std::shared_ptr<LogSink> sink = factory(spec, why);

        if (nullptr == sink) {
            /* With the dot, and not "sink." + id alone: an id of "a" would
             * otherwise pass off an error text about "sink.ab.token" as its own */
            const string ownKeys = "sink." + spec.id + ".";

            if (why.empty())
                errors.push_back("sink." + spec.id + ": the sink could not be made");
            else if (0 == why.compare(0, ownKeys.size(), ownKeys))
                /* A factory that knows which of its own settings is wrong names
                 * that key itself - "sink.page.token: ..." - and is quoted as it
                 * stands rather than behind a second "sink.page:" */
                errors.push_back(why);
            else
                errors.push_back("sink." + spec.id + ": " + why);

            continue;
        }

        built[spec.id] = sink;

        /* The level the file gave, or the one this KIND of sink says it wants
         * where the file gave none: TRACE for a file or a console, ERROR for a
         * pager.  A sink does no filtering of its own - this threshold is the
         * only one - so the sink is the right place for that default to live */
        thresholds[spec.id] = spec.levelGiven ? spec.level : sink->defaultThreshold();
    }

    if (!errors.empty()) {
        // Nothing at all changes; what was built is let go of here, unlocked
        built.clear();

        capErrors(errors);

        return false;
    }

    std::vector<std::shared_ptr<LogSink>> replaced;

    {
        State& registry = state();

        const std::lock_guard<std::mutex> guard(registry.lock);

        /* What each logger the file names is to become, and the loggers it names
         * that do not exist yet, worked out before a single logger is changed:
         * nothing below creates a logger, so the one pass that follows really is
         * over every logger there is */
        std::map<Logger*, LoggerChange> changes;

        registry.configuredLoggers.clear();

        for (const LoggerSpec& spec : config.loggers) {
            const string name = normaliseName(spec.name);
            LoggerChange& change = changes[getLocked(registry, name)];

            if (spec.level)
                change.level = spec.level;

            if (spec.additive)
                change.additive = spec.additive;

            for (const string& id : spec.sinks) {
                const std::map<string, std::shared_ptr<LogSink>>::const_iterator sink =
                    built.find(id);

                // parseLogConfig() refuses a file naming a sink it has not, so
                // this only skips a LogConfig somebody built by hand
                if (built.end() == sink)
                    continue;

                const std::map<string, Verbosity>::const_iterator threshold = thresholds.find(id);

                change.sinks.emplace_back(
                    sink->second, thresholds.end() == threshold ? TRACE : threshold->second);
            }

            /* A level line is what makes a logger a configured one; a line that
             * only speaks of its additivity says nothing about where it logs */
            if (spec.level)
                registry.configuredLoggers.insert(name);
        }

        /* One pass over every logger of the process, each of them given its new
         * configuration under its own mutex in one go.  A logger the file does
         * not mention is given nothing, which is how a line dropped from the
         * file stops applying; what the code asked for - its sinks, its
         * defaults, its additivity - is not this function's to touch */
        for (const std::pair<const string, Logger*>& entry : registry.loggers) {
            const std::map<Logger*, LoggerChange>::iterator change = changes.find(entry.second);

            if (changes.end() == change)
                entry.second->applyConfig(std::nullopt, std::nullopt,
                                          std::vector<Logger::SinkEntry>(), replaced);
            else
                entry.second->applyConfig(change->second.level, change->second.additive,
                                          std::move(change->second.sinks), replaced);
        }

        /* With a configuration in force the root no longer needs the console it
         * was given so that whatever start-up logged would be seen */
        if (nullptr != registry.bootstrapConsole)
            registry.rootLogger->removeSink(registry.bootstrapConsole);
    }

    // The sinks of the configuration before this one go here, outside the lock
    replaced.clear();

    return true;
}

bool LogManager::isConfigured(const string& name) {
    State& registry = state();
    const string normalised = normaliseName(name);

    const std::lock_guard<std::mutex> guard(registry.lock);

    return registry.configuredLoggers.end() != registry.configuredLoggers.find(normalised);
}

/**
 * Reads a logging.conf and applies it.
 *
 * Whatever went wrong is logged once configure() has returned and no lock of the
 * registry is held any more: reporting a problem is itself logging, and logging
 * writes to sinks.
 *
 * A file is data, and data neither stops this process nor takes its logging
 * away: whatever reading one may cost - the allocation a monstrous file asks
 * for, say - is caught here and reported like any other thing wrong with it.
 * What was in force stays in force.
 */
bool LogManager::loadFile(const string& fileName) {
    LogConfig config;
    std::vector<string> errors;

    bool applied = false;

    try {
        applied = parseLogConfig(fileName, config, errors);

        if (applied) {
            warnIfSecretsAreReadable(fileName, config);

            applied = configure(config, errors);
        }
    } catch (const std::exception& e) {
        applied = false;
        errors.clear();
        errors.push_back(string("the file could not be read: ") + e.what());
    } catch (...) {
        applied = false;
        errors.clear();
        errors.push_back("the file could not be read");
    }

    if (!applied) {
        Logger* const reporter = get("core.config");

        for (const string& error : errors)
            LOG_TO(reporter, ERROR, "logging.conf: {}", error);
    }

    return applied;
}

/// The one sink id the command line is about: the debug file of -d and -D
static const string debugLogSinkId("debuglog");

/**
 * The configuration of a process that has no logging.conf: the debug log and
 * the console, both fed by the root at INFO, and the old elog stream at the
 * DEBUG level it has always had.
 *
 * This is the same thing the file
 *
 *     sink.console.type    = console
 *     sink.debuglog.type   = file
 *     sink.debuglog.path   = debug.log
 *     sink.debuglog.format = text
 *     logger.root          = INFO, debuglog, console
 *     logger.legacy        = on
 *
 * would be, built here rather than written anywhere: an installation without a
 * logging.conf logs what it always logged.
 */
static LogConfig builtInLogConfig() {
    LogConfig config;

    SinkSpec console;
    console.id = "console";
    console.type = "console";
    config.sinks.push_back(console);

    SinkSpec debugLog;
    debugLog.id = debugLogSinkId;
    debugLog.type = "file";
    debugLog.path = "debug.log";
    debugLog.json = false;
    config.sinks.push_back(debugLog);

    LoggerSpec root;
    // The root's name is empty, however a file spells it
    root.level = INFO;
    root.sinks.push_back(debugLogSinkId);
    root.sinks.push_back("console");
    config.loggers.push_back(root);

    LoggerSpec legacy;
    legacy.name = "legacy";
    legacy.level = DEBUG;
    config.loggers.push_back(legacy);

    return config;
}

/**
 * The command line on top of a configuration, whether that came from a file or
 * from builtInLogConfig(): -d puts the debug log where it said, and -D takes it
 * away altogether, so that "no debug log" keeps meaning no debug log whatever
 * logging.conf asks for.  Every other sink is the file's business alone.
 */
static void adjustForCommandLine(LogConfig& config, const LogStartOptions& options) {
    if (options.debugLogFileGiven)
        for (SinkSpec& sink : config.sinks)
            if (debugLogSinkId == sink.id)
                sink.path = options.debugLogFile;

    if (options.debugLog)
        return;

    for (std::vector<SinkSpec>::iterator sink = config.sinks.begin(); sink != config.sinks.end();)
        if (debugLogSinkId == sink->id)
            sink = config.sinks.erase(sink);
        else
            ++sink;

    // And with it every mention of it, which would otherwise be a dangling id
    for (LoggerSpec& logger : config.loggers)
        for (std::vector<string>::iterator id = logger.sinks.begin(); id != logger.sinks.end();)
            if (debugLogSinkId == *id)
                id = logger.sinks.erase(id);
            else
                ++id;
}

/// The path the debug log would be written to, empty when there is none
static string debugLogPath(const LogConfig& config) {
    for (const SinkSpec& sink : config.sinks)
        if (debugLogSinkId == sink.id)
            return sink.path;

    return string();
}

/**
 * Says what went wrong with logging.conf, one record per problem, in the words
 * LogManager::loadFile() uses for the same thing.  These are records like any
 * other: they go wherever the configuration in force sends "core.config", which
 * is why the caller says them once it has applied one.
 *
 * The function is the caller's rather than this one's: what the reader of a log
 * file wants to see is where the configuration was being read, not the three
 * lines that write the message.
 */
static void reportLogConfigErrors(const char* function, const std::vector<string>& errors) {
    Logger* const reporter = coreLogger(CoreLogger::Config);

    for (const string& error : errors)
        reporter->writeFunc(ERROR, function, "logging.conf: {}", error);
}

void LogManager::start(const LogStartOptions& options) {
    /* logging.conf is data, and data can neither stop this process nor take its
     * logging away.  Whatever reading one may cost - the allocation a monstrous
     * name or a monstrous list asks for - is caught here: on a reload the
     * configuration in force stays in force and the reason is logged, and at
     * start-up the process carries on with whatever it can still be given.
     * Nothing here exits, and nothing here throws on */
    string failure;

    try {
        bool haveFile = false;

        {
            std::ifstream probe(options.fileName.c_str());

            haveFile = probe.is_open();
        }

        /* Everything that was wrong with the file, said once at the end: reporting a
         * problem is itself logging, and said here it would go wherever the process
         * happened to be logging before - at start-up, nowhere at all */
        std::vector<string> problems;

        LogConfig config;

        // Whether the configuration that ends up in force is the one in that file
        bool fromFile = false;

        if (haveFile) {
            std::vector<string> errors;

            if (parseLogConfig(options.fileName, config, errors))
                fromFile = true;
            else
                problems = errors;
        }

        /* A file that is not a configuration changes nothing at all on a reload:
         * what is in force stays in force, and the process goes on logging where it
         * was logging.  At start-up there is nothing to keep, and the built-in
         * default is what a process with no usable file has */
        if (haveFile && !fromFile && options.reload) {
            reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

            return;
        }

        if (!fromFile)
            config = builtInLogConfig();

        adjustForCommandLine(config, options);

        std::vector<string> errors;
        bool applied = configure(config, errors);

        if (!applied) {
            problems.insert(problems.end(), errors.begin(), errors.end());

            // As above: on a reload the configuration in force is the one to keep
            if (options.reload) {
                reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

                return;
            }

            /* At start-up a file that cannot be applied - a sink whose file will
             * not open, a kind of sink this build has not got - is no reason to log
             * nowhere at all: the built-in default is tried instead */
            if (fromFile) {
                fromFile = false;
                config = builtInLogConfig();
                adjustForCommandLine(config, options);

                errors.clear();
                applied = configure(config, errors);
                problems.insert(problems.end(), errors.begin(), errors.end());
            }

            /* And when even that will not do - an unwritable debug log - the
             * process starts all the same and logs wherever it still can.  Nothing
             * here exits: writing a log file is not what this process is for */
            if (!applied)
                std::clog << "*** Unable to open log file: " << debugLogPath(config) << std::endl;
        }

        /* Said last of all, so that every one of these goes where the configuration
         * just applied says it goes rather than wherever the one before it did */
        reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

        /* A file that holds a token - a pushover sink's - and that anybody on
         * this host may read is worth one warning.  Only for the file that is
         * really in force: the built-in default holds no secret */
        if (fromFile)
            warnIfSecretsAreReadable(options.fileName, config);

        if (!options.reload && !haveFile)
            LOG_CORE(Core, INFO,
                     "No logging.conf found; using built-in defaults (see "
                     "bin/logging.example.conf)");

        if (options.reload && fromFile)
            LOG_CORE(Core, INFO, "Reloaded {}", options.fileName);

        return;
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "an unknown error";
    }

    if (options.reload) {
        LOG_CORE(Config, ERROR, "logging.conf: {}", failure);

        return;
    }

    /* At start-up there is nothing in force to keep, so the built-in default is
     * tried; when even that will not go on, the console the root was given while
     * the process started up is where this process logs */
    std::clog << "*** logging.conf: " << failure << std::endl;

    try {
        LogConfig config = builtInLogConfig();

        adjustForCommandLine(config, options);

        std::vector<string> errors;

        configure(config, errors);
    } catch (...) {
        std::clog << "*** Unable to apply the built-in logging defaults" << std::endl;
    }
}

/**
 * Says once that a file holding a token is readable by somebody else.
 *
 * A file nobody but this process's own user may read is the normal case and says
 * nothing at all; a file that cannot be asked about - it went away between being
 * read and being asked about - says nothing either.  This is a warning and never
 * anything more: a configuration file is data.
 */
void LogManager::warnIfSecretsAreReadable(const string& fileName, const LogConfig& config) {
    bool holdsSecret = false;

    for (const SinkSpec& spec : config.sinks)
        if (spec.options.end() != spec.options.find("token")) {
            holdsSecret = true;
            break;
        }

    if (!holdsSecret)
        return;

    struct stat about;

    if (0 != ::stat(fileName.c_str(), &about))
        return;

    /* READ, and only read: a token is a secret because somebody else can read
     * it, and a mode of 0610 - which a group may write and not read - is not the
     * problem this warns about.  0044 is the group's and the world's read bit */
    if (0 == (about.st_mode & 0044))
        return;

    LOG_TO(get("core.config"), WARN,
           "{} holds a token and is readable by others (mode {:04o}); chmod 600 it", fileName,
           static_cast<unsigned int>(about.st_mode & 07777));
}

/**
 * Gives the root a human-readable log file, for a process with no logging.conf
 * to say where records go.  This is a sink of the code, so a configuration read
 * later leaves it where it is; the same path twice is the same sink once.
 *
 * A path that will not open is said once on stderr and attached all the same.
 * This runs while the process starts up, before anything has been configured: a
 * log file nobody could open would otherwise swallow every record the process
 * ever logs without a word about it, and the sink's reopen() on a SIGHUP is what
 * picks the file up once the directory it wants is there.
 */
void LogManager::bootstrapFile(const string& path) {
    {
        State& registry = state();

        const std::lock_guard<std::mutex> guard(registry.lock);

        if (!registry.bootstrapFiles.insert(path).second)
            return;
    }

    // Opening the file, and attaching it, with no lock of the registry held
    const std::shared_ptr<FileSink> sink = std::make_shared<FileSink>(path, false);

    if (!sink->isOpen())
        std::cerr << "gnuworld: cannot open log file " << escapeControl(path) << std::endl;

    root()->addSink(sink, TRACE);
}

} // namespace gnuworld
