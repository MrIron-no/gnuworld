/**
 * server.h
 * This is the header file for the server itself.
 * This class originally created by Orlando Bassotto.
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
 * $Id: server.h,v 1.107 2010/08/31 21:16:45 denspike Exp $
 */

#ifndef __SERVER_H
#define __SERVER_H "$Id: server.h,v 1.107 2010/08/31 21:16:45 denspike Exp $"

#include <optional>
#include <format>
#include <span>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <list>
#include <sstream>
#include <map>
#include <queue>
#include <algorithm>

#include <ctime>
#include <cassert>

#include "NetworkTarget.h"
#include "iServer.h"
#include "iClient.h"
#include "Buffer.h"
#include "events.h"
#include "Gline.h"
#include "misc.h"
#include "moduleLoader.h"
#include "TimerHandler.h"
#include "ServerCommandHandler.h"
#include "ConnectionManager.h"
#include "ConnectionHandler.h"
#include "Connection.h"
#include "gnuworld_config.h"

namespace gnuworld {

/// Forward declaration of xClient
class xClient;

/// Forward declaration of Channel
class Channel;

/**
 * This class is the server proper; it is responsible for the connection
 * to the IRC network, and for maintaining the services clients.
 */
class xServer : public ConnectionManager, public ConnectionHandler, public NetworkTarget {
    /**
     * xClient makes its channel changes with the protected changeModes()
     * and its kin below.
     */
    friend class xClient;

  protected:
    /**
     * The type of the structure to hold Gline's internally.
     */
    typedef std::map<std::string, Gline*, noCaseCompare> glineListType;

  public:
    /**
     * The xServer constructor.  It takes parsed configuration
     * parameters from the command line.
     */
    xServer(bool verbose, bool doDebug, bool logSocket, const std::string& elogFileName,
            bool elogFileGiven, const std::string& socketFileName,
            const std::string& configFileName, const std::string& simFileName);

    /**
     * Destroy the server and its clients, disconnect
     * from network.
     */
    virtual ~xServer();

    /**
     * The actual run method.  This method is invoked by main(),
     * and contains the xServer's main loop.
     */
    void run();

    /**
     * This type is used for passing information to handler
     * methods for channel op mode changes.
     */
    typedef std::vector<std::pair<bool, ChannelUser*>> opVectorType;

    /**
     * This type is used for passing information to handler
     * methods for channel voice mode changes.
     */
    typedef opVectorType voiceVectorType;

    /**
     * This type is used for passing information to handler
     * methods for channel ban changes.
     */
    typedef std::vector<std::pair<bool, std::string>> banVectorType;

    /**
     * This type is used for passing multiple argument-less modes
     * to handler methods of simple channel modes.
     */
    typedef std::vector<std::pair<bool, Channel::modeType>> modeVectorType;

    /**
     * The iterator type used to iterate through the
     * structure of glines.
     */
    typedef glineListType::iterator glineIterator;

    /**
     * The const iterator type used to iterate through the
     * structure of glines.
     */
    typedef glineListType::const_iterator const_glineIterator;

    /**
     * Return a const iterator to the beginning of the gline
     * structure.
     */
    inline const_glineIterator glines_begin() const { return glineList.begin(); }

    /**
     * Return a const iterator to the end of the gline
     * structure.
     */
    inline const_glineIterator glines_end() const { return glineList.end(); }

    /**
     * Return an iterator to the beginning of the gline structure.
     */
    inline glineIterator glines_begin() { return glineList.begin(); }

    /**
     * Return an iterator to the end of the gline structure.
     */
    inline glineIterator glines_end() { return glineList.end(); }

    /**
     * This method is called when a Connection is disconnected.
     * Inherited from ConnectionHandler.
     */
    virtual void OnDisconnect(Connection*) override;

    /**
     * This method is called when a Connection attempt succeeds.
     * Inherited from ConnectionHandler.
     */
    virtual void OnConnect(Connection*) override;

    /**
     * This method is called when a Connection attempt fails.
     * Inherited from ConnectionHandler.
     */
    virtual void OnConnectFail(Connection*) override;

    /**
     * This method is called when a line of data is read from
     * Connection.
     * Inherited from ConnectionHandler.
     */
    virtual void OnRead(Connection*, const std::string&) override;

    /**
     * Request that all data in the output buffer be flushed to
     * the network connection.  This will possibly block the
     * the server in the network send, so be careful about using
     * it.
     * The flush request is valid only for the next call to
     * the main process loop, it is reset after that.
     */
    virtual void FlushData();

    /**
     * Attach a fake server to this services server.
     * Use this method to jupe a server, just set the iServer's
     * JUPE flag.
     */
    virtual bool AttachServer(iServer*, xClient*);

    /**
     * Detach a fake server from the services server.
     */
    virtual bool DetachServer(iServer*);

    /**
     * Squit a server from the network and remove it
     * from the network tables.
     */
    virtual bool SquitServer(const std::string& name, const std::string& reason);

    /**
     * Append a std::string to the output buffer.
     */
    virtual bool Write(const std::string&);

    /**
     * Append a message with IRCv3 message-tags.
     */
    virtual bool Write(const xParameters::tagListType& tags, const std::string& line);
    virtual bool Write(const xParameters::tagListType& tags, const std::stringstream& line);
    template <typename... Args>
    bool Write(const xParameters::tagListType& tags, CheckedFormat<Args...> fmt, Args&&... args) {
        return Write(tags, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Append a message with an IRCv3 @time tag (server-time)
     * set to the current UTC timestamp.
     */
    virtual bool WriteWithTime(const std::string& line);
    virtual bool WriteWithTime(const std::stringstream& line);
    template <typename... Args> bool WriteWithTime(CheckedFormat<Args...> fmt, Args&&... args) {
        return WriteWithTime(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Similar to the above signature of Write() except that data
     * will be written to the normal output buffer even during
     * burst time.
     */
    virtual bool WriteDuringBurst(const std::string&);

    /**
     * Append a C variable argument list/character array to the output
     * buffer.
     * Since this method uses a variable argument list, this
     * method cannot support a final default argument -- this method
     * defaults to NOT writing during burst.
     */
    template <typename... Args> bool Write(CheckedFormat<Args...> fmt, Args&&... args) {
        return Write(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * This method is similar to the above Write(), except
     * that the data will be written to the normal output
     * buffer even during burst time.
     */
    template <typename... Args> bool WriteDuringBurst(CheckedFormat<Args...> fmt, Args&&... args) {
        return WriteDuringBurst(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Append a std::stringstream to the output buffer.
     * The second argument determines if data should be written
     * during burst time.
     */
    virtual bool Write(const std::stringstream&);

    /**
     * This method is similar to the above Write(), except
     * that the data will be written to the normal output
     * buffer even during burst time.
     */
    virtual bool WriteDuringBurst(const std::stringstream&);

    /**
     * Process is responsible for parsing lines of data.
     */
    virtual void Process(char* String);

    /**
     * Add a network gline and update glines table.
     */
    virtual bool setGline(const std::string& setBy, const std::string& userHost,
                          const std::string& reason, const time_t& duration,
                          const time_t& lastmod = ::time(0), const xClient* setClient = NULL,
                          const std::string& server = "*");

    /**
     * Remove a network gline and update internal gline table.
     */
    virtual bool removeGline(const std::string& userHost, const xClient* remClient = NULL);

    /**
     * Erase a gline from the internal data structures.  This does
     * NOT send a message to the network; for that functionality,
     * use RemoveGline() instead.
     * The caller of this method must also be sure to deallocate
     * the internal Gline associated with the iterator.
     */
    virtual void eraseGline(glineIterator removeMe) { glineList.erase(removeMe); }

    /**
     * Add a gline to the internal data structures.  This does
     * NOT send a message to the network; for that functionality,
     * use SetGline() instead.
     */
    virtual void addGline(Gline* newGline);

    /**
     * Find a gline by lexical searching, case insensitive.
     */
    virtual const Gline* findGline(const std::string& userHost) const;

    /**
     * Find a gline by userHost (exact match only), and return
     * an interator to that gline.
     */
    virtual glineIterator findGlineIterator(const std::string& userHost);

    /**
     * Find one or more glines matching a given userHost string.
     */
    virtual std::vector<const Gline*> matchGline(const std::string& userHost) const;

    /**
     * Send all glines to the network.
     */
    virtual void sendGlinesToNetwork();

    /* Client stuff */

    /**
     * Attach a fake client to a this or a fake (juped) server.
     * The server must exist and must already be attached
     * to this server.  Otherwise, if attaching to the current
     * server, it already exists :)
     * All integrity of the iClient will be verified: non-empty
     * nick/user/hostname, etc.  Also, if the nickname
     * is already in use on the network, then false will be
     * returned.
     * The client's intXXX/charXXX will be set by this method.
     * The xClient* is the owner to whom messages will be sent.
     */
    virtual bool AttachClient(iClient* Client, xClient*);

    /**
     * Quit a hosted client from the network with the given
     * quit message.
     */
    virtual bool DetachClient(iClient*, const std::string& = std::string("Exiting, moo"));

    /**
     * Attempt to load a client given its client module name.
     */
    virtual void LoadClient(const std::string& moduleName, const std::string& configFileName);

    /**
     * Attempt to unload a client given its module name.
     * Be sure that you have the proper fully qualified
     * moduleName.  If uncertain, use the other form of
     * the UnloadClient() method.
     */
    virtual void UnloadClient(const std::string& moduleName, const std::string& reason);

    /**
     * Attempt to unload a client given its pointer.
     */
    virtual void UnloadClient(xClient*, const std::string& reason);

    /**
     * Attach a client to the server.  This will add the client
     * to the internal table, and call the client's ImplementServer()
     * method.
     * Clients must *not* call this method, use LoadClient()
     * instead.
     */
    virtual bool AttachClient(xClient* Client, bool doBurst = false);

    /**
     * Attach a client to the server.  This will add the client
     * to the internal table, and call the client's ImplementServer()
     * method.
     * Locate the client by its module name.
     * Clients must *not* call this method, use LoadClient()
     * instead.
     */
    virtual bool AttachClient(const std::string& moduleName, const std::string& configFileName,
                              bool doBurst = false);

    /**
     * Detach a client from the server.  This will call the
     * client's Exit() method and remove the client from the
     * internal tables.
     * Clients must *not* call this method, use UnloadClient()
     * instead.
     */
    virtual bool DetachClient(const std::string& moduleName, const std::string& reason);

    /**
     * Detach a client from the server.  This will call the
     * client's Exit() method and remove the client from the
     * internal tables.
     * Clients must *not* call this method, use UnloadClient()
     * instead.
     */
    virtual bool DetachClient(xClient* Client, const std::string& reason);

    /**
     * Output the information for a channel, and make the given
     * xClient operator in that channel.
     * This works at all times, bursting or not.
     */
    virtual bool JoinChannel(xClient*, const std::string& chanName,
                             const std::string& chanModes = "+tn", const time_t& joinTime = 0,
                             bool getOps = true);

    /**
     * Similar to JoinChannel, except that the server will just
     * burst the channel, without joining a client.
     * - The channel must already exist
     * - The burst time must be older than the existing channel's
     *   creation time.  All modes will be removed from the channel
     *   without generating any events.
     * - chanModes cannot contain any '-' polarity modes.
     */
    virtual bool BurstChannel(const std::string& chanName, const std::string& chanModes,
                              const time_t& burstTime);

    /**
     * Have a fake (only) client join a channel.
     * This will NOT create the channel if it does not already exist.
     */
    virtual bool JoinChannel(iClient*, const std::string& chanName);

    /**
     * Notify the network that one of the services clients has
     * parted a channel.
     */
    virtual void PartChannel(xClient* theClient, const std::string& chanName,
                             const std::string& reason = std::string());

    /**
     * Notify the network that a fake client has parted a channel.
     */
    virtual void PartChannel(iClient* theClient, const std::string& chanName,
                             const std::string& reason = std::string());

    /**
     * Notify the network that one of the services clients has
     * parted a channel.
     */
    virtual void PartChannel(xClient* theClient, Channel* theChan,
                             const std::string& reason = std::string());

    /**
     * Handle the parting of a services client from a channel.  This
     * method updates internal tables.
     */
    virtual void OnPartChannel(xClient* theClient, const std::string& chanName);

    /**
     * Handle the parting of a services client from a channel.  This
     * method updates internal tables.
     */
    virtual void OnPartChannel(xClient* theClient, Channel* theChan);

    /**
     * Handle the parting of a network client from a channel.  This method
     * updates internal tables.
     */
    virtual void OnPartChannel(iClient* theClient, const std::string& chanName);

    /**
     * Handle the parting of a network client from a channel.  This method
     * updates internal tables.
     */
    virtual void OnPartChannel(iClient* theClient, Channel* theChan);

    /**
     * OnXQuery is called when an XQ command
     * is received.
     */
    virtual void OnXQuery(iServer* Sender, const std::string& Token, const std::string& Message);

    /**
     * OnXReply is called when an XR command
     * is received.
     */
    virtual void OnXReply(iServer* Sender, const std::string& Token, const std::string& Message);

    /**
     * Output the information about an xClient to the network.
     * (localClient) is true when the xClient is to appear to
     * reside on this xServer, false when it is to reside on
     * a juped/fake server.
     */
    virtual void BurstClient(xClient*);

    /**
     * Burst a (fake) client to the network.
     */
    virtual void BurstClient(iClient*);

    /**
     * Burst a (fake) server to the network.
     */
    virtual void BurstServer(iServer*);

    /**
     * Send a wallops to the network as the server.
     */
    virtual int Wallops(const std::string&);

    /**
     * Sent a notice to a client as the server.
     */
    virtual bool Notice(iClient*, const std::string&);
    template <typename... Args>
    bool Notice(iClient* theClient, CheckedFormat<Args...> fmt, Args&&... args) {
        return Notice(theClient, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Sent a notice to a channel as the server.
     */
    template <typename... Args>
    bool serverNotice(Channel* theChan, CheckedFormat<Args...> fmt, Args&&... args) {
        return serverNotice(theChan, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    virtual bool serverNotice(Channel*, const std::string&);

    /**
     * A message (PRIVMSG), where Notice() and serverNotice() send a notice,
     * as the server: to a client, and to a channel.
     */
    virtual bool Message(iClient*, const std::string&);
    virtual bool serverMessage(Channel*, const std::string&);

    /**
     * A numeric reply to a client, from this server: what a module answers
     * a WHOIS of one of its clients with.  `text` is what follows the
     * target: "<nick> <user> <host> * :<real name>" for a 311.
     */
    virtual bool SendNumeric(unsigned int numeric, const iClient* to, const std::string& text);

    /**
     * Set a network configuration value (CONFIG, P11): tells the network,
     * and records it as our own msg_CF does for one that arrives.
     */
    virtual bool SetNetConf(const std::string& key, const std::string& value);

    /**
     * New account flags for a client that is logged in.  A P11 uplink is
     * told, with the ACCOUNT the login itself was announced with; a P10
     * uplink has no such update, and the flags are ours alone.
     */
    virtual void UpdateAccountFlags(iClient*, iClient::flagType flags);

    /**
     * True for a client the network holds us responsible for: an xClient,
     * or a fake client, whether it sits on this server or on one a module
     * spawned.  Towards ircu we are the server of all of them.
     */
    virtual bool isOurClient(const iClient*) const;

    /**
     * OPMODE on a client: change its user modes, as the server.  "+o" makes
     * it an oper.  Its own server tells the network, and us, what came of it.
     */
    virtual bool OpMode(iClient* target, const std::string& userModes);

    /**
     * A notice to every user on the network, from the server or from a
     * client of ours: NOTICE $*, which only an oper or a server may send.
     */
    virtual bool GlobalNotice(const std::string& text, const iClient* from = nullptr);

    /**
     * Sending XQuery and XReply as a server to a server.
     */
    virtual bool XQuery(iServer*, const std::string&, const std::string&);
    virtual bool XReply(iServer*, const std::string&, const std::string&);

    /**
     * Set modes as the server, update internal tables, and notify
     * all clients of the mode change(s).
     * The first argument is the xClient requesting the mode
     * change(s).  If this argument is NULL, then the modes are
     * set as the server, otherwise the modes are set as the
     * client.
     * Note that this method is used by xClient::Mode(), but the
     * argument semantics are a bit different because of the addition
     * of the xClient*.
     */
    virtual bool Mode(xClient*, Channel*, const std::string& modes, const std::string& args);

    /*
     * Changing a channel, as a server.
     *
     * Who makes a change is the object the method is called on: these send
     * as the gnuworld server, and the methods of the same name on xClient
     * send as that client.  A fake client is only an iClient and cannot be
     * called on, so it is named by the last argument instead:
     *
     *   MyUplink->Op(theChan, target);              // the gnuworld server
     *   MyUplink->Op(theChan, target, fakeClient);  // a fake client
     *
     * `from` is null for the gnuworld server.  A client has to be on the
     * channel, opped, or the call fails: the network would bounce the
     * change.  The server needs neither.
     *
     * Each of them works out what would really change, tells the network,
     * then updates the channel and notifies the modules.  A call that would
     * change nothing sends nothing and returns true.
     *
     * Every MODE line ends in the channel's creation time, and a server that
     * is told an older one than it has takes it over (ircu, mode_parse()).
     * `olderTimestamp` is for a caller that knows an older one: mod.cservice
     * sets +R on a registered channel with the time the channel was
     * registered with.  If it is older than ours it becomes ours, and the
     * network's with the line; one that is not older changes nothing.
     */

    virtual bool Mode(Channel*, const std::string& modes, const std::string& args,
                      const iClient* from = nullptr, time_t olderTimestamp = 0);

    /// Clear these modes: a flag or a setting itself, for 'o' and 'v' every
    /// member holding it, for 'b' every ban.
    virtual bool ClearMode(Channel*, const std::string& modes, const iClient* from = nullptr);

    virtual bool Op(Channel*, iClient*, const iClient* from = nullptr);
    virtual bool Op(Channel*, const std::vector<iClient*>&, const iClient* from = nullptr);
    virtual bool DeOp(Channel*, iClient*, const iClient* from = nullptr);
    virtual bool DeOp(Channel*, const std::vector<iClient*>&, const iClient* from = nullptr);
    virtual bool Voice(Channel*, iClient*, const iClient* from = nullptr);
    virtual bool Voice(Channel*, const std::vector<iClient*>&, const iClient* from = nullptr);
    virtual bool DeVoice(Channel*, iClient*, const iClient* from = nullptr);
    virtual bool DeVoice(Channel*, const std::vector<iClient*>&, const iClient* from = nullptr);

    /// Ban these clients by the mask Channel::createBan() gives them.
    virtual bool Ban(Channel*, iClient*, const iClient* from = nullptr);
    virtual bool Ban(Channel*, const std::vector<iClient*>&, const iClient* from = nullptr);
    /// Set, or remove, bans by mask.
    virtual bool Ban(Channel*, const banVectorType&, const iClient* from = nullptr);
    virtual bool UnBan(Channel*, const std::string& banMask, const iClient* from = nullptr);
    virtual bool UnBan(Channel*, const banVectorType&, const iClient* from = nullptr);

    /// Kick, and take the member off the channel.  Network services (+k)
    /// are never kicked.
    virtual bool Kick(Channel*, iClient*, const std::string& reason, const iClient* from = nullptr);
    virtual bool Kick(Channel*, const std::vector<iClient*>&, const std::string& reason,
                      const iClient* from = nullptr);

    /// Set the topic.  A client source has to be on the channel, and opped
    /// if the channel is +t.
    virtual bool Topic(Channel*, const std::string& newTopic, const iClient* from = nullptr);

    /// Invite a client.  Only a client can invite: from a server it is a
    /// protocol violation, so `from` has no default here and must be one.
    virtual bool Invite(iClient* target, Channel*, const iClient* from);

    /* Event registration stuff */

    /**
     * RegisterEvent is called by xClient's wishing
     * to receive a particular event.
     * When a particular event occurs, the server will call
     * OnEvent for each xClient registered to receive that
     * event.
     */
    virtual bool RegisterEvent(const eventType&, xClient*);

    /**
     * Halt delivery of the given event to the given xClient.
     */
    virtual bool UnRegisterEvent(const eventType&, xClient*);

    /**
     * The channel event distribution system is rather
     * expensive...such is the nature of IRC.
     * Each channel name is converted to lower case.
     * When a channel event occurs, the server will call
     * each OnChannelEvent() with the event type
     * and channel name for each xClient registered for
     * that <event,channel> pair.
     */
    virtual bool RegisterChannelEvent(const std::string&, xClient*);

    /**
     * Halt delivery of any channel event for the particular channel
     * to the particular xClient.
     */
    virtual bool UnRegisterChannelEvent(const std::string&, xClient*);

    /**
     * The type used to represent client timer events.
     */
    typedef TimerHandler::timerID timerID;

    /**
     * Register for a timer event.  The first argument is the
     * absolute time at which the timed event is to occur.
     * The second argument is a pointer to an argument to be
     * passed to the timer handler.
     * Returns 0 on failure, a valid timerID otherwise.
     */
    virtual timerID RegisterTimer(const time_t& absoluteTime, TimerHandler* theHandler,
                                  void* data = 0);

    /**
     * Remove a timed event from the timer system.
     * If data is non-NULL, the timer argument passed to
     * RegisterTimer() will be returned through data.
     * Return true if successful, false otherwise.
     */
    virtual bool UnRegisterTimer(const timerID&, void* data);

    /**
     * This method is called by the xClient's to notify the network
     * that an iClient has logged into that particular service.
     * The iClient's internal state will be updated to reflect this
     * login, and the message will be sent to the network.
     * The fifth argument (the source xClient) is the xClient issuing
     * the login event.  If this argument is NULL, then all xClients
     * will receive the EVT_ACCOUNT event.  If the argument is non-NULL,
     * then all but the sourceClient will receive the event.
     */
    virtual void UserLogin(iClient*, const std::string&, const unsigned int,
                           const unsigned short int, xClient* = 0);

    /**
     * Post a system event to the rest of the system.  Note
     * that this method is public, so xClients may post
     * events.
     * The last argument is the an exclude xClient -- the
     * event will NOT be sent to that client (in the case that
     * an xClient calls PostEvent(), it may not want to receive
     * that event back).
     */
    virtual void PostEvent(const eventType&, void* = 0, void* = 0, void* = 0, void* = 0,
                           const xClient* ourClient = 0);

    /**
     * Post a channel event to the rest of the system.  Note
     * that this method is public, so xClients may post
     * channel events.
     */
    virtual void PostChannelEvent(const channelEventType&, Channel* theChan, void* = 0, void* = 0,
                                  void* = 0, void* = 0);

    /**
     * This method is called when a kick occurs on a channel
     * for which this client is registered to receive events.
     * The srcClient may be NULL, as the ircu protocol still
     * allows servers to issue KICK commands.
     * The authoritative variable is true if the kick
     * transaction is complete, false otherwise.  If it is false,
     * then the destClient is still on the channel pending
     * a PART from its server, and it is in the ZOMBIE state.
     */
    virtual void PostChannelKick(Channel* theChan,
                                 iClient* srcClient, // may be NULL
                                 iClient* destClient, const std::string& kickMessage,
                                 bool authoritative);

    /**
     * This variable represents "all channels."  Clients may
     * register for events of this channel, and each will receive
     * this event for every channel in existence on the network.
     */
    static const std::string CHANNEL_ALL;

    /**
     * Post a signal to the server and all clients.
     * Returns true if the signal was handled, false
     * otherwise.
     */
    virtual bool PostSignal(int);

    /* Utility methods */

    /**
     * Return true if no BURST state exists, false otherwise.
     */
    virtual bool IsEndOfBurst() const { return !bursting; }

    /**
     * Return true if this server is currently bursting, false
     * otherwise.
     */
    inline bool isBursting() const { return bursting; }

    /**
     * Return true if there is one more iteration of the main
     * processing loop to be performed before shutting down.
     */
    inline bool isLastLoop() const { return lastLoop; }

    /**
     * Return true if an EA will be sent after the EB.
     * Some modules may want the "endless" burst effect.
     * Default value is true.
     */
    inline bool getSendEA() const { return sendEA; }

    /**
     * Invoke this method with the value 'true' if you want
     * the EA token to be sent.  Setting to
     * false will create the endless burst effect, and bursting
     * will always be true.
     * Default value is true.
     * Only modify this variable if you know what youre doing.
     */
    inline void setSendEA(bool newVal = true) { sendEA = newVal; }

    /**
     * Return true if an EB will be sent after the EB.
     * Some modules may want the "endless" burst effect.
     * Default value is true.
     */
    inline bool getSendEB() const { return sendEB; }

    /**
     * Invoke this method with the value 'true' if you want
     * the EB token to be sent.  Setting to
     * false will create the endless burst effect, and bursting
     * will always be true.
     * Default value is true.
     * Only modify this variable if you know what youre doing.
     */
    inline void setSendEB(bool newVal = true) { sendEB = newVal; }

    /**
     * Set the bursting value to the given argument, with default
     * argument set to true.
     * This method should NOT be called by anything other than the
     * server command handlers.
     */
    virtual void setBursting(bool newVal = true);

    /**
     * Return true if the server has a valid connection to
     * its uplink, false otherwise.
     */
    virtual bool isConnected() const {
        return (serverConnection != 0 && serverConnection->isConnected());
    }

    /**
     * Return true if verbosity is enabled.
     */
    virtual bool isVerbose() const { return verbose; }

    /**
     * Return true if debug is enabled
     */
    virtual bool isDebug() const { return doDebug; }

    /**
     * Return true if the server is to auto reconnect on
     * connection termination, false otherwise.
     */
    virtual bool getAutoConnect() const { return autoConnect; }

    /* Numeric utility methods */

    /**
     * Return an unsigned int representation of this server's uplink's
     * server numeric.
     */
    inline unsigned int getUplinkIntYY() const { return Uplink->getIntYY(); }

    /**
     * Return a string representation of this server's uplink's
     * server numeric.
     */
    inline const std::string getUplinkCharYY() const { return Uplink->getCharYY(); }

    /* General server utility methods */

    /**
     * Return this server's name, as the network sees it.
     */
    inline const std::string& getName() const { return ServerName; }

    /**
     * Return a description of this server.
     */
    inline const std::string& getDescription() const { return ServerDescription; }

    /**
     * Return this server's uplink password.
     */
    inline const std::string& getPassword() const { return Password; }

    /**
     * Return the time at which this server was instantiated.
     */
    inline const time_t& getStartTime() const { return StartTime; }

    /**
     * Return the time this server last connected to its uplink.
     */
    inline const time_t& getConnectionTime() const { return ConnectionTime; }

    /**
     * Return a pointer to this server's uplink.
     */
    inline iServer* getUplink() const { return Uplink; }

    /**
     * Return a pointer to this server's iServer representation.
     */
    inline iServer* getMe() const { return me; }

    /**
     * Set this server's uplink.
     * This method should ONLY be called by the server command
     * handlers.
     * When our link to the uplink is TLS, mark the uplink iServer
     * as TLS regardless of whether its SERVER line advertised +z.
     */
    inline void setUplink(iServer* newUplink) {
        Uplink = newUplink;
        if (Uplink != nullptr && serverConnection != nullptr && serverConnection->isTLS()) {
            Uplink->setTLS();
        }
    }

    /**
     * Enable or disable the burstBuffer.
     * This method should ONLY be called by the server command
     * handlers.
     */
    inline void setUseHoldBuffer(bool newVal) { useHoldBuffer = newVal; }

    /**
     * Set the time of the most recent end of burst.
     * This method should ONLY be called by the server command
     * handlers.
     */
    inline void setBurstEnd(const time_t newVal) { burstEnd = newVal; }

    /**
     * The time the uplink's burst ended on this link, 0 for as long as
     * it has not.
     */
    inline time_t getBurstEnd() const { return burstEnd; }

    /**
     * Set the time of the most recent start of burst.
     * This method should ONLY be called by the server command
     * handlers.
     */
    inline void setBurstStart(const time_t newVal) { burstStart = newVal; }

    /**
     * Transfer the burstHoldBuffer data to the outputBuffer.
     * This method should ONLY be called by the server command
     * handlers.
     */
    virtual void WriteBurstBuffer();

    /**
     * Shutdown the server.
     */
    virtual void Shutdown(const std::string& reason = std::string("Server Shutdown"));

    /**
     * Set the reason for the server shutdown, to be displayed in
     * the SQ message.
     */
    inline void setShutDownReason(const std::string& newReason) { shutDownReason = newReason; }

    /**
     * Return the reason for the server shutdown.
     */
    inline const std::string& getShutDownReason() const { return shutDownReason; }

    /**
     * Output server statistics to the console (clog).
     */
    virtual void dumpStats();

    /**
     * Sets the logging system up from logging.conf, or from the built-in
     * default when there is no such file, and applies the command line on top
     * of it.
     *
     * Called once before the first module is loaded, so that a module finds
     * logging configured, and again with reload true for every SIGHUP.  On a
     * reload a file that cannot be read or cannot be applied changes nothing
     * and is reported on "core.config": a mistake in logging.conf can neither
     * stop nor silence a running process.
     */
    virtual void setupLogging(bool reload = false);

    /**
     * Start logging.
     */
    virtual void startLogging(bool logrotate = false);

    /**
     * Rotate logs.
     */
    virtual void rotateLogs();

    /**
     * Execute any waiting client timers.
     * Return the number of timers executed.
     */
    virtual unsigned int CheckTimers();

    /**
     * The main loop which runs the server.  This contains
     * all of the server essential logic.
     */
    void mainLoop();

    /**
     * This method is invoked when one or more channels modes
     * are set.  Each mode does NOT have an argument.  This
     * simplifies handling of simple modes such as t, i, n, etc.
     */
    virtual void OnChannelMode(Channel*, ChannelUser*, const modeVectorType&);

    /**
     * This method is called when a channel mode 'l' change is
     * detected.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeL(Channel*, bool, ChannelUser*, unsigned int);

    /**
     * This method is called when a channel mode 'k' change is
     * detected.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeK(Channel*, bool, ChannelUser*, const std::string&);

    /**
     * This method is called when a channel mode 'A' change is
     * detected.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeA(Channel*, bool, ChannelUser*, const std::string&);

    /**
     * This method is called when a channel mode 'U' change is
     * detected.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeU(Channel*, bool, ChannelUser*, const std::string&);

    /**
     * This method is called when one or more mode 'o' changes
     * are set/unset on a particular channel.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeO(Channel*, ChannelUser*, const opVectorType&);

    /**
     * This method is called when one or more mode 'v' changes
     * are set/unset on a particular channel.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     */
    virtual void OnChannelModeV(Channel*, ChannelUser*, const voiceVectorType&);

    /**
     * This method is called when one or more mode 'b' changes
     * are set/unset on a particular channel.
     * Keep in mind that the source ChannelUser may be NULL
     * if the mode is being set/unset by a server.
     * `banInfo`, when it is not empty, holds one entry for each ban given,
     * in the same order: who set it and when, for the channel to record
     * (see Channel::BanInfo).  It is not passed on to the modules, whose
     * xClient::OnChannelModeB() is unchanged; they read a ban's details
     * from the channel, with Channel::getBanInfo().
     */
    virtual void OnChannelModeB(Channel*, ChannelUser*, banVectorType&,
                                std::span<const Channel::BanInfo> banInfo = {});

    /**
     * Apply channel mode changes: the one place where a parsed change
     * becomes a change of state.  It goes through the OnChannelMode*()
     * methods above.  `sourceUser` is the member that made the change; null
     * for a server, or for a client that is not on the channel (OPMODE).
     *
     * A change it cannot apply is logged, and the others applied: that is
     * a +o/+v whose target is unknown or not on the channel, which happens
     * when the change crosses a KILL or a KICK of ours on the way.
     * `where` starts the log lines: "msg_M>".
     *
     * `setBy` is what a ban of this change is recorded as having been set
     * by: the nick of a user source, the name of a server source, empty
     * where the caller cannot know.  `sourceUser` is not enough for it: it
     * is null for a client that is not on the channel as well as for a
     * server.  A caller with nothing but removals has no use for it.
     */
    virtual void ApplyChannelModes(Channel*, ChannelUser* sourceUser,
                                   std::span<const Channel::ModeChange> changes,
                                   std::string_view where, std::string_view setBy = {});

    /**
     * A line from the uplink that cannot be parsed.  Our picture of the
     * network is only of use while it is the uplink's, and after such a
     * line it is not: say which line, and what is wrong with it, and
     * abort, as a failed assert() does.  Call it before the line has
     * changed anything.
     *
     * This is for the network only.  A module that asks for something
     * invalid is refused, and gnuworld carries on: see Mode().
     */
    [[noreturn]] void ProtocolError(std::string_view where,
                                    std::span<const std::string> problems) const;

    /**
     * A line has at least as many parameters as the shortest form the
     * protocol has for its command, the source counted: fewer is a protocol
     * error.  A form that is legal but of no use to us is not: the handler
     * leaves that one alone.
     */
    void RequireParameters(std::string_view where, const xParameters& line,
                           xParameters::size_type minimum) const {
        if (line.size() < minimum) {
            const std::string problem =
                std::format("{} parameters, where the shortest form has {}", line.size(), minimum);
            ProtocolError(where, std::span(&problem, 1));
        }
    }

    /**
     * A number the uplink itself writes into the line, such as a timestamp
     * or a count: if it is not one, that is a protocol error.  `what` names
     * it in the log: "channel timestamp".  Text the uplink only passes on
     * for somebody else is not for these: parseNumber() it, and cope.
     */
    template <std::integral T>
    T RequireNumber(std::string_view where, std::string_view what, std::string_view text) const {
        if (const std::optional<T> value = parseNumber<T>(text)) {
            return *value;
        }
        const std::string problem = std::format("invalid {}: {}", what, text);
        ProtocolError(where, std::span(&problem, 1));
    }

    /// RequireNumber() for a time.  ircu writes them unsigned.
    time_t RequireTimestamp(std::string_view where, std::string_view what,
                            std::string_view text) const {
        return static_cast<time_t>(RequireNumber<std::uint64_t>(where, what, text));
    }

    /**
     * Check the list of glines for any that are about to
     * expire.
     */
    virtual void updateGlines();

    /**
     * Burst out information about all xClients on this server.
     */
    virtual void BurstClients();

    /**
     * Output channel information for each client on this server.
     */
    virtual void BurstChannels();

    /**
     * Output glines information for each client on this server.
     */
    virtual void BurstGlines();

    /**
     * Return the length of time needed for the last burst.
     */
    virtual time_t getLastBurstDuration() const { return (burstEnd - burstStart); }

    /**
     * Return the number of bytes since the beginning of the
     * last burst.
     */
    virtual size_t getBurstBytes() const { return burstBytes; }

    /**
     * Return the number of commands processed since the beginning
     * of the last burst.
     */
    virtual size_t getBurstLines() const { return burstLines; }

    /**
     * Return true if the given nickname corresponds to a
     * server control nickname.
     * Return false otherwise.
     */
    virtual bool findControlNick(const std::string&) const;

    /**
     * Handle a control message.
     */
    virtual void ControlCommand(iClient* srcClient, const std::string& message);

    /**
     * This method overrides ConnectionHandler's OnTimeout method.
     * This method is called if a connection timeout occurs.
     * The given Connection is no longer valid when this method
     * is called.
     */
    virtual void OnTimeout(Connection*) override;

  protected:
    /**
     * Update a channel for these modes without raising any event.  For our
     * own clients' joins, which have never raised mode events.
     */
    /**
     * Every Write*() ends up here.  Sends exactly one line: the line ending
     * is ours to add, and anything behind a CR, LF or NUL inside the text is
     * dropped, and logged, so that text cannot start a line of its own.
     */
    bool writeLine(std::string_view text, bool duringBurst);

    /// The line being processed, as it was received: for ProtocolError()
    std::string currentLine;

    /// One line in elog for each problem: "<where> (<channel>): ...".
    void logModeProblems(std::string_view where, std::string_view channelName,
                         std::span<const std::string> problems) const;

    /*
     * What the channel methods (Mode(), Op(), Ban(), Kick()...) of xServer
     * and of xClient are made of.  Not part of the API for modules.
     */

    /// The numeric a change goes out under.
    std::string numericOf(const iClient* from) const;

    /**
     * Whoever changes a channel has to be able to.  A server (null) always
     * can; a client has to be on the channel, opped.  One of our xClients
     * that is not on the channel joins it for as long as the change takes:
     * `joined` is then set to it, and the caller has it Part() afterwards.
     */
    bool enterChannel(const iClient* from, Channel* theChan, xClient*& joined);

    /**
     * Make these mode changes: the one way a channel mode change of ours
     * happens.  Checks each against the channel, names an o/v target by its
     * numeric (it may be given by nick), and drops what would change
     * nothing; false if one of them cannot be made, and nothing is then
     * done.  Tells the network, then updates the channel and the modules,
     * to which `eventSource` is reported as the member who did it.
     *
     * `olderTimestamp`, if not 0, is a creation time for the channel that
     * the caller knows of: if it is older than ours it becomes ours, and the
     * line carries it, which makes it the network's.  See Mode().
     */
    bool changeModes(Channel* theChan, std::vector<Channel::ModeChange> requested,
                     const iClient* from, ChannelUser* eventSource, time_t olderTimestamp = 0);

    /**
     * +/-o or +/-v for these clients.  A target that is null, not on the
     * channel, or a network service being deopped is skipped, or with a
     * single target fails the call.
     */
    bool changeMembers(Channel* theChan, char letter, bool set, std::span<iClient* const> targets,
                       const iClient* from, ChannelUser* eventSource);

    /// +/-b for these masks.
    bool changeBans(Channel* theChan, const banVectorType& bans, const iClient* from,
                    ChannelUser* eventSource);

    /// The bans for these clients; network services and those not on the
    /// channel are skipped.
    banVectorType bansFor(const Channel* theChan, std::span<iClient* const> targets) const;

    /**
     * Kick those of the targets that are on the channel and not a network
     * service: send the kicks, take the members off and notify the modules,
     * to which `kicker` is reported; null for a server.  With nobody to
     * kick, a single target is a refusal and several are nothing to do.
     */
    bool kickMembers(Channel* theChan, std::span<iClient* const> targets, const std::string& reason,
                     const iClient* from, iClient* kicker);

    /**
     * Send channel mode changes to the network, as `source` (a server or
     * client numeric).  This only writes: it does not check the changes
     * against the channel and does not update our tables, which is what
     * Mode() is for, and why this is not public.  Every channel MODE line
     * gnuworld sends is formatted here, by Channel::formatModeLines().
     */
    bool sendChannelModes(const std::string& source, Channel* theChan,
                          std::span<const Channel::ModeChange> changes);

    /**
     * The same, for a channel named by hand.  Needed while joining: the
     * modes of a channel we create go out before its Channel exists, and
     * carry the timestamp it was created with.
     */
    bool sendChannelModes(const std::string& source, const std::string& chanName, time_t timestamp,
                          std::span<const Channel::ModeChange> changes);

    /// The kinds of text message there are.
    enum class TextType {
        PRIVMSG,  ///< token P
        NOTICE,   ///< token O
        WALLCHOPS ///< token WC: a notice to the ops of a channel
    };

    /**
     * What Message(), Notice() and NoticeChannelOps() of xClient, and our
     * own Notice(), are sent with.  `from` is null for the server; `target`
     * is a numnick or a channel name.  A line break in the text starts
     * another message, and a line too long for one message is continued in
     * the next.
     */
    bool sendText(TextType type, const iClient* from, std::string_view target,
                  std::string_view text);

    /// The modes of a channel we join or burst: they change the channel, and
    /// have never been reported to the modules.
    void applyModesSilently(Channel* theChan, std::span<const Channel::ModeChange> changes);

    /**
     * Allow only subclasses to call the default
     * constructor.
     */
    xServer() {}

    /**
     * Disable copy constructor, this method is declared but
     * NOT defined.
     */
    xServer(const xServer&);

    /**
     * Disable assignment, this method is declared but NOT
     * defined.
     */
    xServer operator=(const xServer&);

    /**
     * This will remove all clients and clear the internal
     * tables nicely, and do all functions necessary
     * shutdown the server.
     */
    virtual void doShutdown();

    /**
     * Remove glines which match the given userHost, post event.
     */
    virtual void removeMatchingGlines(const std::string&);

    /**
     * This method is responsible for updating the systems internal
     * data structures, and deallocating the given xClient when it
     * is being removed from the server.
     */
    virtual void removeClient(xClient*);

    /**
     * Remove all modes from a channel, used when bursting an
     * older timestamp into a channel.
     */
    virtual void removeAllChanModes(Channel*);

    /**
     * Returns true if the given mask uses the nick!user@host syntax.
     */
    virtual bool banSyntax(const std::string&) const;

    /**
     * Read the config file.  Return true if success, false
     * otherwise.
     */
    virtual bool readConfigFile(const std::string&);

    /**
     * Parses a config file and attempts to load all modules
     * specified therein.  If any part of the process fails,
     * false is returned.  Otherwise, true is returned.
     */
    virtual bool loadClients(const std::string&);

    /**
     * Signal handler for the server itself.
     * Returns true if the signal was handled.
     */
    virtual bool OnSignal(int);

    /**
     * Return true if the given AC username has access enough to
     * issue server control commands.
     */
    virtual bool hasControlAccess(const std::string&) const;

    /**
     * The structure type holds information about client timed
     * events.
     */
    struct timerInfo {
        /// Instantiate a new timerInfo structure.
        timerInfo(const timerID& _ID, const time_t& _absTime, TimerHandler* _theHandler,
                  void* _data = 0)
            : ID(_ID), absTime(_absTime), theHandler(_theHandler), data(_data) {}

        /// The unique identifier of this timer
        timerID ID;

        /// The absolute time at which the timer expires
        time_t absTime;

        /// The handler for this timed event
        TimerHandler* theHandler;

        /// The argument to pass to the handler
        void* data;
    };

    /**
     * This structure is used as a comparator functor for
     * the timerQueue.
     */
    struct timerGreater {
        inline bool operator()(const std::pair<time_t, timerInfo*>& lhs,
                               const std::pair<time_t, timerInfo*>& rhs) const {
            return lhs.first > rhs.first;
        }
    };

    /**
     * Register the timers that the server uses.
     */
    virtual void registerServerTimers();

    /**
     * Return a unique timerID.
     */
    virtual timerID getUniqueTimerID();

    /**
     * Remove all timers registered by the given xClient.
     * Note that this does not attempt to deallocate any
     * heap space allocated to the argument.
     * Instead, that data is returned to the xClient in question
     * by calling its OnTimerDestroy() method.
     */
    virtual void removeAllTimers(TimerHandler*);

    /**
     * Bounds checker for events.
     */
    inline bool validEvent(const eventType& theEvent) const {
        return (theEvent >= 0 && theEvent < EVT_NOOP);
    }

    /**
     * The server's uplink to the network.
     */
    Connection* serverConnection;

    /**
     * The name of the server, as the network sees it.
     */
    std::string ServerName;

    /**
     * This server's description line.
     */
    std::string ServerDescription;

    /**
     * The password for our uplink server.
     */
    std::string Password;

    /**
     * The hostname/IP of our uplink
     */
    std::string UplinkName;

    /**
     * The type used to store the system event map.
     */
    typedef std::vector<std::list<xClient*>> eventListType;

    /**
     * This is the vector of lists of xClient pointers.
     * When clients register to receive an event, that xClient's
     * pointer is added to eventListType[ theEvent ].
     * When an event of that type occurs, each xClient in the
     * list is called (OnEvent()).
     */
    eventListType eventList;

    /**
     * Type used to store the channel event map.
     */
    typedef std::map<std::string, std::list<xClient*>*, rfc1459Compare> channelEventMapType;

    /**
     * The structure used to maintain xClient registrations for
     * channel events.
     */
    channelEventMapType channelEventMap;

    /**
     * This structure holds the current network glines.
     */
    glineListType glineList;

    /**
     * This is the time this xServer was instantiated.
     */
    time_t StartTime;

    /**
     * This is the time that we last connected to our uplink.
     */
    time_t ConnectionTime;

    /**
     * Time at which we began the last burst.
     */
    time_t burstStart;

    /**
     * Time at which we ended the last burst.
     */
    time_t burstEnd;

    /**
     * The timer interval for the glineUpdateTimer timer.
     */
    time_t glineUpdateInterval;

    /**
     * The timer interval for the PINGTimer timer.
     */
    time_t pingUpdateInterval;

    /**
     * This is the version of the xServer, pretty useless.
     */
    long Version;

    /**
     * Whether or not TLS encrypted connections are enabled (and required)
     * for the server uplink
     */
    bool tlsEnabled;

    /**
     * This variable is true when this server is bursting.
     */
    bool bursting;

    /**
     * This variable is set to true to indicate that the server
     * will send EA.  Some modules may want
     * the "endless" burst effect.
     * Default value is true.
     * Only modify this variable if you know what youre doing.
     */
    bool sendEA;

    /**
     * This variable is set to true to indicate that the server
     * will send EB.  Some modules may want
     * the "endless" burst effect.
     * Default value is true.
     * Only modify this variable if you know what youre doing.
     */
    bool sendEB;

    /**
     * This variable is used during a requested shutdown to
     * allow one last iteration of the main processing loop.
     * This will give xClient's a chance to request flushing
     * of output data, and timers to expire.
     */
    bool lastLoop;

    /**
     * This variable will be true when the default behavior
     * of Write() is to write to the burstHoldBuffer.
     */
    bool useHoldBuffer;

    /**
     * This variable remains true while the server should continue
     * running.  It may be set false by user input, or caught
     * signals.
     */
    bool keepRunning;

    /**
     * This is the port number to which we connect on our uplink.
     */
    unsigned short int Port;

    /**
     * This is a pointer into the network table to our uplink
     * server.  It is kept here for convenience.
     */
    iServer* Uplink;

    /**
     * A pointer to the iServer* representation of this server.
     */
    iServer* me;

    /**
     * This buffer will hold data to be written during burst time.
     */
    Buffer burstHoldBuffer;

    /**
     * The name of the file which contains the command handler
     * mapping from network message to handler.
     */
    std::string commandMapFileName;

    /**
     * The path prefix to the gnuworld libraries, of the form
     * "/path/to/lib/dir"
     */
    std::string libPrefix;

    /**
     * Burst() is called when the network connection is
     * established.  Its purpose is to call each xClient's
     * Connect() method so that each client may burst itself
     * and its channels.
     * NOTE: The scope of this method will be altered in
     * the future to reduce the requirements of xClient
     * when bursting its information.
     */
    void Burst();

    /**
     * Type used to store runtime client modules.
     */
    typedef std::vector<moduleLoader<xClient*>*> clientModuleListType;

    /**
     * Structure used to store runtime client modules.
     */
    clientModuleListType clientModuleList;

    /**
     * The type of the modules used to load ServerCommandHandlers
     * from dynamically loadable libraries.
     * This is stored here in order to properly close them when
     * needed, including reloading command handlers.
     */
    typedef moduleLoader<ServerCommandHandler*, xServer*> commandModuleType;

    /**
     * A vector of modules representing ServerCommandHandlers.
     */
    typedef std::vector<commandModuleType*> commandModuleListType;

    /**
     * The structure of moduleLoader's, each representing a
     * ServerCommandHandler.
     */
    commandModuleListType commandModuleList;

    /**
     * The type used to store ServerCommandHandlers, each
     * associated with a particular server message (key).
     */
    typedef std::map<std::string, ServerCommandHandler*, noCaseCompare> commandMapType;

    /**
     * The structure used to store ServerCommandHandlers, each
     * associated with a particular server message (key).
     */
    commandMapType commandMap;

    /**
     * Attempt to locate a commandModuleType by its key.  This is
     * used to reload a module, and to ensure that a module is
     * not accidentally loaded more than once.
     */
    commandModuleType* lookupCommandModule(const std::string&) const;

    /**
     * The type used to store timed events.
     */
    typedef std::priority_queue<std::pair<time_t, timerInfo*>,
                                std::vector<std::pair<time_t, timerInfo*>>, timerGreater>
        timerQueueType;

    /**
     * The structure used to store timed events.
     */
    timerQueueType timerQueue;

    /**
     * The type used to store timer ID's currently in use.
     */
    typedef std::map<timerID, bool> uniqueTimerMapType;

    /**
     * The structure used to store timer ID's current in use.
     */
    uniqueTimerMapType uniqueTimerMap;

    /**
     * The last unique timerID to be used.
     */
    timerID lastTimerID;

    /**
     * The output file to which to write raw data read from
     * the network.
     */
    std::ofstream socketFile;

    /**
     * The reason used to shutdown the server, displayed in
     * the SQ message.
     */
    std::string shutDownReason;

    /**
     * The char array to be used to read in network data.
     * This is allocated only once in the server for
     * performance reasons.
     * Sized for IRCv3 message-tags (IRC_MAX_TAGS) plus the
     * classic IRC line (IRC_MAX_LINE), plus a NUL terminator.
     */
    char inputCharBuffer[IRC_MAX_MESSAGE + 1];

    /**
     * True if all elog data should be output to clog.
     */
    bool verbose;

    /**
     * True if debug is enabled.
     */
    bool doDebug;

    /**
     * True if logging of raw input data to file is enabled.
     */
    bool logSocket;

    /**
     * The name of the file for which elog to write all
     * debugging information.
     */
    std::string elogFileName;

    /**
     * True when that name came from the command line rather than from the
     * default, which is what lets it override the path logging.conf gives the
     * sink named "debuglog".
     */
    bool elogFileGiven;

    /**
     * The name of the file to write socket info
     */
    std::string socketFileName;

    /**
     * The name of the server config file.
     */
    std::string configFileName;

    /**
     * The name of the simulation file from which to read all
     * simulation data, empty if in real mode.
     */
    std::string simFileName;

    /**
     * The path to the TLS key file
     */
    std::string tlsKeyFile;

    /**
     * The path to the TLS cert file
     */
    std::string tlsCertFile;

    /**
     * True if autoreconnect is enabled, false otherwise.
     */
    bool autoConnect;

    /**
     * The name of the logging configuration file: the optional "logging_conf"
     * key of the main configuration file, or logging.conf when it says nothing.
     */
    std::string loggingConfFileName() const;

    /**
     * This method initializes the entire server.
     */
    void initializeSystem();

    /**
     * This method initializes all server variables.
     */
    void initializeVariables();

    /**
     * This method loads all command handlers.
     */
    bool loadCommandHandlers();

    /**
     * This method initializes all of the TLS contexts
     */
    bool initTls();

    /**
     * Load an individual command handler from a file (fileName),
     * and associate that handler with the network message
     * (commandKey).
     */
    bool loadCommandHandler(const std::string& fileName, const std::string& symbolName,
                            const std::string& commandKey);

    /// Some debugging information, just a curiosity
    /// burstLines is the total number of lines that have been
    /// processed since the beginning of the last burst.
    size_t burstLines;

    /// burstBytes is the total number of bytes that have been
    /// processed since the beginning of the last burst.
    size_t burstBytes;

    /**
     * The type used to store the nicknames of control clients.
     */
    typedef std::set<std::string, noCaseCompare> controlNickSetType;

    /**
     * The structure used to store the nicknames of control clients.
     */
    controlNickSetType controlNickSet;

    /**
     * The type used to store the AC account usernames allowed to
     * issue control commands.
     * AC account usernames are case sensitive, so cannot use
     * noCaseCompare here.
     */
    typedef std::set<std::string> allowControlSetType;

    /**
     * The structure used to store the AC account usernames allowed to
     * issue control commands.
     */
    allowControlSetType allowControlSet;
};

} // namespace gnuworld

#endif // __SERVER_H
