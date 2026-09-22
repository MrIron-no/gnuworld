/**
 * server_glines.cc
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
 * $Id: server_glines.cc,v 1.3 2007/12/14 03:06:10 isomer Exp $
 */

#include <new>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include "server.h"
#include "Network.h"
#include "Gline.h"
#include "ELog.h"

namespace gnuworld {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

bool xServer::removeGline(const string& userHost, const xClient* remClient) {
    // The gline being removed, if we have it.  The post below runs module code
    // which may erase from glineList, so nothing here may hold an iterator
    // into it across that.
    const glineIterator gItr = findGlineIterator(userHost);
    Gline* const theGline = (gItr != glines_end()) ? gItr->second : nullptr;

    // Notify the network that we are removing it
    stringstream s;
    if (theGline != 0) {
        s << getCharYY() << " GL * -" << userHost << " " << theGline->getExpiration() // expiration
          << " " << ::time(0)                                                         // lastmod
          << " " << theGline->getExpiration()                                         // expiration
          << " :" << theGline->getReason();
    } else {
        // Even if we didn't find the gline here, it may be present
        // to someone on the network *shrug*
        s << getCharYY() << " GL * -" << userHost << " " << ::time(0) + 60 // expire
          << " " << ::time(0)                                              // lastmod
          << " " << ::time(0) + 60                                         // lifetime
          << " "
          << ":Unknown G-Line";
    }

    // Write the data to the network output buffer(s)
    Write(s);

    // Did we find the gline in the interal gline structure?
    if (theGline != 0) {
        // Let all clients know that the gline has been removed
        if (remClient) {
            PostEvent(EVT_REMGLINE, static_cast<void*>(theGline), 0, 0, 0, remClient);
        } else {
            PostEvent(EVT_REMGLINE, static_cast<void*>(theGline));
        }

        // Remove the gline from the internal gline structure, and deallocate
        // it - unless a handler of the event above removed it first, in which
        // case it is already on its way out and this is nothing
        destroy(takeGline(theGline));
    }

    // Return success
    return theGline != 0;
}

// C GL * +~*@209.9.117.131 180 :Banned (~*@209.9.117.131) until 957235403 (On Mon May  1
// 22:40:23 2000 GMT from SE5 for 180 seconds: remgline test.. 	[0])
bool xServer::setGline(const string& setBy, const string& userHost, const string& reason,
                       const time_t& duration, const time_t& lastmod, const xClient* setClient,
                       const string& server) {
    // Remove any old matches
    {
        xServer::glineIterator gItr = findGlineIterator(userHost);
        if (gItr != glines_end()) {
            // This gline is already present
            destroy(gItr->second);
            eraseGline(gItr);
        }
    }

    Gline* newGline = new (std::nothrow) Gline(setBy, userHost, reason, duration, lastmod);
    assert(newGline != 0);

    // Notify the rest of the network
    stringstream s;
    s << getCharYY() << " GL " << server << " !+" << userHost << ' ' << duration << ' ' << lastmod
      << " :" << reason;
    Write(s);

    glineList.insert(glineListType::value_type(newGline->getUserHost(), newGline));
    if (setClient) {
        PostEvent(EVT_GLINE, static_cast<void*>(newGline), 0, 0, 0, setClient);
    } else {
        PostEvent(EVT_GLINE, static_cast<void*>(newGline));
    }

    return true;
}

vector<const Gline*> xServer::matchGline(const string& userHost) const {
    vector<const Gline*> retMe;

    for (const_glineIterator ptr = glines_begin(); ptr != glines_end(); ++ptr) {
        if (!match(ptr->second->getUserHost(), userHost)) {
            retMe.push_back(ptr->second);
        }
    }

    return retMe;
}

const Gline* xServer::findGline(const string& userHost) const {
    const_glineIterator gItr = glineList.find(userHost);
    if (gItr == glineList.end()) {
        return 0;
    }
    return gItr->second;
}

xServer::glineIterator xServer::findGlineIterator(const string& userHost) {
    return glineList.find(userHost);
}

Gline* xServer::takeGline(Gline* theGline) {
    assert(theGline != 0);

    const glineIterator gItr = glineList.find(theGline->getUserHost());
    if (gItr == glines_end() || gItr->second != theGline) {
        // Gone, or a different gline of the same user@host
        return nullptr;
    }

    glineList.erase(gItr);
    return theGline;
}

void xServer::addGline(Gline* newGline) {
    assert(newGline != 0);
    glineList.insert(glineListType::value_type(newGline->getUserHost(), newGline));
}

void xServer::sendGlinesToNetwork() {
    time_t now = ::time(0);

    for (const_glineIterator ptr = glines_begin(); ptr != glines_end(); ++ptr) {
        stringstream s;
        s << getCharYY() << " GL * +" << ptr->second->getUserHost() << ' '
          << (ptr->second->getExpiration() - now) << ' ' << ptr->second->getLastmod() << " :"
          << ptr->second->getReason();

        Write(s);
    }
}

void xServer::removeMatchingGlines(const string& wildHost) {
    // Which ones first: a post may run module code that changes glineList,
    // and no iteration of it may span one.
    vector<Gline*> matched;
    for (const_glineIterator ptr = glines_begin(); ptr != glines_end(); ++ptr) {
        // TODO: Does this work with two wildHost's?
        if (!match(wildHost, ptr->second->getUserHost())) {
            matched.push_back(ptr->second);
        }
    }

    for (Gline* theGline : matched) {
        PostEvent(EVT_REMGLINE, static_cast<void*>(theGline));

        // The gline we hold, not its user@host: a handler of the event above
        // may have removed it already, and destroying it twice is a double free
        destroy(takeGline(theGline));
    }
}

void xServer::BurstGlines() {
    xNetwork::localClientIterator ptr = Network->localClient_begin();
    while (ptr != Network->localClient_end()) {
        ptr->second->BurstGlines();
        ++ptr;
    }
}

void xServer::updateGlines() {
    time_t now = ::time(0);

    // Which ones first: a post may run module code that changes glineList,
    // and no iteration of it may span one.
    vector<Gline*> expired;
    for (const_glineIterator ptr = glines_begin(); ptr != glines_end(); ++ptr) {
        if (ptr->second->getExpiration() <= now) {
            expired.push_back(ptr->second);
        }
    }

    for (Gline* theGline : expired) {
        PostEvent(EVT_REMGLINE, static_cast<void*>(theGline));

        // The gline we hold, not its user@host: a handler of the event above
        // may have removed it already, and destroying it twice is a double free
        destroy(takeGline(theGline));
    }
} // updateGlines()

} // namespace gnuworld
