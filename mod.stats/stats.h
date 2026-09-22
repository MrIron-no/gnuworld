/**
 * stats.h
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
 * $Id: stats.h,v 1.18 2004/06/04 20:17:24 jeekay Exp $
 */

#ifndef __STATS_H
#define __STATS_H "$Id: stats.h,v 1.18 2004/06/04 20:17:24 jeekay Exp $"

#include <fstream>
#include <string>
#include <list>

#include "client.h"
#include "iClient.h"
#include "server.h"
#include "events.h"

namespace gnuworld {

class stats : public xClient {
  public:
    /**
     * Create a new stats object, given its configuration filename.
     */
    stats(const std::string&);

    /**
     * Destroy a stats object.
     */
    virtual ~stats();

    /**
     * This method required to load into the gnuworld core.
     * It basically just calls the base class OnAttach(),
     * and registers for events.
     */
    virtual void OnAttach() override;

    /**
     * This method is invoked when someone sends a private
     * message to the stats bot.
     */
    virtual void OnPrivateMessage(iClient*, const std::string&, bool = false) override;

    /**
     * This method is invoked when someone sends a private
     * message CTCP to the stats bot.
     */
    virtual void OnCTCP(iClient*, const std::string&, const std::string&, bool = false) override;

    /**
     * This method is invoked when someone sends a channel
     * message to the stats bot.
     */
    virtual void OnChannelMessage(iClient*, Channel*, const std::string&) override;

    /**
     * This method is invoked when someone sends a channel
     * CTCP to the stats bot.
     */
    virtual void OnChannelCTCP(iClient*, Channel*, const std::string&, const std::string&) override;

    /**
     * This method is invoked when someone sends a private
     * notice to the stats bot.
     */
    virtual void OnPrivateNotice(iClient*, const std::string&, bool) override;

    /**
     * This method is invoked when someone sends a channel
     * notice to the stats bot.
     */
    virtual void OnChannelNotice(iClient*, Channel*, const std::string&) override;

    /**
     * Every event this module is registered for.  Each one counts itself and
     * does nothing with what it carries, so they are listed in the order
     * events.h numbers them - which is the order of the counters and of the
     * log files.  Three events are absent: EVT_KICK is counted by
     * OnNetworkKick() below, EVT_RAW is not registered for, and nothing posts
     * EVT_JUPE or EVT_UNJUPE.
     */
    virtual void OnOper(iClient*) override;
    virtual void OnNetBreak(iServer*, const iServer*, std::string_view) override;
    virtual void OnNetJoin(iServer*, const iServer*) override;
    virtual void OnBurstComplete(iServer*) override;
    virtual void OnBurstAck(iServer*) override;
    virtual void OnEndOfBurstAckSent(iServer*) override;
    virtual void OnGline(Gline*) override;
    virtual void OnRemGline(Gline*) override;
    virtual void OnQuit(iClient*, std::string_view) override;
    virtual void OnKill(const NetworkTarget*, iClient*, std::string_view) override;
    virtual void OnNick(iClient*) override;
    virtual void OnNickChange(iClient*, std::string_view) override;
    virtual void OnAccount(iClient*) override;
    virtual void OnAccountFlags(iClient*) override;
    virtual void OnXQuery(iServer*, std::string_view, std::string_view) override;
    virtual void OnXReply(iServer*, std::string_view, std::string_view) override;
    virtual void OnNetConf(iServer*, std::string_view) override;
    virtual void OnRemNetConf(iServer*, std::string_view) override;
    virtual void OnJoin(Channel*, iClient*, ChannelUser*) override;
    virtual void OnPart(Channel*, iClient*, std::string_view) override;
    virtual void OnServerMode(Channel*, iServer*) override;
    virtual void OnTopic(Channel*, iClient*, std::string_view) override;
    virtual void OnCreate(Channel*, iClient*, ChannelUser*) override;
    virtual void OnBurstJoin(Channel*, iClient*, ChannelUser*) override;

    /**
     * This method is invoked when a channel kick occurs.
     */
    virtual void OnNetworkKick(Channel*, iClient*, iClient*, const std::string&, bool) override;

    /**
     * This method is called when a registered timer
     * expires.
     */
    virtual void OnTimer(const xServer::timerID&, void*) override;

    /**
     * Return the part message stats will use when it parts
     * a channel.
     */
    inline const std::string& getPartMessage() const { return partMessage; }

    /**
     * Set the part message stats will use when it parts
     * a channel.
     */
    inline void setPartMessage(const std::string& newVal) { partMessage = newVal; }

    /**
     * Send the current running stats to the given iClient.
     */
    virtual void dumpStats(iClient*);

  protected:
    /**
     * Count one event, which is all any of the handlers above does.
     */
    void countEvent(eventType);

    /**
     * WriteLog() will flush all data to the log files.
     */
    virtual void writeLog();

    /**
     * Opens all of the log files.
     */
    virtual void openLogFiles();

    /**
     * Return true if the given account name is permitted
     * access to this module.
     */
    virtual bool hasAccess(const std::string&) const;

    /// True if the stats should log during net burst (of gnuworld)
    bool logDuringBurst;

    /// True if opers can use this service, false otherwise
    bool allowOpers;

    /// list of account names who can use this service
    std::list<std::string> allowAccess;

    /// The path to the directory in which to store the logs files.
    /// If the path does not exist, it will NOT be created.
    std::string data_path;

    /// The message to use when parting a channel.
    std::string partMessage;

    /// The absolute time at which stats collection began.
    time_t startTime;

    /// This variable holds the totals for each event,
    /// and is reset each minute when the log files are
    /// written.
    unsigned long int eventMinuteTotal[eventNames.size()];

    /// This variable holds the totals for each event
    /// since time of connect (optionally excluding net
    /// bursts).
    unsigned long int eventTotal[eventNames.size()];

    /// This variable holds pointers to the individual
    /// log files.
    std::ofstream fileTable[eventNames.size()];

    /// The name of the file to which channel information will
    /// be written.
    std::string channelInfoFileName;

    /// The name of the file to which user information will be
    // written.
    std::string userInfoFileName;
};

} // namespace gnuworld

#endif // __STATS_h
