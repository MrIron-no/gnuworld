/**
 * Events.h
 * This file defines the basic network and channel events that
 * the GNUWorld server may distribute to its services clients.
 * Hopefully most of these are pretty self explanatory.
 * This file was originally created by Orlando Bassotto.
 *
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
 * $Id: events.h,v 1.22 2010/09/05 17:26:35 denspike Exp $
 */

#ifndef __EVENTS_H
#define __EVENTS_H "$Id: events.h,v 1.22 2010/09/05 17:26:35 denspike Exp $"

#include <array>
#include <cstddef>
#include <string_view>

namespace gnuworld {

/**
 * Which network (non-channel) event this is.  What each one means, when core
 * posts it and what it carries is said once, at the virtual that delivers it:
 * see xClient::OnOper() and the named virtuals after it in include/client.h.
 * Every one of these has a post function of its own, xServer::postOper() and
 * its kin in include/server.h.
 */
enum NetworkEvent : int {
    EVT_OPER,
    EVT_NETBREAK,
    EVT_NETJOIN,
    EVT_BURST_CMPLT,
    EVT_BURST_ACK,
    EVT_EA_SENT,
    EVT_GLINE,
    EVT_REMGLINE,
    EVT_JUPE,
    EVT_UNJUPE,
    EVT_QUIT,
    EVT_KILL,
    EVT_NICK,
    EVT_CHNICK,
    EVT_ACCOUNT,
    EVT_ACCOUNT_FLAGS,
    EVT_RAW,
    EVT_XQUERY,
    EVT_XREPLY,
    EVT_NETCONF,
    EVT_REMNETCONF
};

/// How many network events there are: what eventList is indexed by.
constexpr std::size_t networkEventCount = EVT_REMNETCONF + 1;

/**
 * The old end marker of the network enum, which is also where the channel
 * enum starts.  Only the untyped API still needs it, and it goes with it.
 */
constexpr int EVT_NOOP = networkEventCount;

/**
 * Which channel event this is.  As above, each is documented at the virtual
 * that delivers it: xClient::OnJoin() and its kin in include/client.h.
 */
enum ChannelEvent : int {
    EVT_JOIN = EVT_NOOP,
    EVT_PART,
    EVT_SERVERMODE, // when server performs modes.
    EVT_TOPIC,      // passed even if TRACK_TOPIC is disabled
    EVT_KICK,       // moved to xClient::OnNetworkKick()
    EVT_CREATE,
    EVT_BURST
};

/// How many channel events there are.
constexpr std::size_t channelEventCount = EVT_BURST - EVT_JOIN + 1;

/**
 * The types used to represent an event.  They are an int and not the enums
 * above because the untyped API is registered for, posted and delivered by
 * number; a listener registration is still indexed by it.
 */
typedef int eventType;
typedef int channelEventType;

/*
 * The name of an event, for a module that reports or counts them.  Each is one
 * switch with no default, so that an event added without a name here is a
 * -Wswitch warning and therefore a failed build, rather than a hole nobody
 * notices.  eventNames[] below is built from these two.
 */

constexpr std::string_view eventName(NetworkEvent whichEvent) {
    switch (whichEvent) {
    case EVT_OPER:
        return "Oper Up";
    case EVT_NETBREAK:
        return "Net Break";
    case EVT_NETJOIN:
        return "Net Join";
    case EVT_BURST_CMPLT:
        return "Burst Complete";
    case EVT_BURST_ACK:
        return "Burst Acknowledge";
    case EVT_EA_SENT:
        return "Burst Acknowledge Sent";
    case EVT_GLINE:
        return "Gline Add";
    case EVT_REMGLINE:
        return "Gline Remove";
    case EVT_JUPE:
        return "Server Jupe";
    case EVT_UNJUPE:
        return "Server UnJupe";
    case EVT_QUIT:
        return "Client Quit";
    case EVT_KILL:
        return "Client Kill";
    case EVT_NICK:
        return "Client Connect";
    case EVT_CHNICK:
        return "Nick Change";
    case EVT_ACCOUNT:
        return "Account Login";
    case EVT_ACCOUNT_FLAGS:
        return "Account Flags";
    case EVT_RAW:
        return "Raw";
    case EVT_XQUERY:
        return "XQuery";
    case EVT_XREPLY:
        return "XReply";
    case EVT_NETCONF:
        return "Netconf Add";
    case EVT_REMNETCONF:
        return "Netconf Remove";
    }
    return {};
}

constexpr std::string_view eventName(ChannelEvent whichEvent) {
    switch (whichEvent) {
    case EVT_JOIN:
        return "Channel Join";
    case EVT_PART:
        return "Channel Part";
    case EVT_SERVERMODE:
        return "Channel Mode By Server";
    case EVT_TOPIC:
        return "Channel Topic Change";
    case EVT_KICK:
        return "Channel Kick";
    case EVT_CREATE:
        return "Channel Create";
    case EVT_BURST:
        return "Channel Burst";
    }
    return {};
}

/**
 * Every event's name, indexed by the event: the network events and then the
 * channel events, which is the order the two enums number them in.  Built from
 * eventName() so that it can no longer be one entry short of the enums, as it
 * was - it had no "Account Flags" and was therefore misaligned from EVT_RAW up,
 * and reading its last entry was reading past its end.
 */
constexpr std::array<std::string_view, networkEventCount + channelEventCount> eventNames = []() {
    std::array<std::string_view, networkEventCount + channelEventCount> names;
    for (std::size_t whichEvent = 0; whichEvent < networkEventCount; ++whichEvent) {
        names[whichEvent] = eventName(static_cast<NetworkEvent>(whichEvent));
    }
    for (std::size_t whichEvent = 0; whichEvent < channelEventCount; ++whichEvent) {
        names[EVT_JOIN + whichEvent] = eventName(static_cast<ChannelEvent>(EVT_JOIN + whichEvent));
    }
    return names;
}();

} // namespace gnuworld

#endif // __EVENTS_H
