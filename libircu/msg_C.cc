/**
 * msg_C.cc
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
 * $Id: msg_C.cc,v 1.10 2007/03/16 15:31:28 mrbean_ Exp $
 */

#include <new>
#include <string>
#include <utility>

#include <cassert>

#include "gnuworld_config.h"
#include "server.h"
#include "Network.h"
#include "events.h"

#include "StringTokenizer.h"
#include "xparameters.h"
#include "iClient.h"
#include "Channel.h"
#include "ChannelUser.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {
using std::endl;
using std::pair;
using std::string;

CREATE_HANDLER(msg_C)

/**
 * Someone has just joined an empty channel (create)
 * UAA C #xfactor 957134023
 * zBP C #OaXaCa,#UruApan,#skatos 957207634
 */
bool msg_C::Execute(const xParameters& Param) {

    // Verify that there exist sufficient arguments to successfully
    // handle this command
    // client_numeric #channel[,#channel2,...] timestamp
    requireParameters(Param, 3);

    // Find the client in question.
    iClient* theClient = Network->findClient(Param[0]);

    // Did we find the client?
    if (NULL == theClient) {
        // Nope, log the error
        LOG(WARN, "({}) Unable to find client: {}", std::string(Param[1]), std::string(Param[0]));

        // Return error
        return false;
    }

    // Grab the creation time.
    const time_t creationTime = requireTimestamp(Param[Param.size() - 1]);

    iServer* nickUplink = 0;
    char serverYY[3];
    strncpy(serverYY, Param[0], 2);
    serverYY[2] = '\0';
    nickUplink = Network->findServer(serverYY);
    if (!nickUplink->isBursting()) {
        // Set the server's lag time
        time_t lag = 0;
        if (::time(0) > creationTime)
            lag = ::time(0) - creationTime;
        else
            lag = 0;
        nickUplink->setLag(lag);
    }

    // Tokenize based on ','.  Multiple channels may be put into the
    // same C(REATE) command.
    StringTokenizer st(Param[1], ',');

    for (StringTokenizer::const_iterator ptr = st.begin(); ptr != st.end(); ++ptr) {

        // Is this a modeless channel?
        if ('+' == (*ptr)[0]) {
            // Modeless channel, ignore it
            continue;
        }

        // Find the channel in question.
        Channel* theChan = Network->findChannel(*ptr);

        // Did we find the channel?
        if (NULL == theChan) {
            // Channel doesn't exist..this transmutes to a create
            theChan = new (std::nothrow) Channel(*ptr, creationTime);
            assert(theChan != 0);

            // Add this channel to the network channel table
            if (!Network->addChannel(theChan)) {
                // Addition failed, log the error
                LOG_MSG(ERROR, "Failed to add channel: {chan}").with("chan", theChan).log();

                // Prevent memory leaks by removing the unused
                // channel
                destroy(theChan);
                theChan = 0;

                // continue to next one *shrug*
                continue;
            }
        }

        ChannelUser* theUser = 0;
        if (theClient->findChannel(theChan)) {
            // The client knows about this channel already
            // Verify that the channel knows about the user.
            theUser = theChan->findUser(theClient);
            if (0 == theUser) {
                // The client knows of the channel, but not
                // the other way around.
                // theUser will be created and added to the
                // channel membership table below.
                LOG_MSG(WARN, "Half-way membership found  for client {client}, in channel {chan}")
                    .with("client", theClient)
                    .with("chan", theChan)
                    .log();
            } else {
                // User is already in the channel, probably lag or
                // a non-authoritative kick
                //			theUser->removeZombie() ;
            }
        } else {
            // Add this channel to the client's channel structure.
            if (!theClient->addChannel(theChan)) {
                LOG_MSG(ERROR, "Unable to add channel {chan} to iClient {client}")
                    .with("chan", theChan)
                    .with("client", theClient)
                    .log();

                continue;
            }
        }

        // The client now knows about the channel, build the second
        // half of the channel<->user association, if necessary.
        if (0 == theUser) {
            // Create a new ChannelUser to represent this iClient's
            // membership in this channel.
            theUser = new (std::nothrow) ChannelUser(theClient);
            assert(theUser != 0);

            // Add the ChannelUser to the Channel's information
            if (!addUser(theChan, theUser)) {
                // Addition failed, log the error
                // This should never happen.
                LOG_MSG(ERROR, "Unable to add user {client} to channel {chan}")
                    .with("client", theClient)
                    .with("chan", theChan)
                    .log();

                // Prevent a memory leak by deallocating the
                // unused ChannelUser structure
                destroy(theUser);
                theUser = 0;

                // Remove the channel information from the client
                theClient->removeChannel(theChan);

                // Continue to next channel
                continue;
            }
        }

        // The user who created the channel is automatically +o
        if (creationTime == theChan->getCreationTime()) {
            setMemberMode(theUser, ChannelUser::MODE_CHANOP);
        }
        if (creationTime < theChan->getCreationTime()) {
            // Need to clean all the channel modes and op the user who created the channel
            removeAllModes(theChan);
            setMemberMode(theUser, ChannelUser::MODE_CHANOP);
            setCreationTime(theChan, creationTime);
        }

        // Notify all listening xClients of this event
        theServer->postJoin(theChan, theClient, theUser, JoinKind::Create);

    } // for()

    return true;
}

} // namespace gnuworld
