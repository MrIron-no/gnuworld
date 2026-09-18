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
#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
 * ".a.b" and "a.b." all name the logger "a.b", "" and "root" the root itself.
 */
string LogManager::normaliseName(const string& name) {
    if ("root" == name)
        return string();

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

} // namespace gnuworld
