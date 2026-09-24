/**
 * ServerTimerHandlers.h
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
 * $Id: ServerTimerHandlers.h,v 1.6 2004/05/19 19:46:33 jeekay Exp $
 */

#ifndef __SERVERTIMERHANDLERS_H
#define __SERVERTIMERHANDLERS_H "$Id: ServerTimerHandlers.h,v 1.6 2004/05/19 19:46:33 jeekay Exp $"

#include "TimerHandler.h"

#include <ctime>

namespace gnuworld {

/// Forward declaration of the xServer class
class xServer;

/**
 * This is the abstract base class from which each timer to be used
 * by the GNUWorld server core must derive.  Each server timer
 * must inherit from this class, and define the abstract method
 * schedule(), which books the timer that runs it.  The handler is
 * the owner of that timer, so it is the handler, and never one of
 * the clients, that removeAllTimers() would match the timer to.
 */
class ServerTimerHandler : public TimerHandler {
  public:
    /**
     * The constructor receives a pointer to the server core,
     * and the interval at which to reschedule the timer,
     * if the timer handler chooses to do so.
     */
    ServerTimerHandler(xServer* _theServer, time_t _updateInterval)
        : theServer(_theServer), updateInterval(_updateInterval) {}

    /**
     * This is the virtual destructor, which currently does
     * nothing, but must be present in a class hierarchy.
     */
    virtual ~ServerTimerHandler() {}

    /**
     * Book the timer that runs this handler.  The callback the timer
     * carries does the handler's work, and a handler that runs
     * periodically books its next run from in there by calling this
     * method again.
     */
    virtual void schedule() = 0;

  protected:
    /**
     * This is a pointer to the server core.
     */
    xServer* theServer;

    /**
     * This is the timer rescheduling interval.
     */
    time_t updateInterval;
};

/**
 * This macro is used to create a generic subclass of class
 * ServerTimerHandler, and to declare the schedule() method as
 * concrete.
 */
#define SUBCLASS_SERVERTIMERHANDLER(className)                                                     \
    class className##Timer : public ServerTimerHandler {                                           \
      public:                                                                                      \
        className##Timer(xServer* theServer, time_t updateInterval)                                \
            : ServerTimerHandler(theServer, updateInterval) {}                                     \
        virtual ~className##Timer() {}                                                             \
        virtual void schedule() override;                                                          \
    };

SUBCLASS_SERVERTIMERHANDLER(GlineUpdate)
SUBCLASS_SERVERTIMERHANDLER(PING)

} // namespace gnuworld

#endif // __SERVERTIMERHANDLER_H
