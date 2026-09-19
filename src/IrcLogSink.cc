/**
 * IrcLogSink.cc
 * The IRC channel sink of the logging system.
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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Channel.h"
#include "IrcLogSink.h"
#include "LogFormat.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "Network.h"
#include "logger.h"
#include "misc.h"
#include "server.h"

namespace gnuworld {

namespace {

/**
 * The thread that owns the server connection.  A record that is logged
 * anywhere else is queued instead of sent.  The id is written once, from the
 * main thread, before any worker thread exists; the flag is what other threads
 * read, so that an unset id is never compared against.
 */
std::thread::id mainThreadId;
std::atomic<bool> mainThreadKnown(false);

} // namespace

/**
 * The live sinks, and the lock that guards the list.  Both are allocated on
 * first use and never destroyed: a static destructor elsewhere may still log,
 * and flushAll() may still be reached, while the process is on its way out.
 */
static std::mutex& registryLock() {
    static std::mutex* const lock = new std::mutex();

    return *lock;
}

static std::vector<IrcLogSink*>& registry() {
    static std::vector<IrcLogSink*>* const sinks = new std::vector<IrcLogSink*>();

    return *sinks;
}

IrcLogSink::IrcLogSink(xServer* server, std::string channel, bool highlight,
                       const std::string& rate)
    : server(server), channel(std::move(channel)), highlight(highlight), droppedCount(0) {
    std::size_t count = 0;
    std::chrono::seconds per(0);

    /* A rate the configuration parser already accepted; anything else - a rate
     * from code, say - is no limit rather than a wrong one */
    if (!rate.empty() && LogRateLimit::parse(rate, count, per))
        limit.emplace(count, per);

    const std::lock_guard<std::mutex> guard(registryLock());

    registry().push_back(this);
}

IrcLogSink::~IrcLogSink() {
    const std::lock_guard<std::mutex> guard(registryLock());

    std::vector<IrcLogSink*>& sinks = registry();

    sinks.erase(std::remove(sinks.begin(), sinks.end(), this), sinks.end());
}

void IrcLogSink::setMainThread() {
    mainThreadId = std::this_thread::get_id();
    mainThreadKnown.store(true);
}

bool IrcLogSink::isMainThread() {
    if (!mainThreadKnown.load())
        return true;

    return std::this_thread::get_id() == mainThreadId;
}

void IrcLogSink::emit(const LogRecord& record) {
    if (!isMainThread()) {
        const std::lock_guard<std::mutex> guard(lock);

        if (queue.size() >= maxQueuedRecords) {
            ++droppedCount;
            return;
        }

        queue.push_back(record);

        return;
    }

    deliver(record);
}

void IrcLogSink::flush() {
    std::deque<LogRecord> pending;
    std::size_t lost = 0;

    {
        const std::lock_guard<std::mutex> guard(lock);

        pending.swap(queue);

        // What this flush has to own up to: one flush says what it lost, and
        // no flush says it twice
        lost = droppedCount - reportedCount;
        reportedCount = droppedCount;
    }

    // The lock is gone: nothing of this sink is held while notices are sent
    for (const LogRecord& record : pending)
        deliver(record);

    /* What was thrown away is worth one record of its own, once per flush and
     * after the queue is empty.  This runs on the main thread and outside any
     * dispatch, so the record below is delivered rather than queued, and the
     * counters it reports are already settled: it cannot make work for itself */
    if (0 != lost)
        LOG_TO(LogManager::get("core"), WARN,
               "A channel log sink dropped {} records that arrived from other threads faster "
               "than the main loop could send them",
               lost);
}

void IrcLogSink::forgetServer(const xServer* gone) {
    const std::lock_guard<std::mutex> guard(registryLock());

    for (IrcLogSink* const sink : registry()) {
        const std::lock_guard<std::mutex> sinkGuard(sink->lock);

        if (sink->server == gone)
            sink->server = nullptr;
    }
}

void IrcLogSink::flushAll() {
    std::vector<std::shared_ptr<IrcLogSink>> sinks;

    {
        const std::lock_guard<std::mutex> guard(registryLock());

        const std::vector<IrcLogSink*>& live = registry();

        sinks.reserve(live.size());

        for (IrcLogSink* const sink : live) {
            // A sink whose last owner is gone is on its way out: taking a
            // share of it now would resurrect it
            std::shared_ptr<IrcLogSink> owned = sink->weak_from_this().lock();

            if (nullptr != owned)
                sinks.push_back(std::move(owned));
        }
    }

    // The registry lock is gone, and every sink below is held for the whole of
    // its flush: a notice of one of them may destroy anything but these
    for (const std::shared_ptr<IrcLogSink>& sink : sinks)
        sink->flush();
}

/**
 * The channels that were found missing and said to be, lower-cased.  Of the
 * process and not of a sink: a reload makes a new sink for the same channel, and
 * that is no reason to say it again.  A channel is taken out when it is found,
 * so one that disappears later is news once more.  Main thread only, as
 * deliver() is, and never destroyed, like the registry.
 */
static std::set<std::string>& missingChannels() {
    static std::set<std::string>* const channels = new std::set<std::string>();

    return *channels;
}

/**
 * A server notice goes to a channel the network has, and a channel nobody is in
 * is one it does not have: the record is lost, which the file that names the
 * channel gives no hint of.  Said once per channel, to the other sinks - this
 * one hears it too, finds the channel in the set, and says nothing.
 *
 * Not before the uplink's burst is over: the channel may be on its way, and the
 * records of start-up are lost to a channel sink whatever it is given.
 */
static void sayChannelIsMissing(const xServer& theServer, const std::string& theChannel) {
    if (0 == theServer.getBurstEnd() || theServer.isBursting())
        return;

    if (!missingChannels().insert(string_lower(theChannel)).second)
        return;

    LOG_TO(LogManager::get("core"), WARN,
           "The log channel {} does not exist on the network, so what is logged to it is lost: "
           "a channel exists while somebody, a bot of ours included, is in it",
           theChannel);
}

void IrcLogSink::deliver(const LogRecord& record) {
    std::string theChannel;
    bool theHighlight = false;
    xServer* theServer = nullptr;

    {
        const std::lock_guard<std::mutex> guard(lock);

        theChannel = channel;
        theHighlight = highlight;
        theServer = server;
    }

    // Nowhere to put the record: the logging system says nothing about it
    if (nullptr == theServer || !theServer->isConnected() || theChannel.empty())
        return;

    if (nullptr == Network)
        return;

    Channel* const theChan = Network->findChannel(theChannel);

    if (nullptr == theChan) {
        sayChannelIsMissing(*theServer, theChannel);

        return;
    }

    if (!missingChannels().empty())
        missingChannels().erase(string_lower(theChannel));

    /* The rate limit, taken off here and nowhere else: what is counted is a
     * RECORD, however many notices it is about to become, and only a record
     * that really would have gone to the channel */
    if (limit) {
        if (!limit->admit())
            return;

        const std::size_t suppressed = limit->takeSuppressed();

        /* What the limit refused, said once in front of the record that got
         * through, and looking like every other line of this channel: one
         * synthetic WARN record on "core", through the same formatter */
        if (0 != suppressed) {
            LogRecord notice;

            notice.time = std::chrono::system_clock::now();
            notice.level = WARN;
            notice.logger = "core";
            notice.message = std::to_string(suppressed) +
                             " log records were not sent to this channel (rate limit " +
                             limit->describe() + ")";

            theServer->serverNotice(
                theChan, formatIrcLine(notice, notice.message, notice.spans, theHighlight));
        }
    }

    for (const std::pair<std::string, std::vector<LogSpan>>& line : splitLines(record)) {
        // A std::string, never a format: a '%' or a '{}' of a log message is
        // text, and picking the CheckedFormat overload would read it as syntax
        const std::string notice = formatIrcLine(record, line.first, line.second, theHighlight);

        theServer->serverNotice(theChan, notice);
    }
}

void IrcLogSink::setChannel(std::string newChannel) {
    const std::lock_guard<std::mutex> guard(lock);

    channel = std::move(newChannel);
}

std::string IrcLogSink::getChannel() const {
    const std::lock_guard<std::mutex> guard(lock);

    return channel;
}

void IrcLogSink::setHighlight(bool newHighlight) {
    const std::lock_guard<std::mutex> guard(lock);

    highlight = newHighlight;
}

std::size_t IrcLogSink::dropped() const {
    const std::lock_guard<std::mutex> guard(lock);

    return droppedCount;
}

} // namespace gnuworld
