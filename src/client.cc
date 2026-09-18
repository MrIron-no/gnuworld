/**
 * client.cc
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *		Orlando Bassotto
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
 * $Id: client.cc,v 1.89 2008/04/16 20:29:39 danielaustin Exp $
 */

#include <new>
#include <map>
#include <string>
#include <span>
#include <optional>
#include <sstream>
#include <vector>
#include <iostream>
#include <filesystem>

#include <cstdio>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

#include "gnuworld_config.h"
#include "misc.h"
#include "iClient.h"
#include "iServer.h"
#include "Network.h"
#include "ip.h"
#include "NetworkTarget.h"
#include "client.h"
#include "EConfig.h"
#include "StringTokenizer.h"
#include "ELog.h"
#ifdef HAVE_PGSQL
#include "MigrationChecker.h"
#endif
#include "events.h"

namespace gnuworld {

using std::endl;
using std::make_pair;
using std::string;
using std::stringstream;

xClient::xClient() {}

xClient::xClient(const string& fileName) : configFileName(fileName) {
    EConfig conf(fileName);
    nickName = conf.Require("nickname")->second;
    userName = conf.Require("username")->second;
    hostName = conf.Require("hostname")->second;
    userDescription = conf.Require("userdescription")->second;

    Mode(conf.Require("mode")->second);

    if (conf.Find("stealth") != conf.end()) {
        stealth = conf.Require<bool>("stealth");
    }

    /* Initialize logger */
    logger = std::make_unique<Logger>(this);
}

xClient::~xClient() {}

void xClient::BurstChannels() {}

bool xClient::BurstGlines() { return true; }

void xClient::OnAttach() {
    // We are connected to an xServer here.
    Connected = true;
}

void xClient::OnDetach(const string& Message) {
    if (!isConnected()) {
        return;
    }

    // Stealth modules were never introduced on the network
    if (!IsStealth()) {
        stringstream s;
        s << getCharYYXXX() << " Q :" << Message;
        MyUplink->Write(s);
    }

    // xClient is no longer connected to the xServer.
    Connected = false;
}

string xClient::getModes() const {
    string Mode("+");

    if (mode & iClient::MODE_DEAF)
        Mode += 'd';
    if (mode & iClient::MODE_SERVICES)
        Mode += 'k';
    if (mode & iClient::MODE_OPER)
        Mode += 'o';
    if (mode & iClient::MODE_WALLOPS)
        Mode += 'w';
    if (mode & iClient::MODE_INVISIBLE)
        Mode += 'i';
    if (mode & iClient::MODE_TLS)
        Mode += 'z';

    return Mode;
}

bool xClient::Mode(const string& Value) {
    // elog	<< "xClient::Mode> Value: "
    //	<< Value
    //	<< endl ;

    // Set the bot's modes, and output to
    // the network if we are connected

    // Clear the internal modes
    mode = 0;

    // Iterate through the array and
    // set modes appropriately
    string::const_iterator ptr = Value.begin(), end = Value.end();

    for (; ptr != end; ++ptr) {
        switch (*ptr) {
        case '+':
            break;
        case 'd':
            mode |= iClient::MODE_DEAF;
            break;
        case 'k':
            mode |= iClient::MODE_SERVICES;
            break;
        case 'o':
            mode |= iClient::MODE_OPER;
            break;
        case 'w':
            mode |= iClient::MODE_WALLOPS;
            break;
        case 'i':
            mode |= iClient::MODE_INVISIBLE;
            break;
        case 'z':
            mode |= iClient::MODE_TLS;
            break;

        default:
            elog << "xClient::Mode> Unknown mode: " << *ptr << endl;
            break;
        } // switch()
    } // close while

    // Output to the network if we are connected
    // Stealth modules have no client numeric on the wire
    if (isConnected() && !Value.empty() && !IsStealth()) {
        stringstream s;
        s << getCharYYXXX() << " M " << getCharYYXXX() << " " << Value;

        return MyUplink->Write(s);
    }

    return false;
}

bool xClient::QuoteAsServer(const string& Message) {
    if (isConnected()) {
        return MyUplink->Write(Message);
    }
    return false;
}

bool xClient::Wallops(const string& Message) { return Write(getCharYYXXX() + " WA :" + Message); }

bool xClient::WallopsAsServer(const string& buf) {
    if (!isConnected()) {
        return false;
    }
    return MyUplink->Wallops(buf);
}

bool xClient::Mode(const string& chanName, const string& modes, const string& args,
                   bool modeAsServer) {
    if (!isConnected()) {
        return false;
    }

    Channel* theChan = Network->findChannel(chanName);
    if (0 == theChan) {
        return false;
    }

    return Mode(theChan, modes, args, modeAsServer);
}

bool xClient::Mode(Channel* theChan, const string& modes, const string& args, bool modeAsServer) {
    assert(theChan != 0);
    // A stealth module has no client on the network to act as
    modeAsServer = modeAsServer || IsStealth();

    if (!isConnected()) {
        return false;
    }

    bool doJoinPart = false;
    if (!modeAsServer && !isOnChannel(theChan)) {
        doJoinPart = true;
        Join(theChan, string(), 0, true);
    }

    xClient* theClient = 0;
    if (!modeAsServer) {
        // Set the mode as the client, so theClient needs to
        // be non-NULL
        theClient = this;
    }

    bool retVal = getUplink()->Mode(theClient, theChan, modes, args);

    if (doJoinPart) {
        Part(theChan);
    }
    return retVal;
}

namespace {

/// "\001<CTCP>[ <message>]\001", with no stray space when there is no message.
string ctcpText(const string& CTCP, const string& Message) {
    string text("\001");
    text += CTCP;
    if (!Message.empty()) {
        text += ' ' + Message;
    }
    text += '\001';
    return text;
}

/// A channel name given with or without its '#'.
string channelName(const string& name) {
    return (!name.empty() && '#' == name[0]) ? name : '#' + name;
}

} // namespace

/*
 * Every message below is sent by xServer::sendText(), from getInstance():
 * this client, or the server if it is a stealth module with no client on the
 * network.
 */

bool xClient::DoCTCP(iClient* Target, const string& CTCP, const string& Message) {
    if (!isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", getInstance(), Target->getCharYYXXX(), ctcpText(CTCP, Message));
}

bool xClient::DoFakeCTCP(const iClient* destClient, const iClient* srcClient, const string& CTCP,
                         const string& Message) {
    assert(destClient != 0);
    assert(srcClient != 0);

    if (!isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", srcClient, destClient->getCharYYXXX(), ctcpText(CTCP, Message));
}

bool xClient::FakeMessage(const iClient* destClient, const iClient* srcClient,
                          const string& Message) {
    if (Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("P", srcClient, destClient->getCharYYXXX(), Message);
}

bool xClient::FakeNotice(const iClient* destClient, const iClient* srcClient,
                         const string& Message) {
    if (Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", srcClient, destClient->getCharYYXXX(), Message);
}

bool xClient::FakeMessage(const Channel* theChan, const iClient* srcClient, const string& Message) {
    if (Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("P", srcClient, theChan->getName(), Message);
}

bool xClient::FakeNotice(const Channel* theChan, const iClient* srcClient, const string& Message) {
    if (Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", srcClient, theChan->getName(), Message);
}

bool xClient::Message(const iClient* Target, const string& Message) {
    if (!isConnected()) {
        return false;
    }
    return MyUplink->sendText("P", getInstance(), Target->getCharYYXXX(), Message);
}

bool xClient::Message(const Channel* theChan, const string& Message) {
    assert(theChan != 0);

    if (!isConnected()) {
        return false;
    }
    return MyUplink->sendText("P", getInstance(), theChan->getName(), Message);
}

bool xClient::Message(const string& chanName, const string& Message) {
    if (chanName.empty() || Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("P", getInstance(), channelName(chanName), Message);
}

bool xClient::Notice(const iClient* Target, const string& Message) {
    if (!isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", getInstance(), Target->getCharYYXXX(), Message);
}

bool xClient::Notice(const string& Channel, const string& Message) {
    if (Channel.empty() || Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", getInstance(), channelName(Channel), Message);
}

bool xClient::Notice(const Channel* theChan, const string& Message) {
    assert(theChan != 0);

    if (Message.empty() || !isConnected()) {
        return false;
    }
    return MyUplink->sendText("O", getInstance(), theChan->getName(), Message);
}

bool xClient::NoticeChannelOps(const Channel* theChan, const string& Message) {
    assert(theChan != 0);

    // Nothing to say is not a failure, as it never was
    if (Message.empty() || !isConnected()) {
        return true;
    }
    return MyUplink->sendText("WC", getInstance(), theChan->getName(), Message);
}

bool xClient::NoticeChannelOps(const string& chanName, const string& Message) {
    const Channel* theChan = Network->findChannel(chanName);
    return (theChan != 0) && NoticeChannelOps(theChan, Message);
}

void xClient::OnCTCP(iClient*, const string&, const string&, bool) {}

void xClient::OnFakeCTCP(iClient*, iClient*, const string&, const string&, bool) {}

void xClient::OnChannelCTCP(iClient*, Channel*, const string&, const string&) {}

void xClient::OnFakeChannelCTCP(iClient*, iClient*, Channel*, const string&, const string&) {}

void xClient::OnEvent(const eventType&, void*, void*, void*, void*) {}

void xClient::OnChannelEvent(const channelEventType&, Channel*, void*, void*, void*, void*) {}

void xClient::OnNetworkKick(Channel*,
                            iClient*,      // srcClient, may be NULL
                            iClient*,      // destClient
                            const string&, // kickMessage,
                            bool)          // authoritative
{}

void xClient::OnChannelMode(Channel*, ChannelUser*, const xServer::modeVectorType&) {}

void xClient::OnChannelModeL(Channel*, bool, ChannelUser*, const unsigned int&) {}

void xClient::OnChannelModeK(Channel*, bool, ChannelUser*, const string&) {}

void xClient::OnChannelModeA(Channel*, bool, ChannelUser*, const string&) {}

void xClient::OnChannelModeU(Channel*, bool, ChannelUser*, const string&) {}

void xClient::OnChannelModeO(Channel*, ChannelUser*, const xServer::opVectorType&) {}

void xClient::OnChannelModeV(Channel*, ChannelUser*, const xServer::voiceVectorType&) {}

void xClient::OnChannelModeB(Channel*, ChannelUser*, const xServer::banVectorType&) {}

void xClient::OnPrivateMessage(iClient*, const string&, bool) {}

void xClient::OnFakePrivateMessage(iClient*, iClient*, const string&, bool) {}

void xClient::OnFakeChannelMessage(iClient*, iClient*, Channel*, const string&) {}

void xClient::OnChannelMessage(iClient*, Channel*, const string&) {}

void xClient::OnPrivateNotice(iClient*, const string&, bool) {}

void xClient::OnFakePrivateNotice(iClient*, iClient*, const string&, bool) {}

void xClient::OnChannelNotice(iClient*, Channel*, const string&) {}

void xClient::OnFakeChannelNotice(iClient*, iClient*, Channel*, const string&) {}

void xClient::OnServerMessage(iServer*, const string&, bool) {}

void xClient::OnConnect() {}

void xClient::OnDisconnect() {}

void xClient::OnShutdown(const string& /* reason */) {}

void xClient::OnKill() {}

void xClient::OnWhois(iClient*, iClient*) {}

void xClient::OnInvite(iClient*, Channel*) {}

bool xClient::Kill(iClient* theClient, const string& reason) {
    return Kill(theClient, reason, true);
}

bool xClient::Kill(iClient* theClient, const string& reason, bool asServer) {
    assert(theClient != 0);

    if (theClient->isModeK() || !isConnected()) {
        return false;
    }

    if (asServer) {
        if (getUplink()->getUplink()->getProtocol() < 11) {
            Write("{} D {} :{} ({})", MyUplink->getCharYY(), theClient->getCharYYXXX(),
                  MyUplink->getName(), reason);
        } else {
            Write("{} D {} {} :{}", MyUplink->getCharYY(), theClient->getCharYYXXX(),
                  MyUplink->getName(), reason);
        }
    } else {
        if (getUplink()->getUplink()->getProtocol() < 11) {
            Write("{} D {} :{} ({})", getCharYYXXX(), theClient->getCharYYXXX(), getNickName(),
                  reason);
        } else {
            Write("{} D {} {} :{}", getCharYYXXX(), theClient->getCharYYXXX(), getNickName(),
                  reason);
        }
    }

    // Why was all this commented out? -- gk
    // beats me -- dan

    // Do NOT cast away constness
    string localReason(reason);

    MyUplink->PostEvent(EVT_KILL, 0, static_cast<void*>(theClient),
                        static_cast<void*>(&localReason));

    // Remove the user
    delete Network->removeClient(theClient);

    return true;
}

bool xClient::enterToChange(Channel* theChan, bool& joined) {
    joined = false;

    if (!isOnChannel(theChan)) {
        // Join, having the server op us; the caller parts again
        Join(theChan, string(), 0, true);
        joined = true;
        return true;
    }

    const ChannelUser* meUser = theChan->findUser(me);
    if (NULL == meUser) {
        elog << "xClient::enterToChange> Unable to find myself in channel: " << theChan->getName()
             << endl;
        return false;
    }
    return meUser->isModeO();
}

/**
 * Op(), DeOp(), Voice() and DeVoice() as this client.  What is to change is
 * worked out, sent and applied by xServer; what is ours is being on the
 * channel with ops while it happens.
 */
bool xClient::changeMembers(Channel* theChan, char letter, bool set,
                            std::span<iClient* const> targets) {
    assert(theChan != 0);

    if (!isConnected()) {
        return false;
    }

    if (IsStealth()) {
        const std::vector<iClient*> all(targets.begin(), targets.end());
        const bool isOp = ('o' == letter);
        return isOp ? (set ? MyUplink->Op(theChan, all) : MyUplink->DeOp(theChan, all))
                    : (set ? MyUplink->Voice(theChan, all) : MyUplink->DeVoice(theChan, all));
    }

    const std::optional<xServer::opVectorType> members =
        MyUplink->planMemberModes(theChan, letter, set, targets);
    if (!members) {
        return false;
    }
    if (members->empty()) {
        // Nothing to change, so nothing to join for
        return true;
    }

    bool joined = false;
    if (!enterToChange(theChan, joined)) {
        return false;
    }

    // These have always reported the change without a source member
    MyUplink->commitMemberModes(getCharYYXXX(), 0, theChan, letter, *members);

    if (joined) {
        Part(theChan);
    }
    return true;
}

/// Every Ban() and UnBan() as this client.
bool xClient::changeBans(Channel* theChan, xServer::banVectorType bans) {
    assert(theChan != 0);

    if (!isConnected()) {
        return false;
    }
    if (bans.empty()) {
        return true;
    }
    if (IsStealth()) {
        return MyUplink->Ban(theChan, bans);
    }

    bool joined = false;
    if (!enterToChange(theChan, joined)) {
        return false;
    }

    MyUplink->commitBans(getCharYYXXX(), 0, theChan, std::move(bans));

    if (joined) {
        Part(theChan);
    }
    return true;
}

bool xClient::Op(Channel* theChan, iClient* theClient) {
    iClient* const target[] = {theClient};
    return changeMembers(theChan, 'o', true, target);
}

bool xClient::Op(Channel* theChan, const std::vector<iClient*>& clientVector) {
    return changeMembers(theChan, 'o', true, clientVector);
}

bool xClient::DeOp(Channel* theChan, iClient* theClient) {
    iClient* const target[] = {theClient};
    return changeMembers(theChan, 'o', false, target);
}

bool xClient::DeOp(Channel* theChan, const std::vector<iClient*>& clientVector) {
    return changeMembers(theChan, 'o', false, clientVector);
}

bool xClient::Voice(Channel* theChan, iClient* theClient) {
    iClient* const target[] = {theClient};
    return changeMembers(theChan, 'v', true, target);
}

bool xClient::Voice(Channel* theChan, const std::vector<iClient*>& clientVector) {
    return changeMembers(theChan, 'v', true, clientVector);
}

bool xClient::DeVoice(Channel* theChan, iClient* theClient) {
    iClient* const target[] = {theClient};
    return changeMembers(theChan, 'v', false, target);
}

bool xClient::DeVoice(Channel* theChan, const std::vector<iClient*>& clientVector) {
    return changeMembers(theChan, 'v', false, clientVector);
}

bool xClient::Ban(Channel* theChan, iClient* theClient) {
    assert(theChan != NULL);
    assert(theClient != NULL);

    // A network service (+k) is not banned
    if (theClient->isModeK()) {
        return false;
    }
    iClient* const target[] = {theClient};
    return changeBans(theChan, MyUplink->planBans(theChan, target));
}

bool xClient::Ban(Channel* theChan, const std::vector<iClient*>& clientVector) {
    assert(theChan != nullptr);
    return changeBans(theChan, MyUplink->planBans(theChan, clientVector));
}

bool xClient::Ban(Channel* theChan, const xServer::banVectorType& banVector) {
    assert(theChan != nullptr);
    return changeBans(theChan, MyUplink->planBans(theChan, banVector));
}

bool xClient::UnBan(Channel* theChan, const string& banMask) {
    assert(theChan != 0);
    return changeBans(theChan,
                      MyUplink->planBans(theChan, xServer::banVectorType{{false, banMask}}));
}

bool xClient::UnBan(Channel* theChan, const xServer::banVectorType& banVector) {
    assert(theChan != 0);
    return changeBans(theChan, MyUplink->planBans(theChan, banVector));
}

bool xClient::BanKick(Channel* theChan, iClient* theClient, const string& reason) {
    assert(theChan != 0);
    assert(theClient != 0);

    if (!isConnected()) {
        return false;
    }

    if (theClient->isModeK()) {
        return false;
    }

    if (0 == theChan->findUser(theClient)) {
        // User is not on that channel
        return true;
    }

    bool OnChannel = isOnChannel(theChan);
    if (!OnChannel) {
        // Join, giving ourselves ops
        Join(theChan, string(), 0, true);
    } else {
        // Bot is already on the channel
        ChannelUser* meUser = theChan->findUser(me);
        if (NULL == meUser) {
            elog << "xClient::BanKick> Unable to find myself in "
                 << "channel: " << theChan->getName() << endl;
            return false;
        }

        // Make sure we have ops
        if (!meUser->getMode(ChannelUser::MODE_O)) {
            // The bot does NOT have ops
            return false;
        }

        // The bot has ops
    }

    string banMask = Channel::createBan(theClient);

    const Channel::ModeChange ban{true, *Channel::findMode('b'), banMask};
    MyUplink->SendChannelModes(getCharYYXXX(), theChan, std::span(&ban, 1));

    Write("{} K {} {} :{}", getCharYYXXX(), theChan->getName(), theClient->getCharYYXXX(), reason);

    if (!OnChannel) {
        Part(theChan);
    }

    // Update the channel's ban list
    theChan->setBan(banMask);

    return true;
}

bool xClient::Topic(Channel* theChan, const std::string& newTopic) {
    assert(theChan != 0);
    // An empty topic is fine

    if (!isConnected()) {
        return false;
    }

    if (IsStealth()) {
        // No client on the network to join with: the server sets it
        return MyUplink->Topic(theChan, newTopic);
    }

    // We have to be on the channel, and opped if it is +t.  Joining has
    // the server op us; if we are already there without ops, it does so now.
    bool joined = false;
    if (!isOnChannel(theChan)) {
        Join(theChan, string(), 0, true);
        joined = true;
    } else if (theChan->getMode(Channel::MODE_T)) {
        MyUplink->Op(theChan, me);
    }

    const bool sent = MyUplink->Topic(theChan, newTopic, getInstance());

    if (joined) {
        Part(theChan);
    }
    return sent;
}

bool xClient::Kick(Channel* theChan, iClient* theClient, const string& reason, bool modeAsServer) {
    assert(theChan != NULL);
    assert(theClient != NULL);

    iClient* const target[] = {theClient};
    if (MyUplink->planKick(theChan, target).empty()) {
        // A network service, or not on the channel
        return false;
    }
    return Kick(theChan, std::vector<iClient*>(target, target + 1), reason, modeAsServer);
}

bool xClient::Kick(Channel* theChan, const std::vector<iClient*>& theClients, const string& reason,
                   bool modeAsServer) {
    assert(theChan != NULL);
    // A stealth module has no client on the network to act as
    modeAsServer = modeAsServer || IsStealth();

    if (!isConnected()) {
        return false;
    }

    const std::vector<iClient*> kicked = MyUplink->planKick(theChan, theClients);
    if (kicked.empty()) {
        return true;
    }

    // As ourselves we have to be on the channel, opped, while it happens.
    // Either way the modules are told that we did it, as they always were.
    bool joined = false;
    if (!modeAsServer && !enterToChange(theChan, joined)) {
        return false;
    }

    MyUplink->commitKick(modeAsServer ? string(MyUplink->getCharYY()) : getCharYYXXX(),
                         getInstance(), theChan, kicked, reason);

    if (joined) {
        Part(theChan);
    }
    if (theChan->empty()) {
        delete Network->removeChannel(theChan->getName());
    }
    return true;
}

bool xClient::Kick(Channel* theChan, const string& IP, const string& reason, bool modeAsServer) {
    assert(theChan != NULL);

    if (!isConnected()) {
        return false;
    }

    if (IP.empty()) {
        return true;
    }

    bool OnChannel = isOnChannel(theChan);
    if (!OnChannel && !modeAsServer) {
        // Join, giving ourselves ops
        Join(theChan, string(), 0, true);
    } else if (!modeAsServer) {
        // Bot is already on the channel
        ChannelUser* meUser = theChan->findUser(me);
        assert(meUser != 0);

        // Make sure we have ops
        if (!meUser->getMode(ChannelUser::MODE_O)) {
            // The bot does NOT have ops
            return false;
        }

        // The bot has ops
    }
    std::vector<iClient*> toBoot;
    for (Channel::userIterator chanUsers = theChan->userList_begin();
         chanUsers != theChan->userList_end(); ++chanUsers) {
        ChannelUser* tmpUser = chanUsers->second;
        string currIP = xIP(tmpUser->getClient()->getIP()).GetNumericIP();
        string currIP64 = xIP(tmpUser->getClient()->getIP()).GetNumericIP(true);
        /* Idented and unidented clients need to be handled separately
         * In case of floodpro kick, IP can be in format of ident@ip !
         */
        if (IP.find('@') != string::npos) {
            currIP = tmpUser->getUserName() + "@" + currIP;
            currIP64 = tmpUser->getUserName() + "@" + currIP64;
        }
        if ((!IP.compare(currIP)) || (!IP.compare(currIP64))) {
            /* Don't kick +k things */
            if (!tmpUser->getClient()->getMode(iClient::MODE_SERVICES)) {
                toBoot.push_back(tmpUser->getClient());
            }
        }
    }
    return Kick(theChan, toBoot, reason, modeAsServer);
}

bool xClient::Join(const string& chanName, const string& chanModes, const time_t& joinTime,
                   bool getOps) {
    Channel* theChan = Network->findChannel(chanName);
    return theChan ? Join(theChan, chanModes, joinTime, getOps)
                   : MyUplink->JoinChannel(this, chanName, chanModes, joinTime, getOps);
}

bool xClient::Join(Channel* theChan, const string& chanModes, const time_t& joinTime, bool getOps) {
    if (!isConnected()) {
        return false;
    }
    assert(theChan != NULL);
    if ((joinTime > 0) && (joinTime < theChan->getCreationTime())) {
        // The join is older than the creation time of the channel, need to remove all the modes of
        // the channel
        theChan->removeAllModes();
        // Now set the channel creation ts to the join ts
        theChan->setCreationTime(joinTime);
    }

    return MyUplink->JoinChannel(this, theChan->getName(), chanModes, joinTime, getOps);
}

bool xClient::Part(const string& chanName, const string& reason) {
    if (!isConnected()) {
        return false;
    }

    // Ask the server to part us from the channel.
    MyUplink->PartChannel(this, chanName, reason);

    return true;
}

bool xClient::Part(Channel* theChan) {
    assert(theChan != NULL);

    return Part(theChan->getName());
}

bool xClient::Invite(iClient* theClient, const string& chanName) {
    // No need for this assert as we dont use theClient and its tested in
    // Invite(iClient*,Channel*)
    // assert( theClient != NULL ) ;

    Channel* theChan = Network->findChannel(chanName);
    if (0 == theChan) {
        return false;
    }

    return Invite(theClient, theChan);
}

bool xClient::Invite(iClient* theClient, Channel* theChan) {
    assert(theClient != 0);
    assert(theChan != 0);

    if (!isConnected()) {
        return false;
    }
    // A service may invite to a channel it is not on, so nothing is joined
    return MyUplink->Invite(theClient, theChan, getInstance());
}

bool xClient::isOnChannel(const string& chanName) const {
    Channel* theChannel = Network->findChannel(chanName);
    if (0 == theChannel) {
        return false;
    }
    return isOnChannel(theChannel);
}

bool xClient::isOnChannel(const Channel* theChan) const {
    assert(theChan != NULL);

    ChannelUser* meUser = theChan->findUser(me);

    return (meUser != NULL);
}

void xClient::OnJoin(Channel* theChan) {
    assert(theChan != 0);

    addChan(theChan);
}

void xClient::OnJoin(const string& chanName) {
    // elog << "xClient::OnJoin " << chanName << endl;
    Channel* theChan = Network->findChannel(chanName);
    if (NULL == theChan) {
        elog << "xClient::OnJoin> Failed to find channel: " << chanName << endl;
        return;
    }
    OnJoin(theChan);
}

void xClient::OnPart(Channel* theChan) {
    assert(theChan != 0);

    removeChan(theChan);
}

void xClient::OnPart(const string& chanName) {
    Channel* theChan = Network->findChannel(chanName);
    if (NULL == theChan) {
        elog << "xClient::OnPart> Failed to find channel: " << chanName << endl;
        return;
    }
    OnPart(theChan);
}

bool xClient::addChan(Channel*) { return true; }

bool xClient::removeChan(Channel*) { return true; }

void xClient::OnTimer(const xServer::timerID&, void*) {}

void xClient::OnTimerDestroy(xServer::timerID, void*) {}

void xClient::OnSignal(int) {}

// This method courtesy of OUTSider
bool xClient::ClearMode(Channel* theChan, const string& modes, bool modeAsServer) {
    assert(theChan != 0);

    if (!Connected) {
        return false;
    }
    // As the server, or as ourselves.  An oper needs no ops for a CLEARMODE;
    // anybody else has to be opped on the channel, or it fails.
    return (modeAsServer || IsStealth()) ? MyUplink->ClearMode(theChan, modes)
                                         : MyUplink->ClearMode(theChan, modes, getInstance());
}

bool xClient::checkMigrationsAfterDBConnect(const std::string& moduleName, dbHandle* db) {
    // Guard against repeated checks
    if (migrationsChecked) {
        // Migrations were already checked; return the result of that check
        // (true means they passed; if they failed, we would have already exited)
        return true;
    }

    migrationsChecked = true;

    // Only proceed if this module has a database connection
    if (!db || moduleName.empty()) {
        return true;
    }

#ifdef HAVE_PGSQL
    // Try multiple possible locations for migrations directory
    // 1. First try installed location: ../migrations/{module}
    // 2. Fall back to source directory: ./mod.{module}/migrations

    std::string installedDir = "../migrations/" + moduleName;
    std::string sourceDir = "./mod." + moduleName + "/migrations";

    std::string migrationsDir = sourceDir; // default to source directory

    // Check if installed location exists
    if (std::filesystem::exists(installedDir)) {
        migrationsDir = installedDir;
    }

    // Create MigrationChecker and run check
    MigrationChecker checker(moduleName, db, logger.get(), migrationsDir);
    return checker.check();
#else
    return true;
#endif
}

} // namespace gnuworld
