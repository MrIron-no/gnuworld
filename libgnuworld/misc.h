/**
 * misc.h
 * Author: Daniel Karrels (dan@karrels.com
 * Purpose: This file contains a few miscellaneous methods.
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
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
 * $Id: misc.h,v 1.9 2007/12/27 20:45:15 kewlio Exp $
 */

#ifndef __MISC_H
#define __MISC_H "$Id: misc.h,v 1.9 2007/12/27 20:45:15 kewlio Exp $"

#include <string>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <iostream>
#include <chrono>

#include <cctype>
#include <cstring>
#include <cstdlib>

#include "match.h"

namespace gnuworld {
using std::string;

/**
 * Converts a character to its RFC1459 lowercase equivalent.
 * In addition to standard ASCII A-Z -> a-z, RFC1459 defines:
 *   [ -> {, ] -> }, \\ -> |, ^ -> ~
 * @param c The character to convert.
 * @return The RFC1459-lowercased character.
 */
unsigned char rfc1459_tolower(unsigned char c);

/**
 * Compares two strings using RFC1459 case-insensitive rules.
 * Returns -1 if a < b, 1 if a > b, 0 if equal (RFC1459-insensitive).
 * Shorter string is considered less if one is a prefix of the other.
 * @param a First string (std::string_view)
 * @param b Second string (std::string_view)
 * @return Comparison result: -1, 0, or 1
 */
int rfc1459_compare(std::string_view a, std::string_view b);

/**
 * Checks if two strings are equal under RFC1459 case-insensitive rules.
 * @param a First string (std::string_view)
 * @param b Second string (std::string_view)
 * @return true if equal (RFC1459-insensitive), false otherwise
 */
bool rfc1459_equal(std::string_view a, std::string_view b);

/**
 * Return 0 if the two strings are equivalent, according to
 * case insensitive searches.
 * Otherwise, it returns the comparison between
 * s1 and s2.
 */
int strcasecmp(const string&, const string&);

/**
 * Case insensitive comparison struct for use by STL structures/algorithms.
 */
struct noCaseCompare {
    inline bool operator()(const string& lhs, const string& rhs) const {
        return (strcasecmp(lhs, rhs) < 0);
    }
};

/**
 * rfc1459 comparison struct for use by STL structures/algorithms.
 */
struct rfc1459Compare {
    bool operator()(const std::string& a, const std::string& b) const {
        return rfc1459_compare(a, b) < 0;
    }
};

/**
 * A case insensitive binary predicate comparator for two
 * string's.
 */
struct eqstr {
    inline bool operator()(const string& s1, const string& s2) const {
        return (0 == strcasecmp(s1, s2));
    }
};

/**
 * A hashing operator for the system hash tables.
 * This is not used for now, since hash_map has been removed
 * from gnuworld completely.
 */
struct eHash {
    inline size_t operator()(const string& s) const {
        if (s.empty()) {
            return 0;
        }

        size_t __h = 0;
        for (const char* ptr = s.c_str(); *ptr; ++ptr) {
            __h = (5 * __h) + tolower(*ptr);
        }
        return __h;
    }
};

/**
 * A functor suitable for using in STL style containers which provides
 * wildcard matching routine.
 */
struct Match {
    inline bool operator()(const string& lhs, const string& rhs) const {
        return (match(lhs, rhs) < 0);
    }
};

/**
 * Return a copy of a given C++ string, whose characters
 * are all lower case.
 */
string string_lower(const string&);

/**
 * Return a copy of a given C++ string, whose
 * characters are all upper case.
 */
string string_upper(const string&);

/**
 * Convert all characters of a given C++ string to
 * lower case.
 */
void string_tolower(string&);

/**
 * Convert all characters of a given C++ string to
 * upper case.
 */
void string_toupper(string&);

/**
 * Examine a given C++ string and return true if it contains
 * a time specification, return false otherwise.
 */
bool IsTimeSpec(const string&);

/**
 * Examine a given C++ string and return true if it contains
 * all numeric characters, return false otherwise.
 */
bool IsNumeric(std::string_view);

/**
 * A channel name given with or without its '#': the name with it.
 */
string withChannelPrefix(const string& name);

/**
 * Returns the time which is given as #<d/h/m/s> as seconds
 */
time_t extractTime(string Length, unsigned int defaultUnits);

int atoi(const string&);

/* itoa:  convert n to characters in s */
string itoa(int n);

/**
 * Extract the parts of a *valid* nick!user@hostip address
 */
string extractNick(const string&);

string extractUser(const string&);

string extractNickUser(const string&);

string extractHostIP(const string&);

// Check for valid hostmask.
bool validUserMask(const string&);

bool validCIDRLength(const string&);

string fixAddress(const string&);

// Check if we have !at least! a *@hostip format
bool isUserHost(const string&);

bool isAllWildcard(const string&, bool checkfordots = false);

/* Truncate a > /64 IPv6 address to a /64 cidr address
 * or creates a between /32 - /64 valid cidr address
 */
unsigned char fixToCIDR64(string&);

// Same as above, just returns the fixed address. Easier to use many times
string fixToCIDR64(const string&);

// Constructs a 'generally valid' banmask for a [nick!]user@hostip address
string createBanMask(const string&);

/* Create a 24/64 cidr address (optionally wildcarded)
 * for an IPv4 or IPv6 address, or for a hostname
 * If the parameter is a user@host it will be returned correspondingly
 */
string createClass(const string&, bool wildcard = false);

/* Formats a timestamp into a "X Days, XX:XX:XX" from 'Now'. */
const string prettyDuration(int);

/* Comma separates thousands for a number (e.g. 1000 to 1,000) */
const string prettyNumber(int);

/* Formats a timestamp into %F %H:%M:%S */
const std::string prettyTime(const std::time_t&, bool = true);

/* Returns the number of milliseconds having lapsed from the startTime,
 * provided as an argument.
 */
template <typename Clock = std::chrono::high_resolution_clock,
          typename Duration = std::chrono::milliseconds>
long long elapsedMs(const typename Clock::time_point& startTime) {
    return std::chrono::duration_cast<Duration>(Clock::now() - startTime).count();
}

/* Returns the current timepoint. */
template <typename Clock = std::chrono::high_resolution_clock>
typename Clock::time_point currentTimePoint() {
    return Clock::now();
}

/* Converts a timepoint into a time_t epoch timestamp. */
template <typename Clock = std::chrono::high_resolution_clock,
          typename Duration = std::chrono::seconds>
time_t timePointToEpoch(const typename Clock::time_point& timePoint) {
    return std::chrono::duration_cast<Duration>(timePoint.time_since_epoch()).count();
}

int getCurrentGMTHour(); /* returns the current hour in GMT (00-23) */

/* General assemble parameters into one result string (C-style) */
const string TokenStringsParams(const char*, ...);

/**
 * Global method to replace ' with \' in strings for safe placement in
 * SQL statements.
 */
const std::string escapeSQLChars(const std::string&);

/**
 * Global method to replace wildcards (* and ?) with % and _ in strings for wildcard
 * searches in SQL statements.
 */
const std::string searchSQL(const std::string&);

/**
 * Takes as the first arugment a reference to a string containing modes (and args if any),
 * and as the second argument the modes (without args) to be stripped from the modestring.
 *
 * Example: stripModes("+ntkl key 12", "l") will amend the string to "+ntk key".
 */
void stripModes(std::string&, const std::string&);

/* Returns the memory usage of gnuworld in KB. */
size_t getMemoryUsage();

std::string compactToCanonical(const std::string&);

// Converts "12:AS:B2:3B..." back to "12asb23b..."
std::string canonicalToCompact(const std::string&);

bool isValidSHA256Fingerprint(const std::string&);

/* Returns the CPU time used by gnuworld in seconds. */
double getCPUTime();

/* Masks a string for logging purposes. Typically used for passwords. */
inline std::string mask(const std::string& s) { return std::string(s.size(), '*'); }

/* Escape a string for JSON logging. */
std::string escapeJsonString(const std::string& input);

/* Returns current UTC timestamp in ISO 8601 format. */
std::string getCurrentTimestamp();

/* Generates a unique gline tracking ID: "<prefix><epoch-seconds>-<counter>".
 * The counter is a per-process value that never resets, so combined with
 * the current time (taken at the moment of each call) the result is never
 * reused, even across a process restart.
 */
std::string generateGlineId(char prefix);

/**
 * IRCv3 server-time / @time tag value: YYYY-MM-DDThh:mm:ss.sssZ (UTC).
 */
std::string formatServerTime();

/**
 * The number that `text` is, or nothing.
 *
 * atoi() answers 0 for text that is not a number, stops quietly at the first
 * character it does not like, and is undefined on overflow.  For what arrives
 * from the network that is the wrong answer: 0 is a timestamp, the oldest one
 * there is, and the oldest timestamp wins every conflict.
 *
 * This accepts the whole of `text` or none of it: decimal digits, with a
 * leading '-' for a signed type, and nothing else.  No whitespace, no '+',
 * no "12abc", nothing that does not fit the type.
 *
 * Not constexpr: std::from_chars only becomes that in C++23.
 */
template <std::integral T> std::optional<T> parseNumber(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* const end = text.data() + text.size();
    const auto [parsedTo, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsedTo != end) {
        return std::nullopt;
    }
    return value;
}

namespace detail {

/**
 * The number of arguments a std::format string consumes: the highest
 * explicit index plus one if it numbers its fields ("{0} {1}"), and
 * otherwise the number of fields, counting one nested in another for a
 * dynamic width or precision ("{:{}}").  "{{" and "}}" are literal braces.
 */
constexpr std::size_t formatArgumentCount(std::string_view fmt) noexcept {
    std::size_t automatic = 0;
    std::size_t highestExplicit = 0;
    bool anyExplicit = false;

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '{') {
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
            ++i;
            continue;
        }

        std::size_t index = 0;
        bool hasIndex = false;
        std::size_t j = i + 1;
        for (; j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9'; ++j) {
            index = index * 10 + static_cast<std::size_t>(fmt[j] - '0');
            hasIndex = true;
        }
        if (hasIndex) {
            anyExplicit = true;
            if (index + 1 > highestExplicit) {
                highestExplicit = index + 1;
            }
        } else {
            ++automatic;
        }
        // A nested '{' is met by the loop as a field of its own
    }
    return anyExplicit ? highestExplicit : automatic;
}

} // namespace detail

namespace detail {

/**
 * The number of arguments a printf format consumes: one for every
 * conversion, and one more for each '*' that stands for a width or a
 * precision.  "%%" is a literal '%'.
 */
constexpr std::size_t printfArgumentCount(std::string_view fmt) noexcept {
    constexpr std::string_view flags = "-+#0";
    constexpr std::string_view lengths = "hlqjztL";
    constexpr std::string_view conversions = "diouxXeEfgGcspaA";
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };

    std::size_t count = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        std::size_t j = i + 1;
        if (j < fmt.size() && '%' == fmt[j]) {
            i = j;
            continue;
        }

        // %[flags][width][.precision][length]conversion.  Anything else is
        // a '%' in the text, as in "100% sure": the space flag is left out
        // for that reason.
        std::size_t stars = 0;
        while (j < fmt.size() && flags.find(fmt[j]) != std::string_view::npos) {
            ++j;
        }
        for (int part = 0; part < 2; ++part) {
            if (1 == part) {
                if (j >= fmt.size() || fmt[j] != '.') {
                    break;
                }
                ++j;
            }
            if (j < fmt.size() && '*' == fmt[j]) {
                ++stars;
                ++j;
            } else {
                while (j < fmt.size() && isDigit(fmt[j])) {
                    ++j;
                }
            }
        }
        while (j < fmt.size() && lengths.find(fmt[j]) != std::string_view::npos) {
            ++j;
        }
        if (j < fmt.size() && conversions.find(fmt[j]) != std::string_view::npos) {
            count += 1 + stars;
            i = j;
        }
    }
    return count;
}

/// A value as printf takes it: a string as a "const char*", which the
/// holder keeps alive for the call; anything it cannot take as "?".
template <typename T> auto printfHold(const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_arithmetic_v<U> || std::is_pointer_v<U> || std::is_enum_v<U>) {
        return value;
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::string(std::string_view(value));
    } else {
        return static_cast<const char*>("?");
    }
}

template <typename H> auto printfPass(const H& held) {
    if constexpr (std::is_same_v<H, std::string>) {
        return held.c_str();
    } else {
        return held;
    }
}

/// snprintf() into a string.  The format is not a literal here: it was
/// counted against the arguments when it was one, and comes from our own
/// tables when it was not.
template <typename... Held> std::string printfText(const char* format, const Held&... held) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
    const int length = std::snprintf(nullptr, 0, format, printfPass(held)...);
    if (length <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    std::snprintf(out.data(), out.size() + 1, format, printfPass(held)...);
#pragma GCC diagnostic pop
    return out;
}

} // namespace detail

/**
 * What the network functions take in place of "const char*, ...": a format
 * and, behind it, the arguments as a typed pack.  Either syntax will do:
 *
 *     Notice(theClient, "{} is not on {}", nick, channel);   // std::format
 *     Notice(theClient, "%s is not on %s", nick, channel);   // printf
 *     Notice(theClient, getResponse(...).c_str(), nick);     // printf, read at run time
 *
 * A literal is checked when it is compiled: it has to be of one syntax, and
 * to use exactly the arguments given; std::format checks their types too.
 * A format that is only known at run time, which is what the language
 * tables of cservice hold, is printf and cannot be checked.  No va_list in
 * any of it: the arguments reach snprintf() with the types they have, so a
 * std::string for a %s is a string, where through "..." it was a crash.
 */
template <typename... Args> struct BasicCheckedFormat {
    /// A literal.
    template <typename T>
        requires(std::convertible_to<const T&, std::string_view> && !std::is_pointer_v<T>)
    consteval BasicCheckedFormat(const T& literal) : text(literal) {
        const std::size_t fields = detail::formatArgumentCount(text);
        const std::size_t conversions = detail::printfArgumentCount(text);

        if (0 == conversions && fields == sizeof...(Args)) {
            // Has std::format check the fields against the types
            (void)std::format_string<Args...>(text);
        } else if (0 == fields && conversions == sizeof...(Args)) {
            printfStyle = true;
        } else {
            // Not a constant expression, so this is a compile error, and the
            // name shows up in it.
            formatDoesNotUseExactlyTheArgumentsGiven_orMixesPrintfAndFormatSyntax();
        }
    }

    /// A format read at run time: printf, and NUL terminated.
    template <typename T>
        requires(std::is_pointer_v<T> && std::convertible_to<T, const char*>)
    BasicCheckedFormat(const T& runtimeFormat) : text(runtimeFormat), printfStyle(true) {}

    std::string_view text;
    bool printfStyle = false;

  private:
    static void formatDoesNotUseExactlyTheArgumentsGiven_orMixesPrintfAndFormatSyntax();
};

/// As std::format_string does, keep the parameter out of template argument
/// deduction: Args come from the arguments alone.
template <typename... Args> using CheckedFormat = BasicCheckedFormat<std::type_identity_t<Args>...>;

/// The text of a checked format and its arguments.
template <typename... Args> std::string formatMessage(CheckedFormat<Args...> fmt, Args&&... args) {
    if (!fmt.printfStyle) {
        return std::vformat(fmt.text, std::make_format_args(args...));
    }

    return detail::printfText(std::string(fmt.text).c_str(), detail::printfHold(args)...);
}

} // namespace gnuworld

#endif /* __MISC__ */
