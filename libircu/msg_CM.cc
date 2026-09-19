/*
 * msg_CM. cc
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
 * $Id: msg_CM.cc,v 1.11 2008/04/16 20:29:37 danielaustin Exp $
 */

#include <map>
#include <optional>
#include <string>
#include <iostream>
#include <string>
#include <vector>

#include "gnuworld_config.h"
#include "server.h"
#include "Channel.h"
#include "ChannelUser.h"
#include "Network.h"
#include "iClient.h"
#include "xparameters.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {
using std::endl;
using std::make_pair;
using std::pair;

CREATE_HANDLER(msg_CM)

/**
 * CLEARMODE message handler.
 * ZZAAA CM #channel obv
 * The above message would remove all ops, bans, and voice modes
 *  from channel #channel.
 */
bool msg_CM::Execute(const xParameters& Param) {
    requireParameters(Param, 3);

    Channel* tmpChan = Network->findChannel(Param[1]);
    if (!tmpChan) {
        // Log Error.
        LOG(WARN, "Unable to locate channel: {}", std::string(Param[1]));
        return false;
    }

    // What clearing these letters takes off the channel as it is now
    std::vector<std::string> problems;
    const std::vector<Channel::ModeChange> changes = tmpChan->changesToClear(Param[2], problems);
    if (!problems.empty()) {
        protocolError(problems);
    }

    // Go ahead and post the server mode event
    iServer* serverSource = 0;

    if (NULL != strchr(Param[0], '.')) {
        // Server, by name
        serverSource = Network->findServerName(Param[0]);
    } else if (strlen(Param[0]) < 3) {
        // 1 or 2 char numeric, server
        serverSource = Network->findServer(Param[0]);
    }

    if (serverSource != 0)
        theServer->PostChannelEvent(EVT_SERVERMODE, tmpChan, static_cast<void*>(serverSource));

    theServer->ApplyChannelModes(tmpChan, 0, changes, where);

    return true;
}

} // namespace gnuworld
