/**
 * ServerCommandHandler.h
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
 * "$Id: ServerCommandHandler.h,v 1.3 2003/06/28 01:21:18 dan_karrels Exp $"
 */

#ifndef __SERVERCOMMANDHANDLER_H
#define __SERVERCOMMANDHANDLER_H                                                                   \
    "$Id: ServerCommandHandler.h,v 1.3 2003/06/28 01:21:18 dan_karrels Exp $"

#include "xparameters.h"
#include "ELog.h"
#include "ChannelUser.h"

namespace gnuworld {

class xServer;

class ServerCommandHandler {
  protected:
    xServer* theServer;

    /*
     * The handlers are the IRC parser: what the network says has happened,
     * they record.  The classes that keep their state from the modules name
     * this class as a friend, and since C++ does not pass friendship on to
     * a derived class, the handlers reach that state through these.
     */

    /// Set a mode of a member, such as the op of whoever creates a channel.
    static void setMemberMode(ChannelUser* member, ChannelUser::modeType whichMode) {
        member->setMode(whichMode);
    }

  public:
    ServerCommandHandler(xServer* _theServer) : theServer(_theServer) {}
    virtual ~ServerCommandHandler() {}

    virtual bool Execute(const xParameters&) = 0;
};

#define CREATE_HANDLER(name)                                                                       \
    class name : public ServerCommandHandler {                                                     \
      public:                                                                                      \
        name(xServer* theServer) : ServerCommandHandler(theServer) {}                              \
        virtual ~name() {}                                                                         \
                                                                                                   \
        virtual bool Execute(const xParameters&);                                                  \
    };                                                                                             \
                                                                                                   \
    extern "C" {                                                                                   \
    name* _gnuwinit_##name(xServer* theServer) { return new name(theServer); }                     \
    }

#define CREATE_LOADER(name)                                                                        \
    extern "C" {                                                                                   \
    name* _gnuwinit_##name(xServer* theServer) { return new name(theServer); }                     \
    }

} // namespace gnuworld

#endif // __SERVERCOMMANDHANDLER_H
