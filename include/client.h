/**
 * client.h
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *                    Orlando Bassotto
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
 * $Id: client.h,v 1.59 2005/09/29 17:40:06 kewlio Exp $
 */

#ifndef __CLIENT_H
#define __CLIENT_H "$Id: client.h,v 1.59 2005/09/29 17:40:06 kewlio Exp $"

#include <map>
#include <sstream>
#include <span>
#include <format>
#include <string>
#include <string_view>

#include "NetworkTarget.h"
#include "server.h"
#include "iClient.h"
#include "TimerHandler.h"
#include "logger.h"

namespace gnuworld {

// Forward declarations
class dbHandle;

/**
 * This is the public concrete base class that represents
 * a client that may connect to this server.  To build a new
 * services client, simple subclass this class, inheriting
 * much functionality and state, and overloading a few
 * appropriate functions.
 * This has proven to be extremely easy: I built a functioning
 * services client in 11 minutes, though it didn't do much :)
 */
class xClient : public TimerHandler, public NetworkTarget {
    /// Let xServer access our protected members.
    friend class xServer;

    /// Let xNetwork access our protected members.
    friend class xNetwork;

  public:
    /**
     * Declare the type for the xClient's internal
     * mode representation.  Note that this typedef
     * is deliberately public.
     */
    typedef iClient::modeType modeType;

    /**
     * Construct a new xClient from a config file.
     */
    xClient(const std::string&);

    /**
     * The primary purpose of this method is do call the
     * deallocation methods of any xClient's which are storing
     * data in the customDataMap.
     * Client responsibilities:
     *  Removing all timers registered with the xServer
     *  Removing all custom data members held in the iClient's
     */
    virtual ~xClient();

    /**
     * This method must call xServer::BurstChannel() with appropriate
     * channel modes for any channel it wishes to own.
     */
    virtual void BurstChannels();

    /**
     * BurstGlines is called before eob, for the client to burst
     * all of its glines
     */
    virtual bool BurstGlines();

    /// SILENCE: stop `whom` from reaching us from this mask, and tell it so.
    /// Both of these return false for a stealth module: the protocol has no
    /// server form of a SILENCE.
    virtual bool Silence(const iClient* whom, const std::string& mask);

    /// Lift a SILENCE again.
    virtual bool UnSilence(const std::string& mask);

    /**
     * Kill will issue a KILL command to the network for
     * the given iClient (network generic client).  From a stealth module it
     * is the server's kill either way.
     */
    virtual bool Kill(iClient*, const std::string&);
    virtual bool Kill(iClient*, const std::string&, bool);

    /**
     * QuoteAsServer will send data to the network as the
     * server itself.  Try to avoid using this method.
     */
    virtual bool QuoteAsServer(const std::string& Command);

    /**
     * Write a string of data to the network.
     */
    virtual bool Write(const std::string& s) { return QuoteAsServer(s); }

    /**
     * Write a string of data to the network.
     */
    virtual bool Write(const std::stringstream& s) { return QuoteAsServer(s.str()); }

    /**
     * Write a variable length argument list to the network.
     */
    template <typename... Args> bool Write(CheckedFormat<Args...> fmt, Args&&... args) {
        return Write(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * This method will change modes in a channel.
     * If the fourth argument is true, then the modes will be
     * changed as the server, otherwise the client will set
     * the modes (joining and parting the channel if necessary).
     * Removing mode 'k' expects an argument, but it doesn't matter
     * what the argument is.
     * Removing mode 'l' requires NO argument to be issued.
     */
    virtual bool Mode(const std::string& chanName, const std::string& modes,
                      const std::string& args, bool modeAsServer = false);

    /**
     * This method will change modes in a channel.
     * If the fourth argument is true, then the modes will be
     * changed as the server, otherwise the client will set
     * the modes (joining and parting the channel if necessary).
     * Removing mode 'k' expects an argument, but it doesn't matter
     * what the argument is.
     * Removing mode 'l' requires NO argument to be issued.
     */
    virtual bool Mode(Channel*, const std::string& modes, const std::string& args,
                      bool modeAsServer = false);

    /**
     * Mode is used to set the bot's modes.  If connected to the
     * network already, these new modes will be written to the
     * network.
     */
    virtual bool Mode(const std::string& Mode);

    /**
     * Issue a clearmode (CM) for the given channel and update
     * all internal structures.
     * This method will also post a channel event for each
     * mode.
     */
    virtual bool ClearMode(Channel* theChan, const std::string& modes, bool modeAsServer = false);

    /**
     * OnConnect is called when the server connects to the
     * network, during burst time.  The client's NICK
     * information has already been sent to the network.
     */
    virtual void OnConnect();

    /**
     * Invoked when the uplink has been terminated.
     */
    virtual void OnDisconnect();

    /**
     * This method is invoked when the server has been requested
     * to shutdown.  If currently connected to the network, this
     * method gives xClient's a chance to gracefully QUIT from
     * the network, or whatever other processing is useful.
     * To force data to be written before final shutdown (again,
     * if connected), set xServer::FlushData().
     * Timers will be executed after this method is invoked, once,
     * depending upon target time of course :)
     */
    virtual void OnShutdown(const std::string& reason);

    /**
     * Invoked after the client has been loaded, perform
     * initialization stuff here.
     */
    virtual void OnAttach();

    /**
     * This method will be invoked when the server is unloading
     * the client for whatever reason.
     */
    virtual void OnDetach(const std::string& = std::string("Server Shutdown"));

    /**
     * OnKill() is called when the client has been KILL'd.
     */
    virtual void OnKill();

    /**
     * This method is called when a network client performs
     * a whois on this xClient.
     */
    virtual void OnWhois(iClient* sourceClient, iClient* targetClient);

    /**
     * This method is called when a network client invites
     * a services client to a channel.
     */
    virtual void OnInvite(iClient* sourceClient, Channel* theChan);

    /*
     * One named method per event core posts, each called for a client
     * registered for that event: RegisterEvent() for a network event,
     * RegisterChannelEvent() for a channel one.  Overload the ones this client
     * cares about; the rest do nothing.
     */

    /// A client has been given +o
    virtual void OnOper(iClient* theClient);

    /// A server has left the network, as one of a split or by itself.  uplink
    /// is the server it broke from, and is null for a leaf of a split whose
    /// root has already been taken out of the tables.
    virtual void OnNetBreak(iServer* theServer, const iServer* uplink, std::string_view reason);

    /// A server has joined the network, or a jupe of one has been added.
    /// uplink may be null: only an inbound SERVER names it.
    virtual void OnNetJoin(iServer* theServer, const iServer* uplink);

    /// theServer has finished its net burst; for our own uplink this is also
    /// where our burst ends
    virtual void OnBurstComplete(iServer* theServer);

    /// theServer has acknowledged the end of a burst (EA)
    virtual void OnBurstAck(iServer* theServer);

    /// We have written our own end of burst acknowledgement to theServer
    virtual void OnEndOfBurstAckSent(iServer* theServer);

    /// A G-line has been set, by the network or by one of our own clients
    virtual void OnGline(Gline* theGline);

    /// A G-line has been removed, or has expired
    virtual void OnRemGline(Gline* theGline);

    /// A client has quit, or is going with the server it was on, or is a client
    /// of ours being detached.  reason may be empty.
    ///
    /// A client that quit is still fully attached, because this is posted before
    /// it is removed.  A client going with its server is NOT: xNetwork::
    /// removeServer() removes it first and posts afterwards, so the object is
    /// live but the network no longer knows it, nor the server it was on.  Ask
    /// the network about anything before you act on it.
    virtual void OnQuit(iClient* theClient, std::string_view reason);

    /// theClient has been killed, and is still fully attached.  source is the
    /// client or the server that did it, and is null when one of our own
    /// modules did (xClient::Kill()).
    virtual void OnKill(const NetworkTarget* source, iClient* theClient, std::string_view reason);

    /// A client has appeared on the network, or a module has spawned a fake one
    virtual void OnNick(iClient* theClient);

    /// theClient has changed nick; it already answers to the new one
    virtual void OnNickChange(iClient* theClient, std::string_view oldNick);

    /// theClient has logged in to an account
    virtual void OnAccount(iClient* theClient);

    /// The account flags of theClient, which was logged in already, have changed
    virtual void OnAccountFlags(iClient* theClient);

    /// One line read from the uplink, after it has been handled
    virtual void OnRaw(std::string_view line);

    /// An inter-service query from theServer, to be answered with an XR
    virtual void OnXQuery(iServer* theServer, std::string_view routing, std::string_view message);

    /// The answer to one of those
    virtual void OnXReply(iServer* theServer, std::string_view routing, std::string_view message);

    /// A network configuration variable has been set by theServer
    virtual void OnNetConf(iServer* theServer, std::string_view key);

    /// A network configuration variable has been removed by theServer
    virtual void OnRemNetConf(iServer* theServer, std::string_view key);

    /// theClient is on theChan, having joined it, created it or arrived with a
    /// net burst; kind says which, and channelEventOf() which event that is
    virtual void OnJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser, JoinKind kind);

    /// theClient has left theChan, and is already off it.  message may be
    /// empty: only a PART from the network carries one.
    virtual void OnPart(Channel* theChan, iClient* theClient, std::string_view message);

    /// The topic of theChan has been set.  theClient is null when a server set
    /// it, as one arriving in a burst is.
    virtual void OnTopic(Channel* theChan, iClient* theClient, std::string_view topic);

    /// theServer has changed the modes of theChan.  The changes themselves
    /// arrive through OnChannelMode() and its kin below.
    virtual void OnServerMode(Channel* theChan, iServer* theServer);

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
    virtual void OnNetworkKick(Channel* theChan,
                               iClient* srcClient, // may be NULL
                               iClient* destClient, const std::string& kickMessage,
                               bool authoritative);

    /**
     * This method is invoked when one or more "simple" (no argument)
     * channel modes are set or unset.
     * The ChannelUser* (source) user may be NULL if the modes
     * are being changed by a server.
     */
    virtual void OnChannelMode(Channel*, ChannelUser*, const xServer::modeVectorType&);

    /**
     * This method is invoked when a user sets or removes
     * channel mode l (limit).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     * If the mode is being removed, the limit argument
     * will be 0.
     */
    virtual void OnChannelModeL(Channel*, bool polarity, ChannelUser*, const unsigned int&);

    /**
     * This method is invoked when a user sets or removes
     * channel mode k (key).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     * If the mode is being removed, the key argument will
     * be empty.
     */
    virtual void OnChannelModeK(Channel*, bool polarity, ChannelUser*, const std::string&);

    /**
     * This method is invoked when a user sets or removes
     * channel mode A (Apass).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     * If the mode is being removed, the Apass argument will
     * be empty.
     */
    virtual void OnChannelModeA(Channel*, bool polarity, ChannelUser*, const std::string&);

    /**
     * This method is invoked when a user sets or removes
     * channel mode U (Upass).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     * If the mode is being removed, the Upass argument will
     * be empty.
     */
    virtual void OnChannelModeU(Channel*, bool polarity, ChannelUser*, const std::string&);

    /**
     * This method is invoked when a user sets or removes
     * one or more channel mode (o).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     */
    virtual void OnChannelModeO(Channel*, ChannelUser*, const xServer::opVectorType&);

    /**
     * This method is invoked when a user sets or removes
     * one or more channel mode (v).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     */
    virtual void OnChannelModeV(Channel*, ChannelUser*, const xServer::voiceVectorType&);

    /**
     * This method is invoked when a user sets or removes
     * one or more channel mode (b).  Keep in mind that the
     * source ChannelUser may be NULL if a server is
     * setting the mode.
     */
    virtual void OnChannelModeB(Channel*, ChannelUser*, const xServer::banVectorType&);

    /**
     * This method is called for each signal that occurs
     * in the system.  There is no registration needed to
     * receive signals, just overload this method.  Be
     * sure to end the method with a call to the base
     * class OnSignal (or the closest base class).
     */
    virtual void OnSignal(int);

    /**
     * OnCTCP is called when a CTCP command is issued to
     * the client.
     */
    virtual void OnCTCP(iClient* Sender, const std::string& CTCP, const std::string& Message,
                        bool Secure = false);

    /**
     * OnFakeCTCP is called when a CTCP command is issued to
     * one of this xClient's fake clients.
     */
    virtual void OnFakeCTCP(iClient* Sender, iClient* fakeClient, const std::string& CTCP,
                            const std::string& Message, bool Secure = false);

    /**
     * This method is called when a channel CTCP occurs
     * in a channel in which an xClient resides, and the
     * xClient is user mode -d.
     */
    virtual void OnChannelCTCP(iClient* Sender, Channel* theChan, const std::string& CTCPCommand,
                               const std::string& Message);

    /**
     * This method is called when a channel CTCP occurs
     * in a channel in which an xClient resides, and the
     * xClient is user mode -d.
     */
    virtual void OnFakeChannelCTCP(iClient* Sender, iClient* fakeClient, Channel* theChan,
                                   const std::string& CTCPCommand, const std::string& Message);

    /**
     * OnPrivateMessage is called when a PRIVMSG command
     * is issued to the client.
     */
    virtual void OnPrivateMessage(iClient* Sender, const std::string& Message, bool secure = false);

    /**
     * Invoked when a private message arives for a fake client
     * owned by this xClient.
     */
    virtual void OnFakePrivateMessage(iClient* Sender, iClient* Target, const std::string& Message,
                                      bool secure = false);

    /**
     * This method is called when a channel message occurs
     * in a channel in which an xClient resides, and the
     * xClient is user mode -d.
     */
    virtual void OnChannelMessage(iClient* Sender, Channel* theChan, const std::string& Message);

    /**
     * Invoked when a fake client in a channel receives
     * a channel message.
     */
    virtual void OnFakeChannelMessage(iClient* Sender, iClient* Target, Channel* theChan,
                                      const std::string& Message);

    /**
     * OnPrivateNotice is called when a NOTICE command
     * is issued to the client.
     */
    virtual void OnPrivateNotice(iClient* Sender, const std::string& Message, bool secure = false);

    /**
     * Invoked when a private notice arives for a fake client
     * owned by this xClient.
     */
    virtual void OnFakePrivateNotice(iClient* Sender, iClient* Target, const std::string& Message,
                                     bool secure = false);

    /**
     * OnChannelNotice is called when a module receives
     * channel notice, and is mode -d.
     */
    virtual void OnChannelNotice(iClient* Sender, Channel* theChan, const std::string& Message);

    /**
     * Invoked when a fake client in a channel receives
     * a channel notice.
     */
    virtual void OnFakeChannelNotice(iClient* Sender, iClient* Target, Channel* theChan,
                                     const std::string& Message);

    /**
     * OnServerMessage is called when a server messages
     * a client.
     */
    virtual void OnServerMessage(iServer* Sender, const std::string& Message, bool secure = false);

    /**
     * Handle a timer event.  The first argument is the
     * handle for the timer registration, and the second is
     * the arguments that were passed when registering the
     * timer.
     * This method overloads the pure virtual TimerHandler
     * base class method declaration.
     */
    virtual void OnTimer(const xServer::timerID&, void*);

    /**
     * A timer has been destroyed by the server (such as during
     * a shutdown).  Perform cleanup for the timer here.
     */
    virtual void OnTimerDestroy(xServer::timerID, void*);

    /* Utility methods */

    /**
     * Op a user on a channel, join/part the channel if necessary.
     */
    virtual bool Op(Channel*, iClient*);

    /**
     * Op one or more users on a channel, join/part the channel
     * if necessary.
     */
    virtual bool Op(Channel*, const std::vector<iClient*>&);

    /**
     * Voice a user on a channel, join/part the channel if necessary.
     */
    virtual bool Voice(Channel*, iClient*);

    /**
     * Voice one or more users on a channel, join/part the channel
     * if necessary.
     */
    virtual bool Voice(Channel*, const std::vector<iClient*>&);

    /**
     * Deop a user on a channel, join/part the channel if necessary.
     */
    virtual bool DeOp(Channel*, iClient*);

    /**
     * Deop a user on a channel, join/part the channel if necessary.
     */
    virtual bool DeOp(Channel*, const std::vector<iClient*>&);

    /**
     * Devoice a user on a channel, join/part the channel if necessary.
     */
    virtual bool DeVoice(Channel*, iClient*);

    /**
     * Devoice one or more users on a channel, join/part the channel
     * if necessary.
     */
    virtual bool DeVoice(Channel*, const std::vector<iClient*>&);

    /**
     * Set a ban on a channel, join/part the channel if necessary.
     */
    virtual bool Ban(Channel*, iClient*);

    /**
     * Set a ban on a channel, join/part the channel if necessary.
     */
    virtual bool Ban(Channel*, const std::vector<iClient*>&);

    /**
     * Set bans on a channel from a banVector, join/part the channel if necessary.
     * The banVector must not include duplicates.
     */
    virtual bool Ban(Channel*, const xServer::banVectorType&);

    /**
     * Ban kick a client from a channel for the given reason.
     */
    virtual bool BanKick(Channel*, iClient*, const std::string&);

    /**
     * Remove a channel ban.
     */
    virtual bool UnBan(Channel*, const std::string&);

    /**
     * Removes channel bans from a banVector, join/part the channel if necessary.
     * The banVector must not include duplicates, and only include exact existing
     * bans on the channel.
     */
    virtual bool UnBan(Channel*, const xServer::banVectorType&);

    /**
     * Kick a user from a channel, join/part if necessary.
     */
    virtual bool Kick(Channel*, iClient*, const std::string&, bool modeAsServer = false);

    /**
     * Kick several users from a channel, join/part if necessary.
     */
    virtual bool Kick(Channel*, const std::vector<iClient*>&, const std::string&,
                      bool modeAsServer = false);

    /**
     * Set the topic in a channel, joining, opping, and parting
     * the client if necessary.
     */
    virtual bool Topic(Channel*, const std::string&);

    /**
     * Join will cause the client to join a channel.  Both overloads return
     * false for a stealth module: the protocol has no server form of a JOIN.
     */
    virtual bool Join(const std::string& chanName, const std::string& modes = std::string(),
                      const time_t& joinTime = 0, bool getOps = false);

    /**
     * Join the given channel.
     */
    virtual bool Join(Channel* theChan, const std::string& modes = std::string(),
                      const time_t& joinTime = 0, bool getOps = false);

    /**
     * This method is called when the bot joins a channel.
     */
    virtual void OnJoin(Channel*);

    /**
     * This method is called when the bot joins a channel.
     */
    virtual void OnJoin(const std::string&);

    /**
     * Part will cause the client to part a channel.  Both overloads do nothing
     * for a stealth module: the protocol has no server form of a PART.
     */
    virtual bool Part(const std::string&, const std::string& = std::string());

    /**
     * Part the given channel.
     */
    virtual bool Part(Channel*);

    /**
     * This method is called when the bot parts a channel.
     */
    virtual void OnPart(Channel*);

    /**
     * This method is called when the bot parts a channel.
     */
    virtual void OnPart(const std::string&);

    /**
     * Invite a user to a channel.  Join the channel if necessary
     * (and then part).  Both overloads return false for a stealth module: the
     * protocol has no server form of an INVITE.
     */
    virtual bool Invite(iClient*, const std::string&);

    /**
     * Invite a user to a channel.  Join the channel if necessary
     * (and then part).
     */
    virtual bool Invite(iClient*, Channel*);

    /**
     * Return true if the bot is on the given channel.
     */
    virtual bool isOnChannel(const std::string& chanName) const;

    /**
     * Return true if the bot is on the given channel.
     */
    virtual bool isOnChannel(const Channel* theChan) const;

    /**
     * The text of a CTCP, "\001<CTCP>[ <message>]\001", with no stray
     * space when there is no message: for whoever sends it some other way
     * than DoCTCP(), as the server or as a fake client.
     */
    static std::string ctcpText(const std::string& CTCP, const std::string& Message);

    /// The text of an action, what a client's "/me <action>" sends
    static std::string actionText(const std::string& action);

    /**
     * DoCTCP will issue a CTCP (reply) to the given iClient.
     */
    virtual bool DoCTCP(iClient* Target, const std::string& CTCP, const std::string& Message);

    /**
     * DoFakeCTCP will issue a CTCP (reply) to the given
     * iClient with a fake client interface.
     */
    virtual bool DoFakeCTCP(const iClient* Target, const iClient* srcClient,
                            const std::string& CTCP, const std::string& Message);

    /**
     * Message will PRIVMSG a string of data to the given iClient.
     */
    template <typename... Args>
    bool Message(const iClient* Target, CheckedFormat<Args...> fmt, Args&&... args) {
        return Message(Target, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Message an iClient with a fake client interface.
     */
    virtual bool FakeMessage(const iClient* Target, const iClient* srcClient,
                             const std::string& Message);

    /**
     * Message a channel with a fake client interface.
     */
    virtual bool FakeMessage(const Channel* theChan, const iClient* srcClient,
                             const std::string& Message);

    /**
     * Notice an iClient with a fake client interface.
     */
    virtual bool FakeNotice(const iClient* Target, const iClient* srcClient,
                            const std::string& Message);

    /**
     * Notice a channel with a fake client interface.
     */
    virtual bool FakeNotice(const Channel* theChan, const iClient* srcClient,
                            const std::string& Message);

    /**
     * Message will PRIVMSG a string of data to the given iClient.
     */
    virtual bool Message(const iClient* Target, const std::string& Message);

    /**
     * This format of Message will write a string of data
     * to a channel.
     */
    template <typename... Args>
    bool Message(const std::string& Channel, CheckedFormat<Args...> fmt, Args&&... args) {
        return Message(Channel, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * This format of Message will write a string of data
     * to a channel.
     */
    virtual bool Message(const std::string& Channel, const std::string& Message);

    /**
     * This format of Message will write a string of data
     * to a channel.
     */
    virtual bool Message(const Channel* theChan, const std::string& Message);

    /**
     * Have this module message a channel.
     */
    template <typename... Args>
    bool Message(const Channel* theChan, CheckedFormat<Args...> fmt, Args&&... args) {
        return Message(theChan, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Notice will send a NOTICE command to the given iClient.
     */
    template <typename... Args>
    bool Notice(const iClient* Target, CheckedFormat<Args...> fmt, Args&&... args) {
        return Notice(Target, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Notice will send a NOTICE command to the given iClient.
     */
    virtual bool Notice(const iClient* Target, const std::string&);

    /**
     * This Notice() signature will send a channel NOTICE.
     */
    /// To a channel by name; the '#' may be left off.
    virtual bool Notice(const std::string& Channel, const std::string& Message);

    template <typename... Args>
    bool Notice(const std::string& Channel, CheckedFormat<Args...> fmt, Args&&... args) {
        return Notice(Channel, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * This Notice() signature will send a channel NOTICE.
     */
    template <typename... Args>
    bool Notice(const Channel* theChan, CheckedFormat<Args...> fmt, Args&&... args) {
        return Notice(theChan, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Notice channel operators with given message.  Every overload returns
     * false for a stealth module: the network does not carry a WALLCHOPS that
     * no client sent.
     */
    virtual bool NoticeChannelOps(const Channel* theChan, const std::string& Message);

    template <typename... Args>
    bool NoticeChannelOps(const Channel* theChan, CheckedFormat<Args...> fmt, Args&&... args) {
        return NoticeChannelOps(theChan, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Notice channel operators with given message.
     */
    virtual bool NoticeChannelOps(const std::string& chanName, const std::string& Message);

    template <typename... Args>
    bool NoticeChannelOps(const std::string& chanName, CheckedFormat<Args...> fmt, Args&&... args) {
        return NoticeChannelOps(chanName, formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * This Notice() signature will send a channel NOTICE.
     */
    virtual bool Notice(const Channel*, const std::string&);

    /**
     * Have this bot send a global wallops message.  From a stealth module
     * it is the server's.
     */
    virtual bool Wallops(const std::string&);

    /**
     * Have this bot send a global wallops message.
     */
    template <typename... Args> bool Wallops(CheckedFormat<Args...> fmt, Args&&... args) {
        return Wallops(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Have the server send a wallops.
     */
    virtual bool WallopsAsServer(const std::string&);

    /**
     * Have the server send a wallops.
     */
    template <typename... Args> bool WallopsAsServer(CheckedFormat<Args...> fmt, Args&&... args) {
        return WallopsAsServer(formatMessage<Args...>(fmt, std::forward<Args>(args)...));
    }

    /**
     * Return this xClient's network instance (iClient*).  Null for a
     * stealth module, which has no client on the network: what it does is
     * sent by the server, as a null source is everywhere in xServer.
     */
    inline iClient* getInstance() const { return me; }

    /**
     * Return this bot's nick name.
     */
    inline const std::string& getNickName() const { return nickName; }

    /**
     * Retrieve this bot's user name.
     */
    inline const std::string& getUserName() const { return userName; }

    /**
     * Retrieve this bot's host name.
     */
    inline const std::string& getHostName() const { return hostName; }

    /**
     * Retrieve this bot's description.
     */
    inline const std::string& getDescription() const { return userDescription; }

    /**
     * Retrieve this bot's uplink's numeric, integer format.
     */
    unsigned int getUplinkIntYY() const { return MyUplink->getIntYY(); }

    /**
     * Retrieve this bot's uplink's highest client count
     * numeric, integer format.
     */
    inline unsigned int getUplinkIntXXX() const { return MyUplink->getIntXXX(); }

    /**
     * Retrieve this bot's uplink's numeric, character
     * array format.
     */
    inline const std::string getUplinkCharYY() const { return MyUplink->getCharYY(); }

    /**
     * Retrieve this bot's uplink's client count numeric,
     * character array format.
     */
    inline const std::string getUplinkCharXXX() const { return MyUplink->getCharXXX(); }

    /**
     * Retrieve this bot's uplink's network numeric,
     * std::string format.
     */
    inline const std::string getUplinkCharYYXXX() const { return MyUplink->getCharYYXXX(); }

    /**
     * Retrieve this bot's uplink's server name.
     */
    inline const std::string& getUplinkName() const { return MyUplink->getName(); }

    /**
     * Retrieve this bot's uplink's description.
     */
    inline const std::string& getUplinkDescription() const { return MyUplink->getDescription(); }

    /**
     * Accessor method for the bot's user modes.
     */
    virtual std::string getModes() const;

    /**
     * Return true if an arbitrary mode is set, false otherwise.
     */
    inline bool getMode(const modeType& whichMode) const {
        return ((mode & whichMode) == whichMode);
    }

    /**
     * Obtain a pointer to the single xServer instance, which
     * is the uplink of every services bot.
     * Use of this method is discouraged.
     */
    inline xServer* getUplink() const { return MyUplink; }

    /**
     * Return the name of the configuration file from which this
     * client derived its configuration information.
     */
    inline const std::string& getConfigFileName() const { return configFileName; }

    /**
     * Returns a pointer to the logger object of this client.
     */
    inline Logger* getLogger() { return logger; }

    /**
     * Return true if the server is connected to a network.
     */
    inline bool isConnected() const { return (MyUplink && MyUplink->isConnected()); }

    /**
     * Return true if this module runs in stealth mode (no iClient
     * created or burst; addressable only via nick@server).
     */
    inline bool IsStealth() const { return stealth; }

    /**
     * Utility method for outputting client information to
     * a gnuworld logging stream.
     */
    friend ELog& operator<<(ELog& out, const xClient& theClient) {
        out << theClient.nickName << '!' << theClient.userName << '@' << theClient.hostName
            << " Numeric: " << theClient.getCharYYXXX()
            << ", int YY/XXX/YYXXX: " << theClient.getIntYY() << '/' << theClient.getIntXXX() << '/'
            << theClient.getIntYYXXX();
        return out;
    }

  protected:
    /**
     * Allow sub classes to call default constructor
     * This method is defined in the source file.
     */
    xClient();

    /**
     * Disallow copying.
     * This method is declared, but NOT defined.
     */
    xClient(const xClient&);

    /**
     * Disallow default assignment.
     * This method is declared, but NOT defined.
     */
    xClient operator=(const xClient&);

    /**
     * This method is called by the xServer, and its purpose is
     * to reset its iClient instance.
     */
    inline void resetInstance() { me = 0; }

    /**
     * This method is called to add a channel to the bot's
     * internal channel database; typically called from
     * OnJoin().  This is used to maintain the integrity
     * of isOnChannel() calls.
     */
    virtual bool addChan(Channel*);

    /**
     * This method is called to remove a channel from the
     * bot's internal database; typically called from OnPart().
     * This is used to maintain the integrity of isOnChannel()
     * calls.
     */
    virtual bool removeChan(Channel*);

    /**
     * This method sets the iClient instance of this xClient.
     */
    virtual void setInstance(iClient* me) { this->me = me; }

    /**
     * The iClient representation of this xClient.
     * This variable will be set by xNetwork once the
     * xClient links to the xServer.
     */
    iClient* me = nullptr;

    /**
     * MyUplink is a pointer to the xServer to which this
     * client is attached.
     */
    xServer* MyUplink = nullptr;

    /**
     * This bot's nick name.
     */
    std::string nickName;

    /**
     * This bot's user name.
     */
    std::string userName;

    /**
     * This bot's host name.
     */
    std::string hostName;

    /**
     * This bot's description.
     */
    std::string userDescription;

    /**
     * Connected is true when we are connected to an xServer.
     * It says nothing of whether the xServer is connected
     * to a network.
     */
    bool Connected = false;

    /**
     * True when this module operates without an online iClient
     * (stealth = yes in config). Messages arrive via nick@server.
     */
    bool stealth = false;

    /**
     * This is the user mode of this client.
     */
    modeType mode;

    /**
     * The name of the config file from which this client read
     * its configuration information.
     */
    std::string configFileName;

    /**
     * The logger of this module, which is the one the registry keeps under the
     * module's name: it is not owned here and it outlives this client, so that a
     * module which is unloaded and loaded again writes to the same logger.
     *
     * It has no sink of its own: where the records of a module go is what
     * logging.conf says, and the sinks of the root are where they go when it
     * says nothing.  A module that wants more attaches it itself.
     */
    Logger* logger = nullptr;

    /**
     * Teaches the logging system how to show one of this module's own object
     * types in a log message, for as long as the module is loaded.
     */
    template <class T> void registerLogExtractor(std::function<LogObject(const T*)> f) {
        Logger::registerExtractor<T>(this, std::move(f));
    }

    /**
     * Flag to track whether migrations have been checked for this module.
     * Prevents repeated checks on the same instance.
     */
    bool migrationsChecked = false;

    /**
     * Check database migrations for this module.
     * Verifies that all migration files have been applied to the database.
     * Must be called after database connection is established.
     *
     * @param moduleName The name of the module (e.g., "cservice", "ccontrol")
     *                   This is also used as the database name for migration tracking
     * @param db Pointer to the database connection handle
     * @return true if all migrations are applied, false otherwise
     *         On failure, detailed error messages are logged to elog
     */
    bool checkMigrationsAfterDBConnect(const std::string& moduleName, dbHandle* db);
};

} // namespace gnuworld

#endif // __CLIENT_H
