/**
 * LogExtractors.cc
 * The extractors of the core objects for the logging system.
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
 */

#include <cstdint>
#include <string>

#include "Channel.h"
#include "ChannelUser.h"
#include "LogExtractors.h"
#include "LogRecord.h"
#include "iClient.h"
#include "iServer.h"
#include "ip.h"

namespace gnuworld {

namespace {

/// What every extractor makes of a pointer it is not supposed to be given
LogObject nullObject() { return LogObject{"(null)", {}}; }

} // namespace

LogObject logObjectFor(const iClient* client) {
    if (nullptr == client)
        return nullObject();

    LogObject object;

    object.display = client->getNickName();

    object.fields.emplace_back("nick", std::string(client->getNickName()));
    object.fields.emplace_back("userhost", std::string(client->getRealUserHost()));
    object.fields.emplace_back("ip", xIP(client->getIP()).GetNumericIP());
    object.fields.emplace_back("numeric", std::string(client->getCharYYXXX()));

    if (client->isModeR()) {
        object.fields.emplace_back("account", std::string(client->getAccount()));
        object.fields.emplace_back("account_id",
                                   static_cast<std::uint64_t>(client->getAccountID()));
    }

    object.fields.emplace_back("is_oper", client->isOper());

    return object;
}

LogObject logObjectFor(const iServer* server) {
    if (nullptr == server)
        return nullObject();

    LogObject object;

    object.display = server->getName();

    object.fields.emplace_back("name", std::string(server->getName()));
    object.fields.emplace_back("numeric", std::string(server->getCharYY()));
    object.fields.emplace_back("uplink", static_cast<std::uint64_t>(server->getUplinkIntYY()));
    object.fields.emplace_back("is_bursting", server->isBursting());

    return object;
}

LogObject logObjectFor(const Channel* channel) {
    if (nullptr == channel)
        return nullObject();

    LogObject object;

    object.display = channel->getName();

    object.fields.emplace_back("name", std::string(channel->getName()));
    object.fields.emplace_back("modes", std::string(channel->getModeString()));

    return object;
}

LogObject logObjectFor(const ChannelUser* theUser) {
    if (nullptr == theUser)
        return nullObject();

    LogObject object;

    const iClient* const client = theUser->getClient();

    object.display = nullptr == client ? "(null)" : client->getNickName();

    object.fields.emplace_back("is_op", theUser->getMode(ChannelUser::MODE_CHANOP));
    object.fields.emplace_back("is_voice", theUser->getMode(ChannelUser::MODE_VOICE));

    return object;
}

void registerCoreLogExtractors() {
#if 0 // enabled in logger-flip
    Logger::registerExtractor<iClient>(nullptr, [](const iClient* c) { return logObjectFor(c); });
    Logger::registerExtractor<iServer>(nullptr, [](const iServer* s) { return logObjectFor(s); });
    Logger::registerExtractor<Channel>(nullptr, [](const Channel* c) { return logObjectFor(c); });
    Logger::registerExtractor<ChannelUser>(nullptr,
                                           [](const ChannelUser* u) { return logObjectFor(u); });
#endif
}

} // namespace gnuworld
