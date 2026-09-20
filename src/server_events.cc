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
#include <array>
#include <cstddef>
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

namespace {

/// What one of a post's four arguments points at
enum class payloadKind {
    object, ///< An object of the network: the holding list keeps it alive
    text,   ///< A std::string
    chars   ///< A NUL-terminated string, cast to void*
};

/**
 * Which of an event's four arguments are text.  A post takes void*: only the
 * event says what each argument points at, and a post that has to wait its
 * turn must own its text, because several callers hand it the address of a
 * local of their own.
 *
 * EVT_NETBREAK's second argument is left out on purpose: two of its three post
 * sites pass the source as a std::string, and the third, a cascading squit in
 * xNetwork::OnSplit(), passes the uplink iServer, so core cannot tell from the
 * event alone which of the two it has.
 */
payloadKind payloadKindOf(int theEvent, std::size_t which) {
    switch (theEvent) {
    case EVT_RAW:
        return (0 == which) ? payloadKind::text : payloadKind::object;
    case EVT_QUIT:
    case EVT_CHNICK:
    case EVT_NETCONF:
    case EVT_REMNETCONF:
    case EVT_PART:
    case EVT_TOPIC:
        return (1 == which) ? payloadKind::text : payloadKind::object;
    case EVT_KILL:
    case EVT_NETBREAK:
        return (2 == which) ? payloadKind::text : payloadKind::object;
    case EVT_XQUERY:
    case EVT_XREPLY:
        return (1 == which || 2 == which) ? payloadKind::chars : payloadKind::object;
    default:
        return payloadKind::object;
    }
}

/**
 * A post's four arguments, with its own copy of whatever text they point at.
 * data() is what a handler is given, and is asked for where the post is
 * delivered rather than where it is made: moving the copies moves the text.
 */
class ownedPayload {
  public:
    ownedPayload(int theEvent, void* data1, void* data2, void* data3, void* data4)
        : raw{data1, data2, data3, data4} {
        for (std::size_t which = 0; which < raw.size(); ++which) {
            if (0 == raw[which]) {
                // No argument here, and so nothing to own
                continue;
            }

            kind[which] = payloadKindOf(theEvent, which);
            if (payloadKind::text == kind[which]) {
                text[which] = *static_cast<const string*>(raw[which]);
            } else if (payloadKind::chars == kind[which]) {
                text[which] = static_cast<const char*>(raw[which]);
            }
        }
    }

    /// The argument as a handler is to see it, text pointing into here
    void* data(std::size_t which) {
        switch (kind[which]) {
        case payloadKind::text:
            return static_cast<void*>(&text[which]);
        case payloadKind::chars:
            return static_cast<void*>(text[which].data());
        case payloadKind::object:
            break;
        }
        return raw[which];
    }

  private:
    std::array<void*, 4> raw;
    std::array<payloadKind, 4> kind = {};
    std::array<string, 4> text;
};

} // anonymous namespace

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
 * notify() for a channel event, whose listeners are those of chanName.
 */
template <typename Call> void xServer::notifyChannel(const string& chanName, Call call) {
    notify([this, chanName](std::vector<xClient*>& out) { channelListeners(chanName, out); },
           std::move(call));
}

/**
 * Deliver every post that waited for the dispatch, oldest first.  A drained
 * post dispatches like any other, so what IT posts is appended behind what is
 * already waiting and the queue empties breadth first.  Only then is the
 * holding list released: by that point nothing is left that could be handed
 * one of the objects on it.
 */
void xServer::settle() {
    while (!pendingEvents.empty()) {
        const std::function<void()> next = std::move(pendingEvents.front());
        pendingEvents.pop_front();
        next();
    }

    for (const std::function<void()>& release : holdingList) {
        release();
    }
    holdingList.clear();
}

/**
 * This method will distribute to each xClient listening
 * for the given event (theEvent) an event with the proper
 * arguments.
 * Events are not guaranteed to be distributed in any
 * particular order.
 */
void xServer::PostEvent(const eventType& theEvent, void* Data1, void* Data2, void* Data3,
                        void* Data4, const xClient* excludeMe) {
    // Make sure the event is valid.
    if (!validEvent(theEvent)) {
        LOG(WARN, "Invalid event number: {}", static_cast<int>(theEvent));
        return;
    }

    notify(
        [this, theEvent](std::vector<xClient*>& out) {
            const std::vector<xClient*>& listeners = eventList[theEvent];
            out.insert(out.end(), listeners.begin(), listeners.end());
        },
        [theEvent, owned = ownedPayload(theEvent, Data1, Data2, Data3, Data4),
         excludeMe](xClient* theClient) mutable {
            // Notify this client of the event
            // if he didnt cause the event to trigger
            if (theClient != excludeMe) {
                theClient->OnEvent(theEvent, owned.data(0), owned.data(1), owned.data(2),
                                   owned.data(3));
            }
        });
}

/**
 * This method will distribute to any xClient registered to
 *  receive events for the given channel (chanName), case
 *  insensitive, an event with the proper arguments.
 * Events are not guaranteed to be distributed in any
 *  particular order.
 */
void xServer::PostChannelEvent(const channelEventType& theEvent, Channel* theChan, void* Data1,
                               void* Data2, void* Data3, void* Data4) {
    assert(theChan != 0);

    notifyChannel(theChan->getName(),
                  [theEvent, theChan, owned = ownedPayload(theEvent, Data1, Data2, Data3, Data4)](
                      xClient* theClient) mutable {
                      theClient->OnChannelEvent(theEvent, theChan, owned.data(0), owned.data(1),
                                                owned.data(2), owned.data(3));
                  });
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
