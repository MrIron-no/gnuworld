/**
 * msg_EB.cc
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
 * $Id: msg_EB.cc,v 1.10 2005/03/25 03:07:29 dan_karrels Exp $
 */

#include <sys/types.h>
#include <sys/time.h>

#include <iostream>

#include "gnuworld_config.h"
#include "server.h"
#include "iServer.h"
#include "events.h"
#include "Network.h"
#include "xparameters.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {
using std::clog;
using std::endl;

CREATE_HANDLER(msg_EB)

// Q EB
// Q: Remote server numeric
// EB: End Of Burst
bool msg_EB::Execute(const xParameters& params) {
    if (!strcmp(params[0], theServer->getUplinkCharYY().c_str())) {
        //	elog	<< "msg_EB> sendEA: "
        //		<< theServer->getSendEA()
        //		<< endl ;

        // It's my uplink
        if (theServer->getSendEA() && theServer->getSendEB()) {
            theServer->setBurstEnd(::time(0));

            // Our uplink is done bursting
            // This is done here instead of down below the if/else
            // structure because some of the methods called here
            // may depend or use the Uplink's isBursting() method
            theServer->getUplink()->stopBursting();
        }

        // Signal that all Write()'s should write to the
        // normal output buffer
        theServer->setUseHoldBuffer(false);

        // Burst our clients
        theServer->BurstClients();

        // Burst our channels
        theServer->BurstChannels();

        // Only need EB to be sent to turn off bursting, since after
        // EB no bursts can be sent
        if (theServer->getSendEB()) {
            // We are no longer bursting
            theServer->setBursting(false);
        }

        // Posted here to notify all attached clients
        // that we are no longer bursting.  This will ensure
        // that all end of burst items are written to the
        // burstOutputBuffer before the burst if officially
        // completed (as seen by the network)
        theServer->postBurstComplete(theServer->getUplink());

        if (theServer->getSendEB()) {
            // Send our EB
            theServer->Write("{} EB\n", theServer->getCharYY());
        }

        if (theServer->getSendEA()) {
            // Acknowledge their end of burst
            theServer->Write("{} EA\n", theServer->getCharYY());
        }

        // Is the burstOutputBuffer empty?
        theServer->WriteBurstBuffer();

        theServer->postEndOfBurstAckSent(theServer->getUplink());
    } else {
        /* Its another server that has just completed its net.burst. */
        iServer* targetServer = Network->findServer(params[0]);
        if (NULL == targetServer) {
            LOG(WARN, "Unable to find server: {}", std::string(params[0]));
            return -1;
        }

        targetServer->stopBursting();
        theServer->postBurstComplete(targetServer);
    }

    return true;
}

} // namespace gnuworld
