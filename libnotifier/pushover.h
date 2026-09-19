/**
 * pushover.h
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

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "LogRateLimit.h"
#include "LogRecord.h"
#include "gnuworld_config.h"
#include "notifier.h"

#ifdef USE_THREAD
#include "threadworker.h"
#endif

namespace gnuworld {

class LogSink;

struct SinkSpec;

typedef std::vector<std::string> pushoverKeysType;

/**
 * A pager: one Pushover notification per log record it is given.
 *
 * This is a kind of sink logging.conf names, "sink.<id>.type = pushover", so it
 * may be attached to any logger - the root included - which is what the three
 * protections below are for.
 *
 * A LEVEL OF ITS OWN.  A pager nobody asked for a level of is at ERROR, not at
 * TRACE like every other sink: nobody wants a notification per protocol
 * message.  The threshold is the sink's own, because the level the file gave -
 * or did not give - is applied by LogManager::configure() before this class
 * hears about it.
 *
 * A LOOP GUARD.  This sink's own delivery failures are logged on
 * "core.notifier", and with a worker thread they are logged from that thread,
 * where the logger's thread-local re-entrancy guard does not apply.  A record
 * of "core.notifier", or of anything below it, is therefore dropped here: that
 * is what keeps a pushover sink attached to the root from paging about its own
 * failure to page, for ever.  Those failures are themselves reported at most
 * once a minute per sink, with a count of what they stand for, so that a dead
 * network cannot fill a log file either.
 *
 * A RATE LIMIT.  At most rateCount notifications per ratePer leave this sink;
 * what the limit refuses is counted, and the next notification that does go out
 * is preceded by one saying how many stood behind it.
 *
 * ITS OWN WORKER.  With USE_THREAD every request runs on a thread this object
 * owns and joins, so nothing of this class can run after it is gone.  The
 * worker is declared LAST of the members on purpose: members are destroyed in
 * reverse order of declaration, so the worker - which drains its queue and
 * joins its thread - is destroyed first, while everything a queued job touches
 * is still alive.  ~PushoverClient() asks the queued jobs to do nothing at all
 * before that, so a dead endpoint cannot make a destructor take a minute.
 *
 * NO SECRET IS EVER LOGGED.  The token and the user keys reach nothing but the
 * request body: a record or an error about a user names its position in the
 * list ("user #1"), and about the token nothing at all.
 */
class PushoverClient : public notifier {

  public:
    /// The endpoint of the real service, which is what a sink uses by default
    static const char* defaultUrl() { return "https://api.pushover.net/1/messages.json"; }

    /// The type name logging.conf knows this kind of sink under
    static const char* typeName() { return "pushover"; }

    /**
     * A pager for these user keys with this token, sending to url (empty: the
     * real service), dropping records less important than threshold, and at
     * most rateCount of them per ratePer.
     */
    PushoverClient(std::string token, pushoverKeysType users, std::string url = std::string(),
                   Verbosity threshold = ERROR, std::size_t rateCount = 10,
                   std::chrono::seconds ratePer = std::chrono::seconds(60));

    /// Drains the worker without running what is still queued, and joins it
    ~PushoverClient() override;

    /**
     * Makes the sink a "sink.<id>.type = pushover" line asks for, or says why
     * it cannot be made - which refuses the whole configuration file.
     *
     * The options it reads are token (required), userkey (required, a comma
     * separated list), rate (default 10/min) and url (default the service's);
     * the common level and highlight are the parser's, and any other option is
     * an error naming the key and not the value.
     */
    static std::shared_ptr<LogSink> makeSink(const SinkSpec& spec, std::string& error);

    /// Teaches logging.conf this kind of sink; asking again replaces it
    static void registerSinkType();

    [[nodiscard]] size_t getSuccessful() const override { return statSuccessful.load(); }

    [[nodiscard]] size_t getErrors() const override { return statErrors.load(); }

    [[nodiscard]] size_t userKeys_size() const { return userKeys.size(); }

    /**
     * Delivers one log record as a notification: the logger it was logged on
     * and its level make the title, the sentence behind the function it came
     * from - at every level but INFO - the body.
     */
    void emit(const LogRecord& r) override;

    bool sendMessage(int level, const std::string message) override;

    bool sendMessage(const std::string title, const std::string message);

    bool sendMessage(const std::string title, const std::string message, int priority,
                     int retry = 60, int expire = 3600);

  private:
    /// Whether this logger is core.notifier, or anything below it
    static bool isNotifierLogger(const std::string& logger);

#ifdef HAVE_LIBCURL
    void initialise_curl();
#endif

    std::string truncate(const std::string input, std::size_t maxLength) const {
        return input.size() <= maxLength ? input : input.substr(0, maxLength - 3) + "...";
    }

    /// Queues the message on the worker, or sends it where there is no worker
    bool queueMessage(const std::string& title, const std::string& message, int priority, int retry,
                      int expire);

    bool processMessage(const std::string title, const std::string message, int priority, int retry,
                        int expire);

    /**
     * Sends to the user at this position of the list, which is what a record
     * about it names: a user key is never logged.
     */
    bool sendToUser(std::size_t position, const std::string user, const std::string title,
                    const std::string message, int priority, int retry, int expire);

    /// Logs one delivery failure on core.notifier, at most one a minute
    void reportFailure(const std::string& what);

    const std::string apiToken;
    const pushoverKeysType userKeys;
    const std::string apiUrl;

    /// The least important record this sink pages about
    const Verbosity threshold;

    std::atomic<std::size_t> statSuccessful{0};
    std::atomic<std::size_t> statErrors{0};

    /// How many notifications may leave this sink, and what it refused
    LogRateLimit rateLimit;

    /// One delivery failure on core.notifier a minute, and no more
    LogRateLimit failureReports;

#ifdef USE_THREAD
    /**
     * Set by the destructor before the worker below is joined: a job that has
     * not started yet then does nothing at all, so that destroying this sink
     * with fifty requests queued costs nothing rather than fifty timeouts.
     */
    std::atomic<bool> stopping{false};

    /**
     * The thread every request runs on, owned by this sink and joined by it.
     * LAST of the members on purpose: it is therefore the FIRST to be
     * destroyed, so its queue is drained and its thread joined while
     * everything a job touches - the token, the keys, the counters, the
     * limits - is still there.  Nothing of this class can outlive it.
     */
    ThreadWorker threadWorker;
#endif
};

} // namespace gnuworld
