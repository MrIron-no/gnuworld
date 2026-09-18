/**
 * gnutest.cc
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
 * $Id: gnutest.cc,v 1.26 2005/01/17 23:09:53 dan_karrels Exp $
 */
#include <map>
#include <optional>
#include <string>
#include <iostream>
#include <sstream>
#include "client.h"
#include "gnutest.h"
#include "iClient.h"
#include "StringTokenizer.h"
#include "EConfig.h"
#include "Network.h"

namespace gnuworld {

using std::cout;
using std::endl;
using std::string;

/*
 *  Exported function used by moduleLoader to gain an
 *  instance of this module.
 */

extern "C" {
xClient* _gnuwinit(const string& args) { return new gnutest(args); }
}

gnutest::gnutest(const string& fileName) : xClient(fileName) {
    EConfig conf(fileName);
    operChan = conf.Require("operchan")->second;

    // Optional: "burstchannel = <#channel> <timestamp> [<modes> [<args>]]".
    // During our burst, claim that channel with xServer::BurstChannel().
    if (conf.Find("burstchannel") != conf.end()) {
        burstChannel = conf.Find("burstchannel")->second;
    }

    helpTable.insert(std::make_pair("shutdown", "Shutdown the server"));
    helpTable.insert(std::make_pair("reload", "Reload the gnutest module"));
    helpTable.insert(std::make_pair("help", "Print this menu"));
    helpTable.insert(std::make_pair("moo <args>", "Issue a raw command to the network"));
    helpTable.insert(std::make_pair("join <chan>", "Join a channel"));
    helpTable.insert(
        std::make_pair("joinmodes <chan> [modes [args]]", "Join a channel, setting these modes"));
    helpTable.insert(std::make_pair("joinops <chan> [modes [args]]",
                                    "Join a channel with these modes, opped by the server"));
    helpTable.insert(std::make_pair("fakesay|fakenotice <fakenick> <#chan|nick> <text>",
                                    "A message or notice from one of my fake clients"));
    helpTable.insert(std::make_pair("part <chan>", "Part a channel"));
    helpTable.insert(std::make_pair("say <chan> <message>", "Send a message to a channel"));
    helpTable.insert(
        std::make_pair("clearmode <chan> <modes>", "Clear the given list of modes from a channel"));
    helpTable.insert(
        std::make_pair("chaninfo <chan>", "Print some useless information about a channel"));
    helpTable.insert(std::make_pair("ban <chan> <nick> [nick ...]", "Ban users from a channel"));
    helpTable.insert(
        std::make_pair("unban <chan> <banmask> [banmask ...]", "Remove bans from a channel"));
    helpTable.insert(
        std::make_pair("bankick <chan> <nick> <reason>", "Bankick a user from a channel"));
    helpTable.insert(
        std::make_pair("servmode <chan> <modestring>", "Change modes in a channel as the server"));
    helpTable.insert(
        std::make_pair("mode <chan> <modestring>", "Change modes in a channel as the client"));
    helpTable.insert(std::make_pair("op <chan> <nick> [nick ...]", "Op nicks in a channel"));
    helpTable.insert(std::make_pair("deop <chan> <nick> [nick ...]", "Deop nicks in a channel"));
    helpTable.insert(std::make_pair("serv<op|deop|voice|devoice|ban|unban|banmask> ...",
                                    "The same, sent as the server"));
    helpTable.insert(std::make_pair("schedule <chan>", "Schedule a visit to a channel!"));
    helpTable.insert(std::make_pair("voice <chan> <nick> [nick ...]", "Voice nicks in a channel"));
    helpTable.insert(
        std::make_pair("devoice <chan> <nick> [nick ...]", "Devoice nicks in a channel"));
    helpTable.insert(
        std::make_pair("banmask <chan> <banmask> [banmask ...]", "Set bans by mask in a channel"));
    helpTable.insert(std::make_pair("kick <chan> <nick> <reason>", "Kick a nick from a channel"));
    helpTable.insert(std::make_pair("kickasserver <chan> <nick> <reason>",
                                    "Kick a nick as the server, by xClient::Kick(..., true)"));
    helpTable.insert(std::make_pair("topic <chan> <text>", "Set a channel's topic"));
    helpTable.insert(std::make_pair("spawnclient <nick>", "Spawn a fake client"));
    helpTable.insert(
        std::make_pair("removeclient <nick>", "Remove a fake client from the network"));
    helpTable.insert(
        std::make_pair("removeserver <name>", "Remove a fake server from the network"));
    helpTable.insert(std::make_pair("spawnserver <name> <description>",
                                    "Spawn a fake server with the given description"));
    helpTable.insert(
        std::make_pair("spawnjoin <nick> <chan>", "Order a fake client to join a channel"));
    helpTable.insert(
        std::make_pair("spawnpart <nick> <chan>", "Order a fake client to part a channel"));
}

gnutest::~gnutest() {}

void gnutest::OnAttach() {
    // MyUplink->setSendEB( false ) ;
    // elog	<< "gnutest::OnAttach()" << endl ;
    xClient::OnAttach();
}

void gnutest::OnDetach(const string& reason) {
    // elog	<< "gnutest::OnDetach("
    //	<< reason
    //	<< ")" << endl ;
    xClient::OnDetach(reason);
}

void gnutest::OnConnect() {
    // elog	<< "gnutest::OnConnect()" << endl ;
    xClient::OnConnect();
}

void gnutest::OnDisconnect() {
    // elog	<< "gnuteset::OnDisconnect()" << endl ;
    xClient::OnDisconnect();
}

void gnutest::BurstChannels() {
    Join(operChan);

    if (!burstChannel.empty()) {
        StringTokenizer st(burstChannel);
        const std::optional<time_t> ts =
            (st.size() >= 2) ? parseNumber<time_t>(st[1]) : std::nullopt;
        if (!ts) {
            elog << "gnutest::BurstChannels> burstchannel wants \"<#channel> <timestamp> "
                 << "[<modes> [<args>]]\", got: " << burstChannel << endl;
        } else {
            const bool done =
                MyUplink->BurstChannel(st[0], (st.size() > 2) ? st.assemble(2) : string(), *ts);
            elog << "gnutest::BurstChannels> BurstChannel(" << burstChannel
                 << "): " << (done ? "done" : "refused") << endl;
        }
    }
    MyUplink->RegisterChannelEvent(operChan, this);
    return xClient::BurstChannels();
}

void gnutest::OnChannelEvent(const channelEventType& whichEvent, Channel* theChan, void* data1,
                             void* data2, void* data3, void* data4) {
    if (theChan->getName() != operChan) {
        elog << "gnutest::OnChannelEvent> Got bad channel: " << theChan->getName() << endl;
        return;
    }

    iClient* theClient = 0;

    switch (whichEvent) {
    case EVT_BURST:
    case EVT_CREATE:
        //		elog	<< "gnutest::OnChannelEvent> EVT_CREATE\n" ;
    case EVT_JOIN:
        //		elog	<< "gnutest::OnChannelEvent> Got EVT_JOIN:
        //			<< endl ;
        theClient = static_cast<iClient*>(data1);

        if (theClient->isOper()) {
            Op(theChan, theClient);
        }
        break;
    default:
        break;
    }

    xClient::OnChannelEvent(whichEvent, theChan, data1, data2, data3, data4);
}

void gnutest::OnEvent(const eventType& whichEvent, void* data1, void* data2, void* data3,
                      void* data4) {
    xClient::OnEvent(whichEvent, data1, data2, data3, data4);
}

void gnutest::OnChannelMessage(iClient* theClient, Channel* theChan, const string& message) {
    (void)theClient;
    (void)theChan;
    (void)message;

    // elog	<< "gnutest::OnChannelMessage> theClient: "
    //	<< *theClient
    //	<< ", theChan: "
    //	<< theChan->getName()
    //	<< ", message: "
    //	<< message
    //	<< endl ;
}

/**
 * Commands that change a channel.  Each one is a thin wrapper around a
 * single core API call, so that a test can trigger that call and look at
 * what is sent to the network.
 *
 * Who makes the change follows the core API:
 *   op #chan nick           this xClient:         Op(...)
 *   servop #chan nick       the gnuworld server:  MyUplink->Op(...)
 *   as fake op #chan nick   a fake client, or a   MyUplink->Op(..., fake)
 *                           server we spawned:
 *
 * Given one nick (or mask) the single-target overload is called; given
 * several, the vector overload.
 * Returns false if st[0] is not one of these commands.
 */
bool gnutest::channelCommand(iClient* requester, const StringTokenizer& st, const iClient* fake) {
    const bool asServer = (0 == fake) && st[0].starts_with("serv");
    const string cmd = asServer ? st[0].substr(4) : st[0];

    // Anything but this xClient goes through the server's methods
    const bool viaServer = asServer || (fake != 0);
    const iClient* from = fake;

    // "kickasserver" is the older form, xClient::Kick(..., true): sent as the
    // server, but reported to the modules as this xClient's doing
    const bool takesReason = (cmd == "kick" || cmd == "kickasserver" || cmd == "bankick");
    const bool takesMasks = (cmd == "unban" || cmd == "banmask");
    const bool takesNicks =
        (cmd == "op" || cmd == "deop" || cmd == "voice" || cmd == "devoice" || cmd == "ban");
    const bool takesText = (cmd == "topic" || cmd == "mode" || cmd == "clearmode");
    // "invite #channel" invites whoever asks
    const bool isInvite = (cmd == "invite");

    if (!takesReason && !takesMasks && !takesNicks && !takesText && !isInvite) {
        return false;
    }
    if (viaServer && (cmd == "bankick" || cmd == "kickasserver")) {
        // Not part of the server-side API
        return false;
    }

    if (st.size() < (takesReason ? 4U : isInvite ? 2U : 3U)) {
        Notice(requester, "Usage: {} #channel {}", st[0],
               takesReason  ? "nick reason"
               : takesMasks ? "banmask [banmask ...]"
               : takesNicks ? "nick [nick ...]"
                            : "text");
        return true;
    }

    Channel* theChan = Network->findChannel(st[1]);
    if (NULL == theChan) {
        Notice(requester, "Unable to find channel");
        return true;
    }

    if (cmd == "topic") {
        viaServer ? MyUplink->Topic(theChan, st.assemble(2), from) : Topic(theChan, st.assemble(2));
        return true;
    }
    if (isInvite) {
        // As the server this is refused: only a client can invite
        viaServer ? MyUplink->Invite(requester, theChan, from) : Invite(requester, theChan);
        return true;
    }
    if (cmd == "mode") {
        viaServer ? MyUplink->Mode(theChan, st.assemble(2), string(), from)
                  : Mode(theChan, st.assemble(2), string());
        return true;
    }
    if (cmd == "clearmode") {
        viaServer ? MyUplink->ClearMode(theChan, st[2], from) : ClearMode(theChan, st[2]);
        return true;
    }

    if (takesMasks) {
        const bool adding = (cmd == "banmask");
        if (!adding && 3 == st.size()) {
            if (!theChan->findBan(st[2])) {
                Notice(requester, "Unable to find ban");
                return true;
            }
            viaServer ? MyUplink->UnBan(theChan, st[2], from) : UnBan(theChan, st[2]);
            return true;
        }

        xServer::banVectorType banVector;
        for (StringTokenizer::size_type i = 2; i < st.size(); ++i) {
            banVector.push_back(xServer::banVectorType::value_type(adding, st[i]));
        }
        if (adding) {
            viaServer ? MyUplink->Ban(theChan, banVector, from) : Ban(theChan, banVector);
        } else {
            viaServer ? MyUplink->UnBan(theChan, banVector, from) : UnBan(theChan, banVector);
        }
        return true;
    }

    // Everything else targets members of the channel
    std::vector<iClient*> targets;
    const StringTokenizer::size_type lastNick = takesReason ? 3 : st.size();
    for (StringTokenizer::size_type i = 2; i < lastNick; ++i) {
        iClient* target = Network->findNick(st[i]);
        if (NULL == target) {
            Notice(requester, "Unable to find nickname: {}", st[i]);
            return true;
        }
        if (0 == theChan->findUser(target)) {
            Notice(requester, "{} doesn't appear to be on that channel", st[i]);
            return true;
        }
        targets.push_back(target);
    }

    iClient* const one = targets[0];
    const bool single = (1 == targets.size());

    if (cmd == "kick") {
        viaServer ? MyUplink->Kick(theChan, one, st.assemble(3), from)
                  : Kick(theChan, one, st.assemble(3), false);
    } else if (cmd == "kickasserver") {
        Kick(theChan, one, st.assemble(3), true);
    } else if (cmd == "bankick") {
        BanKick(theChan, one, st.assemble(3));
    } else if (cmd == "op") {
        viaServer
            ? (single ? MyUplink->Op(theChan, one, from) : MyUplink->Op(theChan, targets, from))
            : (single ? Op(theChan, one) : Op(theChan, targets));
    } else if (cmd == "deop") {
        viaServer
            ? (single ? MyUplink->DeOp(theChan, one, from) : MyUplink->DeOp(theChan, targets, from))
            : (single ? DeOp(theChan, one) : DeOp(theChan, targets));
    } else if (cmd == "voice") {
        viaServer ? (single ? MyUplink->Voice(theChan, one, from)
                            : MyUplink->Voice(theChan, targets, from))
                  : (single ? Voice(theChan, one) : Voice(theChan, targets));
    } else if (cmd == "devoice") {
        viaServer ? (single ? MyUplink->DeVoice(theChan, one, from)
                            : MyUplink->DeVoice(theChan, targets, from))
                  : (single ? DeVoice(theChan, one) : DeVoice(theChan, targets));
    } else if (cmd == "ban") {
        viaServer
            ? (single ? MyUplink->Ban(theChan, one, from) : MyUplink->Ban(theChan, targets, from))
            : (single ? Ban(theChan, one) : Ban(theChan, targets));
    }

    return true;
}

void gnutest::OnPrivateMessage(iClient* theClient, const string& message, bool) {
    // if( !theClient->isOper() )
    //	{
    //	elog	<< "gnutest::OnPrivateMessage> Denying access "
    //		<< "to non-oper: "
    //		<< *theClient
    //		<< endl ;
    //	return ;
    //	}

    // elog	<< "gnutest::OnPrivateMessage> Message: "
    //	<< message
    //	<< ", from client: "
    //	<< *theClient
    //	<< endl ;

    StringTokenizer st(message);
    if (st.empty()) {
        Notice(theClient, "Are you speaking to me?");
        return;
    }

    if (st[0] == "shutdown") {
        MyUplink->Shutdown();
        return;
    } else if (st[0] == "reload") {
        Notice(theClient, "Reloading client...see you on the flip side");

        MyUplink->UnloadClient(this, "Reloading...");
        MyUplink->LoadClient("libgnutest", getConfigFileName());
        return;
    } else if (st[0] == "help") {
        Notice(theClient, "--- Help Menu ---");
        for (helpTableType::const_iterator hItr = helpTable.begin(); hItr != helpTable.end();
             ++hItr) {
            Notice(theClient, "{}: {}", hItr->first, hItr->second);
        }
        return;
    }

    if (st.size() < 2) {
        Notice(theClient, "Are you speaking to me?");
        return;
    }

    // "as <fake> <command ...>": have a fake client of ours do it.  It has
    // no object with methods to call, so it is named to the server's.
    if (st[0] == "as") {
        iClient* fakeClient = (st.size() >= 3) ? Network->findNick(st[1]) : 0;
        if (0 == fakeClient || Network->findFakeClientOwner(fakeClient) != this) {
            Notice(theClient, "Usage: as <fake client> <command ...>");
            return;
        }
        StringTokenizer rest(st.assemble(2));
        if (!channelCommand(theClient, rest, fakeClient)) {
            Notice(theClient, "That cannot be done as a fake client");
        }
        return;
    }

    if (channelCommand(theClient, st, nullptr)) {
        return;
    }

    if (st[0] == "fakesay" || st[0] == "fakenotice") {
        // fakesay|fakenotice <fakenick> <#channel|nick> <text>
        iClient* fake = (st.size() >= 4) ? Network->findNick(st[1]) : 0;
        if (0 == fake || Network->findFakeClientOwner(fake) != this) {
            Notice(theClient, "Usage: {} <one of my fake clients> <#channel|nick> <text>", st[0]);
            return;
        }
        const bool notice = (st[0] == "fakenotice");
        if ('#' == st[2][0]) {
            Channel* theChan = Network->findChannel(st[2]);
            if (NULL == theChan) {
                Notice(theClient, "Unable to find channel");
                return;
            }
            notice ? FakeNotice(theChan, fake, st.assemble(3))
                   : FakeMessage(theChan, fake, st.assemble(3));
        } else {
            iClient* target = Network->findNick(st[2]);
            if (NULL == target) {
                Notice(theClient, "Unable to find nickname: {}", st[2]);
                return;
            }
            notice ? FakeNotice(target, fake, st.assemble(3))
                   : FakeMessage(target, fake, st.assemble(3));
        }
        return;
    }

    // silence <nick> <mask>, unsilence <mask>, opmode <nick> <user modes>,
    // globalnotice <text>, servsay <nick|#channel> <text>
    if ((st[0] == "kill" || st[0] == "servkill") && st.size() > 2) {
        if (iClient* victim = Network->findNick(st[1])) {
            Kill(victim, st.assemble(2), st[0] == "servkill");
        }
        return;
    }
    if (st[0] == "servnotice" && st.size() > 2) {
        if (Channel* targetChan = Network->findChannel(st[1])) {
            MyUplink->serverNotice(targetChan, st.assemble(2));
        }
        return;
    }
    if (st[0] == "silence" || st[0] == "opmode" || st[0] == "servsay") {
        iClient* target = (st.size() > 2) ? Network->findNick(st[1]) : 0;
        Channel* targetChan = (st.size() > 2) ? Network->findChannel(st[1]) : 0;
        if (0 == target && !(st[0] == "servsay" && targetChan != 0)) {
            Notice(theClient, "Usage: silence <nick> <mask> | opmode <nick> <modes> | "
                              "servsay <nick|#channel> <text>");
            return;
        }
        if (st[0] == "silence") {
            Silence(target, st[2]);
        } else if (st[0] == "opmode") {
            MyUplink->OpMode(target, st[2]);
        } else if (targetChan != 0) {
            MyUplink->serverMessage(targetChan, st.assemble(2));
        } else {
            MyUplink->Message(target, st.assemble(2));
        }
        return;
    }
    if (st[0] == "unsilence" && st.size() > 1) {
        UnSilence(st[1]);
        return;
    }
    if (st[0] == "globalnotice" && st.size() > 1) {
        MyUplink->GlobalNotice(st.assemble(1), getInstance());
        return;
    }

    if (st[0] == "moo") {
        string raw = st.assemble(1);
        Write(raw);
    } else if (st[0] == "join") {
        Join(st[1]);
    } else if (st[0] == "joinmodes" || st[0] == "joinops") {
        // joinmodes <chan> [modes [args]]: join, setting these modes
        // joinops   <chan> [modes [args]]: the same, and have the server op us
        Join(st[1], st.size() > 2 ? st.assemble(2) : string(), 0, st[0] == "joinops");
    } else if (st[0] == "part") {
        Part(st[1]);
    } else if (st[0] == "say") {
        if (st.size() < 3) {
            Notice(theClient, "Usage: say <channel> <text>");
            return;
        }

        Channel* theChan = Network->findChannel(st[1]);
        if (NULL == theChan) {
            Notice(theClient, "Unable to find channel");
            return;
        }

        Message(theChan, st.assemble(2));
    } else if (st[0] == "chaninfo") {
        Channel* theChan = Network->findChannel(st[1]);
        if (NULL == theChan) {
            Notice(theClient, "Unable to find channel");
            return;
        }

        chanInfo(theChan);
    } else if (st[0] == "schedule") {
        Channel* theChan = Network->findChannel(st[1]);
        if (NULL == theChan) {
            Notice(theClient, "Unable to find channel");
            return;
        }

        xServer::timerID id = MyUplink->RegisterTimer(::time(0) + 60, this);
        if (0 == id) {
            Notice(theClient, "Failed");
        } else {
            Notice(theClient, "Scheduled for 1 minute from now");
            timerChan = theChan->getName();
        }
    } else if (st[0] == "spawnclient") {
        spawnClient(theClient, st);
    } else if (st[0] == "removeclient") {
        removeClient(theClient, st);
    } else if (st[0] == "spawnjoin") {
        spawnJoin(theClient, st);
    } else if (st[0] == "spawnpart") {
        spawnPart(theClient, st);
    } else if (st[0] == "spawnserver") {
        spawnServer(theClient, st);
    } else if (st[0] == "removeserver") {
        removeServer(theClient, st);
    }

    xClient::OnPrivateMessage(theClient, message);
}

void gnutest::OnFakeChannelNotice(iClient* srcClient, iClient* destClient, Channel* theChan,
                                  const string& message) {
    // elog	<< "gnutest::OnFakeChannelNotice> srcClient: "
    //	<< *srcClient
    //	<< ", destClient: "
    //	<< *destClient
    //	<< ", channel: "
    //	<< theChan->getName()
    //	<< ", message: "
    //	<< message
    //	<< endl ;
    (void)srcClient;
    (void)destClient;
    (void)theChan;
    (void)message;
}

void gnutest::OnFakeChannelMessage(iClient* srcClient, iClient* destClient, Channel* theChan,
                                   const string& message) {
    // elog	<< "gnutest::OnFakeChannelMessage> srcClient: "
    //	<< *srcClient
    //	<< ", destClient: "
    //	<< *destClient
    //	<< ", channel: "
    //	<< theChan->getName()
    //	<< ", message: "
    //	<< message
    //	<< endl ;
    if (srcClient->getNickName() == "beware") {
        std::stringstream s;
        s << destClient->getCharYYXXX() << " P " << theChan->getName() << " :" << message << endl;
        //	Write( s.str() ) ;
    }
    if (srcClient->getNickName() == "ripper_") {
        std::stringstream s;
        s << destClient->getCharYYXXX() << " P " << theChan->getName()
          << " :I agree with ripper_...";
        Write(s.str());
    }
}

void gnutest::OnFakePrivateNotice(iClient* srcClient, iClient* destClient, const string& message,
                                  bool secure) {
    // elog	<< "gnutest::OnFakePrivateNotice> srcClient: "
    //	<< *srcClient
    //	<< ", destClient: "
    //	<< *destClient
    //	<< ", message: "
    //	<< message
    //	<< ", secure: "
    //	<< secure
    //	<< endl ;
    (void)srcClient;
    (void)destClient;
    (void)message;
    (void)secure;
}

void gnutest::OnFakePrivateMessage(iClient* /* srcClient */, iClient* destClient,
                                   const string& message, bool) {
    // elog	<< "gnutest::OnFakePrivateMessage> srcClient: "
    //	<< *srcClient
    //	<< ", destClient: "
    //	<< *destClient
    //	<< ", message: "
    //	<< message
    //	<< ", secure: "
    //	<< secure
    //	<< endl ;

    StringTokenizer st(message);
    if (st.size() < 2) {
        return;
    }

    if (st[0] == "join") {
        // st[ 1 ] exists
        MyUplink->JoinChannel(destClient, st[1]);
    } else if (st[0] == "part") {
        MyUplink->PartChannel(destClient, st[1], "gnuworld, the other white meat");
    }
}

void gnutest::spawnServer(iClient* requestingClient, const StringTokenizer& st) {
    if (st.size() < 3) {
        Notice(requestingClient, "Usage: spawnserver <name> <description>");
        return;
    }

    string name(st[1]);
    if (string::npos == name.find('.')) {
        Notice(requestingClient, "Server name must have at least "
                                 "one \'.\'");
        return;
    }

    string description(st.assemble(2));

    string yyxxx("00]]]");
    iServer* newServer =
        new (std::nothrow) iServer(getIntYY(), yyxxx, name, ::time(0), description);
    assert(newServer != 0);

    if (!MyUplink->AttachServer(newServer, this)) {
        elog << "gnutest::spawnServer> Failed to add new iServer: " << *newServer << endl;

        Notice(requestingClient, "Failed to add new server");
    } else {
        elog << "gnutest::spawnServer> Added new iServer: " << *newServer << endl;

        Notice(requestingClient, "Added new server with description: {}", description);
    }
}

void gnutest::removeServer(iClient* requestingClient, const StringTokenizer& st) {
    if (st.size() < 2) {
        Notice(requestingClient, "Usage: removeServer <name>");
    }

    string name(st[1]);

    iServer* theServer = Network->findServerName(name);
    if (0 == theServer) {
        elog << "gnutest::removeServer> Failed to find server name: " << name << endl;

        Notice(requestingClient, "Failed to find server: {}", name);
        return;
    }

    if (!MyUplink->DetachServer(theServer)) {
        elog << "gnutest::removeServer> Failed to DetachServer(): " << *theServer << endl;

        Notice(requestingClient, "Failed to remove server: {}", name);
    } else {
        elog << "gnutest::removeServer> Successfully removed server: " << *theServer << endl;

        Notice(requestingClient, "Successfully removed server: {}", name);
        delete theServer;
        theServer = 0;
    }
}

void gnutest::removeClient(iClient* requestingClient, const StringTokenizer& st) {
    if (st.size() != 2) {
        Notice(requestingClient, "Usage: removeclient <nickname>");
        return;
    }

    string nickName(st[1]);

    elog << "gnutest::removeClient> Removing: " << nickName << endl;

    iClient* removeMe = Network->findFakeNick(nickName);
    if (0 == removeMe) {
        Notice(requestingClient, "Unable to find fake client: {}", nickName);
        return;
    }

    // Verify that it is a fake client, and owned by this module
    xClient* ownerClient = Network->findFakeClientOwner(removeMe);
    if (ownerClient != this) {
        Notice(requestingClient, "I don't own that client!");
        return;
    }

    if (MyUplink->DetachClient(removeMe, "Requested shutdown") != 0) {
        Notice(requestingClient, "Successfully removed fake client: {}", nickName);

        // This module allocated the client, so this module will
        // deallocate it.
        delete removeMe;
        removeMe = 0;
    } else {
        Notice(requestingClient, "Failed to remove fake client: {}", nickName);
    }
}

void gnutest::spawnClient(iClient* requestingClient, const StringTokenizer& st) {
    // spawnclient <nickname> [server]: on this server, or on one that
    // spawnserver made
    iServer* onServer = (st.size() == 3) ? Network->findServerName(st[2]) : MyUplink->getMe();
    if (st.size() < 2 || st.size() > 3 || 0 == onServer) {
        Notice(requestingClient, "Usage: spawnclient <nickname> [spawned server]");
        return;
    }

    string nickName(st[1]);

    elog << "gnutest::spawnClient> Spawning " << nickName << endl;

    char newCharYY[6];
    newCharYY[2] = 0;
    inttobase64(newCharYY, onServer->getIntYY(), 2);

    // elog	<< "gnutest::spawnClient> newCharYY: "
    //	<< newCharYY
    //	<< endl ;

    iClient* newClient = new (std::nothrow) iClient(onServer->getIntYY(), // intYY
                                                    newCharYY,            // charYYXXX
                                                    nickName, "username",
                                                    "AAAAAA",                 // host base 64
                                                    "insecurehost.com",       // insecureHost
                                                    "realInsecureHost.com",   // realInsecureHost
                                                    "+i",                     // mode
                                                    string(),                 // account
                                                    0,                        // account_id
                                                    0,                        // account_flags
                                                    string(),                 // tls fingerprint
                                                    "test spawn client, moo", // description
                                                    31337                     // connect time
    );
    assert(newClient != 0);

    if (!MyUplink->AttachClient(newClient, this)) {
        elog << "gnutest::spawnClient> Failed to add new client: " << *newClient << endl;

        Notice(requestingClient, "Failed to create new fake client");
        delete newClient;
        newClient = 0;
    } else {
        Notice(requestingClient, "Created new client {}", nickName);
        elog << "gnutest::spawnClient> Added client: " << *newClient << endl;
    }
}

void gnutest::OnTimer(const xServer::timerID&, void*) {
    Channel* theChan = Network->findChannel(timerChan);
    if (NULL == theChan) {
        elog << "gnutest::OnTimer> Unable to find channel: " << timerChan << endl;
        return;
    }

    Message(theChan, "Respect my authoritah!");
}

void gnutest::OnFakeChannelCTCP(iClient* srcClient, iClient* fakeClient, Channel* theChan,
                                const string& command, const string& message) {
    // elog	<< "gnutest::OnFakeChannelCTCP> srcClient: "
    //	<< *srcClient
    //	<< ", fakeClient: "
    //	<< *fakeClient
    //	<< ", theChan: "
    //	<< theChan->getName()
    //	<< ", command: "
    //	<< command
    //	<< ", message: "
    //	<< message
    //	<< endl ;
    (void)srcClient;
    (void)theChan;
    (void)fakeClient;
    (void)command;
    (void)message;
}

void gnutest::OnFakeCTCP(iClient* srcClient, iClient* fakeClient, const string& command,
                         const string& message, bool) {
    // elog	<< "gnutest::OnFakeCTCP> srcClient: "
    //	<< *srcClient
    //	<< ", fakeClient: "
    //	<< *fakeClient
    //	<< ", command: "
    //	<< command
    //	<< ", message: "
    //	<< message
    //	<< endl ;
    (void)srcClient;
    (void)fakeClient;
    (void)command;
    (void)message;
}

void gnutest::spawnJoin(iClient* srcClient, const StringTokenizer& st) {
    // st[ 0 ] is "spawnjoin"
    // spawnjoin nick #chan
    if (st.size() != 3) {
        Notice(srcClient, "SPAWNJOIN: Requires 3 arguments");
        return;
    }

    // Find the client
    iClient* fakeClient = Network->findNick(st[1]);
    if (0 == fakeClient) {
        Notice(srcClient, "Nick \'{}\' does not exist", st[1]);
        return;
    }

    // Verify that it is a fake client, and owned by this module
    xClient* ownerClient = Network->findFakeClientOwner(fakeClient);
    if (ownerClient != this) {
        Notice(srcClient, "I don't own that client!");
        return;
    }

    if (!getUplink()->JoinChannel(fakeClient, st[2])) {
        Notice(srcClient,
               "Unable to make \'{}\' join channel "
               "{}",
               st[1], st[2]);
    } else {
        Notice(srcClient, "{} successfully joined {}", st[1], st[2]);
    }
}

void gnutest::spawnPart(iClient* srcClient, const StringTokenizer& st) {
    // st[ 0 ] is "spawnpart"
    // spawnpart nick #chan
    if (st.size() != 3) {
        Notice(srcClient, "SPAWNPART: Requires 3 arguments");
        return;
    }

    // Find the client
    iClient* fakeClient = Network->findNick(st[1]);
    if (0 == fakeClient) {
        Notice(srcClient, "Nick \'{}\' does not exist", st[1]);
        return;
    }

    // Verify that it is a fake client, and owned by this module
    xClient* ownerClient = Network->findFakeClientOwner(fakeClient);
    if (ownerClient != this) {
        Notice(srcClient, "I don't own that client!");
        return;
    }

    getUplink()->PartChannel(fakeClient, st[2]);
    Notice(srcClient, "{} successfully parted {}", st[1], st[2]);
}

void gnutest::chanInfo(const Channel* theChan) {
    elog << *theChan << endl << "--- User information ---" << endl;

    // Iterate through all clients, and return info about each
    // ChannelUser
    for (Channel::const_userIterator cItr = theChan->userList_begin();
         cItr != theChan->userList_end(); ++cItr) {
        const ChannelUser* theUser = cItr->second;
        elog << *theUser << endl;
    }
}

} // namespace gnuworld
