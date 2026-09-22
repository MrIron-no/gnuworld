/**
 * gnutest.h
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
 * $Id: gnutest.h,v 1.14 2005/01/17 23:09:54 dan_karrels Exp $
 */

#ifndef __GNUTEST_H
#define __GNUTEST_H "$Id: gnutest.h,v 1.14 2005/01/17 23:09:54 dan_karrels Exp $"

#include <string>
#include <string_view>
#include <vector>

#include "client.h"
#include "iClient.h"
#include "Channel.h"
#include "StringTokenizer.h"
#include "logger.h"

/* Every file of this module logs under the module's name, which is what the LOG
 * and LOG_MSG macros resolve; it has to stand outside every namespace. */
GNUWORLD_MODULE_LOGGER("gnutest");

namespace gnuworld {

/**
 * The purpose of this class is to test new features added to
 * the gnuworld server core.
 */
class gnutest : public xClient {

  public:
    /**
     * Constructor receives name of config file.
     */
    gnutest(const std::string&);

    /**
     * Destructor closes all streams and deallocates any memory
     * created by this instance.
     */
    virtual ~gnutest();

    /**
     * This method is invoked when this module is first loaded.
     * This is a good place to setup timers, connect to DB, etc.
     * At this point, the server may not yet be connected to the
     * network, so please do not issue join/nick requests.
     */
    virtual void OnAttach() override;

    /**
     * This method is called when this module is being unloaded from
     * the server.  This is a good place to cleanup, including
     * deallocating timers, closing connections, closing log files,
     * and deallocating private data stored in iClients.
     */
    virtual void OnDetach(const std::string& = std::string("Server Shutdown")) override;

    /**
     * This method is called when the server connects to the network.
     * Note that if this module is attached while already connected
     * to a network, this method is still invoked.
     */
    virtual void OnConnect() override;

    /**
     * This method is invoked when the server disconnects from
     * its uplink.
     */
    virtual void OnDisconnect() override;

    /*
     * One override per event core posts, network events first and then the
     * channel ones, each reporting what it was handed under the name the
     * harness knows that event by.  EVT_JUPE and EVT_UNJUPE have no method of
     * their own because nothing posts either.
     */

    virtual void OnOper(iClient* theClient) override;
    virtual void OnNetBreak(iServer* theServer, const iServer* uplink,
                            std::string_view reason) override;
    virtual void OnNetJoin(iServer* theServer, const iServer* uplink) override;
    virtual void OnBurstComplete(iServer* theServer) override;
    virtual void OnBurstAck(iServer* theServer) override;
    virtual void OnEndOfBurstAckSent(iServer* theServer) override;
    virtual void OnGline(Gline* theGline) override;
    virtual void OnRemGline(Gline* theGline) override;
    virtual void OnQuit(iClient* theClient, std::string_view reason) override;
    virtual void OnKill(const NetworkTarget* source, iClient* theClient,
                        std::string_view reason) override;
    virtual void OnNick(iClient* theClient) override;
    virtual void OnNickChange(iClient* theClient, std::string_view oldNick) override;
    virtual void OnAccount(iClient* theClient) override;
    virtual void OnAccountFlags(iClient* theClient) override;
    virtual void OnRaw(std::string_view line) override;
    virtual void OnXQuery(iServer* theServer, std::string_view routing,
                          std::string_view message) override;
    virtual void OnXReply(iServer* theServer, std::string_view routing,
                          std::string_view message) override;
    virtual void OnNetConf(iServer* theServer, std::string_view key) override;
    virtual void OnRemNetConf(iServer* theServer, std::string_view key) override;

    virtual void OnJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser,
                        JoinKind kind) override;
    virtual void OnPart(Channel* theChan, iClient* theClient, std::string_view message) override;
    virtual void OnTopic(Channel* theChan, iClient* theClient, std::string_view topic) override;
    virtual void OnServerMode(Channel* theChan, iServer* theServer) override;

    /*
     * A kick and a channel mode change are the notifications core has never
     * sent as an event: they go to a method of their own, and so "events on"
     * could not see them.  These report them the same way, so that what a
     * module is told about them is pinned from the outside too.
     */

    virtual void OnNetworkKick(Channel*, iClient* srcClient, iClient* destClient,
                               const std::string& kickMessage, bool authoritative) override;

    virtual void OnChannelMode(Channel*, ChannelUser*, const xServer::modeVectorType&) override;
    virtual void OnChannelModeL(Channel*, bool polarity, ChannelUser*,
                                const unsigned int&) override;
    virtual void OnChannelModeK(Channel*, bool polarity, ChannelUser*, const std::string&) override;
    virtual void OnChannelModeA(Channel*, bool polarity, ChannelUser*, const std::string&) override;
    virtual void OnChannelModeU(Channel*, bool polarity, ChannelUser*, const std::string&) override;
    virtual void OnChannelModeO(Channel*, ChannelUser*, const xServer::opVectorType&) override;
    virtual void OnChannelModeV(Channel*, ChannelUser*, const xServer::voiceVectorType&) override;
    virtual void OnChannelModeB(Channel*, ChannelUser*, const xServer::banVectorType&) override;

    /**
     * This method is called for the client to burst all channels
     * once the server connects to the network.
     */
    virtual void BurstChannels() override;

    /**
     * This method is called when a network client messages
     * this client.
     */
    virtual void OnPrivateMessage(iClient*, const std::string&, bool secure = false) override;

    /**
     * This method is called when a channel message occurs
     * in a channel in which an xClient resides, and the
     * xClient is user mode -d.
     */
    virtual void OnChannelMessage(iClient* Sender, Channel* theChan,
                                  const std::string& Message) override;

    /**
     * This method is invoked when a fake client belonging to this
     * xClient receives a channel message.
     */
    virtual void OnFakeChannelMessage(iClient* srcClient, iClient* destClient, Channel* theChan,
                                      const std::string& message) override;

    /**
     * This method is invoked when a fake client belonging to this
     * xClient receives a channel notice.
     */
    virtual void OnFakeChannelNotice(iClient* srcClient, iClient* destClient, Channel* theChan,
                                     const std::string& message) override;

    /**
     * This method is called when a network message arrives for
     * one of the fake clients owned by this xClient.
     */
    virtual void OnFakePrivateMessage(iClient* srcClient, iClient* destClient,
                                      const std::string& message, bool secure = false) override;

    /**
     * This method is called when a network notice arrives for
     * one of the fake clients owned by this xClient.
     */
    virtual void OnFakePrivateNotice(iClient* srcClient, iClient* destClient,
                                     const std::string& message, bool secure = false) override;

    /**
     * Invoked when a fake client of this xClient receives a
     * channel CTCP.
     */
    virtual void OnFakeChannelCTCP(iClient* srcClient, iClient* fakeClient, Channel* theChan,
                                   const std::string& command, const std::string& message) override;

    /**
     * Invoked when a fake client of this xClient receives a
     * channel CTCP.
     */
    virtual void OnFakeCTCP(iClient* srcClient, iClient* fakeClient, const std::string& command,
                            const std::string& message, bool secure = false) override;

    /**
     * This method is called when a timer expires.
     */
    virtual void OnTimer(const xServer::timerID&, void*) override;

  protected:
    /**
     * Spawn a fake client.
     */
    virtual void spawnClient(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Spawn a fake server.
     */
    virtual void spawnServer(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Remove a fake client.
     */
    virtual void removeClient(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Remove a fake server.
     */
    virtual void removeServer(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Request that a spawned (fake) client join a channel.
     */
    virtual void spawnJoin(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Request that a spawned (fake) client part a channel.
     */
    virtual void spawnPart(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Report information about a channel.
     */
    /**
     * Handle the commands that change a channel.  `fake`, if set, is the
     * fake client of ours that makes the change; null for ourselves.
     * Returns false if st[0] is not such a command.
     */
    virtual bool channelCommand(iClient* requestingClient, const StringTokenizer& st,
                                const iClient* fake);

    /**
     * The test-only event commands: "events on|off" reports every event we
     * receive, "onevent <NAME> <action> [args]" arms one action to run from
     * inside the handler the next time that event arrives.  Arming again adds
     * an action rather than replacing the one armed, so a chain of handlers can
     * be driven deeper than one hop per instance.
     * Returns false if st[0] is not one of these.
     */
    virtual bool eventCommand(iClient* requestingClient, const StringTokenizer& st);

    /**
     * Report one event and then run what "onevent" armed for it, which is what
     * every event handler above comes down to.  args are the printable
     * identities of what that event was handed, aboutClient the client it is
     * about and theChan the channel; either of those two may be null.
     */
    virtual void eventArrived(int whichEvent, bool channelEvent,
                              const std::vector<std::string>& args, iClient* aboutClient = nullptr,
                              Channel* theChan = nullptr);

    /**
     * A join, a burst join and a create are one membership arriving under
     * three names: report it and op the arriving client if it is an oper and
     * this is the channel our config names.
     */
    virtual void membership(int whichEvent, Channel* theChan, iClient* theClient,
                            ChannelUser* theUser);

    /**
     * Send one "EVENT <NAME> <arg1> <arg2> ..." notice to whoever turned
     * reporting on, naming the printable identity of each payload.
     */
    virtual void reportEvent(const std::string& name, const std::vector<std::string>& args);

    /**
     * Run the first action "onevent" armed for this event, and take it off the
     * list.  aboutClient is the client the event is about and theChan the
     * channel; either may be null.
     */
    virtual void runArmedAction(int whichEvent, bool channelEvent, iClient* aboutClient,
                                Channel* theChan);

    virtual void chanInfo(const Channel* theChan);

    /**
     * This is the name of the operator only channel on which this
     * client sits.
     */
    std::string operChan;

    /**
     * The numnick "events on" named, empty while reporting is off.  A numnick
     * and not an iClient*, which the next event may be about to delete.
     */
    std::string eventWatcher;

    /**
     * What "onevent" armed, in the order it was armed: each entry runs once,
     * for the first of its event that arrives.
     */
    struct armedEvent {
        int whichEvent;
        bool channelEvent;
        std::string action;
        std::string argument;
    };
    std::vector<armedEvent> armed;

    /// "<#channel> <timestamp> [<modes> [<args>]]" to BurstChannel() during
    /// our burst; empty for none.
    std::string burstChannel;

    /**
     * I have no idea what this is.
     */
    std::string timerChan;

    /**
     * This type is used for the helpTable, which will store basic
     * information about what commands gnutest supports.
     */
    typedef std::map<std::string, std::string> helpTableType;

    /**
     * This is used to store basic information about what commands
     * gnutest supports.
     */
    helpTableType helpTable;
};

} // namespace gnuworld

#endif // __GNUTEST_H
