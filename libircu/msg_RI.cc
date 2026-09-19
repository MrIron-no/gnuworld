/**
 * msg_RI.cc
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
 */

#include <sys/types.h>
#include <sys/time.h>

#include <iostream>
#include <sstream>

#include "server.h"
#include "iServer.h"
#include "iClient.h"
#include "events.h"
#include "Network.h"
#include "xparameters.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_MODULE_LOGGER("core.proto");

namespace gnuworld {

using std::endl;
using std::stringstream;

CREATE_HANDLER(msg_RI)

/* RPING:
 *
 * Ar == random.server.netgamers.org
 * Ay == cservice.netgamers.org
 *
 * This is an RPING from AyAAA to Ar, ie we are Ar in this scenario.
 *
 * IN : Ay RI Ar AyAAA 1085745254 37971 :<No client start time>
 * OUT: Ar RO cservice.netgamers.org AyAAA 1085745254 37971 :<No client start time>
 *
 * Due to gnuworld wierdness, we actually get:
 * params: AB Az ABAAA 1085753348 87482 <No client start time>
 */

bool msg_RI::Execute(const xParameters& params) {

    // return true;

    // From a server: <server> RI <target> <requester> <sec> <usec> :<remark>
    // From an oper:   <oper> RI <target mask> <start server> :<remark>
    theServer->RequireParameters("msg_RI>", params, 3);
    if (params.size() < 5) {
        // The oper's form, which asks the start server to do the pinging.
        // Legal, and not something we do.
        return true;
    }

    if (params[1] != theServer->getCharYY()) {
        LOG(WARN, "Got RPING destined for someone else.");
        return false;
    }

    iServer* remoteServer = Network->findServer(params[0]);
    iClient* remoteClient = Network->findClient(params[2]);

    if (!remoteServer || !remoteClient) {
        LOG(WARN, "Got RPING from non-existant client/server.");
        return false;
    }

    /* Construct our reply */
    stringstream reply;
    reply << theServer->getCharYY() << " RO " << remoteServer->getName() << " "
          << remoteClient->getCharYYXXX() << " " << params[3] << " " << params[4];

    if (params.size() > 5) {
        reply << " :" << params.assemble(5);
    }

    theServer->Write(reply);

    return true;
}

} // namespace gnuworld
