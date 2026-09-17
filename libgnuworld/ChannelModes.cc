/**
 * ChannelModes.cc
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

#include "ChannelModes.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace gnuworld::chanmode {

namespace {

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool allDigits(std::string_view s) noexcept {
    return !s.empty() && std::ranges::all_of(s, isDigit);
}

/// An argument that is a single token on the wire: not empty, no leading
/// ':' (which would start the trailing parameter), nothing at or below ' '.
bool isSingleToken(std::string_view s) noexcept {
    return !s.empty() && s.front() != ':' &&
           std::ranges::none_of(s, [](char c) { return static_cast<unsigned char>(c) <= ' '; });
}

/// The problem with this argument for this mode, if any.
std::optional<Error> validate(const Mode& mode, std::string_view arg) noexcept {
    switch (mode.kind) {
    case Kind::Flag:
        break;
    case Kind::Key:
    case Kind::Password:
        if (!isValidKey(arg)) {
            return Error::InvalidKey;
        }
        break;
    case Kind::Limit:
        if (!isValidLimit(arg)) {
            return Error::InvalidLimit;
        }
        break;
    case Kind::Member:
        // A numeric, a numeric with an oplevel ("ABAAA:999"), or a nick
        // when the change comes from a module.
        if (!isSingleToken(arg) || arg.find(',') != std::string_view::npos) {
            return Error::InvalidTarget;
        }
        break;
    case Kind::Ban:
        if (!isSingleToken(arg)) {
            return Error::InvalidMask;
        }
        break;
    }
    return std::nullopt;
}

} // namespace

bool isValidKey(std::string_view key) noexcept {
    return isSingleToken(key) && key.size() <= maxKeyLength &&
           key.find(',') == std::string_view::npos;
}

bool isValidLimit(std::string_view limit) noexcept {
    if (!allDigits(limit)) {
        return false;
    }
    unsigned long value = 0;
    const auto [end, error] = std::from_chars(limit.data(), limit.data() + limit.size(), value);
    return error == std::errc{} && end == limit.data() + limit.size() && value >= 1 &&
           value <= static_cast<unsigned long>(std::numeric_limits<int>::max());
}

Parsed parse(std::string_view modeString, std::span<const std::string_view> args,
             bool trailingTimestamp) {
    Parsed result;
    bool set = true;
    std::size_t nextArg = 0;

    for (const char letter : modeString) {
        if (letter == '+' || letter == '-') {
            set = (letter == '+');
            continue;
        }

        const std::optional<Mode> mode = find(letter);
        if (!mode) {
            // Nothing is known about its argument, so none is consumed.
            result.problems.push_back(
                {isLocalOnly(letter) ? Error::LocalOnlyMode : Error::UnknownMode, letter, {}});
            continue;
        }

        if (!mode->takesArg(set)) {
            result.changes.push_back({set, *mode, {}});
            continue;
        }

        if (nextArg >= args.size()) {
            result.problems.push_back({Error::MissingArgument, letter, {}});
            continue;
        }

        // The argument is consumed even when it is invalid, so that the
        // modes behind this one still get their own.
        const std::string_view arg = args[nextArg++];
        if (const std::optional<Error> error = validate(*mode, arg)) {
            result.problems.push_back({*error, letter, std::string(arg)});
            continue;
        }
        result.changes.push_back({set, *mode, std::string(arg)});
    }

    if (trailingTimestamp && nextArg + 1 == args.size() && allDigits(args[nextArg])) {
        std::uint64_t timestamp = 0;
        const std::string_view text = args[nextArg];
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), timestamp);
        if (error == std::errc{} && end == text.data() + text.size()) {
            result.timestamp = timestamp;
            ++nextArg;
        }
    }

    for (; nextArg < args.size(); ++nextArg) {
        result.problems.push_back({Error::UnusedArgument, 0, std::string(args[nextArg])});
    }

    return result;
}

std::string_view describe(Error error) noexcept {
    switch (error) {
    case Error::UnknownMode:
        return "unknown mode";
    case Error::LocalOnlyMode:
        return "mode is local to a server";
    case Error::MissingArgument:
        return "missing argument";
    case Error::InvalidKey:
        return "invalid key";
    case Error::InvalidLimit:
        return "invalid limit";
    case Error::InvalidTarget:
        return "invalid target";
    case Error::InvalidMask:
        return "invalid mask";
    case Error::UnusedArgument:
        return "unused argument";
    }
    return "unknown error";
}

} // namespace gnuworld::chanmode
