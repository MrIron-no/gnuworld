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
#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include "client.h"
#include "gnutest.h"
#include "events.h"
#include "Gline.h"
#include "iClient.h"
#include "StringTokenizer.h"
#include "EConfig.h"
#include "Network.h"

namespace gnuworld {

using std::cout;
using std::endl;
using std::string;

/**
 * Every event core can post, by the name gnutest reports it under: what
 * eventNames[] spells with the spaces taken out, except that EVT_QUIT and
 * EVT_KILL keep the short names and EVT_ACCOUNT_FLAGS has no eventNames[]
 * entry at all - from EVT_RAW up that table is one short and misaligned.
 * The two enums of events.h overlap (EVT_JOIN == EVT_NOOP), so a network
 * event and a channel event are named from their own table.
 */
struct eventNameEntry {
    int whichEvent;
    const char* name;
};

static const eventNameEntry networkEventNames[] = {
    {EVT_OPER, "OperUp"},
    {EVT_NETBREAK, "NetBreak"},
    {EVT_NETJOIN, "NetJoin"},
    {EVT_BURST_CMPLT, "BurstComplete"},
    {EVT_BURST_ACK, "BurstAcknowledge"},
    {EVT_EA_SENT, "BurstAcknowledgeSent"},
    {EVT_GLINE, "GlineAdd"},
    {EVT_REMGLINE, "GlineRemove"},
    {EVT_JUPE, "ServerJupe"},
    {EVT_UNJUPE, "ServerUnJupe"},
    {EVT_QUIT, "Quit"},
    {EVT_KILL, "Kill"},
    {EVT_NICK, "ClientConnect"},
    {EVT_CHNICK, "NickChange"},
    {EVT_ACCOUNT, "AccountLogin"},
    {EVT_ACCOUNT_FLAGS, "AccountFlags"},
    {EVT_RAW, "Raw"},
    {EVT_XQUERY, "XQuery"},
    {EVT_XREPLY, "XReply"},
    {EVT_NETCONF, "NetconfAdd"},
    {EVT_REMNETCONF, "NetconfRemove"},
};

static const eventNameEntry channelEventNames[] = {
    {EVT_JOIN, "ChannelJoin"},
    {EVT_PART, "ChannelPart"},
    {EVT_SERVERMODE, "ChannelModeByServer"},
    {EVT_TOPIC, "ChannelTopicChange"},
    {EVT_KICK, "ChannelKick"},
    {EVT_CREATE, "ChannelCreate"},
    {EVT_BURST, "ChannelBurst"},
};

/// The name gnutest reports this event under
static string eventName(int whichEvent, bool channelEvent) {
    const std::span<const eventNameEntry> table =
        channelEvent ? std::span<const eventNameEntry>(channelEventNames)
                     : std::span<const eventNameEntry>(networkEventNames);
    for (const eventNameEntry& entry : table) {
        if (entry.whichEvent == whichEvent) {
            return string(entry.name);
        }
    }
    return string("?");
}

/// The event one of those names stands for; false if it is not a name we know
static bool findEventByName(const string& name, int& whichEvent, bool& channelEvent) {
    for (const eventNameEntry& entry : networkEventNames) {
        if (name == entry.name) {
            whichEvent = entry.whichEvent;
            channelEvent = false;
            return true;
        }
    }
    for (const eventNameEntry& entry : channelEventNames) {
        if (name == entry.name) {
            whichEvent = entry.whichEvent;
            channelEvent = true;
            return true;
        }
    }
    return false;
}

/* The printable identity of one thing an event was handed: "-" for one that is
 * null, and for text that is empty. */

static string nickOf(const iClient* theClient) {
    return (nullptr == theClient) ? string("-") : theClient->getNickName();
}

static string serverNameOf(const iServer* theServer) {
    return (nullptr == theServer) ? string("-") : theServer->getName();
}

static string memberOf(const ChannelUser* theUser) {
    return (nullptr == theUser) ? string("-") : theUser->getNickName();
}

static string glineMaskOf(const Gline* theGline) {
    return (nullptr == theGline) ? string("-") : theGline->getUserHost();
}

static string stringOf(std::string_view text) { return text.empty() ? string("-") : string(text); }

/**
 * The source of a kill, which is the one thing an event hands over whose type
 * is still not said: a client, a server, or null when one of our own modules
 * did it.  The only way to print one without guessing is to look the pointer up
 * in the network's own tables.  Empty if it is neither.
 */
static string knownObjectOf(const NetworkTarget* target) {
    for (xNetwork::clientIterator ptr = Network->clients_begin(); ptr != Network->clients_end();
         ++ptr) {
        if (ptr->second == target) {
            return ptr->second->getNickName();
        }
    }
    for (xNetwork::serverIterator ptr = Network->servers_begin(); ptr != Network->servers_end();
         ++ptr) {
        if (ptr->second == target) {
            return ptr->second->getName();
        }
    }
    return string();
}

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
            LOG(ERROR, "burstchannel wants \"<#channel> <timestamp> [<modes> [<args>]]\", got: {}",
                burstChannel);
        } else {
            const bool done =
                MyUplink->BurstChannel(st[0], (st.size() > 2) ? st.assemble(2) : string(), *ts);
            LOG(INFO, "BurstChannel({}): {}", burstChannel, string(done ? "done" : "refused"));
        }
    }
    MyUplink->RegisterChannelEvent(operChan, this);
    return xClient::BurstChannels();
}

/*
 * One method per event core posts, each handed exactly what that event carries.
 * Every one of them reports the event and runs whatever "onevent" armed for it,
 * which is the one call they all begin with, and then hands on to xClient.
 */

void gnutest::OnOper(iClient* theClient) {
    eventArrived(EVT_OPER, false, {nickOf(theClient)}, theClient);
    xClient::OnOper(theClient);
}

void gnutest::OnNetBreak(iServer* theServer, const iServer* uplink, std::string_view reason) {
    eventArrived(EVT_NETBREAK, false,
                 {serverNameOf(theServer), serverNameOf(uplink), stringOf(reason)});
    xClient::OnNetBreak(theServer, uplink, reason);
}

void gnutest::OnNetJoin(iServer* theServer, const iServer* uplink) {
    eventArrived(EVT_NETJOIN, false, {serverNameOf(theServer), serverNameOf(uplink)});
    xClient::OnNetJoin(theServer, uplink);
}

void gnutest::OnBurstComplete(iServer* theServer) {
    eventArrived(EVT_BURST_CMPLT, false, {serverNameOf(theServer)});
    xClient::OnBurstComplete(theServer);
}

void gnutest::OnBurstAck(iServer* theServer) {
    eventArrived(EVT_BURST_ACK, false, {serverNameOf(theServer)});
    xClient::OnBurstAck(theServer);
}

void gnutest::OnEndOfBurstAckSent(iServer* theServer) {
    eventArrived(EVT_EA_SENT, false, {serverNameOf(theServer)});
    xClient::OnEndOfBurstAckSent(theServer);
}

void gnutest::OnGline(Gline* theGline) {
    eventArrived(EVT_GLINE, false, {glineMaskOf(theGline)});
    xClient::OnGline(theGline);
}

void gnutest::OnRemGline(Gline* theGline) {
    eventArrived(EVT_REMGLINE, false, {glineMaskOf(theGline)});
    xClient::OnRemGline(theGline);
}

void gnutest::OnQuit(iClient* theClient, std::string_view reason) {
    eventArrived(EVT_QUIT, false, {nickOf(theClient), stringOf(reason)}, theClient);
    xClient::OnQuit(theClient, reason);
}

void gnutest::OnKill(const NetworkTarget* source, iClient* theClient, std::string_view reason) {
    const string sourceName = knownObjectOf(source);
    const string killedBy = (nullptr == source)  ? string("-")
                            : sourceName.empty() ? string("?")
                                                 : sourceName;

    eventArrived(EVT_KILL, false, {killedBy, nickOf(theClient), stringOf(reason)}, theClient);
    xClient::OnKill(source, theClient, reason);
}

void gnutest::OnNick(iClient* theClient) {
    eventArrived(EVT_NICK, false, {nickOf(theClient)}, theClient);
    xClient::OnNick(theClient);
}

void gnutest::OnNickChange(iClient* theClient, std::string_view oldNick) {
    eventArrived(EVT_CHNICK, false, {nickOf(theClient), stringOf(oldNick)}, theClient);
    xClient::OnNickChange(theClient, oldNick);
}

void gnutest::OnAccount(iClient* theClient) {
    eventArrived(EVT_ACCOUNT, false, {nickOf(theClient)}, theClient);
    xClient::OnAccount(theClient);
}

void gnutest::OnAccountFlags(iClient* theClient) {
    eventArrived(EVT_ACCOUNT_FLAGS, false, {nickOf(theClient)}, theClient);
    xClient::OnAccountFlags(theClient);
}

void gnutest::OnRaw(std::string_view line) {
    eventArrived(EVT_RAW, false, {stringOf(line)});
    xClient::OnRaw(line);
}

void gnutest::OnXQuery(iServer* theServer, std::string_view routing, std::string_view message) {
    eventArrived(EVT_XQUERY, false,
                 {serverNameOf(theServer), stringOf(routing), stringOf(message)});
    xClient::OnXQuery(theServer, routing, message);
}

void gnutest::OnXReply(iServer* theServer, std::string_view routing, std::string_view message) {
    eventArrived(EVT_XREPLY, false,
                 {serverNameOf(theServer), stringOf(routing), stringOf(message)});
    xClient::OnXReply(theServer, routing, message);
}

void gnutest::OnNetConf(iServer* theServer, std::string_view key) {
    eventArrived(EVT_NETCONF, false, {serverNameOf(theServer), stringOf(key)});
    xClient::OnNetConf(theServer, key);
}

void gnutest::OnRemNetConf(iServer* theServer, std::string_view key) {
    eventArrived(EVT_REMNETCONF, false, {serverNameOf(theServer), stringOf(key)});
    xClient::OnRemNetConf(theServer, key);
}

void gnutest::OnJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser, JoinKind kind) {
    membership(channelEventOf(kind), theChan, theClient, theUser);
    xClient::OnJoin(theChan, theClient, theUser, kind);
}

void gnutest::OnPart(Channel* theChan, iClient* theClient, std::string_view message) {
    // The part message is passed on one path only
    eventArrived(EVT_PART, true, {theChan->getName(), nickOf(theClient), stringOf(message)},
                 theClient, theChan);
    xClient::OnPart(theChan, theClient, message);
}

void gnutest::OnTopic(Channel* theChan, iClient* theClient, std::string_view topic) {
    // The client is null for a topic that arrives in a burst
    eventArrived(EVT_TOPIC, true, {theChan->getName(), nickOf(theClient), stringOf(topic)},
                 theClient, theChan);
    xClient::OnTopic(theChan, theClient, topic);
}

void gnutest::OnServerMode(Channel* theChan, iServer* theServer) {
    // The one channel event that is about no client
    eventArrived(EVT_SERVERMODE, true, {theChan->getName(), serverNameOf(theServer)}, nullptr,
                 theChan);
    xClient::OnServerMode(theChan, theServer);
}

/*
 * The notifications that are not events: a kick and each kind of channel mode
 * change.  They are reported like an event, under the name of the method that
 * delivers them, with the channel first and then the source - which for a mode
 * is the member who set it, and is "-" when a server did.
 */

/// One mode change as it would be on the wire: "+m", "-b *!*@bad"
static string modeChange(bool polarity, const string& what) {
    return string(polarity ? "+" : "-") + what;
}

/// The letter of a simple mode, by the bit it occupies
static string modeLetter(Channel::modeType whichMode) {
    for (const Channel::ModeInfo& mode : Channel::modeTable) {
        if (mode.flag == whichMode && Channel::ModeType::Flag == mode.type) {
            return string(1, mode.letter);
        }
    }
    return string("?");
}

void gnutest::OnNetworkKick(Channel* theChan, iClient* srcClient, iClient* destClient,
                            const string& kickMessage, bool authoritative) {
    if (!eventWatcher.empty()) {
        reportEvent("ChannelKick", {theChan->getName(), nickOf(srcClient), nickOf(destClient),
                                    kickMessage, authoritative ? "authoritative" : "zombie"});
    }

    xClient::OnNetworkKick(theChan, srcClient, destClient, kickMessage, authoritative);
}

void gnutest::OnChannelMode(Channel* theChan, ChannelUser* sourceUser,
                            const xServer::modeVectorType& modeVector) {
    if (!eventWatcher.empty()) {
        std::vector<string> args{theChan->getName(), memberOf(sourceUser)};
        for (const xServer::modeVectorType::value_type& change : modeVector) {
            args.push_back(modeChange(change.first, modeLetter(change.second)));
        }
        reportEvent("ChannelMode", args);
    }

    xClient::OnChannelMode(theChan, sourceUser, modeVector);
}

void gnutest::OnChannelModeL(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const unsigned int& limit) {
    if (!eventWatcher.empty()) {
        reportEvent("ChannelModeL", {theChan->getName(), memberOf(sourceUser),
                                     modeChange(polarity, std::to_string(limit))});
    }

    xClient::OnChannelModeL(theChan, polarity, sourceUser, limit);
}

void gnutest::OnChannelModeK(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& key) {
    if (!eventWatcher.empty()) {
        reportEvent("ChannelModeK",
                    {theChan->getName(), memberOf(sourceUser), modeChange(polarity, key)});
    }

    xClient::OnChannelModeK(theChan, polarity, sourceUser, key);
}

void gnutest::OnChannelModeA(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Apass) {
    if (!eventWatcher.empty()) {
        reportEvent("ChannelModeA",
                    {theChan->getName(), memberOf(sourceUser), modeChange(polarity, Apass)});
    }

    xClient::OnChannelModeA(theChan, polarity, sourceUser, Apass);
}

void gnutest::OnChannelModeU(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Upass) {
    if (!eventWatcher.empty()) {
        reportEvent("ChannelModeU",
                    {theChan->getName(), memberOf(sourceUser), modeChange(polarity, Upass)});
    }

    xClient::OnChannelModeU(theChan, polarity, sourceUser, Upass);
}

void gnutest::OnChannelModeO(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::opVectorType& opVector) {
    if (!eventWatcher.empty()) {
        std::vector<string> args{theChan->getName(), memberOf(sourceUser)};
        for (const xServer::opVectorType::value_type& change : opVector) {
            args.push_back(modeChange(change.first, memberOf(change.second)));
        }
        reportEvent("ChannelModeO", args);
    }

    xClient::OnChannelModeO(theChan, sourceUser, opVector);
}

void gnutest::OnChannelModeV(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::voiceVectorType& voiceVector) {
    if (!eventWatcher.empty()) {
        std::vector<string> args{theChan->getName(), memberOf(sourceUser)};
        for (const xServer::voiceVectorType::value_type& change : voiceVector) {
            args.push_back(modeChange(change.first, memberOf(change.second)));
        }
        reportEvent("ChannelModeV", args);
    }

    xClient::OnChannelModeV(theChan, sourceUser, voiceVector);
}

void gnutest::OnChannelModeB(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::banVectorType& banVector) {
    if (!eventWatcher.empty()) {
        std::vector<string> args{theChan->getName(), memberOf(sourceUser)};
        for (const xServer::banVectorType::value_type& change : banVector) {
            args.push_back(modeChange(change.first, change.second));
        }
        reportEvent("ChannelModeB", args);
    }

    xClient::OnChannelModeB(theChan, sourceUser, banVector);
}

/**
 * "events on|off" registers this module for every event core posts, on every
 * channel, and reports each one it receives; "onevent <NAME> <action> [args]"
 * arms one action to run from inside the handler the next time that event
 * arrives.  Both are for the harness's event tests, and do nothing until used.
 *
 * The actions, and what their argument names:
 *   kill [<nick>]                that client, or the one the event is about
 *   kick [<#channel> <nick>]     that member, or the event's client on the
 *                                event's own channel
 *   part <#channel>              a channel of ours to leave
 *   register <NAME>              the event to register for
 *   unregister                   the event being dispatched
 *   post                         (none) post EVT_RAW
 *   unloadself, detachself       (none) this instance
 *   unload <nick>, detach <nick> the instance whose client is that nick
 *
 * Returns false if st[0] is not one of these.
 */
bool gnutest::eventCommand(iClient* requester, const StringTokenizer& st) {
    if (st[0] == "events" && st.size() > 1) {
        const bool on = (st[1] == "on");
        for (const eventNameEntry& entry : networkEventNames) {
            on ? MyUplink->RegisterEvent(entry.whichEvent, this)
               : MyUplink->UnRegisterEvent(entry.whichEvent, this);
        }
        on ? MyUplink->RegisterChannelEvent(xServer::CHANNEL_ALL, this)
           : MyUplink->UnRegisterChannelEvent(xServer::CHANNEL_ALL, this);

        eventWatcher = on ? requester->getCharYYXXX() : string();
        Notice(requester, "Events {}", on ? "on" : "off");
        return true;
    }

    if (st[0] == "onevent" && st.size() > 2) {
        armedEvent action;
        if (!findEventByName(st[1], action.whichEvent, action.channelEvent)) {
            Notice(requester, "No such event: {}", st[1]);
            return true;
        }
        action.action = st[2];
        action.argument = (st.size() > 3) ? st.assemble(3) : string();
        armed.push_back(action);
        Notice(requester, "Armed {} on {}", action.action, st[1]);
        return true;
    }

    return false;
}

void gnutest::eventArrived(int whichEvent, bool channelEvent, const std::vector<string>& args,
                           iClient* aboutClient, Channel* theChan) {
    if (!eventWatcher.empty()) {
        reportEvent(eventName(whichEvent, channelEvent), args);
    } else if (theChan != nullptr && theChan->getName() != operChan) {
        // Watching every channel is expected; any other channel is not ours
        LOG_MSG(WARN, "Got bad channel: {chan}").with("chan", theChan).log();
    }

    // Both of the above read theChan, so both run before the armed action: an
    // action may part or kick the channel empty, and then the object lives only
    // as long as the holding list holds it.  The switch this replaced warned
    // afterwards and depended on that.
    runArmedAction(whichEvent, channelEvent, aboutClient, theChan);
}

void gnutest::membership(int whichEvent, Channel* theChan, iClient* theClient,
                         ChannelUser* theUser) {
    eventArrived(whichEvent, true, {theChan->getName(), nickOf(theClient), memberOf(theUser)},
                 theClient, theChan);

    if (theChan->getName() == operChan && theClient->isOper()) {
        Op(theChan, theClient);
    }
}

void gnutest::reportEvent(const string& name, const std::vector<string>& args) {
    iClient* watcher = Network->findClient(eventWatcher);
    if (0 == watcher) {
        // Whoever asked is gone
        return;
    }

    string line("EVENT ");
    line += name;
    for (const string& arg : args) {
        line += ' ';
        line += arg;
    }

    Notice(watcher, line);
}

void gnutest::runArmedAction(int whichEvent, bool channelEvent, iClient* aboutClient,
                             Channel* theChan) {
    const std::vector<armedEvent>::iterator entry =
        std::find_if(armed.begin(), armed.end(), [&](const armedEvent& candidate) {
            return candidate.whichEvent == whichEvent && candidate.channelEvent == channelEvent;
        });
    if (entry == armed.end()) {
        return;
    }

    // Once, whatever the action goes on to do to this module
    const armedEvent action = *entry;
    armed.erase(entry);

    /* An action's argument names what it is to act on instead of what the event
     * was handed, so that a handler can be made to reach for a third object the
     * dispatch knows nothing about.  With no argument each acts as it always
     * has, on the event's own payload or on this instance. */
    const StringTokenizer target(action.argument);

    if (action.action == "kill") {
        iClient* victim = target.empty() ? aboutClient : Network->findNick(target[0]);
        if (victim != nullptr) {
            Kill(victim, "onevent kill");
        }
    } else if (action.action == "kick") {
        Channel* kickChan = target.empty() ? theChan : Network->findChannel(target[0]);
        iClient* victim = target.empty()        ? aboutClient
                          : (target.size() > 1) ? Network->findNick(target[1])
                                                : nullptr;
        if (kickChan != nullptr && victim != nullptr) {
            Kick(kickChan, victim, "onevent kick");
        }
    } else if (action.action == "part") {
        Part(action.argument);
    } else if (action.action == "unregister") {
        channelEvent ? MyUplink->UnRegisterChannelEvent(xServer::CHANNEL_ALL, this)
                     : MyUplink->UnRegisterEvent(whichEvent, this);
    } else if (action.action == "register") {
        int newEvent = 0;
        bool newIsChannel = false;
        if (findEventByName(action.argument, newEvent, newIsChannel)) {
            newIsChannel ? MyUplink->RegisterChannelEvent(xServer::CHANNEL_ALL, this)
                         : MyUplink->RegisterEvent(newEvent, this);
        }
    } else if (action.action == "post") {
        /* One nested event that nothing depends on: EVT_RAW is information
         * only, and core posts it for every line it reads anyway. */
        MyUplink->postRaw("onevent post");
    } else if (action.action == "unloadself") {
        MyUplink->UnloadClient(this, "test");
    } else if (action.action == "detachself") {
        MyUplink->DetachClient(this, "test");
    } else if (action.action == "unload" || action.action == "detach") {
        /* Another module instance, named by the nickname of its client: the
         * module name two instances of one library share tells them apart for
         * nobody, this module included. */
        xClient* other = target.empty() ? nullptr : Network->findLocalNick(target[0]);
        if (other != nullptr) {
            if (action.action == "unload") {
                MyUplink->UnloadClient(other, "test");
            } else {
                MyUplink->DetachClient(other, "test");
            }
        }
    }
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
    if (st[0] == "servmodets" && st.size() > 3) {
        // servmodets <#chan> <timestamp> <modes> [args]: a mode change by
        // the server that names a creation time for the channel
        if (Channel* theChan = Network->findChannel(st[1])) {
            MyUplink->Mode(theChan, st[3], st.size() > 4 ? st.assemble(4) : string(), nullptr,
                           static_cast<time_t>(atol(st[2].c_str())));
        }
        return;
    }
    if (st[0] == "usermode" && st.size() > 1) {
        // Our own user modes: xClient::Mode( modes )
        Mode(st[1]);
        return;
    }
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
    if (st[0] == "wallops" && st.size() > 1) {
        Wallops(st.assemble(1));
        return;
    }
    if (st[0] == "noticechanops" && st.size() > 2) {
        NoticeChannelOps(st[1], st.assemble(2));
        return;
    }
    if (st[0] == "isonchannel" && st.size() > 1) {
        Notice(theClient, "{}: {}", st[1], isOnChannel(st[1]) ? "yes" : "no");
        return;
    }
    if (eventCommand(theClient, st)) {
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
        LOG_MSG(ERROR, "Failed to add new iServer: {server}").with("server", newServer).log();

        Notice(requestingClient, "Failed to add new server");
    } else {
        LOG_MSG(INFO, "Added new iServer: {server}").with("server", newServer).log();

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
        LOG(WARN, "Failed to find server name: {}", name);

        Notice(requestingClient, "Failed to find server: {}", name);
        return;
    }

    if (!MyUplink->DetachServer(theServer)) {
        LOG_MSG(ERROR, "Failed to DetachServer(): {server}").with("server", theServer).log();

        Notice(requestingClient, "Failed to remove server: {}", name);
    } else {
        LOG_MSG(INFO, "Successfully removed server: {server}").with("server", theServer).log();

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

    LOG(DEBUG, "Removing: {}", nickName);

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

    LOG(DEBUG, "Spawning {}", nickName);

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
        LOG_MSG(ERROR, "Failed to add new client: {client}").with("client", newClient).log();

        Notice(requestingClient, "Failed to create new fake client");
        delete newClient;
        newClient = 0;
    } else {
        Notice(requestingClient, "Created new client {}", nickName);
        LOG_MSG(INFO, "Added client: {client}").with("client", newClient).log();
    }
}

void gnutest::OnTimer(const xServer::timerID&, void*) {
    Channel* theChan = Network->findChannel(timerChan);
    if (NULL == theChan) {
        LOG(WARN, "Unable to find channel: {}", timerChan);
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
    LOG_MSG(DEBUG, "Name: {chan}, creation time: {}\n--- User information ---",
            theChan->getCreationTime())
        .with("chan", theChan)
        .log();

    // Iterate through all clients, and return info about each
    // ChannelUser
    for (Channel::const_userIterator cItr = theChan->userList_begin();
         cItr != theChan->userList_end(); ++cItr) {
        const ChannelUser* theUser = cItr->second;
        LOG_MSG(DEBUG, "{user}!{}@{} {} user modes: {}", theUser->getUserName(),
                theUser->getHostName(), theUser->getCharYYXXX(), theUser->getModeString())
            .with("user", theUser)
            .log();
    }
}

} // namespace gnuworld
