/**
 * LogRateLimit.h
 * The token bucket a sink that talks to the outside world keeps: how many
 * records a minute may leave this process through it, and how many it had to
 * refuse while the bucket was empty.
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

#ifndef __LOGRATELIMIT_H
#define __LOGRATELIMIT_H

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace gnuworld {

/**
 * A rate limit of the shape a configuration file writes it, "10/min": at most
 * count records per period, refilled continuously rather than in windows, so
 * that a burst is spread out instead of being dropped to the last record of
 * every minute.
 *
 * The bucket starts FULL: the first thing a sink is asked to deliver goes out,
 * which is what an operator who set a limit of one an hour means by it.
 *
 * Everything here is thread-safe.  A sink's emit() is called from whatever
 * thread logged the record, and its own worker asks the same bucket, so this
 * takes its own lock and holds it for nothing but the arithmetic.
 *
 * A refused record is counted, and takeSuppressed() hands that count over and
 * forgets it: that is how the next record a sink does deliver can say how many
 * stood behind it, without the count being reported twice.
 *
 * This is header-only and names nothing outside the standard library on
 * purpose: it is used by libgnuworld's own sinks, by core's channel sink and by
 * the notifier, which are three different libraries.
 */
class LogRateLimit {
  public:
    using clock = std::chrono::steady_clock;

    /**
     * At most count records per per, the bucket full to begin with.  A count of
     * nothing and a period of nothing are not rates: they are taken as one and
     * as a second, so that a bucket built from a value parse() refused is still
     * a bucket and not a division by zero.
     */
    LogRateLimit(std::size_t count, std::chrono::seconds per)
        : capacity(0 == count ? 1 : count), period(per.count() < 1 ? std::chrono::seconds(1) : per),
          tokens(static_cast<double>(0 == count ? 1 : count)) {}

    /**
     * Whether this record may go out, taking a token when it may and counting
     * the refusal when it may not.
     *
     * now is a parameter so that a test can inject the clock; a caller that
     * does not care lets it default to the moment of the call.  A now earlier
     * than the last one this was asked about - a caller replaying a moment, not
     * a clock that jumped, because steady_clock does not - adds no tokens and
     * takes none away.
     */
    bool admit(clock::time_point now = clock::now()) {
        const std::lock_guard<std::mutex> guard(lock);

        /* The bucket's clock starts at the first record, not at construction:
         * a sink built while a configuration is read and first written to a
         * second later would otherwise count that second as refill */
        if (!started) {
            started = true;
            refilledAt = now;
        } else if (now > refilledAt) {
            /* Multiplied before it is divided, so that exactly one period's
             * worth of time really is exactly capacity tokens */
            const double elapsed = std::chrono::duration<double>(now - refilledAt).count();
            const double gained =
                elapsed * static_cast<double>(capacity) / static_cast<double>(period.count());

            tokens = tokens + gained;

            if (tokens > static_cast<double>(capacity))
                tokens = static_cast<double>(capacity);

            refilledAt = now;
        }

        if (tokens >= 1.0) {
            tokens -= 1.0;

            return true;
        }

        ++suppressed;

        return false;
    }

    /// How many records this bucket refused since the last time it was asked
    std::size_t takeSuppressed() {
        const std::lock_guard<std::mutex> guard(lock);

        const std::size_t refused = suppressed;

        suppressed = 0;

        return refused;
    }

    /**
     * The rate as a message says it: "10/min".  This is the rate itself and not
     * the text a file happened to write it as, so nothing a file wrote is
     * quoted back into a log record or a notification.
     */
    std::string describe() const {
        const std::string number = std::to_string(capacity);

        switch (period.count()) {
        case 1:
            return number + "/sec";
        case 60:
            return number + "/min";
        case 3600:
            return number + "/hour";
        default:
            return number + "/" + std::to_string(period.count()) + "s";
        }
    }

    /**
     * Reads a rate of the shape "<N>/min", "<N>/hour" or "<N>/sec", with any
     * amount of space around it and around the slash, and the unit in any case.
     * Nothing else is a rate: a count of zero, a negative one, a count without
     * a unit, a unit this does not know, and a count larger than anyone means
     * are all refused, and then count and per are left as they were.
     */
    static bool parse(const std::string& text, std::size_t& count, std::chrono::seconds& per) {
        /// The largest rate this reads; anything above it is a typo, not a rate
        const std::size_t maximumCount = 1000000000;

        const std::string::size_type slash = text.find('/');

        if (std::string::npos == slash)
            return false;

        const std::string digits = trimmed(text.substr(0, slash));
        const std::string unit = lowered(trimmed(text.substr(slash + 1)));

        if (digits.empty())
            return false;

        std::size_t value = 0;

        for (const char c : digits) {
            if (c < '0' || c > '9')
                return false;

            value = value * 10 + static_cast<std::size_t>(c - '0');

            if (value > maximumCount)
                return false;
        }

        if (0 == value)
            return false;

        if ("sec" == unit)
            per = std::chrono::seconds(1);
        else if ("min" == unit)
            per = std::chrono::seconds(60);
        else if ("hour" == unit)
            per = std::chrono::seconds(3600);
        else
            return false;

        count = value;

        return true;
    }

  private:
    /// The text without the blanks at either end of it
    static std::string trimmed(const std::string& text) {
        static const std::string blanks(" \t\r\n\v\f");

        const std::string::size_type begin = text.find_first_not_of(blanks);

        if (std::string::npos == begin)
            return std::string();

        return text.substr(begin, text.find_last_not_of(blanks) - begin + 1);
    }

    /// The text in lower case, for the unit, which is read in any case
    static std::string lowered(const std::string& text) {
        std::string folded;
        folded.reserve(text.size());

        for (const char c : text)
            folded += static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);

        return folded;
    }

    mutable std::mutex lock;

    /// How many tokens the bucket holds at most, and over what time it fills
    const std::size_t capacity;
    const std::chrono::seconds period;

    /// What is in it now, which is fractional because the refill is continuous
    double tokens;

    /// When the tokens above were worked out, and whether that has happened
    clock::time_point refilledAt;
    bool started = false;

    /// How many records were refused since takeSuppressed() last said
    std::size_t suppressed = 0;
};

} // namespace gnuworld

#endif // __LOGRATELIMIT_H
