/**
 * TimerHandler.h
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
 * $Id: TimerHandler.h,v 1.6 2004/05/19 19:46:33 jeekay Exp $
 */

#ifndef __TIMERHANDLER_H
#define __TIMERHANDLER_H "$Id: TimerHandler.h,v 1.6 2004/05/19 19:46:33 jeekay Exp $"

namespace gnuworld {

/**
 * This is the base class used in the GNUWorld timer system, and it is
 * nothing but the identity of a timer's owner: a timer runs the callback
 * it was registered with, so there is no method here for it to call.
 * What the owner is for is removeAllTimers(), which cancels every timer
 * whose owner is the one it is given, and so needs one type common to
 * everything that can own a timer -- every xClient, and the server core's
 * own timer handlers, which are not xClients.
 */
class TimerHandler {

  public:
    /**
     * The constructor does nothing.
     */
    TimerHandler() {}

    /**
     * The destructor does nothing.
     */
    virtual ~TimerHandler() {}

    /**
     * The type used to represent timer events.
     */
    typedef unsigned int timerID;
};

} // namespace gnuworld

#endif // __TIMERHANDLER_H
