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
#include "ELog.h"
#include "ServerCommandHandler.h"

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
    if (Param.size() < 3) {
        elog << "msg_CM> Invalid number of parameters" << endl;
        return false;
    }

    Channel* tmpChan = Network->findChannel(Param[1]);
    if (!tmpChan) {
        // Log Error.
        elog << "msg_CM> Unable to locate channel: " << Param[1] << endl;
        return false;
    }

    /*
     * First, determine what we are going to clear.
     */
    std::string Modes = Param[2];

    std::vector<std::string> problems;
    for (const char letter : Modes) {
        if (!Channel::findMode(letter)) {
            problems.push_back(std::string(Channel::isLocalOnlyMode(letter)
                                               ? "mode is local to a server: "
                                               : "unknown mode: ") +
                               letter);
        }
    }
    if (!problems.empty()) {
        theServer->ProtocolError("msg_CM>", problems);
    }

    // These three variables will be set to true if we are to clear either
    // the ops, voice, or bans, respectively
    bool clearOps = false;
    bool clearVoice = false;
    bool clearBans = false;

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

    xServer::modeVectorType modeVector;

    for (const char letter : Modes) {
        // Every letter was looked up above
        const Channel::ModeInfo mode = *Channel::findMode(letter);

        switch (mode.type) {
        case Channel::ModeType::Flag:
            modeVector.push_back(make_pair(false, mode.flag));
            break;
        case Channel::ModeType::SetOnly:
            theServer->OnChannelModeL(tmpChan, false, 0, 0);
            break;
        case Channel::ModeType::Setting:
            if ('k' == letter) {
                theServer->OnChannelModeK(tmpChan, false, 0, std::string());
            } else if ('A' == letter) {
                theServer->OnChannelModeA(tmpChan, false, 0, std::string());
            } else {
                theServer->OnChannelModeU(tmpChan, false, 0, std::string());
            }
            break;
        case Channel::ModeType::Prefix:
            ('o' == letter ? clearOps : clearVoice) = true;
            break;
        case Channel::ModeType::List:
            clearBans = true;
            break;
        }
    }

    if (!modeVector.empty()) {
        theServer->OnChannelMode(tmpChan, 0, modeVector);
    }

    if (clearOps || clearVoice) {
        /*
         * Lets loop over everyone in the channel and either deop
         * or devoice them.
         */
        xServer::opVectorType opVector;
        xServer::voiceVectorType voiceVector;

        for (Channel::const_userIterator ptr = tmpChan->userList_begin();
             ptr != tmpChan->userList_end(); ++ptr) {
            if (clearOps && ptr->second->isModeO()) {
                opVector.push_back(pair<bool, ChannelUser*>(false, ptr->second));
            }
            if (clearVoice && ptr->second->isModeV()) {
                voiceVector.push_back(pair<bool, ChannelUser*>(false, ptr->second));
            }
        }

        if (!voiceVector.empty()) {
            theServer->OnChannelModeV(tmpChan, 0, voiceVector);
        }
        if (!opVector.empty()) {
            theServer->OnChannelModeO(tmpChan, 0, opVector);
        }
    } // if( clearOps || clearVoice )

    if (clearBans) {
        xServer::banVectorType banVector;

        for (Channel::banIterator ptr = tmpChan->banList_begin(), endPtr = tmpChan->banList_end();
             ptr != endPtr; ++ptr) {
            banVector.push_back(pair<bool, std::string>(false, *ptr));
        }

        theServer->OnChannelModeB(tmpChan, 0, banVector);
    } // if( clearBans )

    return true;
}

} // namespace gnuworld
