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
#include <vector>
#include <span>
#include <list>
#include <stack>
#include <iostream>

#include <csignal>

#include "server.h"
#include "misc.h"
#include "Network.h"
#include "iClient.h"
#include "ELog.h"

namespace gnuworld {

using std::endl;
using std::list;
using std::stack;
using std::string;

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

    // Obtain a pointer to the list of xClient's registered for events
    // on the given channel.
    channelEventMapType::iterator chanPtr = channelEventMap.find(chanName);
    if (chanPtr == channelEventMap.end()) {
        // Channel event list doesn't exist yet
        channelEventMap.insert(channelEventMapType::value_type(chanName, new list<xClient*>));
        chanPtr = channelEventMap.find(chanName);
    }

    // Add the xClient as a listener for events in this channel.
    chanPtr->second->push_back(theClient);

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

    // Iterate through the list of registered listeners for
    // this event and attempt to find the xClient wishing to be removed.
    //
    list<xClient*>::iterator ptr = eventList[theEvent].begin(), end = eventList[theEvent].end();

    // Continue until we find the xClient.
    // Since each xClient may only be registered for any given
    // we may return as soon as we find the client.
    while (ptr != end) {
        // Is this the one?
        if ((*ptr) == theClient) {
            // Yup, remove it and return true
            eventList[theEvent].erase(ptr);
            return true;
        }
        ++ptr;
    }

    // Unable to find the client in the list of registered
    // listeners for this event. *shrug*
    return false;
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

    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        if (*ptr == theClient) {
            listPtr->erase(ptr);

            // Are there any listeners remaining for this channel?
            if (listPtr->empty()) {
                // Nope, remove the listener list from the
                // channelEventMap and deallocat the list.
                channelEventMap.erase(chanName);
                delete listPtr;
            }

            return true;
        }
    }

    // Unable to find the key/xClient pair.
    return false;
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
        elog << "xServer::PostEvent> Invalid event number: " << theEvent << endl;
        return;
    }

    // Iterate through the list of listeners for this event.
    list<xClient*>::iterator ptr = eventList[theEvent].begin(), end = eventList[theEvent].end();

    // Continue while there are more listeners for this event.
    for (; ptr != end; ++ptr) {
        // Notify this client of the event
        // if he didnt cause the event to trigger
        if ((*ptr) != excludeMe) {
            (*ptr)->OnEvent(theEvent, Data1, Data2, Data3, Data4);
        }
    }
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
    string channelName = theChan->getName();

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelEvent(theEvent, theChan, Data1, Data2, Data3, Data4);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(channelName);
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    /* 2024-01-05 (by Hidden): It is possible that the channel was destroyed (only one user in the
     * channel was kicked or killed by the service right after the creation of the channel in the
     * above onChannelEvent()). We have to make sure theChan is still valid.
     */
    Channel* theChan2 = Network->findChannel(channelName.c_str());
    if (!theChan2) {
        channelEventMap.erase(chanPtr);
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelEvent(theEvent, theChan, Data1, Data2, Data3, Data4);
    }
}

// srcClient may be NULL, when the source is a server
void xServer::PostChannelKick(Channel* theChan, iClient* srcClient, iClient* destClient,
                              const string& kickMessage, bool authoritative) {
    // Public method, verify arguments
    assert(theChan != 0);
    assert(destClient != 0);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnNetworkKick(theChan, srcClient, destClient, kickMessage, authoritative);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnNetworkKick(theChan, srcClient, destClient, kickMessage, authoritative);
    }
}

bool xServer::PostSignal(int whichSig) {
    // First, notify the server signal handler
    bool handledSignal = OnSignal(whichSig);

    // Pass this signal on to each xClient.
    xNetwork::localClientIterator ptr = Network->localClient_begin();
    for (; ptr != Network->localClient_end(); ++ptr) {
        //	if( NULL == *ptr )
        //		{
        //		continue ;
        //		}
        ptr->second->OnSignal(whichSig);
    }

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

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelMode(theChan, sourceUser, modeVector);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelMode(theChan, sourceUser, modeVector);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeL(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             unsigned int limit) {
    theChan->onModeL(polarity, limit);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeL(theChan, polarity, sourceUser, limit);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeL(theChan, polarity, sourceUser, limit);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeK(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& key) {
    theChan->onModeK(polarity, key);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeK(theChan, polarity, sourceUser, key);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeK(theChan, polarity, sourceUser, key);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeA(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Apass) {
    theChan->onModeA(polarity, Apass);

    // First delivery this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeA(theChan, polarity, sourceUser, Apass);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners fort his channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeA(theChan, polarity, sourceUser, Apass);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeU(Channel* theChan, bool polarity, ChannelUser* sourceUser,
                             const string& Upass) {
    theChan->onModeU(polarity, Upass);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeU(theChan, polarity, sourceUser, Upass);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listenersfor this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeU(theChan, polarity, sourceUser, Upass);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occurs
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeO(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::opVectorType& opVector) {
    theChan->onModeO(opVector);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeO(theChan, sourceUser, opVector);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeO(theChan, sourceUser, opVector);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeV(Channel* theChan, ChannelUser* sourceUser,
                             const xServer::voiceVectorType& voiceVector) {
    theChan->onModeV(voiceVector);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeV(theChan, sourceUser, voiceVector);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeV(theChan, sourceUser, voiceVector);
    }
}

// Handle a channel mode change
// theChan is the channel on which the mode change occured
// polarity is true if the mode is being set, false otherwise
// sourceUser is the source of the mode change; this variable
// may be NULL if a server is setting the mode
void xServer::OnChannelModeB(Channel* theChan, ChannelUser* sourceUser,
                             xServer::banVectorType& banVector) {

    // Channel::onModeB() may modify banVector with the extra bans
    // that have been removed due to overlaps
    theChan->onModeB(banVector);

    // First deliver this channel event to any listeners for all channel
    // events.
    channelEventMapType::iterator allChanPtr = channelEventMap.find(CHANNEL_ALL);
    if (allChanPtr != channelEventMap.end()) {
        for (list<xClient*>::iterator ptr = allChanPtr->second->begin(),
                                      endPtr = allChanPtr->second->end();
             ptr != endPtr; ++ptr) {
            (*ptr)->OnChannelModeB(theChan, sourceUser, banVector);
        }
    }

    // Find listeners for this specific channel
    channelEventMapType::iterator chanPtr = channelEventMap.find(theChan->getName());
    if (chanPtr == channelEventMap.end()) {
        // No listeners for this channel's events
        return;
    }

    // Iterate through the listeners for this channel's events
    // and notify each listener of the event
    list<xClient*>* listPtr = chanPtr->second;
    for (list<xClient*>::iterator ptr = listPtr->begin(), end = listPtr->end(); ptr != end; ++ptr) {
        (*ptr)->OnChannelModeB(theChan, sourceUser, banVector);
    }
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
                                std::string_view where) {
    std::vector<Channel::ModeProblem> problems;

    modeVectorType modeVector;
    opVectorType opVector;
    voiceVectorType voiceVector;
    banVectorType banVector;

    for (const Channel::ModeChange& change : changes) {
        const Channel::ModeInfo& mode = change.mode;

        switch (mode.kind) {
        case Channel::ModeKind::Flag:
            modeVector.emplace_back(change.set, mode.flag);
            break;

        case Channel::ModeKind::Limit:
            // parseModes() has validated the argument; -l carries none
            OnChannelModeL(theChan, change.set, sourceUser,
                           parseNumber<unsigned int>(change.arg).value_or(0));
            break;

        case Channel::ModeKind::Key:
            OnChannelModeK(theChan, change.set, sourceUser, change.arg);
            break;

        case Channel::ModeKind::Password:
            if ('A' == mode.letter) {
                OnChannelModeA(theChan, change.set, sourceUser, change.arg);
            } else {
                OnChannelModeU(theChan, change.set, sourceUser, change.arg);
            }
            break;

        case Channel::ModeKind::Member: {
            // With oplevels the target is "<numeric>:<level>"; the level
            // is not tracked.
            const std::string numeric = change.arg.substr(0, change.arg.find(':'));
            iClient* target = Network->findClient(numeric);
            ChannelUser* member = (target != 0) ? theChan->findUser(target) : 0;
            if (0 == member) {
                problems.push_back({Channel::ModeError::InvalidTarget, mode.letter, change.arg});
                continue;
            }
            ('o' == mode.letter ? opVector : voiceVector).emplace_back(change.set, member);
            break;
        }

        case Channel::ModeKind::Ban:
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
        OnChannelModeB(theChan, sourceUser, banVector);
    }

    // What could not be applied is ours to report: the caller has nothing
    // to do about it.
    logModeProblems(where, theChan->getName(), problems);
}

void xServer::ApplyChannelModes(Channel* theChan, ChannelUser* sourceUser,
                                const Channel::ParsedModes& parsed, std::string_view where) {
    // A line from the network is applied as far as it is valid
    logModeProblems(where, theChan->getName(), parsed.problems);
    ApplyChannelModes(theChan, sourceUser, parsed.changes, where);
}

void xServer::logModeProblems(std::string_view where, std::string_view channelName,
                              std::span<const Channel::ModeProblem> problems) const {
    for (const Channel::ModeProblem& problem : problems) {
        elog << where << " (" << channelName << "): " << Channel::describe(problem.error);
        if (problem.letter != 0) {
            elog << " for mode '" << problem.letter << "'";
        }
        if (!problem.detail.empty()) {
            elog << ": " << problem.detail;
        }
        elog << endl;
    }
}

} // namespace gnuworld
