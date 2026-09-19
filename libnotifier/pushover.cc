/**
 * pushover.cc
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

#include "defs.h"
#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "LogConfig.h"
#include "LogManager.h"
#include "LogRateLimit.h"
#include "LogSink.h"
#include "logger.h"
#include "pushover.h"
#include "threadworker.h"

GNUWORLD_MODULE_LOGGER("core.notifier");

namespace gnuworld {

namespace {

/// The logger this sink reports its own failures on, and takes nothing from
const std::string notifierLogger("core.notifier");

/// What truncateUtf8() puts where it cut
const std::string ellipsis("...");

/// Whether this byte is one that continues a UTF-8 character begun before it
bool isContinuationByte(char byte) { return 0x80 == (static_cast<unsigned char>(byte) & 0xC0); }

/**
 * at, or the first position before it that begins a UTF-8 character.
 *
 * Nothing but the beginning of the string stops the walk back, so a string of
 * continuation bytes - which is not UTF-8 at all - ends it at 0 rather than
 * before it.
 */
std::size_t characterBoundaryAt(const std::string& input, std::size_t at) {
    while (0 != at && at < input.size() && isContinuationByte(input[at]))
        --at;

    return at;
}

#ifdef HAVE_LIBCURL
/**
 * Throws the endpoint's answer away.
 *
 * Without a write callback of its own libcurl writes what it received to
 * STDOUT: every notification would put Pushover's {"status":1,...} on the
 * terminal of whoever started this process, from a worker thread, in the
 * middle of whatever else is there.  Nothing here reads the body - the HTTP
 * status is what says whether a notification was accepted.
 */
std::size_t discardResponse(char*, std::size_t size, std::size_t count, void*) {
    return size * count;
}

/// One field of a form body: a literal name, and a value that is anything at all
typedef std::pair<const char*, std::string> formFieldType;

/**
 * value percent-encoded, or nothing at all.
 *
 * curl_easy_escape() is given the length, so a value holding a NUL byte is
 * encoded rather than cut at it, and its answer is freed with curl_free().  It
 * returns a null pointer when it cannot allocate: there is no answer to that
 * but to fail the delivery, which is why this says so with false rather than
 * handing back what it was given.
 */
bool escapeValue(CURL* curl, const std::string& value, std::string& escaped) {
    char* const encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));

    if (nullptr == encoded)
        return false;

    escaped = encoded;

    curl_free(encoded);

    return true;
}

/**
 * A libcurl easy handle and the header list of its one request, freed however
 * this request is left.
 *
 * Building the body can now fail - a value libcurl will not encode is a
 * delivery that does not happen - and the handle is needed before the body is
 * built, because it is what escapes the values.  A throw between the two would
 * otherwise leak the handle once per notification.
 */
class Request {
  public:
    Request() : handle(curl_easy_init()) {}

    ~Request() {
        if (nullptr != headers)
            curl_slist_free_all(headers);

        if (nullptr != handle)
            curl_easy_cleanup(handle);
    }

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    CURL* get() const { return handle; }

    /// Adds one header to this request, and keeps the list to free it
    void addHeader(const char* header) {
        headers = curl_slist_append(headers, header);

        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    }

  private:
    CURL* handle;
    struct curl_slist* headers = nullptr;
};

/**
 * The fields as an application/x-www-form-urlencoded body.
 *
 * EVERY value is percent-encoded, which is the whole point of this function.  A
 * notification's title and message are a rendered log sentence, and a log
 * sentence holds whatever a remote server or an IRC user put into it: a channel
 * name may legally contain "&", "=", "%", "#" and "+", so a value written into
 * the body as it stands is a value that can end its own field and begin another
 * one - a second "user" redirecting the page, with this deployment's own token,
 * to somebody else's account, or an "html", a "url", a "sound" nobody
 * configured.  The NAMES are literals of this file and never come from anywhere
 * else, so only the values need encoding.
 *
 * Throws when libcurl cannot encode a value: a body with one unescaped field in
 * it is worse than a notification that was not sent.
 */
std::string formBody(CURL* curl, const std::vector<formFieldType>& fields) {
    std::ostringstream body;

    for (const formFieldType& field : fields) {
        std::string escaped;

        if (!escapeValue(curl, field.second, escaped))
            throw std::runtime_error("curl_easy_escape failed");

        if (0 != body.tellp())
            body << '&';

        body << field.first << '=' << escaped;
    }

    return body.str();
}

/// The parts of a comma separated list, each without the space around it
std::vector<std::string> splitList(const std::string& value) {
    static const std::string blanks(" \t\r\n\v\f");

    std::vector<std::string> parts;
    std::string::size_type at = 0;

    for (;;) {
        const std::string::size_type comma = value.find(',', at);
        const std::string piece =
            std::string::npos == comma ? value.substr(at) : value.substr(at, comma - at);

        const std::string::size_type begin = piece.find_first_not_of(blanks);

        parts.push_back(std::string::npos == begin
                            ? std::string()
                            : piece.substr(begin, piece.find_last_not_of(blanks) - begin + 1));

        if (std::string::npos == comma)
            break;

        at = comma + 1;
    }

    return parts;
}
#endif // HAVE_LIBCURL

} // namespace

std::string truncateUtf8(const std::string& input, std::size_t limit) {
    if (input.size() <= limit)
        return input;

    /* No room to both say something and say that something was cut: what fits,
     * and nothing to mark it with */
    if (limit <= ellipsis.size())
        return input.substr(0, characterBoundaryAt(input, limit));

    return input.substr(0, characterBoundaryAt(input, limit - ellipsis.size())) + ellipsis;
}

std::string withoutControlCharacters(const std::string& input) {
    std::string cleaned;

    cleaned.reserve(input.size());

    for (const char byte : input) {
        const unsigned char value = static_cast<unsigned char>(byte);

        // The newline is a line of a multi-line record; the rest of C0, and DEL, are not
        if ((0x20 > value && '\n' != byte) || 0x7F == value)
            continue;

        cleaned.push_back(byte);
    }

    return cleaned;
}

PushoverClient::PushoverClient(std::string token, pushoverKeysType users, std::string url,
                               Verbosity threshold, std::size_t rateCount,
                               std::chrono::seconds ratePer)
    : apiToken(std::move(token)), userKeys(std::move(users)),
      apiUrl(url.empty() ? std::string(defaultUrl()) : std::move(url)), threshold(threshold),
      rateLimit(rateCount, ratePer), failureReports(1, std::chrono::seconds(60)) {}

PushoverClient::~PushoverClient() {
#ifdef USE_THREAD
    /* Whatever is still queued is asked to do nothing: the worker below is
     * destroyed - which drains its queue and joins its thread - before any
     * other member of this object is, so a job that runs now finds everything
     * it touches alive, and a job that has not started yet returns at once
     * rather than spending a timeout on an endpoint that is not answering */
    stopping.store(true);
#endif
}

#ifdef HAVE_LIBCURL
void PushoverClient::initialise_curl() {
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}
#endif

/**
 * Whether a record of this logger is one this sink must not touch.
 *
 * Its own delivery failures are logged on core.notifier, from the worker thread
 * where the logger's re-entrancy guard does not apply, so a sink attached to the
 * root would page about failing to page - and fail, and page.  Dropping every
 * record of that logger and of everything below it is what breaks the loop.
 */
bool PushoverClient::isNotifierLogger(const std::string& logger) {
    if (logger.size() < notifierLogger.size())
        return false;

    if (0 != logger.compare(0, notifierLogger.size(), notifierLogger))
        return false;

    return logger.size() == notifierLogger.size() || '.' == logger[notifierLogger.size()];
}

void PushoverClient::emit(const LogRecord& r) {
    // The loop guard: this sink's own logger, and everything below it
    if (isNotifierLogger(r.logger))
        return;

    /* A pager's own threshold.  logging.conf's "level" is applied before a
     * record ever gets here, but a file that gave none leaves that at TRACE,
     * and TRACE is no level for a pager */
    if (r.level > threshold)
        return;

    sendMessage(std::format("[{}] {}", r.logger.empty() ? std::string("root") : r.logger,
                            levelName(r.level)),
                (r.level == INFO ? std::string() : r.function + "> ") + r.message);
}

bool PushoverClient::sendMessage(int level, const std::string message) {
    /* A direct caller has no record to name a logger, so the title says only
     * which process the notification comes from */
    return sendMessage(std::format("[gnuworld] {}", levelName(static_cast<Verbosity>(level))),
                       message);
}

bool PushoverClient::sendMessage(const std::string title, const std::string message) {
    return sendMessage(title, message, 0, 60, 3600);
}

/**
 * One notification, if the rate limit lets it through.
 *
 * What the limit refused is counted by the limit itself, and the next
 * notification that does go out is preceded by one saying how many did not: a
 * pager that goes quiet under a flood would otherwise say nothing about the
 * flood.  That summary costs no token of its own - it is about the limit, not a
 * record of its own - and it is sent before the record it stands in front of.
 */
bool PushoverClient::sendMessage(const std::string title, const std::string message, int priority,
                                 int retry, int expire) {
    if (!rateLimit.admit())
        return false;

    const std::size_t suppressed = rateLimit.takeSuppressed();

    if (0 != suppressed)
        queueMessage("[gnuworld] suppressed",
                     std::to_string(suppressed) +
                         " messages were suppressed by the rate limit of " + rateLimit.describe(),
                     priority, retry, expire);

    return queueMessage(title, message, priority, retry, expire);
}

bool PushoverClient::queueMessage(const std::string& title, const std::string& message,
                                  int priority, int retry, int expire) {
#ifdef USE_THREAD
    /* On this sink's own worker, which this sink joins: a job cannot outlive
     * the object it was queued on, and one queued while the object is going
     * away does nothing at all */
    threadWorker.submitJob([this, title, message, priority, retry, expire]() {
        if (stopping.load())
            return;

        this->processMessage(title, message, priority, retry, expire);
    });

    return true; // Assume success for async operations
#else
    const bool ok = processMessage(title, message, priority, retry, expire);

    if (!ok) {
        /* Logged on core.notifier, which this sink takes no record of: see
         * isNotifierLogger() for why that is what keeps it from feeding itself */
        reportFailure(std::format("Failed to send a notification titled: {}", title));
        ++statErrors;
    }

    return ok;
#endif
}

bool PushoverClient::processMessage(const std::string title, const std::string message,
                                    int priority, int retry, int expire) {
    bool allSucceeded = true;

    for (std::size_t at = 0; at < userKeys.size(); ++at) {
        const bool ok = sendToUser(at + 1, userKeys[at], title, message, priority, retry, expire);

        if (!ok) {
            /* A failure the logger hears about, on core.notifier and by the
             * user's POSITION in the list: a user key is a secret, and a log
             * file is read by whoever can read a log file.  At most one of
             * these a minute, each standing for however many it was not worth
             * repeating */
            reportFailure(std::format("Failed to send to user #{}", at + 1));
            ++statErrors;
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

/**
 * One ERROR on core.notifier, and no more than one a minute per sink.
 *
 * An endpoint that is not there fails once per record per user key, and a log
 * file is not the place to hear about every one of them: the record that is
 * logged says how many failures went unsaid behind it.
 */
void PushoverClient::reportFailure(const std::string& what) {
    if (!failureReports.admit())
        return;

    const std::size_t hidden = failureReports.takeSuppressed();

    if (0 == hidden)
        LOG(ERROR, "{}", what);
    else
        LOG(ERROR, "{} ({} further failures were not logged)", what, hidden);
}

bool PushoverClient::sendToUser([[maybe_unused]] std::size_t position,
                                [[maybe_unused]] const std::string user,
                                [[maybe_unused]] const std::string title,
                                [[maybe_unused]] const std::string message,
                                [[maybe_unused]] int priority, [[maybe_unused]] int retry,
                                [[maybe_unused]] int expire) {
#ifdef HAVE_LIBCURL
    try {
        initialise_curl();

        /* Cleaned, then cut on a character boundary, and only then escaped: the
         * limits are Pushover's documented ones (a title of up to 250
         * characters, a message of up to 1024) counted in BYTES, which is the
         * stricter reading of the two and the one that needs no decoding */
        const std::string safeTitle = truncateUtf8(withoutControlCharacters(title), 250);
        const std::string safeMessage = truncateUtf8(withoutControlCharacters(message), 1024);

        Request request;

        if (nullptr == request.get())
            throw std::runtime_error("curl_easy_init failed");

        std::vector<formFieldType> fields;

        fields.push_back(formFieldType("token", apiToken));
        fields.push_back(formFieldType("user", user));
        fields.push_back(formFieldType("title", safeTitle));
        fields.push_back(formFieldType("message", safeMessage));
        fields.push_back(formFieldType("priority", std::to_string(priority)));

        if (priority == 2) {
            retry = std::max(10, retry);
            expire = std::min(10800, expire);

            fields.push_back(formFieldType("retry", std::to_string(retry)));
            fields.push_back(formFieldType("expire", std::to_string(expire)));
        }

        /* Every value of it percent-encoded, so the body holds exactly these
         * fields whatever the sentence in it says.  No byte of it can be a NUL -
         * escapeValue() encodes one as %00 - so the copy CURLOPT_COPYPOSTFIELDS
         * makes of it, which is measured with strlen, is the whole body */
        const std::string body = formBody(request.get(), fields);

        CURL* const curl = request.get();

        request.addHeader("Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, apiUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);

        // Or libcurl writes the service's answer to this process's stdout
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &discardResponse);

        CURLcode rc = curl_easy_perform(curl);
        long status = 0;

        if (CURLE_OK == rc)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        if (rc != CURLE_OK)
            throw std::runtime_error(curl_easy_strerror(rc));

        /* An endpoint that answers, but not with a success, is a failure of
         * delivery like any other: a token the service rejected would otherwise
         * look like a notification that went out */
        if (status < 200 || status > 299)
            throw std::runtime_error("the endpoint answered " + std::to_string(status));

        ++statSuccessful;

        /* The user's position, never the key itself, and the title and the
         * sentence, which are the log record this notification was made of */
        LOG(TRACE, "Message sent to user #{} with title: {} and message: {}", position, safeTitle,
            safeMessage);

        return true;
    } catch (const std::exception& e) {
        reportFailure(std::format("Failed to send to user #{}: {}", position, e.what()));
    } catch (...) {
        reportFailure(std::format("Failed to send to user #{}: an unknown error", position));
    }
#endif
    return false;
}

/**
 * The sink a "sink.<id>.type = pushover" line asks for.
 *
 * Everything it says about what is wrong names a KEY of the file and never a
 * value: an option of this kind of sink may be a token or a user key, and this
 * error goes into a log file, where a secret has no business being.
 */
std::shared_ptr<LogSink> PushoverClient::makeSink([[maybe_unused]] const SinkSpec& spec,
                                                  std::string& error) {
    const std::string prefix = "sink." + spec.id + ".";

#ifndef HAVE_LIBCURL
    error = prefix + "type: pushover support is not compiled in (libcurl was not found at build "
                     "time)";

    return nullptr;
#else
    std::string token;
    pushoverKeysType users;
    std::string url;
    std::size_t rateCount = 10;
    std::chrono::seconds ratePer(60);

    for (const std::pair<const std::string, std::string>& option : spec.options) {
        if ("token" == option.first) {
            token = option.second;
        } else if ("userkey" == option.first) {
            users = splitList(option.second);
        } else if ("url" == option.first) {
            url = option.second;
        } else if ("rate" == option.first) {
            if (!LogRateLimit::parse(option.second, rateCount, ratePer)) {
                error =
                    prefix + option.first + ": not a rate, expected <N>/min, <N>/hour or <N>/sec";

                return nullptr;
            }
        } else {
            error = prefix + option.first + ": unknown setting for a pushover sink";

            return nullptr;
        }
    }

    if (token.empty()) {
        error = prefix + "token: a pushover sink needs an application token";

        return nullptr;
    }

    if (users.empty()) {
        error = prefix + "userkey: a pushover sink needs at least one user key";

        return nullptr;
    }

    for (const std::string& user : users)
        if (user.empty()) {
            error = prefix + "userkey: an empty user key in the list";

            return nullptr;
        }

    /* A pager nobody gave a level to is at ERROR: TRACE, which every other kind
     * of sink defaults to, is a notification per protocol message */
    const Verbosity level = spec.levelGiven ? spec.level : ERROR;

    return std::make_shared<PushoverClient>(token, users, url, level, rateCount, ratePer);
#endif
}

void PushoverClient::registerSinkType() {
    LogManager::registerSinkType(typeName(), &PushoverClient::makeSink);
}

} // namespace gnuworld
