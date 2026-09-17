/**
 * CheckedFormat.h
 * A std::format string that also has to use every argument it is given.
 *
 * Copyright (C) 2026 The GNUWorld developers
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
 */

#ifndef GNUWORLD_CHECKEDFORMAT_H
#define GNUWORLD_CHECKEDFORMAT_H

#include <concepts>
#include <cstddef>
#include <format>
#include <string_view>
#include <type_traits>

namespace gnuworld {

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

/**
 * What the network functions take in place of "const char*, ...".
 *
 * std::format_string checks at compile time that the fields fit the types
 * of the arguments, but it accepts arguments that no field uses.  This code
 * base is being moved off printf, where that matters: a call left behind,
 *
 *     Notice(theClient, "%s is not on %s", nick, channel);
 *
 * is a valid std::format string with no fields and two unused arguments, and
 * would send "%s is not on %s" to the user.  Requiring the counts to match
 * makes it a compile error instead.
 */
template <typename... Args> struct BasicCheckedFormat {
    template <typename T>
        requires std::convertible_to<const T&, std::string_view>
    consteval BasicCheckedFormat(const T& text) : format(text) {
        if (detail::formatArgumentCount(std::string_view(text)) != sizeof...(Args)) {
            // Not a constant expression, so this is a compile error, and the
            // name shows up in it.
            formatStringDoesNotUseExactlyTheArgumentsGiven_isItStillPrintfStyle();
        }
    }

    std::format_string<Args...> format;

  private:
    static void formatStringDoesNotUseExactlyTheArgumentsGiven_isItStillPrintfStyle();
};

/// As std::format_string does, keep the parameter out of template argument
/// deduction: Args come from the arguments alone.
template <typename... Args> using CheckedFormat = BasicCheckedFormat<std::type_identity_t<Args>...>;

} // namespace gnuworld

#endif // GNUWORLD_CHECKEDFORMAT_H
