/**
 * msg_RV.cc
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

#include <cstdlib>
#include <ctime>

#include "gnuworld_config.h"
#include "server.h"
#include "iClient.h"
#include "Channel.h"
#include "Network.h"
#include "ELog.h"
#include "xparameters.h"
#include "ServerCommandHandler.h"

namespace gnuworld {

using std::endl;

CREATE_HANDLER(msg_RV)

/**
 * REVEAL (P11 only, see ircu doc/P11.md 8.12).
 *
 * ACAAO RV #p11-deljoin 1789633182
 *
 * ACAAO: the member that has revealed itself
 * #p11-deljoin: the channel
 * 1789633182: the channel's creation time, as known by the sender
 *
 * A hidden (delayed join) member has revealed itself by speaking to the
 * channel.  Channel messages only travel toward servers that have a member
 * on the channel, so this token is what tells everyone else.  Reveals
 * caused by MODE (+o/+v) or TOPIC carry no token; those commands already
 * reach every server.
 *
 * We are a leaf, so there is nothing to relay.
 */
bool msg_RV::Execute(const xParameters& Param) {
    if (Param.size() < 3) {
        elog << "msg_RV> Invalid number of arguments" << endl;
        return false;
    }

    iClient* theClient = Network->findClient(Param[0]);
    if (0 == theClient) {
        // A server source is a protocol violation, and so is a client
        // we have never heard of.
        elog << "msg_RV> (" << Param[1] << "): Unable to find client: " << Param[0] << endl;
        return false;
    }

    if (Param[1][0] != '#') {
        elog << "msg_RV> Not a global channel: " << Param[1] << endl;
        return false;
    }

    Channel* theChan = Network->findChannel(Param[1]);
    if (0 == theChan) {
        // Every server should have the channel.  If we do not, there is
        // nothing to reveal.
        return true;
    }

    // A reveal for a newer incarnation of the channel lost the timestamp
    // race: it is not about the channel we know.
    const time_t chanTS = theServer->RequireTimestamp("msg_RV>", "channel timestamp", Param[2]);
    if (chanTS > theChan->getCreationTime()) {
        return true;
    }

    // Idempotent: the member may already be visible, or may have left.
    revealUser(theChan, theClient);

    return true;
}

} // namespace gnuworld
