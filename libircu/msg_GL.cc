/**
 * msg_GL.cc
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
 * $Id: msg_GL.cc,v 1.9 2009/08/01 08:32:21 mrbean_ Exp $
 */

#include <new>
#include <iostream>

#include <cassert>

#include "gnuworld_config.h"
#include "ServerCommandHandler.h"
#include "server.h"
#include "events.h"
#include "Gline.h"
#include "ELog.h"
#include "xparameters.h"

namespace gnuworld {
using std::endl;

CREATE_HANDLER(msg_GL)

/**
 * GLine message handler.
 * C GL * +~*@209.9.117.131 180 :Banned (~*@209.9.117.131) until 957235403
 *  (On Mon May 1 22:40:23 2000 GMT from SE5 for 180 seconds: remgline
 *  test.. 	[0])
 */
bool msg_GL::Execute(const xParameters& Params) {
    // <source> GL <target> <+|-><mask> ...: ircu's ms_gline() takes three
    requireParameters(Params, 3);

    if ('-' == Params[2][0]) {
        // Removing a gline

        const xServer::glineIterator gItr = theServer->findGlineIterator(Params[2] + 1);
        if (gItr == theServer->glines_end()) {
            // Unable to find the gline to be removed *shrug*
            return true;
        }

        // The post below runs module code which may erase from the gline list,
        // so the iterator may not outlive it
        Gline* const theGline = gItr->second;

        // Let the modules know that it has been removed
        theServer->PostEvent(EVT_REMGLINE, static_cast<void*>(theGline));

        // Clean up memory: the gline we hold, unless a handler of the event
        // above removed it already, in which case this is nothing
        destroy(theServer->takeGline(theGline));

        return true;
    }

    // Else, adding a gline
    if (Params.size() < 5) {
        // "GL * +<mask> <lastmod>": the activation of a G-line the sender
        // does not have either.  A legal form, but with no expiry and no
        // reason there is nothing here to add.
        return true;
    }

    // <source> GL <target> +<mask> <expire> [<lastmod> [<lifetime>]] :<reason>
    const time_t expires = requireTimestamp(Params[3]);
    const time_t lastmod = (Params.size() > 5) ? requireTimestamp(Params[4]) : 0;

    Gline* newGline = new (std::nothrow)
        Gline(Params[0], Params[2] + 1, Params[Params.size() - 1], expires, lastmod);
    assert(newGline != 0);

    // Temporary variable
    {
        xServer::glineIterator gItr = theServer->findGlineIterator(newGline->getUserHost());
        if (gItr != theServer->glines_end()) {
            // This gline is already present
            destroy(gItr->second);
            theServer->eraseGline(gItr);
        }
    }

    theServer->addGline(newGline);
    theServer->PostEvent(EVT_GLINE, static_cast<void*>(newGline));

    return true;
}

} // namespace gnuworld
