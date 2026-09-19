/**
 * msg_AC.cc
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
 * "$Id: msg_AC.cc,v 1.9 2005/03/25 03:07:29 dan_karrels Exp $"
 */

#include <string>
#include <iostream>

#include "gnuworld_config.h"
#include "ServerCommandHandler.h"
#include "server.h"
#include "xparameters.h"
#include "Channel.h"
#include "Network.h"
#include "iClient.h"
#include "logger.h"

GNUWORLD_MODULE_LOGGER("core.proto");

namespace gnuworld {

CREATE_HANDLER(msg_AC)

/**
 * ACCOUNT message handler.
 * SOURCE AC TARGET ACCOUNT ACCOUNT_ID ACCOUNT_FLAGS
 * Eg:
 * AXAAA AC BQrTd Hidden 1694970265 1024
 * Note: ACCOUNT_ID and ACCOUNT_FLAGS are optional
 */
bool msg_AC::Execute(const xParameters& Param) {
    theServer->RequireParameters("msg_AC>", Param, 3);

    // Find the target user
    iClient* theClient = Network->findClient(Param[1]);
    if (!theClient) {
        LOG(WARN, "Unable to find target client: {}", std::string(Param[1]));
        return false;
    }

    // The id and the flags are optional.  ircu keeps both as 64 bit numbers
    // and writes them itself, so one that is not a number is its error.
    const std::string account(Param[2]);
    unsigned int account_id = 0;
    iClient::flagType account_flags = 0;

    if (Param.has(3)) {
        account_id = static_cast<unsigned int>(
            theServer->RequireNumber<std::uint64_t>("msg_AC>", "account id", Param[3]));
    }
    if (Param.has(4)) {
        account_flags = static_cast<iClient::flagType>(
            theServer->RequireNumber<std::uint64_t>("msg_AC>", "account flags", Param[4]));
    }

    // Is this a change of flags or a new login?
    bool alreadyAuthed = false;
    if (theClient->isModeR())
        alreadyAuthed = true;

    // Update user information
    theClient->setAccount(account);
    theClient->setAccountID(account_id);
    theClient->setAccountFlags(account_flags);

    // Post event to listening clients
    theServer->PostEvent(alreadyAuthed ? EVT_ACCOUNT_FLAGS : EVT_ACCOUNT,
                         static_cast<void*>(theClient));

    // Return success
    return true;
}

} // namespace gnuworld
