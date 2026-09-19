/**
 * msg_D.cc
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
 * $Id: msg_D.cc,v 1.5 2005/03/25 03:07:29 dan_karrels Exp $
 */

#include <new>
#include <string>

#include <cassert>

#include "gnuworld_config.h"
#include "server.h"
#include "iClient.h"
#include "iServer.h"
#include "events.h"
#include "Network.h"
#include "StringTokenizer.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {
using std::endl;
using std::string;

CREATE_HANDLER(msg_D)

/**
 * Kill command
 * QAA D BB5 :localhost!_reppir (Im using my super duper clone detecting
 *  skills)
 * G D r[l :NewYork-R.NY.US.Undernet.Org!NewYork-R.NY.US.Undernet.org ...
 * The source of the kill could be a server or a client.
 */
bool msg_D::Execute(const xParameters& Param) {
    requireParameters(Param, 3);

    // <source> D <victim> :<path> <reason>     P10
    // <source> D <victim> <path> :<reason>     P11
    // The modules are given both, as they always were: "<path> <reason>".
    const bool splitPath = (Param.size() >= 4);
    const string path =
        splitPath ? string(Param[2]) : string(Param[2]).substr(0, string(Param[2]).find(' '));

    /*
     * A KILL needs no confirmation: every server removes the victim as the
     * KILL passes, and so do we, below.  What ircu does as the victim's own
     * server (ms_kill()) is to send a KILL back the way this one came, for a
     * numeric that was given out again while the KILL was on its way: the
     * uplink then knows a client by this numeric that we are about to
     * forget.  Where the uplink knows none, the KILL ends there.
     */
    if (iClient* ours = Network->findClient(Param[1]); theServer->isOurClient(ours)) {
        if (splitPath) {
            theServer->Write("{} D {} {} :(Ghost 5 Numeric Collided)", theServer->getCharYY(),
                             Param[1], path);
        } else {
            theServer->Write("{} D {} :{} (Ghost 5 Numeric Collided)", theServer->getCharYY(),
                             Param[1], path);
        }
    }

    if ((Param[1][0] == theServer->getCharYY()[0]) && (Param[1][1] == theServer->getCharYY()[1])) {
        // See if the client being killed is one of my own.
        xClient* myClient = Network->findLocalClient(Param[1]);

        // Is the user being killed on this server?
        if (NULL != myClient) {
            // doh, yes it is :(
            myClient->OnKill();

            // Don't detach the client until it requests so.
            // TODO: Work on this system.

            // Note that the client is still attached to the
            // server.
            return true;
        } else {
            // It's a client on my server, but not an xClient.
            // This is ok, the normal client kill handling code
            // (for iClient) will handle removing fake clients.
            // Allow it continue instead of returning.
        }
    }

    // Otherwise, it's a non-local client.
    iClient* source = 0;
    iServer* serverSource = 0;

    if (strchr(Param[0], '.') != NULL) {
        // Server, by name
        serverSource = Network->findServerName(Param[0]);
    } else if (strlen(Param[0]) >= 3) {
        // Client, by numeric
        source = Network->findClient(Param[0]);
    } else {
        // Server, by numeric
        serverSource = Network->findServer(Param[0]);
    }

    if ((NULL == serverSource) && (NULL == source)) {
        LOG(WARN, "Unable to find source: {}", std::string(Param[0]));
        return false;
    }

    // Find the client that is being killed.
    iClient* target = Network->findClient(Param[1]);

    // Make sure we have valid pointers to both source
    // and target.
    if (NULL == target) {
        LOG(WARN, "Unable to find target client: {}", std::string(Param[1]));
        return false;
    }

    // Notify all listeners of the EVT_KILL event before removing the client,
    // so that listeners still see it fully attached (channel membership,
    // numeric, nick all valid).
    string reason(splitPath ? path + ' ' + Param[3] : string(Param[2]));

    if (source != NULL) {
        theServer->PostEvent(EVT_KILL, static_cast<void*>(source), static_cast<void*>(target),
                             static_cast<void*>(&reason));
    } else {
        theServer->PostEvent(EVT_KILL, static_cast<void*>(serverSource), static_cast<void*>(target),
                             static_cast<void*>(&reason));
    }

    // xNetwork::removeClient will remove user<->channel associations
    Network->removeClient(target);

    // Deallocate the memory associated with this iClient.
    delete target;

    return true;
}

} // namespace gnuworld
