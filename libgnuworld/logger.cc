/**
 * logger.cc
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
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include "LogFormat.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogRender.h"
#include "LogSink.h"

#include "logger.h"

namespace gnuworld {

using std::string;

namespace {

/**
 * One registered extractor and the token it was registered under.
 */
struct ExtractorEntry {
    const void* owner;
    Logger::Extractor extractor;
};

/**
 * The extractors of the process, and the lock that guards them.  Both are
 * allocated on first use and never destroyed: a static destructor elsewhere may
 * still log, and so may still ask for an extractor, while the process is on its
 * way out.
 */
std::mutex& extractorLock() {
    static std::mutex* const lock = new std::mutex();

    return *lock;
}

std::map<std::type_index, ExtractorEntry>& extractorRegistry() {
    static std::map<std::type_index, ExtractorEntry>* const registry =
        new std::map<std::type_index, ExtractorEntry>();

    return *registry;
}

/**
 * True while this thread is inside a sink's emit().  A record logged from there
 * is kept away from the sinks that would feed themselves with it.
 */
thread_local bool inSinkDispatch = false;

/**
 * Marks the thread as being inside a sink dispatch for as long as it lives, and
 * remembers whether it already was.
 */
class ReentryGuard {
  public:
    ReentryGuard() : previous(inSinkDispatch) { inSinkDispatch = true; }

    ~ReentryGuard() { inSinkDispatch = previous; }

    ReentryGuard(const ReentryGuard&) = delete;
    ReentryGuard& operator=(const ReentryGuard&) = delete;

    /// True when this thread was already inside a sink dispatch
    bool wasInside() const { return previous; }

  private:
    bool previous;
};

} // namespace

/**
 * Logger constructor - a logger with a name, a place in the hierarchy, no sinks
 * and no level of its own.  Only LogManager calls this; whoever asked it for the
 * logger decides where its records go.
 */
Logger::Logger(std::string loggerName, Logger* theParent)
    : name(std::move(loggerName)), parent(theParent) {}

/**
 * Logger destructor - never called: a logger lives as long as the process does.
 * The sinks are shared, so they go when the last holder of them does.
 */
Logger::~Logger() {}

/**
 * The level this logger logs at.
 *
 * The walk up the hierarchy takes the mutex of one logger at a time and has let
 * it go again before it takes the next: no thread ever holds two of them, and
 * the parent pointers cannot change under it.
 */
Verbosity Logger::effectiveLevel() const {
    for (const Logger* step = this; nullptr != step; step = step->parent) {
        std::optional<Verbosity> own;

        {
            const std::lock_guard<std::mutex> guard(step->logMutex);

            if (step->configLevel)
                own = step->configLevel;
            else if (step->codeDefault)
                own = step->codeDefault;
        }

        if (own)
            return *own;
    }

    // The root of a hierarchy nobody has configured
    return INFO;
}

bool Logger::shouldLog(Verbosity v) const { return v <= effectiveLevel(); }

void Logger::setLevel(Verbosity newLevel) { setConfigLevel(newLevel); }

void Logger::setConfigLevel(std::optional<Verbosity> newLevel) {
    const std::lock_guard<std::mutex> guard(logMutex);

    configLevel = newLevel;
}

void Logger::setCodeDefault(Verbosity newLevel) {
    const std::lock_guard<std::mutex> guard(logMutex);

    // The first caller decides: a module that asks for a default twice, or two
    // modules sharing a logger, do not move each other's
    if (!codeDefault)
        codeDefault = newLevel;
}

void Logger::setAdditive(bool newAdditive) {
    const std::lock_guard<std::mutex> guard(logMutex);

    additive = newAdditive;
}

void Logger::setConfigAdditive(std::optional<bool> newAdditive) {
    const std::lock_guard<std::mutex> guard(logMutex);

    configAdditive = newAdditive;
}

bool Logger::isAdditive() const {
    const std::lock_guard<std::mutex> guard(logMutex);

    return isAdditiveLocked();
}

/**
 * Puts a whole configuration on this logger, or takes the one it had away: the
 * level, the additivity and the sinks of a configuration file, replaced in one
 * go under this logger's mutex.
 *
 * That one acquisition is the point of the method.  A record logged on this
 * logger while a file is being applied takes this very mutex to collect its
 * sinks, so it sees the configuration that was or the configuration that is;
 * clearing first and filling in afterwards would leave a moment in which the
 * logger had no level and no sink, and a record of that moment would be lost.
 *
 * The sinks that were here are moved rather than dropped: the caller is the
 * registry, holding its own lock, and letting go of the last reference to a sink
 * closes a file.
 */
void Logger::applyConfig(std::optional<Verbosity> level, std::optional<bool> newAdditive,
                         std::vector<SinkEntry> sinks,
                         std::vector<std::shared_ptr<LogSink>>& released) {
    const std::lock_guard<std::mutex> guard(logMutex);

    configLevel = level;
    configAdditive = newAdditive;

    released.reserve(released.size() + configSinks.size());

    for (SinkEntry& entry : configSinks)
        released.push_back(std::move(entry.first));

    configSinks = std::move(sinks);
}

/**
 * The logger of this name below this one.  The name of a logger never changes,
 * so it is read here without the mutex; the registry does the rest.
 */
Logger* Logger::child(const std::string& sub) {
    return LogManager::get(name.empty() ? sub : name + '.' + sub);
}

Logger* Logger::child(const std::string& sub, Verbosity theCodeDefault) {
    Logger* const theChild = child(sub);

    theChild->setCodeDefault(theCodeDefault);

    return theChild;
}

void Logger::addSink(std::shared_ptr<LogSink> sink, Verbosity threshold) {
    if (nullptr == sink)
        return;

    const std::lock_guard<std::mutex> guard(logMutex);

    codeSinks.emplace_back(std::move(sink), threshold);
}

void Logger::addConfigSink(std::shared_ptr<LogSink> sink, Verbosity threshold) {
    if (nullptr == sink)
        return;

    const std::lock_guard<std::mutex> guard(logMutex);

    configSinks.emplace_back(std::move(sink), threshold);
}

void Logger::clearConfigSinks() {
    std::vector<SinkEntry> gone;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        gone.swap(configSinks);
    }

    // The sinks go, if nothing else holds them, outside the lock
    gone.clear();
}

void Logger::removeSink(const std::shared_ptr<LogSink>& sink) {
    const std::lock_guard<std::mutex> guard(logMutex);

    const auto isSink = [&sink](const SinkEntry& entry) { return entry.first == sink; };

    configSinks.erase(std::remove_if(configSinks.begin(), configSinks.end(), isSink),
                      configSinks.end());
    codeSinks.erase(std::remove_if(codeSinks.begin(), codeSinks.end(), isSink), codeSinks.end());
}

void Logger::setSinkThreshold(const std::shared_ptr<LogSink>& sink, Verbosity threshold) {
    const std::lock_guard<std::mutex> guard(logMutex);

    for (SinkEntry& entry : configSinks)
        if (entry.first == sink)
            entry.second = threshold;

    for (SinkEntry& entry : codeSinks)
        if (entry.first == sink)
            entry.second = threshold;
}

void Logger::setContext(const std::string& key, LogValue value) {
    const std::lock_guard<std::mutex> guard(logMutex);

    for (LogField& field : context)
        if (field.key == key) {
            field.value = std::move(value);
            return;
        }

    context.push_back(LogField{key, std::move(value), false});
}

/**
 * Adds this logger's sinks to a dispatch, the configured ones before the ones
 * the code attached.  The caller holds this logger's mutex.
 */
void Logger::appendTargetsLocked(std::vector<Target>& targets) const {
    const std::vector<SinkEntry>* const lists[] = {&configSinks, &codeSinks};

    for (const std::vector<SinkEntry>* const list : lists)
        for (const SinkEntry& entry : *list) {
            bool known = false;

            for (Target& target : targets)
                if (target.sink == entry.first) {
                    // The same sink on two loggers of the path hears the record
                    // once, and hears it if either attachment lets it through
                    if (entry.second > target.threshold)
                        target.threshold = entry.second;

                    known = true;
                    break;
                }

            if (!known)
                targets.push_back(Target{entry.first, entry.second});
        }
}

/**
 * Hands one record to every sink that wants it, walking up the hierarchy.
 *
 * The record goes to the sinks of this logger, then, while a logger of the path
 * is additive, to those of its parent, and so on to the root; a sink attached
 * more than once along the way hears the record once.  The whole list is
 * collected first, taking the mutex of one logger at a time, and every mutex is
 * released before the first emit(): no lock of a logger is ever held while a
 * sink writes a file, a terminal or the network, or while another logger's mutex
 * is taken, and the copy keeps a sink alive for the whole of its own emit() even
 * if something removes it meanwhile.
 *
 * A record logged from inside a sink's emit() - on this thread, by this very
 * dispatch - goes only to the sinks that put up with that.
 *
 * The level is looked at here and not only at the call site: a record built
 * without asking shouldLog() first - by the stream API, or by a caller handing
 * over a record of its own - is dropped just the same if this logger does not
 * log at that level, and nothing at all is ever logged at OFF.
 */
void Logger::log(LogRecord&& record) {
    if (OFF == record.level || record.level > effectiveLevel())
        return;

    std::vector<Target> targets;

    for (Logger* step = this; nullptr != step;) {
        bool walkOn = false;

        {
            const std::lock_guard<std::mutex> guard(step->logMutex);

            if (this == step) {
                record.logger = step->name;
                record.context = step->context;
            }

            step->appendTargetsLocked(targets);
            walkOn = step->isAdditiveLocked();
        }

        if (!walkOn)
            break;

        step = step->parent;
    }

    /* What an ancestor says of its records holds for its descendants' too: the
     * bot a module's logger names is the bot of "module.sql" as well.  This walk
     * does not stop where the sinks' walk does - additivity is about where a
     * record goes, not about what it is - and a key the logger nearer to the
     * record has set is not overwritten */
    for (const Logger* step = parent; nullptr != step; step = step->parent) {
        const std::lock_guard<std::mutex> guard(step->logMutex);

        for (const LogField& inherited : step->context) {
            bool known = false;

            for (const LogField& field : record.context)
                if (field.key == inherited.key) {
                    known = true;
                    break;
                }

            if (!known)
                record.context.push_back(inherited);
        }
    }

    if (std::chrono::system_clock::time_point() == record.time)
        record.time = std::chrono::system_clock::now();

    const ReentryGuard guard;

    for (const Target& target : targets) {
        if (record.level > target.threshold)
            continue;

        if (guard.wasInside() && target.sink->suppressOnReentry())
            continue;

        target.sink->emit(record);
    }
}

void Logger::write(Verbosity v, const std::string& theMessage) {
    LogRecord record{};

    record.level = v;
    record.message = theMessage;

    log(std::move(record));
}

void Logger::MessageTemplate::log() const {
    // Nothing is rendered, and no extractor has run, for a record the level
    // rules out
    if (!loggerInstance->shouldLog(level))
        return;

    const RenderResult rendered = renderTemplate(templateStr, positional, fields);

    LogRecord record{};

    record.level = level;
    record.function = parseFunction(nullptr == func ? std::string() : std::string(func));
    record.message = rendered.text;
    record.spans = rendered.spans;
    record.fields = fields;

    loggerInstance->log(std::move(record));
}

Logger::Extractor Logger::findExtractor(std::type_index type) {
    const std::lock_guard<std::mutex> guard(extractorLock());

    const std::map<std::type_index, ExtractorEntry>& registry = extractorRegistry();
    const std::map<std::type_index, ExtractorEntry>::const_iterator entry = registry.find(type);

    if (registry.end() == entry)
        return Extractor();

    return entry->second.extractor;
}

void Logger::setExtractor(std::type_index type, const void* owner, Extractor extractor) {
    const std::lock_guard<std::mutex> guard(extractorLock());

    extractorRegistry()[type] = ExtractorEntry{owner, std::move(extractor)};
}

void Logger::removeExtractors(const void* owner) {
    const std::lock_guard<std::mutex> guard(extractorLock());

    std::map<std::type_index, ExtractorEntry>& registry = extractorRegistry();

    for (std::map<std::type_index, ExtractorEntry>::iterator entry = registry.begin();
         entry != registry.end();) {
        if (entry->second.owner == owner)
            entry = registry.erase(entry);
        else
            ++entry;
    }
}

/**
 * Adds every sink of this logger to the list, under the logger's mutex and
 * without touching any of them: what the caller does with them, it does on its
 * own time.
 */
void Logger::appendSinks(std::vector<std::shared_ptr<LogSink>>& targets) const {
    const std::lock_guard<std::mutex> guard(logMutex);

    targets.reserve(targets.size() + configSinks.size() + codeSinks.size());

    for (const SinkEntry& entry : configSinks)
        targets.push_back(entry.first);

    for (const SinkEntry& entry : codeSinks)
        targets.push_back(entry.first);
}

} // namespace gnuworld
