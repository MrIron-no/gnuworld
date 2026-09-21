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

#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>

#include "xparameters.h"
#include "ELog.h"
#include "ChannelUser.h"
#include "Channel.h"

namespace gnuworld {

class xServer;

class ServerCommandHandler {
  protected:
    xServer* theServer;

    /// "msg_GL>": what a protocol error of this handler is reported under
    const char* const where;

    /*
     * What a handler asks of the line it was given.  The uplink writes these
     * itself, so one that is not what it should be is a protocol error: see
     * xServer::ProtocolError(), which does not return.  Text the uplink only
     * passes on for somebody else is not for these: parseNumber() it, and
     * cope.  Defined at the end of server.h, where xServer is complete.
     */

    /// The line cannot be parsed: say what is wrong with it, and abort
    [[noreturn]] void protocolError(std::span<const std::string> problems) const;

    /// At least as many parameters as the shortest form of the command has,
    /// the source counted.  A legal form of no use to us is not an error.
    void requireParameters(const xParameters& line, xParameters::size_type minimum) const;

    /// A number, such as a count or an id
    std::uint64_t requireNumber(std::string_view text) const;

    /// A time.  ircu writes them unsigned.
    time_t requireTimestamp(std::string_view text) const;

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

    // The same for the channel: each is the method of Channel it is named
    // after, with the channel in front.
    static bool addUser(Channel* theChan, ChannelUser* newUser) {
        return theChan->addUser(newUser);
    }
    static ChannelUser* removeUser(Channel* theChan, iClient* theClient) {
        return theChan->removeUser(theClient);
    }
    static bool revealUser(Channel* theChan, const iClient* theClient) {
        return theChan->revealUser(theClient);
    }
    static void removeAllModes(Channel* theChan) { theChan->removeAllModes(); }
    static void removeAllBans(Channel* theChan) { theChan->removeAllBans(); }
    static void setCreationTime(Channel* theChan, time_t newCT) { theChan->setCreationTime(newCT); }
    static void setTopic(Channel* theChan, const std::string& topic) { theChan->setTopic(topic); }
    static void setTopicWhoSet(Channel* theChan, const std::string& who) {
        theChan->setTopicWhoSet(who);
    }
    static void setTopicTS(Channel* theChan, time_t when) { theChan->setTopicTS(when); }

    /// The same for the server: xServer::destroy(), for a network object this
    /// handler has taken out of the network tables.  Defined at the end of
    /// server.h, where xServer is complete.
    template <typename T> void destroy(T* what) const;

  public:
    ServerCommandHandler(xServer* _theServer, const char* _where)
        : theServer(_theServer), where(_where) {}
    virtual ~ServerCommandHandler() {}

    virtual bool Execute(const xParameters&) = 0;
};

#define CREATE_HANDLER(name)                                                                       \
    class name : public ServerCommandHandler {                                                     \
      public:                                                                                      \
        name(xServer* theServer) : ServerCommandHandler(theServer, #name ">") {}                   \
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
