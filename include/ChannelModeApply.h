/**
 * ChannelModeApply.h
 * Apply validated channel mode changes to gnuworld's state.
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

#ifndef GNUWORLD_CHANNELMODEAPPLY_H
#define GNUWORLD_CHANNELMODEAPPLY_H

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "ChannelModes.h"

namespace gnuworld {

class xServer;
class Channel;
class ChannelUser;
class xParameters;

struct ModeApplyResult {
    /// How many changes were handed on to the channel.
    std::size_t applied = 0;

    /// Changes that could not be applied: the target of a +o/+v is unknown
    /// or not on the channel.  The others are unaffected.
    std::vector<chanmode::Problem> problems;
};

/**
 * Apply channel mode changes that came from the network.
 *
 * This is the one place where a parsed change becomes a change of state.
 * It goes through xServer's OnChannelMode*() methods, which update the
 * Channel and notify the modules, in the order the handlers have always
 * used: a limit, key or password as soon as it is met, then the flags,
 * the ops, the voices and the bans, each as one batch.
 *
 * sourceUser is the member that made the change; null for a server, or for
 * a client that is not on the channel (OPMODE).
 */
ModeApplyResult applyChannelModes(xServer& server, Channel& chan, ChannelUser* sourceUser,
                                  std::span<const chanmode::Change> changes);

/// Write one line to elog for each problem, prefixed with "<where> (<channel>)".
void logModeProblems(std::string_view where, std::string_view channelName,
                     std::span<const chanmode::Problem> problems);

/// Param[first], Param[first + 1], ... as the argument list for chanmode::parse().
std::vector<std::string_view> modeArguments(const xParameters& Param, std::size_t first);

} // namespace gnuworld

#endif // GNUWORLD_CHANNELMODEAPPLY_H
