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
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include "LogFormat.h"
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
 * Logger constructor - a logger with a name, no sinks and no filtering of its
 * own.  Whoever creates it decides where its records go.
 */
Logger::Logger(std::string loggerName) : name(std::move(loggerName)) {}

/**
 * Logger destructor - the sinks are shared, so they go when the last holder of
 * them does, which may well be later than this.
 */
Logger::~Logger() {}

bool Logger::shouldLog(Verbosity v) const {
    const std::lock_guard<std::mutex> guard(logMutex);

    return v <= level;
}

void Logger::setLevel(Verbosity newLevel) {
    const std::lock_guard<std::mutex> guard(logMutex);

    level = newLevel;
}

void Logger::addSink(std::shared_ptr<LogSink> sink, Verbosity threshold) {
    if (nullptr == sink)
        return;

    const std::lock_guard<std::mutex> guard(logMutex);

    sinks.emplace_back(std::move(sink), threshold);
}

void Logger::removeSink(const std::shared_ptr<LogSink>& sink) {
    const std::lock_guard<std::mutex> guard(logMutex);

    sinks.erase(
        std::remove_if(sinks.begin(), sinks.end(),
                       [&sink](const std::pair<std::shared_ptr<LogSink>, Verbosity>& entry) {
                           return entry.first == sink;
                       }),
        sinks.end());

    if (legacyFileSink == sink)
        legacyFileSink.reset();
    if (legacyConsoleSink == sink)
        legacyConsoleSink.reset();
    if (legacyIrcSink == sink) {
        legacyIrcSink.reset();
        legacyChanSetter = nullptr;
    }
}

void Logger::setSinkThreshold(const std::shared_ptr<LogSink>& sink, Verbosity threshold) {
    const std::lock_guard<std::mutex> guard(logMutex);

    for (std::pair<std::shared_ptr<LogSink>, Verbosity>& entry : sinks)
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
 * Hands one record to every sink that wants it.
 *
 * The list of sinks is copied under the logger's mutex and the mutex is
 * released before the first emit(): no lock of this logger is ever held while a
 * sink writes a file, a terminal or the network, and the copy keeps a sink alive
 * for the whole of its own emit() even if something removes it meanwhile.
 *
 * A record logged from inside a sink's emit() - on this thread, by this very
 * dispatch - goes only to the sinks that put up with that.
 */
void Logger::log(LogRecord&& record) {
    std::vector<std::pair<std::shared_ptr<LogSink>, Verbosity>> targets;
    std::shared_ptr<LogSink> fileSlot;
    std::shared_ptr<LogSink> consoleSlot;
    bool wantLogSQL = false;
    bool wantConsoleSQL = false;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        record.logger = name;
        record.context = context;

        targets = sinks;
        fileSlot = legacyFileSink;
        consoleSlot = legacyConsoleSink;
        wantLogSQL = logSQL;
        wantConsoleSQL = consoleSQL;
    }

    if (std::chrono::system_clock::time_point() == record.time)
        record.time = std::chrono::system_clock::now();

    const ReentryGuard guard;

    for (const std::pair<std::shared_ptr<LogSink>, Verbosity>& target : targets) {
        if (SQL == record.level) {
            // Today's routing, until the <module>.sql logger replaces it: the
            // legacy file and console slots if they have been asked for SQL,
            // whatever their threshold, and no other sink at all
            if (target.first == fileSlot) {
                if (!wantLogSQL)
                    continue;
            } else if (target.first == consoleSlot) {
                if (!wantConsoleSQL)
                    continue;
            } else
                continue;
        } else if (record.level > target.second)
            continue;

        if (guard.wasInside() && target.first->suppressOnReentry())
            continue;

        target.first->emit(record);
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

void Logger::setChannel(const std::string& channelName) {
    std::function<void(const std::string&)> setter;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyChanName = channelName;
        setter = legacyChanSetter;
    }

    // Outside the lock: the sink takes its own
    if (setter)
        setter(channelName);
}

std::string Logger::getChannel() const {
    const std::lock_guard<std::mutex> guard(logMutex);

    return legacyChanName;
}

void Logger::setChanVerbosity(unsigned short verbosity) {
    std::shared_ptr<LogSink> sink;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyChanVerbosity = static_cast<Verbosity>(verbosity);
        sink = legacyIrcSink;
    }

    if (nullptr != sink)
        setSinkThreshold(sink, static_cast<Verbosity>(verbosity));
}

void Logger::setLogVerbosity(unsigned short verbosity) {
    std::shared_ptr<LogSink> sink;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyLogVerbosity = static_cast<Verbosity>(verbosity);
        sink = legacyFileSink;
    }

    if (nullptr != sink)
        setSinkThreshold(sink, static_cast<Verbosity>(verbosity));
}

void Logger::setConsoleVerbosity(unsigned short verbosity) {
    std::shared_ptr<LogSink> sink;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyConsoleVerbosity = static_cast<Verbosity>(verbosity);
        sink = legacyConsoleSink;
    }

    if (nullptr != sink)
        setSinkThreshold(sink, static_cast<Verbosity>(verbosity));
}

void Logger::setLogSQL(bool enable) {
    const std::lock_guard<std::mutex> guard(logMutex);

    logSQL = enable;
}

void Logger::setConsoleSQL(bool enable) {
    const std::lock_guard<std::mutex> guard(logMutex);

    consoleSQL = enable;
}

void Logger::setLegacyFileSink(std::shared_ptr<LogSink> sink) {
    Verbosity threshold = TRACE;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyFileSink = sink;
        threshold = legacyLogVerbosity;
    }

    // A module reads its configuration before its sinks exist, so the slot
    // takes over whatever was asked for in the meantime
    if (nullptr != sink)
        setSinkThreshold(sink, threshold);
}

void Logger::setLegacyConsoleSink(std::shared_ptr<LogSink> sink) {
    Verbosity threshold = TRACE;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyConsoleSink = sink;
        threshold = legacyConsoleVerbosity;
    }

    if (nullptr != sink)
        setSinkThreshold(sink, threshold);
}

void Logger::setLegacyIrcSink(std::shared_ptr<LogSink> sink) {
    Verbosity threshold = INFO;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        legacyIrcSink = sink;
        threshold = legacyChanVerbosity;
    }

    if (nullptr != sink)
        setSinkThreshold(sink, threshold);
}

void Logger::setLegacyChanSetter(std::function<void(const std::string&)> setter) {
    const std::lock_guard<std::mutex> guard(logMutex);

    legacyChanSetter = std::move(setter);
}

/**
 * Closes and reopens every destination that has anything to reopen.
 * This function should be called after external log rotation (e.g. via
 * logrotate) so that a file sink writes to the new file rather than the rotated
 * one.  The logger's mutex is not held while a sink reopens.
 */
void Logger::rotateLogs() {
    std::vector<std::shared_ptr<LogSink>> targets;

    {
        const std::lock_guard<std::mutex> guard(logMutex);

        targets.reserve(sinks.size());
        for (const std::pair<std::shared_ptr<LogSink>, Verbosity>& entry : sinks)
            targets.push_back(entry.first);
    }

    for (const std::shared_ptr<LogSink>& sink : targets)
        sink->reopen();
}

} // namespace gnuworld
