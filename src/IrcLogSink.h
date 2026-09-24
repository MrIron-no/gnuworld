/**
 * IrcLogSink.h
 * The sink that writes log records to an IRC channel as server notices.
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

#ifndef __IRCLOGSINK_H
#define __IRCLOGSINK_H

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "LogRateLimit.h"
#include "LogRecord.h"
#include "LogSink.h"

namespace gnuworld {

class xServer;

/**
 * One IRC channel a log is mirrored to.  Each non-empty line of a record's
 * message becomes one server notice, formatted by formatIrcLine().
 *
 * Writing to the network is only safe from the thread that owns the server
 * connection, so mainThreadOnly() is true: a record that arrives on another
 * thread is copied into a queue of this sink and delivered by flushAll() from
 * the main loop, in the order it was logged.  At most maxQueuedRecords records
 * wait there; a queue that is full drops what arrives, and counts it.
 *
 * suppressOnReentry() is true as well: sending a notice logs on its own
 * account (the write path, the protocol layer), and a sink that took those
 * records would feed itself.
 *
 * A sink may have a rate limit, which it has not got unless the configuration
 * asked for one: beyond it a record is dropped and counted, and the next record
 * that does go out is preceded by one notice saying how many did not.
 *
 * No mutex of this class is ever held while a notice is sent.  The queue is
 * swapped out under the lock and delivered after it is released, and the
 * channel and the highlight flag are copied the same way, so that a rehash may
 * change them while records are on their way out.
 *
 * An instance of this class is always owned by a std::shared_ptr, which is how
 * a logger holds a sink: the logger keeps one alive for the whole of its own
 * emit(), and flushAll() does the same for the whole of its flush.
 *
 * Instances register themselves in a process-wide list, which is what
 * flushAll() walks.  Sinks are created and destroyed on the main thread, and
 * that is the only concurrency flushAll() is safe against: a sink whose last
 * owner is already gone, or one that goes while an earlier sink's notice is
 * being sent, is skipped rather than dereferenced.  Creating or destroying one
 * from a worker thread is not supported.
 */
class IrcLogSink : public LogSink, public std::enable_shared_from_this<IrcLogSink> {
  public:
    /// How many records one sink queues for the main loop before dropping them
    static constexpr std::size_t maxQueuedRecords = 1000;

    /**
     * Mirrors to channel on server, which may be null and may be unconnected:
     * the sink drops records for as long as there is nowhere to put them.
     * highlight puts the substituted values of a message in bold.
     *
     * rate is a limit of the shape "5/min" (LogRateLimit::parse), and empty -
     * the default - is no limit at all, which is what this sink has always had.
     * Beyond the limit a RECORD is dropped, however many lines it would have
     * been, and counted; the next record that does go out is preceded by one
     * notice saying how many did not.
     */
    IrcLogSink(xServer* server, std::string channel, bool highlight,
               const std::string& rate = std::string());

    /// Leaves the process-wide list; whatever was still queued is forgotten
    ~IrcLogSink() override;

    void emit(const LogRecord&) override;

    bool mainThreadOnly() const override { return true; }

    bool suppressOnReentry() const override { return true; }

    bool leavesTheHost() const override { return true; }

    /**
     * Remembers the calling thread as the one that may write to the network.
     * Called once, from the main thread, at start-up; until it is, every
     * thread is taken for the main one and nothing is ever queued.
     */
    static void setMainThread();

    /**
     * Delivers what every live sink has queued, oldest record first.  Called
     * from the main loop, and only from the main thread.
     */
    static void flushAll();

    /**
     * Makes every sink that writes through this server forget it, so that a
     * record that arrives afterwards is dropped like one for a server that is
     * not connected.  The server calls this when it is destroyed: main() makes
     * a new one to reconnect, and a sink the configuration attached outlives
     * the server it was made for until the new one configures logging again.
     * Main thread only, like everything that touches the server.
     */
    static void forgetServer(const xServer* server);

    /// The channel notices go to, which a rehash may change
    void setChannel(std::string);

    /// A copy of the channel name, taken under this sink's lock
    std::string getChannel() const;

    /// Whether the substituted values of a message are sent in bold
    void setHighlight(bool);

    /// How many records this sink had to drop because its queue was full
    std::size_t dropped() const;

  private:
    /// Sends one record to the channel, from the main thread, holding no lock
    void deliver(const LogRecord&);

    /// Swaps this sink's queue out and delivers it
    void flush();

    /// True when the calling thread may write to the network
    static bool isMainThread();

    xServer* server;

    mutable std::mutex lock;
    std::string channel;
    bool highlight;
    std::deque<LogRecord> queue;
    std::size_t droppedCount;

    /**
     * How many records a minute this channel is worth, and what the limit
     * refused.  Nothing at all when the file gave no rate, which is what every
     * configuration written before there was one says, and means no limit.
     *
     * The limit is taken off in deliver(), where a record really does go to a
     * channel: a record dropped because nothing is connected is not one this
     * limit has to account for.
     */
    std::optional<LogRateLimit> limit;

    /// How many of those a flush has already said were dropped
    std::size_t reportedCount = 0;
};

} // namespace gnuworld

#endif // __IRCLOGSINK_H
