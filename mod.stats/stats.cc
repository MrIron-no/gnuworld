/**
 * stats.cc
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
 * $Id: stats.cc,v 1.31 2005/01/12 03:50:37 dan_karrels Exp $
 */
#include <list>
#include <string>
#include <map>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <ctime>
#include "stats.h"
#include "iClient.h"
#include "server.h"
#include "client.h"
#include "EConfig.h"
#include "ELog.h"
#include "StringTokenizer.h"
#include "Network.h"
#include "gnuworld_config.h"
#include "misc.h"

namespace gnuworld {

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::stringstream;

/// The column dumpStats() pads an event name out to, and so the longest an event
/// name in events.h may be.
static constexpr std::size_t eventNameColumn = 23;

static constexpr bool everyEventNameFitsColumn() {
    for (const std::string_view name : eventNames) {
        if (name.size() > eventNameColumn) {
            return false;
        }
    }
    return true;
}

/*
 * The table of dumpStats() used to pad by counting down from the column width
 * to the name's length, so a name one character too long wrapped the count and
 * hung the dump.  The padding no longer can wrap, and this says a name never
 * gets that long in the first place.
 */
static_assert(everyEventNameFitsColumn(),
              "an event name in events.h is longer than the stats table column:"
              " widen eventNameColumn");

/*
 *  Exported function used by moduleLoader to gain an
 *  instance of this module.
 */
extern "C" {
xClient* _gnuwinit(const string& args) { return new stats(args); }
}

stats::stats(const string& fileName) : xClient(fileName) {
    EConfig conf(fileName);

    data_path = conf.Require("data_path")->second;
    if (data_path.empty()) {
        cerr << "stats> Invalid data_path" << endl;
        ::exit(-1);
    }

    // Make sure that the last char is a '/'
    if ('/' != data_path[data_path.size() - 1]) {
        data_path += "/";
    }
    // elog	<< "stats> data_path: "
    //	<< data_path
    //	<< endl ;

    allowOpers = false;
    string stringOperAccess = conf.Require("allow_opers")->second;
    if (!strcasecmp(stringOperAccess, "yes") || !strcasecmp(stringOperAccess, "true")) {
        allowOpers = true;
    }

    EConfig::const_iterator ptr = conf.Find("permit_user");
    while (ptr != conf.end() && ptr->first == "permit_user") {
        allowAccess.push_back(ptr->second);
        ++ptr;
    }

    string ldb = conf.Require("logDuringBurst")->second;
    if (ldb == "true") {
        logDuringBurst = true;
    } else {
        logDuringBurst = false;
    }

    partMessage = conf.Require("part_message")->second;
    startTime = 0;

    channelInfoFileName = data_path + "users_per_channel";
    userInfoFileName = data_path + "channels_per_user";

    openLogFiles();

    memset(eventMinuteTotal, 0, sizeof(eventMinuteTotal));
    memset(eventTotal, 0, sizeof(eventTotal));
}

stats::~stats() {
    for (std::size_t whichEvent = 0; whichEvent < eventNames.size(); ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }
        fileTable[whichEvent].flush();
        fileTable[whichEvent].close();
    }
}

void stats::openLogFiles() {
    // For each possible event, open a log file to receive minutely
    // event totals.
    // Note that the file names will be retrieve from the eventNames
    // array, and spaces (' ') will be substituted with underscore ('_').
    for (std::size_t whichEvent = 0; whichEvent < eventNames.size(); ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }

        string fileName(eventNames[whichEvent]);

        // Substitute ' ' for '_'
        for (string::iterator sItr = fileName.begin(); sItr != fileName.end(); ++sItr) {
            if (' ' == *sItr) {
                *sItr = '_';
            }
        } // for( sItr )

        // Update the fileName to include the full path
        fileName = data_path + fileName;

        // File not yet opened
        fileTable[whichEvent].open(fileName.c_str(), std::ios::out | std::ios::trunc);
        if (!fileTable[whichEvent].is_open()) {
            elog << "stats::openLogFiles> Unable to open file: " << fileName << endl;
            return;
        }

        //	elog	<< "stats::openLogFiles> Opened log file: "
        //		<< fileName
        //		<< endl ;
    } // for( whichEvent )
}

void stats::OnAttach() {
    xClient::OnAttach();

    // Register for all events
    for (std::size_t whichEvent = 0; whichEvent < networkEventCount; ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }
        MyUplink->RegisterEvent(whichEvent, this);
    } // for()

    MyUplink->RegisterChannelEvent("*", this);

    // Start flushing data to the log files every minute
    scheduleLogFlush();
}

void stats::scheduleLogFlush() {
    MyUplink->RegisterTimer(::time(0) + 60, this, [this]() {
        // Timed events are once-run, make sure to book the next one
        // 1 minute from now
        scheduleLogFlush();

        // Flush logs
        writeLog();

        // Reset the minutely event counters
        memset(eventMinuteTotal, 0, sizeof(eventMinuteTotal));
    });
}

void stats::OnPrivateNotice(iClient* theClient, const string& theMessage, bool secure) {
    // elog	<< "stats::OnPrivateNotice> theClient: "
    //	<< *theClient
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;
    xClient::OnPrivateNotice(theClient, theMessage, secure);
}

void stats::OnChannelNotice(iClient* theClient, Channel* theChan, const string& theMessage) {
    // elog	<< "stats::OnChannelNotice> theClient: "
    //	<< *theClient
    //	<< ", theChan: "
    //	<< *theChan
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;
    xClient::OnChannelNotice(theClient, theChan, theMessage);
}

void stats::OnCTCP(iClient* theClient, const string& CTCPCommand, const string& theMessage,
                   bool secure) {
    // elog	<< "stats::OnCTCP> theClient: "
    //	<< *theClient
    //	<< ", CTCPCommand: "
    //	<< CTCPCommand
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;
    xClient::OnCTCP(theClient, CTCPCommand, theMessage, secure);
}

void stats::OnChannelMessage(iClient* theClient, Channel* theChan, const string& theMessage) {
    // elog	<< "stats::OnChannelMessage> theClient: "
    //	<< *theClient
    //	<< ", theChan: "
    //	<< *theChan
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;
    xClient::OnChannelMessage(theClient, theChan, theMessage);
}

void stats::OnChannelCTCP(iClient* theClient, Channel* theChan, const string& CTCPCommand,
                          const string& theMessage) {
    // elog	<< "stats::OnChannelCTCP> theClient: "
    //	<< *theClient
    //	<< ", theChan: "
    //	<< *theChan
    //	<< ", CTCPCommand: "
    //	<< CTCPCommand
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;
    xClient::OnChannelCTCP(theClient, theChan, CTCPCommand, theMessage);
}

void stats::OnPrivateMessage(iClient* theClient, const string& theMessage, bool) {
    // elog	<< "stats::OnPrivateMessage> theClient: "
    //	<< *theClient
    //	<< ", theMessage: "
    //	<< theMessage
    //	<< endl ;

    // Get rid of anyone who is not an oper and does not have access
    bool userHasAccess = hasAccess(theClient->getAccount());
    if (!userHasAccess && !theClient->isOper()) {
        // Normal user
        return;
    }

    if (!userHasAccess) {
        // The client must be an oper
        // Are opers allow to use the service?
        if (!allowOpers) {
            // Nope
            return;
        }
    }

    StringTokenizer st(theMessage);
    if (st.empty()) {
        return;
    }

    if (st[0] == "reload") {
        elog << "stats::OnPrivateMessage> Reloading" << endl;

        getUplink()->UnloadClient(this, "Keep smiling, I'm reloading...");
        getUplink()->LoadClient("libstats.la", getConfigFileName());
        return;
    }

    if (st[0] == "stats") {
        dumpStats(theClient);
        return;
    }

    if (st[0] == "shutdown") {
        MyUplink->Shutdown();
        return;
    }

    if (st.size() < 2) {
        // No commands from this point forward can be done without
        // one argument
        return;
    }

    if (st[0] == "join") {
        elog << "stats::OnPrivateMessage> Joining: " << st[1] << endl;
        Join(st[1]);
        return;
    }

    if (st[0] == "part") {
        //	elog	<< "stats::OnPrivateMessage> Part: "
        //		<< st[ 1 ]
        //		<< endl ;
        Part(st[1], partMessage);
        return;
    }

    if (st.size() < 3) {
        return;
    }

    if (st[0] == "say") {
        if (!isOnChannel(st[1])) {
            Notice(theClient, "Im not on channel %s", st[1].c_str());
            return;
        }

        Channel* theChan = Network->findChannel(st[1]);
        if (0 == theChan) {
            Notice(theClient,
                   "I have no information about channel "
                   "%s",
                   st[1].c_str());
            return;
        }

        Message(theChan, st.assemble(2));
        return;
    }
}

void stats::writeLog() {
    // Should we log during a net burst
    if (!logDuringBurst && MyUplink->isBursting()) {
        // Don't log
        return;
    }

    // Get the current time
    time_t now = ::time(0);
    struct tm* nowTM = gmtime(&now);

    for (std::size_t whichEvent = 0; whichEvent < eventNames.size(); ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }

        std::ofstream& outFile = fileTable[whichEvent];

        outFile << nowTM->tm_hour << ":" << nowTM->tm_min << ":" << nowTM->tm_sec << " "
                << eventMinuteTotal[whichEvent] << endl;
    }
}

void stats::countEvent(eventType whichEvent) {
    if (!logDuringBurst && MyUplink->isBursting()) {
        // Don't log
        return;
    }

    if (0 == startTime) {
        startTime = ::time(0);
    }

    eventMinuteTotal[whichEvent]++;
    eventTotal[whichEvent]++;
}

/*
 * One handler per event, each naming the counter it belongs to, in the order
 * events.h numbers them - except OnJoin(), which is handed three, one for each
 * way a membership arrives, and counts each of them separately still.  See
 * stats.h for the three events that are not here.
 */

void stats::OnOper(iClient*) { countEvent(EVT_OPER); }

void stats::OnNetBreak(iServer*, const iServer*, std::string_view) { countEvent(EVT_NETBREAK); }

void stats::OnNetJoin(iServer*, const iServer*) { countEvent(EVT_NETJOIN); }

void stats::OnBurstComplete(iServer*) { countEvent(EVT_BURST_CMPLT); }

void stats::OnBurstAck(iServer*) { countEvent(EVT_BURST_ACK); }

void stats::OnEndOfBurstAckSent(iServer*) { countEvent(EVT_EA_SENT); }

void stats::OnGline(Gline*) { countEvent(EVT_GLINE); }

void stats::OnRemGline(Gline*) { countEvent(EVT_REMGLINE); }

void stats::OnQuit(iClient*, std::string_view) { countEvent(EVT_QUIT); }

void stats::OnKill(const NetworkTarget*, iClient*, std::string_view) { countEvent(EVT_KILL); }

void stats::OnNick(iClient*) { countEvent(EVT_NICK); }

void stats::OnNickChange(iClient*, std::string_view) { countEvent(EVT_CHNICK); }

void stats::OnAccount(iClient*) { countEvent(EVT_ACCOUNT); }

void stats::OnAccountFlags(iClient*) { countEvent(EVT_ACCOUNT_FLAGS); }

void stats::OnXQuery(iServer*, std::string_view, std::string_view) { countEvent(EVT_XQUERY); }

void stats::OnXReply(iServer*, std::string_view, std::string_view) { countEvent(EVT_XREPLY); }

void stats::OnNetConf(iServer*, std::string_view) { countEvent(EVT_NETCONF); }

void stats::OnRemNetConf(iServer*, std::string_view) { countEvent(EVT_REMNETCONF); }

void stats::OnJoin(Channel*, iClient*, ChannelUser*, JoinKind kind) {
    countEvent(channelEventOf(kind));
}

void stats::OnPart(Channel*, iClient*, std::string_view) { countEvent(EVT_PART); }

void stats::OnServerMode(Channel*, iServer*) { countEvent(EVT_SERVERMODE); }

void stats::OnTopic(Channel*, iClient*, std::string_view) { countEvent(EVT_TOPIC); }

void stats::OnNetworkKick(Channel* theChan, iClient* srcClient, iClient* destClient,
                          const string& kickMessage, bool authoritative) {
    countEvent(EVT_KICK);

    xClient::OnNetworkKick(theChan, srcClient, destClient, kickMessage, authoritative);
}

void stats::dumpStats(iClient* theClient) {
    // startTime is 0 until the first event arrives, so counting from it before
    // then would make the elapsed time the whole Unix epoch.  Average over at
    // least a second, because a dump in the same second as the first event
    // would otherwise divide by 0 and make every average inf.
    time_t countingTime = (0 == startTime) ? 0 : ::time(nullptr) - startTime;
    time_t averageTime = (countingTime > 0) ? countingTime : 1;

    Notice(theClient, "I have been counting for {} seconds", countingTime);
    Notice(theClient, "Total Network Users: {}, Total Network Channels: {}",
           Network->clientList_size(), Network->channelList_size());

    typedef std::map<size_t, size_t> channelUserInfoMapType;
    channelUserInfoMapType usersPerChannelMap;

    std::ofstream usersPerChannelFile(channelInfoFileName.c_str());
    if (!usersPerChannelFile) {
        cout << "stats::dumpStats> Failed to open channel info file: " << channelInfoFileName
             << endl;
    }

    Channel* largestChan = 0;
    for (xNetwork::const_channelIterator chanItr = Network->channels_begin();
         chanItr != Network->channels_end(); ++chanItr) {
        usersPerChannelMap[chanItr->second->size()]++;

        if (0 == largestChan) {
            largestChan = chanItr->second;
        } else if (chanItr->second->size() > largestChan->size()) {
            largestChan = chanItr->second;
        }
    }

    usersPerChannelFile << "Channel size\tNumber of channels of that size" << endl;
    for (channelUserInfoMapType::const_iterator itr = usersPerChannelMap.begin();
         itr != usersPerChannelMap.end(); ++itr) {
        usersPerChannelFile << itr->first << "\t" << itr->second << endl;
    } // for( itr )

    if (usersPerChannelFile) {
        usersPerChannelFile.close();
    }

    if (largestChan != 0) {
        Notice(theClient, "Largest channel is {}, with {} users", largestChan->getName(),
               largestChan->size());
    }

    channelUserInfoMapType channelsPerUserMap;
    size_t maxChannels = 0;

    std::ofstream channelsPerUserFile(userInfoFileName.c_str());
    if (!channelsPerUserFile) {
        cout << "stats::dumpStats> Unable to open user info file: " << userInfoFileName.c_str()
             << endl;
    }

    for (xNetwork::const_clientIterator cItr = Network->clients_begin();
         cItr != Network->clients_end(); ++cItr) {
        if (!cItr->second->isModeK()) {
            channelsPerUserMap[cItr->second->channels_size()]++;

            if (cItr->second->channels_size() > maxChannels) {
                maxChannels = cItr->second->channels_size();
            }
        }
    } // for( cItr )

    channelsPerUserFile << "Number of channels\t"
                        << "Number of users with that many channels" << endl;

    for (channelUserInfoMapType::const_iterator itr = channelsPerUserMap.begin();
         itr != channelsPerUserMap.end(); ++itr) {
        channelsPerUserFile << itr->first << "\t" << itr->second << endl;
    } // for( itr )

    if (channelsPerUserFile) {
        channelsPerUserFile.close();
    }

    Notice(theClient, "Maximum channels joined by a user: {}", maxChannels);

    Notice(theClient,
           "Length of last burst: {} seconds, "
           "Number of bytes processed since beginning of last "
           "burst: {}",
           MyUplink->getLastBurstDuration(), MyUplink->getBurstBytes());

    {
        stringstream ss;

        ss.width(eventNameColumn);
        ss.setf(std::ios::left);
        ss << "EventName";

        ss.width(12);
        ss.setf(std::ios::left);
        ss << "EventCount";

        ss.width(15);
        ss.setf(std::ios::left);
        ss << "Average";

        Notice(theClient, "%s", ss.str().c_str());
    }

    unsigned long int totalEvents = 0;

    // First, find the total number of events to occur
    for (std::size_t whichEvent = 0; whichEvent < eventNames.size(); ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }
        totalEvents += eventTotal[whichEvent];
    }

    // Now output number of each event, and percentage of that
    // event to the total events received
    for (std::size_t whichEvent = 0; whichEvent < eventNames.size(); ++whichEvent) {
        if (EVT_RAW == whichEvent) {
            continue;
        }

        string writeMe;
        stringstream ss;

        ss.str(string());
        ss.setf(std::ios::left);
        ss.fill(' ');
        //	ss.width( 25 ) ;
        ss << eventNames[whichEvent];
        writeMe = ss.str();

        // Pad the name out to its column.  Growing writeMe to the column
        // cannot wrap the way subtracting the name's length from it could.
        writeMe.resize(eventNameColumn, ' ');

        ss.str(string());
        ss.width(12);
        ss.setf(std::ios::left);
        ss << eventTotal[whichEvent];
        writeMe += ss.str();

        ss.str(string());
        ss.width(15);
        ss.setf(std::ios::left);
        ss << ((double)eventTotal[whichEvent] / (double)averageTime);
        writeMe += ss.str();

        Notice(theClient, "%s", writeMe.c_str());
    }

    Notice(theClient, "Total Events: {}, Total Average Events/Second: {:f}", totalEvents,
           (double)totalEvents / (double)averageTime);
}

bool stats::hasAccess(const string& accountName) const {
    for (std::list<string>::const_iterator itr = allowAccess.begin(); itr != allowAccess.end();
         ++itr) {
        if (!strcasecmp(accountName, *itr)) {
            return true;
        }
    }
    return false;
}

} // namespace gnuworld
