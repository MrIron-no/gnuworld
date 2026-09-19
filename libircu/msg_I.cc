/**
 * msg_I.cc
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
 * $Id: msg_I.cc,v 1.7 2007/04/18 10:23:39 kewlio Exp $
 */

#include <iostream>

#include "gnuworld_config.h"
#include "server.h"
#include "xparameters.h"
#include "Channel.h"
#include "iClient.h"
#include "client.h"
#include "Network.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_MODULE_LOGGER("core.proto");

namespace gnuworld {
using std::endl;

CREATE_HANDLER(msg_I)

// ABAHo I X :#lksdlkj                           (non-ts)
// ABAHo I X :#lksdlkj 1234567890                (ts)
bool msg_I::Execute(const xParameters& Param) {
    theServer->RequireParameters("msg_I>", Param, 3);

    iClient* srcClient = Network->findClient(Param[0]);
    if (NULL == srcClient) {
        LOG(WARN, "Unable to find source client: {}", std::string(Param[0]));
        return false;
    }

    xClient* destClient = Network->findLocalNick(Param[1]);
    if (NULL == destClient) {
        LOG(WARN, "Unable to find destination client: {}", std::string(Param[1]));
        return false;
    }

    Channel* theChan = Network->findChannel(Param[2]);
    if (NULL == theChan) {
        LOG(WARN, "Unable to find channel: {}", std::string(Param[2]));
        return false;
    }

    destClient->OnInvite(srcClient, theChan);
    return true;
}

} // namespace gnuworld
