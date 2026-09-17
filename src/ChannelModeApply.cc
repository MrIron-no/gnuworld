/**
 * ChannelModeApply.cc
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

#include "ChannelModeApply.h"

#include <charconv>
#include <string>

#include "Channel.h"
#include "ChannelUser.h"
#include "ELog.h"
#include "Network.h"
#include "iClient.h"
#include "server.h"
#include "xparameters.h"

namespace gnuworld {

namespace {

/// The member a +o/+v is aimed at, or null if it is not on the channel.
/// With oplevels the target is "<numeric>:<level>"; the level is not tracked.
ChannelUser* findTarget(const Channel& chan, std::string_view target) {
    const std::string numeric(target.substr(0, target.find(':')));
    iClient* theClient = Network->findClient(numeric);
    return (theClient != nullptr) ? chan.findUser(theClient) : nullptr;
}

} // namespace

ModeApplyResult applyChannelModes(xServer& server, Channel& chan, ChannelUser* sourceUser,
                                  std::span<const chanmode::Change> changes) {
    ModeApplyResult result;

    xServer::modeVectorType modeVector;
    xServer::opVectorType opVector;
    xServer::voiceVectorType voiceVector;
    xServer::banVectorType banVector;

    for (const chanmode::Change& change : changes) {
        const chanmode::Mode& mode = change.mode;

        switch (mode.kind) {
        case chanmode::Kind::Flag:
            modeVector.emplace_back(change.set, mode.flag);
            break;

        case chanmode::Kind::Limit: {
            // parse() has validated the argument; -l carries none
            unsigned int limit = 0;
            std::from_chars(change.arg.data(), change.arg.data() + change.arg.size(), limit);
            server.OnChannelModeL(&chan, change.set, sourceUser, limit);
            break;
        }

        case chanmode::Kind::Key:
            server.OnChannelModeK(&chan, change.set, sourceUser, change.arg);
            break;

        case chanmode::Kind::Password:
            if (mode.letter == 'A') {
                server.OnChannelModeA(&chan, change.set, sourceUser, change.arg);
            } else {
                server.OnChannelModeU(&chan, change.set, sourceUser, change.arg);
            }
            break;

        case chanmode::Kind::Member: {
            ChannelUser* target = findTarget(chan, change.arg);
            if (target == nullptr) {
                result.problems.push_back(
                    {chanmode::Error::InvalidTarget, mode.letter, change.arg});
                continue;
            }
            (mode.letter == 'o' ? opVector : voiceVector).emplace_back(change.set, target);
            break;
        }

        case chanmode::Kind::Ban:
            banVector.emplace_back(change.set, change.arg);
            break;
        }

        ++result.applied;
    }

    if (!modeVector.empty()) {
        server.OnChannelMode(&chan, sourceUser, modeVector);
    }
    if (!opVector.empty()) {
        server.OnChannelModeO(&chan, sourceUser, opVector);
    }
    if (!voiceVector.empty()) {
        server.OnChannelModeV(&chan, sourceUser, voiceVector);
    }
    if (!banVector.empty()) {
        server.OnChannelModeB(&chan, sourceUser, banVector);
    }

    return result;
}

void logModeProblems(std::string_view where, std::string_view channelName,
                     std::span<const chanmode::Problem> problems) {
    for (const chanmode::Problem& problem : problems) {
        elog << where << " (" << channelName << "): " << chanmode::describe(problem.error);
        if (problem.letter != 0) {
            elog << " for mode '" << problem.letter << "'";
        }
        if (!problem.detail.empty()) {
            elog << ": " << problem.detail;
        }
        elog << std::endl;
    }
}

std::vector<std::string_view> modeArguments(const xParameters& Param, std::size_t first) {
    std::vector<std::string_view> args;
    for (std::size_t i = first; i < Param.size(); ++i) {
        args.emplace_back(Param[i]);
    }
    return args;
}

} // namespace gnuworld
