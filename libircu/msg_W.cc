/**
 * msg_W.cc
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
 * $Id: msg_W.cc,v 1.5 2005/03/25 03:07:29 dan_karrels Exp $
 */

#include <iostream>

#include "gnuworld_config.h"
#include "server.h"
#include "xparameters.h"
#include "Network.h"
#include "iClient.h"
#include "client.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {

using std::endl;

CREATE_HANDLER(msg_W)

// ABAG7 W Az :Gte-
bool msg_W::Execute(const xParameters& Param) {
    // <source> W <server> :<nick>[,<nick>...]; ircu's ms_whois() also takes
    // "W :<nick>", which names no server and so is not for us to answer
    requireParameters(Param, 2);
    if (Param.size() != 3) {
        return true;
    }

    iClient* sourceClient = Network->findClient(Param[0]);
    if (NULL == sourceClient) {
        LOG(WARN, "Unable to find source client: {}", std::string(Param[0]));
        return false;
    }

    iClient* targetClient = Network->findNick(Param[2]);
    if (NULL == targetClient) {
        LOG(WARN, "Unable to find target client: {}", std::string(Param[2]));
        return false;
    }

    // WHOIS must be delivered to all xclients
    xNetwork::localClientIterator ptr = Network->localClient_begin();
    for (; ptr != Network->localClient_end(); ++ptr) {
        ptr->second->OnWhois(sourceClient, targetClient);
    }

    return true;
}

} // namespace gnuworld
