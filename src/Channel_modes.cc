/*
 * Channel_modes.cc
 * Author: Daniel Karrels (dan@karrels.com)
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
 * $Id: Channel.cc,v 1.55 2008/04/16 20:29:37 danielaustin Exp $
 */

/*
 * The channel mode table's functions: Channel::parseModes(),
 * formatModeLines() and their helpers.  They are members of Channel, declared
 * in Channel.h, and kept apart from Channel.cc the way xServer is spread over
 * server.cc, server_events.cc and server_connection.cc.  Nothing in here
 * needs the network tables or the connection, which is what lets
 * test/channelmodes.cc build this file by itself.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Channel.h"
#include "gnuworld_config.h"
#include "misc.h"

namespace gnuworld {

namespace {

/// An argument that is a single token on the wire: not empty, no leading
/// ':' (which would start the trailing parameter), nothing at or below ' '.
bool isSingleToken(std::string_view s) noexcept {
    return !s.empty() && s.front() != ':' &&
           std::ranges::none_of(s, [](char c) { return static_cast<unsigned char>(c) <= ' '; });
}

/// The problem with this argument for this mode, if any.
std::optional<Channel::ModeError> validateModeArg(const Channel::ModeInfo& mode,
                                                  std::string_view arg) noexcept {
    switch (mode.kind) {
    case Channel::ModeKind::Flag:
        break;
    case Channel::ModeKind::Key:
    case Channel::ModeKind::Password:
        if (!Channel::isValidKey(arg)) {
            return Channel::ModeError::InvalidKey;
        }
        break;
    case Channel::ModeKind::Limit:
        if (!Channel::isValidLimit(arg)) {
            return Channel::ModeError::InvalidLimit;
        }
        break;
    case Channel::ModeKind::Member:
        // A numeric, a numeric with an oplevel ("ABAAA:999"), or a nick
        // when the change comes from a module.
        if (!isSingleToken(arg) || arg.find(',') != std::string_view::npos) {
            return Channel::ModeError::InvalidTarget;
        }
        break;
    case Channel::ModeKind::Ban:
        if (!isSingleToken(arg)) {
            return Channel::ModeError::InvalidMask;
        }
        break;
    }
    return std::nullopt;
}

} // namespace

bool Channel::isValidKey(std::string_view key) noexcept {
    return isSingleToken(key) && key.size() <= maxKeyLength &&
           key.find(',') == std::string_view::npos;
}

bool Channel::isValidLimit(std::string_view limit) noexcept {
    const std::optional<int> value = parseNumber<int>(limit);
    return value && *value >= 1;
}

Channel::ParsedModes Channel::parseModes(std::string_view modeString,
                                         std::span<const std::string_view> args) {
    return parseModes(modeString, args, ModeParseOptions{});
}

Channel::ParsedModes Channel::parseModes(std::string_view modeString,
                                         std::span<const std::string_view> args,
                                         ModeParseOptions options) {
    ParsedModes result{};
    bool set = true;
    std::size_t nextArg = 0;

    for (const char letter : modeString) {
        if (letter == '+' || letter == '-') {
            set = (letter == '+');
            continue;
        }

        const std::optional<ModeInfo> mode = findMode(letter);
        if (!mode) {
            // Nothing is known about its argument, so none is consumed.
            result.problems.push_back(
                {isLocalOnlyMode(letter) ? ModeError::LocalOnlyMode : ModeError::UnknownMode,
                 letter,
                 {}});
            continue;
        }

        if (!mode->takesArg(set)) {
            result.changes.push_back({set, *mode, {}});
            continue;
        }

        if (nextArg >= args.size()) {
            result.problems.push_back({ModeError::MissingArgument, letter, {}});
            continue;
        }

        // The argument is consumed even when it is invalid, so that the
        // modes behind this one still get their own.
        const std::string_view arg = args[nextArg++];
        if (const std::optional<ModeError> error = validateModeArg(*mode, arg)) {
            result.problems.push_back({*error, letter, std::string(arg)});
            continue;
        }
        result.changes.push_back({set, *mode, std::string(arg)});
    }

    if (options.trailingTimestamp && nextArg + 1 == args.size()) {
        if (const std::optional<std::uint64_t> ts = parseNumber<std::uint64_t>(args[nextArg])) {
            result.timestamp = ts;
            ++nextArg;
        }
    }

    result.argsUsed = nextArg;

    if (!options.allowLeftover) {
        for (; nextArg < args.size(); ++nextArg) {
            result.problems.push_back({ModeError::UnusedArgument, 0, std::string(args[nextArg])});
        }
    }

    return result;
}

std::vector<std::string> Channel::formatModeLines(std::string_view prefix,
                                                  std::span<const ModeChange> changes,
                                                  std::uint64_t timestamp) {
    // IRC_MAX_LINE counts the CR LF
    constexpr std::size_t maxLineLength = IRC_MAX_LINE - 2;

    std::vector<std::string> lines;
    const std::string tail = ' ' + std::to_string(timestamp);

    std::string modeString;
    std::string argString;
    std::size_t modesOnLine = 0;
    std::optional<bool> polarity;

    const auto flush = [&] {
        if (!modeString.empty()) {
            lines.push_back(std::string(prefix) + ' ' + modeString + argString + tail);
        }
        modeString.clear();
        argString.clear();
        modesOnLine = 0;
        polarity.reset();
    };

    for (const ModeChange& change : changes) {
        const bool hasArg = !change.arg.empty();

        // What this change adds to the line: a sign if the polarity turns,
        // the letter, and " <arg>".
        const std::size_t growth =
            (polarity == change.set ? 0 : 1) + 1 + (hasArg ? 1 + change.arg.size() : 0);
        const std::size_t length =
            prefix.size() + 1 + modeString.size() + argString.size() + tail.size();

        // MAX_CHAN_MODES is the number of modes per command, whether or not
        // they take an argument.  ircu itself only counts the arguments, so
        // this is the stricter reading, and the one gnuworld always applied.
        if (!modeString.empty() &&
            (modesOnLine == MAX_CHAN_MODES || length + growth > maxLineLength)) {
            flush();
        }

        if (polarity != change.set) {
            modeString += change.set ? '+' : '-';
            polarity = change.set;
        }
        modeString += change.mode.letter;
        if (hasArg) {
            argString += ' ';
            argString += change.arg;
        }
        ++modesOnLine;
    }
    flush();

    return lines;
}

std::string Channel::burstModeBlock(std::span<const ModeChange> changes) {
    std::vector<const ModeChange*> block;
    for (const ModeChange& change : changes) {
        if (change.set && change.mode.kind != ModeKind::Member &&
            change.mode.kind != ModeKind::Ban) {
            block.push_back(&change);
        }
    }
    if (block.empty()) {
        return {};
    }

    std::ranges::stable_sort(block, {}, [](const ModeChange* c) { return burstOrder(c->mode); });

    std::string out = "+";
    std::string args;
    for (const ModeChange* change : block) {
        out += change->mode.letter;
        if (!change->arg.empty()) {
            args += ' ' + change->arg;
        }
    }
    return out + args;
}

std::string Channel::isupportChanmodes() {
    std::string out;
    for (const ModeGroup group : {ModeGroup::A, ModeGroup::B, ModeGroup::C, ModeGroup::D}) {
        if (group != ModeGroup::A) {
            out += ',';
        }
        for (const ModeInfo& mode : modeTable) {
            if (mode.group() == group) {
                out += mode.letter;
            }
        }
    }
    return out;
}

std::string_view Channel::describe(ModeError error) noexcept {
    switch (error) {
    case ModeError::UnknownMode:
        return "unknown mode";
    case ModeError::LocalOnlyMode:
        return "mode is local to a server";
    case ModeError::MissingArgument:
        return "missing argument";
    case ModeError::InvalidKey:
        return "invalid key";
    case ModeError::InvalidLimit:
        return "invalid limit";
    case ModeError::InvalidTarget:
        return "invalid target";
    case ModeError::InvalidMask:
        return "invalid mask";
    case ModeError::UnusedArgument:
        return "unused argument";
    }
    return "unknown error";
}

} // namespace gnuworld
