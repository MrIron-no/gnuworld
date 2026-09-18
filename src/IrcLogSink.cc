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
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Channel.h"
#include "IrcLogSink.h"
#include "LogFormat.h"
#include "LogRecord.h"
#include "Network.h"
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

IrcLogSink::IrcLogSink(xServer* server, std::string channel, bool highlight)
    : server(server), channel(std::move(channel)), highlight(highlight), droppedCount(0) {
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

    {
        const std::lock_guard<std::mutex> guard(lock);

        pending.swap(queue);
    }

    // The lock is gone: nothing of this sink is held while notices are sent
    for (const LogRecord& record : pending)
        deliver(record);
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

void IrcLogSink::deliver(const LogRecord& record) {
    std::string theChannel;
    bool theHighlight = false;

    {
        const std::lock_guard<std::mutex> guard(lock);

        theChannel = channel;
        theHighlight = highlight;
    }

    // Nowhere to put the record: the logging system says nothing about it
    if (nullptr == server || !server->isConnected() || theChannel.empty())
        return;

    if (nullptr == Network)
        return;

    Channel* const theChan = Network->findChannel(theChannel);

    if (nullptr == theChan)
        return;

    for (const std::pair<std::string, std::vector<LogSpan>>& line : splitLines(record)) {
        // A std::string, never a format: a '%' or a '{}' of a log message is
        // text, and picking the CheckedFormat overload would read it as syntax
        const std::string notice = formatIrcLine(record, line.first, line.second, theHighlight);

        server->serverNotice(theChan, notice);
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
