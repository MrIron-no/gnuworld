/**
 * msg_J.cc
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
 * $Id: msg_J.cc,v 1.10 2007/04/18 11:00:20 kewlio Exp $
 */

#include <new>
#include <string>
#include <iostream>

#include <cassert>

#include "gnuworld_config.h"
#include "server.h"
#include "iClient.h"
#include "Channel.h"
#include "ChannelUser.h"
#include "events.h"
#include "Network.h"
#include "StringTokenizer.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_MODULE_LOGGER("core.proto");

namespace gnuworld {
using std::endl;
using std::string;

class msg_J : public ServerCommandHandler {
  public:
    msg_J(xServer* theServer) : ServerCommandHandler(theServer, "msg_J>") {}
    virtual ~msg_J() {}

    virtual bool Execute(const xParameters&);

  protected:
    void userPartAllChannels(iClient*);
};

CREATE_LOADER(msg_J)

/**
 * Someone has just joined a non-empty channel.
 *
 * 0AT J #coder-com 1234567890
 * OAT J #coder-com,#blah 1234567890
 * OAT J 0 <optional-ts?>
 */
bool msg_J::Execute(const xParameters& Param) {
    // Verify that sufficient arguments have been provided
    // client_numeric #channel[,#channel2,...]
    requireParameters(Param, 2);

    // A JOIN may come without a timestamp: ircu's ms_join() takes one, as
    // "JOIN 0" and other servers' software send it.  ircu then changes no
    // creation time at all, and gives a channel the JOIN creates the time 0,
    // its "not known yet", which a later BURST or CREATE fills in.  0 means
    // nothing of the kind here - it is the oldest time there is - so such a
    // channel is created as of now, and any older time that follows makes it
    // older; like ircu, an existing channel is left as it is.

    // Find the client in question.
    iClient* Target = Network->findClient(Param[0]);

    // Did we find the client?
    if (NULL == Target) {
        // Nope, log the error
        LOG(WARN, "({}) Unable to find user: {}", std::string(Param[1]), std::string(Param[0]));

        // Return error
        return false;
    }

    // Tokenize by ',', as the client may join more than one
    // channel at once.
    StringTokenizer st(Param[1], ',');
    const bool hasTimestamp = Param.size() >= 3;
    const time_t joinTs = hasTimestamp ? requireTimestamp(Param[2]) : ::time(NULL);
    for (StringTokenizer::size_type i = 0; i < st.size(); i++) {
        // Is it a modeless channel?
        if ('+' == st[i][0]) {
            // Don't care about modeless channels
            continue;
        }

        // Is the user parting all channels?
        if ('0' == st[i][0]) {
            // Yup, call userPartAllChannels which will update
            // the user's information and notify listening
            // services clients of the parts
            userPartAllChannels(Target);

            // continue to next channel
            continue;
        }

        // This variable represents which event actually occurs
        channelEventType whichEvent = EVT_JOIN;

        // On a JOIN command, the channel should already exist.
        Channel* theChan = Network->findChannel(st[i]);

        // Does the channel already exist?
        if (NULL == theChan) {
            // Nope, this transmutes to a CREATE
            // Create a new Channel to represent this
            // network channel
            // With the timestamp of the JOIN, which is the channel's: a JOIN
            // also brings back the victim of a kick that was bounced, to a
            // channel we may have removed when the kick left it empty.
            theChan = new (std::nothrow) Channel(st[i], joinTs);
            assert(theChan != 0);

            // Add the channel to the network tables
            if (!Network->addChannel(theChan)) {
                // Addition to network tables failed
                // Log the error
                LOG_MSG(ERROR, "Unable to add channel: {chan}").with("chan", theChan).log();

                // Prevent memory leaks by deallocating the
                // Channel object
                delete theChan;
                theChan = 0;

                // Continue to next channel
                continue;
            }

            // Since this is equivalent to a CREATE, the user is an
            // operator: see the ChannelUser below.

            // Update the event type
            whichEvent = EVT_CREATE;

        } // if( NULL == theChan )
        /*
                else if( theChan->findUser( Target ) != 0 )
                        {
                        // The user is already in the channel...check for
                        // zombie state
                        ChannelUser* oldUser = theChan->findUser( Target ) ;
                        if( oldUser->getMode( ChannelUser::ZOMBIE ) )
                                {
                                // The user was in the zombie state
                                // Remove the zombie state, and continue
                                // to the next channel
                                oldUser->removeMode( ChannelUser::ZOMBIE ) ;

        //			elog	<< "msg_J> Removed zombie: "
        //				<< *oldUser
        //				<< " on channel "
        //				<< theChan->getName()
        //				<< endl ;
                                }
                        else
                                {
                                // User was found in channel, no reason apparent
                                // This message can happen a lot due
                                // to lag....it's not too important tho it
                                // bugs me so.
        //			elog	<< "msg_J> Unexpectedly found "
        //				<< "user "
        //				<< *Target
        //				<< " in channel "
        //				<< theChan->getName()
        //				<< endl ;
                                }

                        // In either case, there is no need to add the newly
                        // created ChannelUser to the channel, because it
                        // is already there.
                        delete theUser ; theUser = 0 ;

                        // Continue, nothing more to do here
                        continue ;
                        }
        */
        else if (hasTimestamp && joinTs < theChan->getCreationTime()) {
            // The time of join is earlier than the creation time of the channel
            // Need to clear all the modes of the channel
            removeAllModes(theChan);
            // Now reset the channel creation time to the join ts
            setCreationTime(theChan, joinTs);
        }
        // A join to a +D channel carries no status, so it is delayed
        // (hidden) until the member gains op or voice, sets the topic,
        // or is announced by a REVEAL.  A creator is opped and therefore
        // never hidden.
        // Only tracked on a P11 uplink: without REVEAL we would rarely
        // learn that a member has spoken, and the flag would go stale.
        bool isHidden = false;
        if (EVT_JOIN == whichEvent && theChan->getMode(Channel::MODE_DELJOINS) &&
            theServer->getUplink()->getProtocol() >= 11) {
            isHidden = true;
        }

        // The ChannelUser structure for this user<->channel association
        ChannelUser* theUser = new (std::nothrow) ChannelUser(
            Target, (EVT_CREATE == whichEvent) ? ChannelUser::MODE_CHANOP : 0, isHidden);
        assert(theUser != 0);

        // Add a new ChannelUser representing this client to this
        // channel's user structure.
        if (!addUser(theChan, theUser)) {
            // Addition of this ChannelUser to the Channel failed
            // Log the error
            LOG_MSG(ERROR, "Unable to add user {client} to channel: {chan}")
                .with("client", Target)
                .with("chan", theChan)
                .log();

            // Prevent memory leaks by deallocating the unused
            // ChannelUser object
            delete theUser;
            theUser = 0;

            if (EVT_CREATE == whichEvent) {
                // The channel did not exist before
                // this message, so go ahead and
                // remove it
                Network->removeChannel(theChan->getName());

                // Do some cleanup
                delete theChan;
                theChan = 0;
            }

            // Continue to next channel
            continue;
        }

        // Add this channel to this client's channel structure.
        if (!Target->addChannel(theChan)) {
            LOG_MSG(ERROR, "Unable to add channel {chan} to iClient {client}")
                .with("chan", theChan)
                .with("client", Target)
                .log();

            // Remove the ChannelUser from this channel, and
            // deallocate the ChannelUser to prevent memory
            // leaks
            removeUser(theChan, Target);
            delete theUser;
            theUser = 0;

            // Did we just create the channel?
            if (EVT_CREATE == whichEvent) {
                // Yup, remove the channel from the network
                // data structures
                Network->removeChannel(theChan->getName());

                // Prevent memory leaks by deallocating
                // the channel
                delete theChan;
                theChan = 0;
            }

            // Continue on with the next channel
            continue;
        }

        // Post the event to the clients listening for events on this
        // channel, if any.
        theServer->PostChannelEvent(whichEvent, theChan, static_cast<void*>(Target),
                                    static_cast<void*>(theUser));

        // TODO: Update event posting so that CREATE is also
        // passed the client who created the channel

    } // for()

    return true;
}

void msg_J::userPartAllChannels(iClient* theClient) {
    // Artifact, user is parting all channels
    for (iClient::channelIterator ptr = theClient->channels_begin(),
                                  endPtr = theClient->channels_end();
         ptr != endPtr;) {

        // Remove this ChannelUser from the Channel's internal
        // structure.
        // Deallocate the ChannelUser
        ChannelUser* theChanUser = removeUser(*ptr, theClient);
        if (NULL == theChanUser) {
            LOG_MSG(ERROR, "Unable to remove iClient {client} from channel {chan}")
                .with("client", theClient)
                .with("chan", *ptr)
                .log();
        }
        delete theChanUser;
        theChanUser = 0;

        // BUG: This iClient has inconsistent state because
        // no channels have been removed from its internal
        // structure until the end of this method.

        // Post this event to all listeners
        theServer->PostChannelEvent(EVT_PART, *ptr,
                                    static_cast<void*>(theClient)); // iClient*

        // Is the channel empty of all network and services
        // clients?
        if ((*ptr)->empty()) {
            // TODO: Post event

            // Yup, remove the channel from the network channel
            // table
            delete Network->removeChannel((*ptr++)->getName());
        } else
            ptr++;
    }

    // Just to be sure
    theClient->clearChannels();

} // userPartAllChannels()

} // namespace gnuworld
