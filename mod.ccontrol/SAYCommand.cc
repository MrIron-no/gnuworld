/**
 * SAYCommand.cc
 * Forces the bot to quote a command
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
 * $Id: SAYCommand.cc,v 1.8 2006/09/26 17:36:01 kewlio Exp $
 */

#include <string>
#include "ccontrol.h"
#include "CControlCommands.h"
#include "StringTokenizer.h"
#include "Network.h"
#include "gnuworld_config.h"

namespace gnuworld {

using std::string;

namespace uworld {

bool SAYCommand::Exec(iClient* theClient, const string& Message) {

    StringTokenizer st(Message);
    if (st.size() < 4) {
        Usage(theClient);
        return true;
    }

    bot->MsgChanLog("%s\n", st.assemble(0).c_str());

    const bool asServer = !strcasecmp(st[1].c_str(), "-s");
    if (!asServer && strcasecmp(st[1].c_str(), "-b")) {
        bot->Notice(theClient, "First argument must be -s for server, or -b for bot");
        return true;
    }

    Channel* targetChan = 0;
    iClient* targetClient = 0;
    if (!strcasecmp(st[2].substr(0, 1), "#")) {
        targetChan = Network->findChannel(st[2]);
        if (!targetChan) {
            bot->Notice(theClient, "Sorry, but i can't find channel %s", st[2].c_str());
            return true;
        }
    } else {
        targetClient = Network->findNick(st[2]);
        if (!targetClient) {
            bot->Notice(theClient, "Sorry, but i can't find nick: %s", st[2].c_str());
            return true;
        }
    }

    // SAY, or DO: an action
    const string text = !match("SAY", st[0])
                            ? st.assemble(3)
                            : ('\001' + string("ACTION ") + st.assemble(3) + '\001');

    if (targetChan != 0) {
        asServer ? bot->getUplink()->serverMessage(targetChan, text)
                 : bot->Message(targetChan, text);
    } else {
        asServer ? bot->getUplink()->Message(targetClient, text) : bot->Message(targetClient, text);
    }
    return true;
}

} // namespace uworld
} // namespace gnuworld
