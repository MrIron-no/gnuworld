/**
 * msg_M.cc
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
 * $Id: msg_M.cc,v 1.14 2008/04/16 20:29:37 danielaustin Exp $
 */

#include <new>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>

#include "gnuworld_config.h"
#include "misc.h"
#include "events.h"

#include "server.h"
#include "iClient.h"
#include "Channel.h"
#include "ChannelUser.h"
#include "Network.h"
#include "ELog.h"
#include "StringTokenizer.h"
#include "ServerCommandHandler.h"

namespace gnuworld {

using std::endl;
using std::make_pair;
using std::pair;
using std::string;
using std::vector;

class msg_M : public ServerCommandHandler {
  public:
    msg_M(xServer* theServer) : ServerCommandHandler(theServer) {}
    virtual ~msg_M() {}

    virtual bool Execute(const xParameters&);

  protected:
    bool onUserModeChange(const xParameters&);
};

CREATE_LOADER(msg_M)

// Mode change
// AeAAA M #zt +l 47 963549578
// OAD M ripper_ :+owg
//
// i M #3dx +o eAA
// J[K M DEMET_33 :+i
bool msg_M::Execute(const xParameters& Param) {
    if (Param.size() < 3) {
        elog << "msg_M> Invalid number of arguments" << endl;
        return false;
    }

    // This source stuff really isn't used here, but it's here for
    // debugging and validation.
    iServer* serverSource = 0;
    iClient* clientSource = 0;

    // Note that the order of this if/else if/else is important
    if (NULL != strchr(Param[0], '.')) {
        // Server, by name
        serverSource = Network->findServerName(Param[0]);
    } else if (strlen(Param[0]) >= 3) {
        // Client numeric
        clientSource = Network->findClient(Param[0]);
    } else {
        // 1 or 2 char numeric, server
        serverSource = Network->findServer(Param[0]);
    }

    if ((NULL == clientSource) && (NULL == serverSource)) {
        elog << "msg_M> Unable to find source: " << Param[0] << endl;
        return false;
    }

    // Is it a user mode change?
    if ('#' != Param[1][0]) {
        // Yup, process the user's mode change(s)
        return onUserModeChange(Param);
    }

    // Find the channel in question
    Channel* theChan = Network->findChannel(Param[1]);
    if (NULL == theChan) {
        elog << "msg_M> Unable to find channel: " << Param[1] << endl;
        return false;
    }

    if (serverSource != 0) {
        //	elog	<< "msg_M ("
        //		<< theChan->getName()
        //		<< ") server "
        //		<< serverSource->getName()
        //		<< " performed a mode"
        //		<< end1;
        theServer->PostChannelEvent(EVT_SERVERMODE, theChan, static_cast<void*>(serverSource));
    }

    /* XXX OPMODE FAILS HERE */
    /* Hidden: But doesn't anymore */
    // Find the ChannelUser of the source client
    // It is possible that the ChannelUser will be NULL, in the
    // case that a server is setting the mode(s)
    ChannelUser* theUser = 0;
    if (clientSource != 0) {
        theUser = theChan->findUser(clientSource);
        if (NULL == theUser) {
            //		elog	<< "msg_M> ("
            //			<< theChan->getName()
            //			<< ") Unable to find channel user: "
            //			<< clientSource->getCharYYXXX()
            //			<< endl ;
            /* 2022-02-21: Hidden: Commenting line below to allow OPMODE to be used
            return false ;
            */
        }
    }

    // <source> M <#channel> <modes> [<args>...] [<channel timestamp>]
    const std::vector<std::string_view> args = Param.views(3);
    const Channel::ParsedModes parsed = Channel::parseModes(
        Param[2], args,
        {.trailingTimestamp = true, .protocol = theServer->getUplink()->getProtocol()});
    if (!parsed.ok()) {
        theServer->ProtocolError("msg_M>", parsed.problems);
    }

    // An older timestamp means the sender knows an older instance of the
    // channel, and we adopt it.  Only a real timestamp counts: an argument
    // that is not one must not be read as 0, the oldest time there is.
    if (parsed.timestamp && *parsed.timestamp != 0) {
        const time_t newCreationTime = static_cast<time_t>(*parsed.timestamp);
        if (theChan->getCreationTime() > newCreationTime) {
            setCreationTime(theChan, newCreationTime);
        }
    }

    theServer->ApplyChannelModes(theChan, theUser, parsed.changes, "msg_M>");

    return true;
}

// OAD M ripper_ :+owg
bool msg_M::onUserModeChange(const xParameters& Param) {
    // Since users aren't allowed to change modes for anyone other than
    // themselves, there is no need to lookup the second user argument
    // For some reason, when a user changes his/her/its modes, it still
    // specifies the second argument to be nickname instaed of numeric.
    iClient* theClient = Network->findClient(Param[0]);
    if (NULL == theClient) {
        elog << "msg_M::OnUserModeChange> Unable to find target "
             << "client: " << Param[1] << endl;
        return false;
    }

    if (theClient->getNickName() != Param[1]) {
        elog << "msg_M::OnUserModeChange> User trying to change "
             << "mode for someone other than itself: " << *theClient << ", nickname: " << Param[1]
             << endl;
        return false;
    }

    // It's important that the mode '+' be default
    bool plus = true;

    for (const char* modePtr = Param[2]; *modePtr; ++modePtr) {
        switch (*modePtr) {
        case '+':
            plus = true;
            break;
        case '-':
            plus = false;
            break;
        case 'i':
            if (plus)
                theClient->setModeI();
            else
                theClient->removeModeI();
            break;
        case 'k':
            if (plus)
                theClient->setModeK();
            else
                theClient->removeModeK();
            break;
        case 'd':
            if (plus)
                theClient->setModeD();
            else
                theClient->removeModeD();
            break;
        case 'w':
            if (plus)
                theClient->setModeW();
            else
                theClient->removeModeW();
            break;
        case 'x':
            if (plus)
                theClient->setModeX();
            else
                theClient->removeModeX();
            break;
        case 'r':
            if (plus)
                theClient->setModeR();
            else
                theClient->removeModeR();
            break;
        case 'g':
            if (plus)
                theClient->setModeG();
            else
                theClient->removeModeG();
            break;
        case 'o':
            if (plus) {
                theClient->setModeO();
                theServer->PostEvent(EVT_OPER, static_cast<void*>(theClient));
            } else {
                //				elog	<< "msg_M::onUserModeChange> "
                //					<< "Caught -o for user: "
                //					<< *theClient
                //					<< endl ;
                theClient->removeModeO();
            }
            break;
        case 'n':
        case 'I':
        case 'h':
        case 'X':
        case 'R':
        case 'f':
            // Unsupported but used on networks that
            // GNUWorld runs on.
            // TODO?
            break;
        default:
            elog << "msg_M::onUserModeChange> "
                 << "Unknown mode: " << *modePtr << endl;
            break;
        } // close switch
    } // close for
    return true;
}

} // namespace gnuworld
