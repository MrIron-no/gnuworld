/**
 * server_events.cc
 * This is the implementation file for the xServer class.
 * This class is the entity which is the GNUWorld server
 * proper.  It manages network I/O, parsing and distributing
 * incoming messages, notifying attached clients of
 * system events, on, and on, and on.
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
 * $Id: server_events.cc,v 1.3 2005/09/29 17:40:06 kewlio Exp $
 */

#include <new>
#include <string>
#include <string_view>
#include <cstdlib>
#include <format>
#include <functional>
#include <thread>
#include <utility>
#include <vector>
#include <span>
#include <stack>
#include <iostream>

#include <csignal>

#include "server.h"
#include "misc.h"
#include "Network.h"
#include "iClient.h"
#include "LogSinks.h"
#include "logger.h"

/* The logger this file writes to: the server itself.  Core is one binary and
 * not one module, so this stands once per .cc file rather than in a header */
GNUWORLD_CORE_LOGGER(Core);

namespace gnuworld {

using std::stack;
using std::string;

/**
 * The thread that may post an event.  A post walks the registries, the queue
 * and the holding list with no lock, as does everything a handler goes on to
 * do, so only one thread may ever be in here.  The loader runs this before
 * main(), on the thread main() then runs on.  IrcLogSink asks the same question
 * of a log record, but keeps isMainThread() to itself.
 */
static const std::thread::id mainThread = std::this_thread::get_id();

/**
 * This method will register the given xClient to receive
 * all events of the given type.
 * Available events are listed in include/events.h
 */
bool xServer::RegisterEvent(const eventType& theEvent, xClient* theClient) {
    assert(theClient != NULL);

    // Make sure that the given event is valid
    // (in the interval of possible events).
    if (!validEvent(theEvent)) {
        return false;
    }

    // Make sure not to add a client more than once
    UnRegisterEvent(theEvent, theClient);

    // Add this client as listener for this event
    eventList[theEvent].push_back(theClient);

    // Registration succeeded
    return true;
}

/**
 * This method will register the given xClient for any
 * channel event that occurs in channel chanName, case
 * insensitive.
 */
bool xServer::RegisterChannelEvent(const string& chanName, xClient* theClient) {
    assert(theClient != NULL);

    // Prevent duplicates of the same channel/client pair
    UnRegisterChannelEvent(chanName, theClient);

    // Add the xClient as a listener for events in this channel, whose list of
    // listeners this may be the first of.
    channelEventMap[chanName].push_back(theClient);

    // Addition successful
    return true;
}

/**
 * This method will attempt to unregister the given client
 * from receiving events of type theEvent.
 * It will fail (return false) when theEvent is not valid, or
 * the xClient is not found as being registered for
 * event theEvent.
 */
bool xServer::UnRegisterEvent(const eventType& theEvent, xClient* theClient) {
    assert(theClient != NULL);

    // Make sure this is a valid event.
    if (!validEvent(theEvent)) {
        return false;
    }

    // Since each xClient may only be registered once for any given event,
    // this either removes it or finds it was not a listener. *shrug*
    return std::erase(eventList[theEvent], theClient) > 0;
}

/**
 * This method will stop the given xClient from receiving any
 * events in the channel chanName, case insensitive.
 */
bool xServer::UnRegisterChannelEvent(const string& chanName, xClient* theClient) {
    assert(theClient != NULL);

    channelEventMapType::iterator chanPtr = channelEventMap.find(chanName);
    if (chanPtr == channelEventMap.end()) {
        // Channel has no xClient's registered for channel events
        // No big deal.
        return true;
    }

    const bool found = (std::erase(chanPtr->second, theClient) > 0);

    // An entry with no listeners left is no entry
    if (chanPtr->second.empty()) {
        channelEventMap.erase(chanPtr);
    }

    // false when the key/xClient pair was not there.
    return found;
}

/**
 * Append the listeners of a channel event: those registered for every channel
 * first, then those registered for this one.
 */
void xServer::channelListeners(const string& chanName, std::vector<xClient*>& out) const {
    const channelEventMapType::const_iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        out.insert(out.end(), allChanPtr->second.begin(), allChanPtr->second.end());
    }

    const channelEventMapType::const_iterator chanPtr = channelEventMap.find(chanName);
    if (chanPtr != channelEventMap.end()) {
        out.insert(out.end(), chanPtr->second.begin(), chanPtr->second.end());
    }
}

/**
 * The one walk of a listener list there is: every call core makes into a
 * module goes through here.
 *
 * It calls into module code, so it walks a copy: a handler may register,
 * unregister, detach or destroy whatever it likes without touching the walk.
 * The listeners of the moment are asked for again before each call, which is
 * one std::find over a handful of pointers, so that a module that has left the
 * event does not see it and one detached during the dispatch is called no
 * more.  A module that joins the event sees the next one, not this one.
 *
 * While this runs, dispatchDepth is not zero: a post made from a handler waits
 * in pendingEvents, and a network object core removes waits in holdingList.
 * Getting back to zero is therefore the invariant the whole of this stands on,
 * and there is deliberately no try around the walk.  An exception out of a
 * handler skips the epilogue below, so the depth never returns to zero and
 * every later post queues forever, silently delivered to nobody: a catch here
 * that swallowed it would make that the daemon's steady state.  Nothing
 * between a wire handler and main() catches either, so what really happens is
 * std::terminate(), which is what this codebase wants of a handler that cannot
 * cope - see ProtocolError().  Do not add one upstream without reading this.
 */
template <typename Listeners, typename Call>
void xServer::dispatch(Listeners listeners, Call call) {
    std::vector<xClient*> snapshot;
    listeners(snapshot);

    ++dispatchDepth;

    std::vector<xClient*> live;
    for (xClient* const theClient : snapshot) {
        live.clear();
        listeners(live);

        if (std::find(live.begin(), live.end(), theClient) != live.end()) {
            call(theClient);
        }
    }

    --dispatchDepth;

    if (0 == dispatchDepth) {
        settle();
    }
}

/**
 * Deliver a notification: now when nothing is being dispatched, which is every
 * post from a libircu handler or the main loop, and otherwise behind the event
 * being dispatched and behind whatever is already waiting, so that every
 * subscriber sees every event in the order things happened.  Whatever the post
 * did to the wire and to the network tables has already happened; it is only
 * the notification that waits.
 */
template <typename Listeners, typename Call> void xServer::notify(Listeners listeners, Call call) {
    assert(std::this_thread::get_id() == mainThread);

    if (dispatchDepth > 0) {
        pendingEvents.push_back(
            [this, listeners, call]() mutable { dispatch(std::move(listeners), std::move(call)); });
        return;
    }

    dispatch(std::move(listeners), std::move(call));
}

/**
 * notify() for a network event, whose listeners are those registered for it.
 */
template <typename Call> void xServer::notifyEvent(eventType theEvent, Call call) {
    notify(
        [this, theEvent](std::vector<xClient*>& out) {
            const std::vector<xClient*>& listeners = eventList[theEvent];
            out.insert(out.end(), listeners.begin(), listeners.end());
        },
        std::move(call));
}

/**
 * notify() for a channel event, whose listeners are those of chanName.
 */
template <typename Call> void xServer::notifyChannel(const string& chanName, Call call) {
    notify([this, chanName](std::vector<xClient*>& out) { channelListeners(chanName, out); },
           std::move(call));
}

/**
 * Deliver every post that waited for the dispatch, oldest first.  A drained
 * post dispatches like any other, so what IT posts is appended behind what is
 * already waiting and the queue empties breadth first.
 */
void xServer::settle() {
    while (!pendingEvents.empty()) {
        const std::function<void()> next = std::move(pendingEvents.front());
        pendingEvents.pop_front();
        next();
    }
}

/**
 * Destroy what the holding list holds.  The queue is drained where the
 * outermost dispatch ends, because a handler's own post must arrive before the
 * Post*() that carried it returns; the holding list is NOT released there,
 * because the core code under that Post*() reads on.  msg_L is the plain case:
 * it posts EVT_PART, a handler empties the channel from inside it, and msg_L
 * then asks that same channel whether it is empty.  So this runs once per
 * iteration of the main loop, when core is between lines and nothing holds a
 * pointer to any of it.
 */
void xServer::releaseHeldObjects() {
    // Never with a dispatch in progress: that is the whole point of the list
    assert(0 == dispatchDepth);

    /* This is also where a module that asked to go while a handler was running
     * is finally let go: nothing of its own is on the stack here, the queue is
     * empty because the outermost dispatch drained it, and the holding list is
     * given back first, so the library can be unmapped.  Finishing one such
     * removal quits the module's clients, which posts events and removes
     * network objects of its own, so both lists can fill again: go round until
     * neither has anything left. */
    while (!holdingList.empty() || !pendingUnloads.empty()) {
        std::vector<std::function<void()>> releasing;
        releasing.swap(holdingList);
        for (const std::function<void()>& release : releasing) {
            release();
        }

        if (!pendingUnloads.empty()) {
            xClient* const theClient = pendingUnloads.front();
            pendingUnloads.pop_front();

            removeClient(theClient);
        }
    }
}

/*
 * One post function per event.  Each hands its arguments to the named method
 * of every listener, through the dispatch above.  A closure that has to wait
 * its turn in pendingEvents owns its text, because a caller may well have
 * handed a local of its own: that is what copying a string_view into a string
 * captured by value is for, and why nothing here keeps the view itself.
 */

void xServer::postOper(iClient* theClient) {
    notifyEvent(EVT_OPER, [theClient](xClient* listener) { listener->OnOper(theClient); });
}

void xServer::postNetBreak(iServer* theServer, const iServer* uplink, std::string_view reason) {
    notifyEvent(EVT_NETBREAK, [theServer, uplink, text = string(reason)](xClient* listener) {
        listener->OnNetBreak(theServer, uplink, text);
    });
}

void xServer::postNetJoin(iServer* theServer, const iServer* uplink) {
    notifyEvent(EVT_NETJOIN,
                [theServer, uplink](xClient* listener) { listener->OnNetJoin(theServer, uplink); });
}

void xServer::postBurstComplete(iServer* theServer) {
    notifyEvent(EVT_BURST_CMPLT,
                [theServer](xClient* listener) { listener->OnBurstComplete(theServer); });
}

void xServer::postBurstAck(iServer* theServer) {
    notifyEvent(EVT_BURST_ACK, [theServer](xClient* listener) { listener->OnBurstAck(theServer); });
}

void xServer::postEndOfBurstAckSent(iServer* theServer) {
    notifyEvent(EVT_EA_SENT,
                [theServer](xClient* listener) { listener->OnEndOfBurstAckSent(theServer); });
}

void xServer::postGline(Gline* theGline, const xClient* exclude) {
    notifyEvent(EVT_GLINE, [theGline, exclude](xClient* listener) {
        if (listener != exclude) {
            listener->OnGline(theGline);
        }
    });
}

void xServer::postRemGline(Gline* theGline, const xClient* exclude) {
    notifyEvent(EVT_REMGLINE, [theGline, exclude](xClient* listener) {
        if (listener != exclude) {
            listener->OnRemGline(theGline);
        }
    });
}

void xServer::postQuit(iClient* theClient, std::string_view reason) {
    notifyEvent(EVT_QUIT, [theClient, text = string(reason)](xClient* listener) {
        listener->OnQuit(theClient, text);
    });
}

void xServer::postKill(const NetworkTarget* source, iClient* theClient, std::string_view reason) {
    notifyEvent(EVT_KILL, [source, theClient, text = string(reason)](xClient* listener) {
        listener->OnKill(source, theClient, text);
    });
}

void xServer::postNick(iClient* theClient) {
    notifyEvent(EVT_NICK, [theClient](xClient* listener) { listener->OnNick(theClient); });
}

void xServer::postNickChange(iClient* theClient, std::string_view oldNick) {
    notifyEvent(EVT_CHNICK, [theClient, text = string(oldNick)](xClient* listener) {
        listener->OnNickChange(theClient, text);
    });
}

void xServer::postAccount(iClient* theClient, const xClient* exclude) {
    notifyEvent(EVT_ACCOUNT, [theClient, exclude](xClient* listener) {
        if (listener != exclude) {
            listener->OnAccount(theClient);
        }
    });
}

void xServer::postAccountFlags(iClient* theClient, const xClient* exclude) {
    notifyEvent(EVT_ACCOUNT_FLAGS, [theClient, exclude](xClient* listener) {
        if (listener != exclude) {
            listener->OnAccountFlags(theClient);
        }
    });
}

void xServer::postRaw(std::string_view line) {
    notifyEvent(EVT_RAW, [text = string(line)](xClient* listener) { listener->OnRaw(text); });
}

void xServer::postXQuery(iServer* theServer, std::string_view routing, std::string_view message) {
    notifyEvent(EVT_XQUERY, [theServer, routingText = string(routing),
                             messageText = string(message)](xClient* listener) {
        listener->OnXQuery(theServer, routingText, messageText);
    });
}

void xServer::postXReply(iServer* theServer, std::string_view routing, std::string_view message) {
    notifyEvent(EVT_XREPLY, [theServer, routingText = string(routing),
                             messageText = string(message)](xClient* listener) {
        listener->OnXReply(theServer, routingText, messageText);
    });
}

void xServer::postNetConf(iServer* theServer, std::string_view key) {
    notifyEvent(EVT_NETCONF, [theServer, text = string(key)](xClient* listener) {
        listener->OnNetConf(theServer, text);
    });
}

void xServer::postRemNetConf(iServer* theServer, std::string_view key) {
    notifyEvent(EVT_REMNETCONF, [theServer, text = string(key)](xClient* listener) {
        listener->OnRemNetConf(theServer, text);
    });
}

void xServer::postJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(), [theChan, theClient, theUser](xClient* listener) {
        listener->OnJoin(theChan, theClient, theUser);
    });
}

void xServer::postBurstJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(), [theChan, theClient, theUser](xClient* listener) {
        listener->OnBurstJoin(theChan, theClient, theUser);
    });
}

void xServer::postCreate(Channel* theChan, iClient* theClient, ChannelUser* theUser) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(), [theChan, theClient, theUser](xClient* listener) {
        listener->OnCreate(theChan, theClient, theUser);
    });
}

void xServer::postPart(Channel* theChan, iClient* theClient, std::string_view message) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(),
                  [theChan, theClient, text = string(message)](xClient* listener) {
                      listener->OnPart(theChan, theClient, text);
                  });
}

void xServer::postTopic(Channel* theChan, iClient* theClient, std::string_view topic) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(),
                  [theChan, theClient, text = string(topic)](xClient* listener) {
                      listener->OnTopic(theChan, theClient, text);
                  });
}

void xServer::postServerMode(Channel* theChan, iServer* theServer) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(), [theChan, theServer](xClient* listener) {
        listener->OnServerMode(theChan, theServer);
    });
}

/// One payload of an untyped post that is text: the std::string it points at
static std::string_view textOf(void* data) {
    return (0 == data) ? std::string_view() : std::string_view(*static_cast<const string*>(data));
}

/// The same for the two events whose text is a NUL-terminated char array
static std::string_view charsOf(void* data) {
    return (0 == data) ? std::string_view() : std::string_view(static_cast<const char*>(data));
}

/**
 * Post an event named by number, with its payloads as void*: which post
 * function that is, and what each void* points at.
 *
 * bridge: removed by events-remove-legacy
 */
void xServer::PostEvent(const eventType& theEvent, void* Data1, void* Data2, void* Data3, void*,
                        const xClient* excludeMe) {
    switch (theEvent) {
    case EVT_OPER:
        postOper(static_cast<iClient*>(Data1));
        break;
    case EVT_NETBREAK:
        postNetBreak(static_cast<iServer*>(Data1), static_cast<const iServer*>(Data2),
                     textOf(Data3));
        break;
    case EVT_NETJOIN:
        postNetJoin(static_cast<iServer*>(Data1), static_cast<const iServer*>(Data2));
        break;
    case EVT_BURST_CMPLT:
        postBurstComplete(static_cast<iServer*>(Data1));
        break;
    case EVT_BURST_ACK:
        postBurstAck(static_cast<iServer*>(Data1));
        break;
    case EVT_EA_SENT:
        postEndOfBurstAckSent(static_cast<iServer*>(Data1));
        break;
    case EVT_GLINE:
        postGline(static_cast<Gline*>(Data1), excludeMe);
        break;
    case EVT_REMGLINE:
        postRemGline(static_cast<Gline*>(Data1), excludeMe);
        break;
    case EVT_QUIT:
        postQuit(static_cast<iClient*>(Data1), textOf(Data2));
        break;
    case EVT_KILL:
        postKill(static_cast<const NetworkTarget*>(Data1), static_cast<iClient*>(Data2),
                 textOf(Data3));
        break;
    case EVT_NICK:
        postNick(static_cast<iClient*>(Data1));
        break;
    case EVT_CHNICK:
        postNickChange(static_cast<iClient*>(Data1), textOf(Data2));
        break;
    case EVT_ACCOUNT:
        postAccount(static_cast<iClient*>(Data1), excludeMe);
        break;
    case EVT_ACCOUNT_FLAGS:
        postAccountFlags(static_cast<iClient*>(Data1), excludeMe);
        break;
    case EVT_RAW:
        postRaw(textOf(Data1));
        break;
    case EVT_XQUERY:
        postXQuery(static_cast<iServer*>(Data1), charsOf(Data2), charsOf(Data3));
        break;
    case EVT_XREPLY:
        postXReply(static_cast<iServer*>(Data1), charsOf(Data2), charsOf(Data3));
        break;
    case EVT_NETCONF:
        postNetConf(static_cast<iServer*>(Data1), textOf(Data2));
        break;
    case EVT_REMNETCONF:
        postRemNetConf(static_cast<iServer*>(Data1), textOf(Data2));
        break;
    default:
        // EVT_JUPE and EVT_UNJUPE among them: nothing posts either, and there
        // is nothing to deliver them to
        LOG(WARN, "Invalid event number: {}", static_cast<int>(theEvent));
        break;
    }
}

/**
 * The same for a channel event.
 *
 * bridge: removed by events-remove-legacy
 */
void xServer::PostChannelEvent(const channelEventType& theEvent, Channel* theChan, void* Data1,
                               void* Data2, void*, void*) {
    assert(theChan != 0);

    switch (theEvent) {
    case EVT_JOIN:
        postJoin(theChan, static_cast<iClient*>(Data1), static_cast<ChannelUser*>(Data2));
        break;
    case EVT_BURST:
        postBurstJoin(theChan, static_cast<iClient*>(Data1), static_cast<ChannelUser*>(Data2));
        break;
    case EVT_CREATE:
        postCreate(theChan, static_cast<iClient*>(Data1), static_cast<ChannelUser*>(Data2));
        break;
    case EVT_PART:
        postPart(theChan, static_cast<iClient*>(Data1), textOf(Data2));
        break;
    case EVT_TOPIC:
        postTopic(theChan, static_cast<iClient*>(Data1), textOf(Data2));
        break;
    case EVT_SERVERMODE:
        postServerMode(theChan, static_cast<iServer*>(Data1));
        break;
    default:
        // EVT_KICK among them: a kick goes to OnNetworkKick()
        LOG(WARN, "Invalid channel event number: {}", static_cast<int>(theEvent));
        break;
    }
}

// srcClient may be NULL, when the source is a server
void xServer::PostChannelKick(Channel* theChan, iClient* srcClient, iClient* destClient,
                              const string& kickMessage, bool authoritative) {
    // Public method, verify arguments
    assert(theChan != 0);
    assert(destClient != 0);

    notifyChannel(theChan->getName(), [theChan, srcClient, destClient, kickMessage,
                                       authoritative](xClient* theClient) {
        theClient->OnNetworkKick(theChan, srcClient, destClient, kickMessage, authoritative);
    });
}

bool xServer::PostSignal(int whichSig) {
    // First, notify the server signal handler.  That is the server acting on
    // the signal, not a notification, and so it is never deferred.
    bool handledSignal = OnSignal(whichSig);

    // Pass this signal on to each xClient.  A module detached while they are
    // being called is no longer a local client, and is not called.
    notify(
        [](std::vector<xClient*>& out) {
            for (xNetwork::localClientIterator ptr = Network->localClient_begin();
                 ptr != Network->localClient_end(); ++ptr) {
                out.push_back(ptr->second);
            }
        },
        [whichSig](xClient* theClient) { theClient->OnSignal(whichSig); });

    return handledSignal;
}

bool xServer::OnSignal(int whichSig) {
    bool retMe = false;
    switch (whichSig) {
    case SIGUSR1:
        dumpStats();
        retMe = true;
        break;
    case SIGHUP:
        rotateLogs();
        retMe = true;
        break;
    case SIGUSR2:
        retMe = true;
        break;
    case SIGINT:
    case SIGTERM:
        Shutdown();
        retMe = true;
        break;
    default:
        break;
    }
    return retMe;
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
// modeVector contains pairs of bool (polarity) and Channel::modeType
// (which mode).
// This method is invoked only for simple (no argument) modes.
void xServer::OnChannelMode(Channel* theChan, ChannelUser* sourceUser,
                            const xServer::modeVectorType& modeVector) {
    theChan->onMode(modeVector);

    notifyChannel(theChan->getName(), [theChan, sourceUser, modeVector](xClient* theClient) {
        theClient->OnChannelMode(theChan, sourceUser, modeVector);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeL(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             unsigned int limit) {
    theChan->onModeL(polarity, limit);

    notifyChannel(theChan->getName(), [theChan, polarity, sourceUser, limit](xClient* theClient) {
        theClient->OnChannelModeL(theChan, polarity, sourceUser, limit);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeK(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& key) {
    theChan->onModeK(polarity, key);

    notifyChannel(theChan->getName(), [theChan, polarity, sourceUser, key](xClient* theClient) {
        theClient->OnChannelModeK(theChan, polarity, sourceUser, key);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeA(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Apass) {
    theChan->onModeA(polarity, Apass);

    notifyChannel(theChan->getName(), [theChan, polarity, sourceUser, Apass](xClient* theClient) {
        theClient->OnChannelModeA(theChan, polarity, sourceUser, Apass);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeU(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Upass) {
    theChan->onModeU(polarity, Upass);

    notifyChannel(theChan->getName(), [theChan, polarity, sourceUser, Upass](xClient* theClient) {
        theClient->OnChannelModeU(theChan, polarity, sourceUser, Upass);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occurs
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeO(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::opVectorType& opVector) {
    theChan->onModeO(opVector);

    notifyChannel(theChan->getName(), [theChan, sourceUser, opVector](xClient* theClient) {
        theClient->OnChannelModeO(theChan, sourceUser, opVector);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeV(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::voiceVectorType& voiceVector) {
    theChan->onModeV(voiceVector);

    notifyChannel(theChan->getName(), [theChan, sourceUser, voiceVector](xClient* theClient) {
        theClient->OnChannelModeV(theChan, sourceUser, voiceVector);
    });
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeB(Channel* theChan, ChannelUser* sourceUser,
                             xServer::banVectorType& banVector,
                             std::span<const Channel::BanInfo> banInfo) {

    // Channel::onModeB() may modify banVector with the extra bans
    // that have been removed due to overlaps
    theChan->onModeB(banVector, banInfo);

    notifyChannel(theChan->getName(),
                  [theChan, sourceUser, bans = banVector](xClient* theClient) mutable {
                      theClient->OnChannelModeB(theChan, sourceUser, bans);
                  });
}

/**
 * The one place where a parsed mode change becomes a change of state.  It
 * goes through the OnChannelMode*() methods above, which update the Channel
 * and notify the modules, in the order the handlers have always used: a
 * limit, key or password as soon as it is met, then the flags, the ops, the
 * voices and the bans, each as one batch.
 */
void xServer::ApplyChannelModes(Channel* theChan, ChannelUser* sourceUser,
                                std::span<const Channel::ModeChange> changes,
                                std::string_view where, std::string_view setBy) {
    std::vector<std::string> problems;

    modeVectorType modeVector;
    opVectorType opVector;
    voiceVectorType voiceVector;
    banVectorType banVector;

    for (const Channel::ModeChange& change : changes) {
        const Channel::ModeInfo& mode = change.mode;

        switch (mode.type) {
        case Channel::ModeType::Flag:
            modeVector.emplace_back(change.set, mode.flag);
            break;

        case Channel::ModeType::SetOnly:
            // parseModes() has validated the argument; -l carries none
            OnChannelModeL(theChan, change.set, sourceUser,
                           parseNumber<unsigned int>(change.arg).value_or(0));
            break;

        case Channel::ModeType::Setting:
            if ('k' == mode.letter) {
                OnChannelModeK(theChan, change.set, sourceUser, change.arg);
            } else if ('A' == mode.letter) {
                OnChannelModeA(theChan, change.set, sourceUser, change.arg);
            } else {
                OnChannelModeU(theChan, change.set, sourceUser, change.arg);
            }
            break;

        case Channel::ModeType::Prefix: {
            iClient* target = Network->findClient(change.arg);
            ChannelUser* member = (target != 0) ? theChan->findUser(target) : 0;
            if (0 == member) {
                problems.push_back(std::string("no such member for mode '") + mode.letter +
                                   "': " + change.arg);
                continue;
            }
            ('o' == mode.letter ? opVector : voiceVector).emplace_back(change.set, member);
            break;
        }

        case Channel::ModeType::List:
            banVector.emplace_back(change.set, change.arg);
            break;
        }
    }

    if (!modeVector.empty()) {
        OnChannelMode(theChan, sourceUser, modeVector);
    }
    if (!opVector.empty()) {
        OnChannelModeO(theChan, sourceUser, opVector);
    }
    if (!voiceVector.empty()) {
        OnChannelModeV(theChan, sourceUser, voiceVector);
    }
    if (!banVector.empty()) {
        // One line, one source: every ban on it was set by setBy, now.  A
        // removal's entry is not read.
        const std::vector<Channel::BanInfo> banInfo(banVector.size(),
                                                    Channel::BanInfo{std::string(setBy), 0});
        OnChannelModeB(theChan, sourceUser, banVector, banInfo);
    }

    // What could not be applied is ours to report: the caller has nothing
    // to do about it.
    // "where" names the caller, which the function captured here cannot
    for (const std::string& problem : problems) {
        LOG(WARN, "{} ({}): {}", where, theChan->getName(), problem);
    }
}

void xServer::ProtocolError(std::string_view where, std::span<const std::string> problems) const {
    // A record goes wherever logging.conf says; the console sink is off on a
    // daemon, so make sure this one is seen somewhere
    std::ostream* fallback = !ConsoleSink::enabled() ? &std::cerr : 0;

    const auto say = [&](const std::string& text) {
        LOG(FATAL, "{}", text);
        if (fallback != 0) {
            *fallback << text << std::endl;
        }
    };

    say(std::format("{} PROTOCOL ERROR, cannot parse this line from the uplink: {}", where,
                    currentLine));
    for (const std::string& problem : problems) {
        say(std::format("{}   {}", where, problem));
    }
    say(std::format("{} Aborting: the state of the network is no longer known.", where));

    ::abort();
}

} // namespace gnuworld
