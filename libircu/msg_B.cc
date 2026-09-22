/**
 * msg_B.cc
 * Author: Daniel Karrels (dan@karrels.com)
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
 * $Id: msg_B.cc,v 1.12 2008/04/16 20:29:37 danielaustin Exp $
 */

#include <sys/types.h>
#include <sys/time.h>

#include <new>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <format>
#include <span>
#include <vector>
#include <iostream>
#include <utility>

#include <cassert>

#include "gnuworld_config.h"
#include "server.h"
#include "xparameters.h"
#include "StringTokenizer.h"
#include "Channel.h"
#include "ChannelUser.h"
#include "Network.h"
#include "iClient.h"
#include "ServerCommandHandler.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(Proto);

namespace gnuworld {
using std::endl;
using std::make_pair;
using std::pair;
using std::string;
using std::vector;

class msg_B : public ServerCommandHandler {
  public:
    msg_B(xServer* theServer) : ServerCommandHandler(theServer, "msg_B>") {}
    virtual ~msg_B() {}

    virtual bool Execute(const xParameters&);

  protected:
    void parseBurstUsers(Channel*, const string&, bool incomingIsNewer);
    void parseBurstBans(Channel*, const string&, const string& burstSource);

    string sourceServerName(const xParameters&) const;
};

CREATE_LOADER(msg_B)

// MSG_B
// This is the BURST command.
// 0 B #merida 957075177 +tn BAA:o
// 0 B #ValHalla 000031335 +stn CAA:o
// 0 B #linux 801203210 +stn OBL,LLf:ov,MBU:o,OBE,MAg,MAh
// 0 B #mylinux 953234759 +tn OBL,MBU:o,OBE,MAg,MAh
// 0 B #krushnet 000031337 +tn LEM,qAD,OAi,2B6,kA],kA9,qAA,2B2,NCR,kAD,OAC,0DK:o,MAL,zDj
// :%*!*lamer@*lamer.lamer.lamer.lamer11.com *!*lamer@*lamer.lamer.lamer.lamer10.com
// *!*lamer@*lamer.lamer.lamer.lamer9.com *!*lamer@*lamer.lamer.lamer.lamer8.com
// *!*lamer@*lamer.lamer.lamer.lamer7.com *!*lamer@*lamer.lamer.lamer.lamer6.com
// *!*lamer@*lamer.lamer.lamer.lamer5.com *!*lamer@*lamer.lamer.lamer.lamer4.com
// *!*lamer@*lamer.lamer.lamer.lamer3.com *!*lamer@*lamer.lamer.lamer.lamer2.com
//
// Q B #ateneo 848728923 +tnl 2000 r]Q,ZLC,Smt,rGN,gPk,uhy,Z]N,oTL,uem,31b,Znt,
//  3x3,oC0,TvC,3vs,oSo,IP7,oXL,aF2,CW9,sTq,Znw,Is9,gPD,rI1,ToI,ZZK,oGB,$Q
// B #ateneo 848728923 4Qt,LE2,LXJ,3ys,oIG,lwc,TQX,HwR,3iZ,g2D,ZP3,3m2,uPi,Z0n,
//  LTi,oG[,a3N,IH4,T3T,La],goY,geE,sar,oid,o90,35Y,TUL,Z7K,Zx7,TN1,C6$Q
// B #ateneo 848728923 :%*!*@203.145.226.149 *!*@203.177.4.* *!*@203.145.226.134
// *!*@202.8.230.*
//
// Q B #hgsd 933357379 +tn PIs,OfK,OAu,PZl:o,eAA
//
// Here is a special case, when the only occupant of the channel
// is a zombie:
// BG B #nails 1036089823
// We ignore this case.
//
// P11 adds a delayed-join bucket, flagged ":d".  Those clients are on
// the channel but not yet revealed, and hold no op or voice:
// AB B #chan 1700000000 +tnD ABAAA:o,ABAAB,ABAAC:d,ABAAD
//
// In P11 each ban is burst as a "mask timestamp setter" triplet:
// AB B #chan 1700000000 +tn ABAAA:o :%*!*@bad.host 1700000100 nick
// *!*@other.host 1700000200 some.server.net
//
// With u2.10.12's oplevels, we have to deal with an extra case:
// <numeric> B <channel> <ts> <modes...> <numnick>:<oplevel>
// Ck B #test 1128024142 +tnAU apass upass BBAAA:999
//
bool msg_B::Execute(const xParameters& Param) {
    // Make sure there are at least four arguments supplied:
    // servernumeric #channel time_stamp arguments
    requireParameters(Param, 3);

    // Attempt to find the channel in the network channel table
    Channel* theChan = Network->findChannel(Param[1]);

    // True if we already know this channel with an older timestamp, in
    // which case the incoming channel lost and its members are fresh
    // joins to ours.  Decides how hidden members are interpreted.
    const time_t burstTS = requireTimestamp(Param[2]);
    const bool incomingIsNewer = (theChan != NULL) && (theChan->getCreationTime() < burstTS);

    // Who a ban of this burst is recorded as having been set by, where the
    // burst itself does not say: the server it came from, as ircu does it.
    const string burstSource = sourceServerName(Param);

    // Was the channel found?
    if (NULL == theChan) {
        // The channel does not yet exist, go ahead and create it.
        theChan = new (std::nothrow) Channel(Param[1], burstTS);
        assert(theChan != 0);

        // Add the new Channel to the network channel table
        if (!Network->addChannel(theChan)) {
            // The addition of this channel failed, *shrug*
            LOG(ERROR, "Failed to add channel: {}", std::string(Param[1]));

            // Prevent a memory leak by deleting the channel
            destroy(theChan);
            theChan = 0;

            // Return error
            return false;
        }
    } // if( NULL == theChan )
    else {
        // The channel was already found.
        // Make sure the timestamp is accurate, this is an oddity imho.
        const time_t newCreationTime = burstTS;

        // Is the old TS greater than the new TS?
        if (theChan->getCreationTime() > newCreationTime) {
            // Nope, update the timestamp
            setCreationTime(theChan, newCreationTime);
            removeAllModes(theChan);
            removeAllBans(theChan);
        }
    }

    if (3 == Param.size()) {
        // Zombie in channel.
        // If the user is dezombified, the client will be shown
        // to issue a "J", and the channel will be created anyway.
        // Only difference is the timestamp difference.
        return true;
    }

    // Parse out the channel state
    xParameters::size_type whichToken = 3;

    // Channel modes will always be the first thing to follow if it's in the burst
    if ('+' == Param[whichToken][0]) {
        // The arguments of the mode block are followed by the member list,
        // so leftovers are expected; argsUsed says where the members start.
        const std::vector<std::string_view> args = Param.views(whichToken + 1);
        const Channel::ParsedModes parsed = Channel::parseModes(
            Param[whichToken], args,
            {.allowLeftover = true, .protocol = theServer->getUplink()->getProtocol()});
        if (!parsed.ok()) {
            protocolError(parsed.problems);
        }
        theServer->ApplyChannelModes(theChan, 0, parsed.changes, where, burstSource);

        whichToken += 1 + parsed.argsUsed;
    }

    // Have we reached the end of this burst command?
    if (whichToken >= Param.size()) {
        return true;
    }

    // Parse the remaining tokens
    for (; whichToken < Param.size(); ++whichToken) {
        // Bans will always be the last thing burst, so no users
        // will be burst afterwards.  This is useful because xParameters
        // will only delimit tokens by ':', so the ban string is guaranteed
        // to be caught.
        if ('~' == Param[whichToken][0]) {
            // Channel ban exceptions
            // Be sure to skip over the '%'
            // parseBurstExcepts( theChan, Param[ whichToken ] + 1 ) ;
        } else if ('%' == Param[whichToken][0]) {
            // Channel bans
            // Be sure to skip over the '%'
            parseBurstBans(theChan, Param[whichToken] + 1, burstSource);
        } else {
            // Userlist
            parseBurstUsers(theChan, Param[whichToken], incomingIsNewer);
        }
    }
    return true;
}

// dA1,jBN:ov,C3K:v,jGZ:o,CkU
// ':' indicates a mode state. Eg: ':ov' indicates this and
// all the following numerics are opped and voiced up to the next
// mode state.
// Mode states will always be in the order ov, v, o if present
// at all.
void msg_B::parseBurstUsers(Channel* theChan, const string& theUsers, bool incomingIsNewer) {
    // This is a protected method, so the method arguments are
    // guaranteed to be valid
    // elog	<< "msg_B::parseBurstUsers> Channel: " << theChan->getName()
    //	<< ", users: " << theUsers << endl ;
    //  Parse out users and their modes
    StringTokenizer st(theUsers, ',');

    // Tracks the mode state of the current bucket as a bitmask.
    // 0 = none (also P11 delayed join), 1 = op, 2 = voice, 3 = opvoice.
    unsigned short int mode_state = 0;

    // True while the current bucket is the P11 hidden (":d") group.
    bool bucketHidden = false;

    const bool isP11 = theServer->getUplink()->getProtocol() >= 11;

    typedef xServer::opVectorType opVectorType;
    typedef xServer::voiceVectorType voiceVectorType;

    opVectorType opVector;
    voiceVectorType voiceVector;

    for (StringTokenizer::const_iterator ptr = st.begin(); ptr != st.end(); ++ptr) {
        // Each token is of the form:
        // abc or abc:modes
        string::size_type pos = (*ptr).find_first_of(':');
        const string numeric((*ptr).substr(0, pos));

        // Parse the suffix before looking the client up, as ircu does:
        // the bucket state must advance even if this member is skipped.
        if (string::npos != pos) {
            // A suffix opens a new bucket.  As in ircu's m_burst, the first
            // 'o', 'v' or 'd' in it replaces the mode state for this client
            // and for every suffix-less client that follows.
            bool needsReset = true;

            for (pos++; pos < (*ptr).size(); ++pos) {
                const char flag = (*ptr)[pos];

                if ('o' == flag) {
                    if (needsReset) {
                        mode_state = 0;
                        needsReset = false;
                    }
                    mode_state |= 1;
                    bucketHidden = false;
                } else if ('v' == flag) {
                    if (needsReset) {
                        mode_state = 0;
                        needsReset = false;
                    }
                    mode_state |= 2;
                    bucketHidden = false;
                } else if ('d' == flag) {
                    /* P11: delayed join.  The client is on the channel
                     * but its JOIN has not been shown yet.  A hidden
                     * member never has status, so 'd' combined with
                     * 'o' or 'v' is just that status.
                     */
                    if (needsReset) {
                        mode_state = 0;
                        needsReset = false;
                    }
                    bucketHidden = (0 == mode_state);
                } else if (flag >= '0' && flag <= '9') {
                    /* An oplevel, which ircu sends with OPLEVELS on.  We do
                     * not support oplevels, and Undernet does not use them;
                     * it is skipped so that it does not count as an error.
                     */
                } else {
                    LOG(WARN, "Unknown mode: {}", flag);
                }
            } // for()
        }

        // Is this member hidden (delayed join)?  Only a status-less
        // member can be, and only on a P11 uplink.  See ircu doc/P11.md 8.1:
        //  - our timestamp equal or older: hidden iff it carried 'd'.
        //  - our timestamp newer: the incoming channel lost, so 'd' is
        //    ignored and the member is hidden iff our channel is +D.
        // On a P10 uplink nothing is ever flagged.  P10 has no REVEAL, so a
        // member that speaks could only be revealed by seeing its message,
        // and we see channel messages only where we have a client.  The
        // flag would be stale on nearly every channel, which is worse than
        // not having it.
        bool memberHidden = false;
        if (isP11 && 0 == mode_state) {
            memberHidden =
                incomingIsNewer ? theChan->getMode(Channel::MODE_DELJOINS) : bucketHidden;
        }

        // Find the client in the client table
        iClient* theClient = Network->findClient(numeric);

        // Was the search successful?
        if (NULL == theClient) {
            // Nope, no such user
            // Log the error
            LOG_MSG(WARN, "({chan}): Unable to find client: {}", numeric)
                .with("chan", theChan)
                .log();

            // Skip this user
            continue;
        }

        //	elog	<< "msg_B::parseBurstUsers> Adding user "
        //		<< theClient->getNickName()
        //		<< "(" << theClient->getCharYYXXX() << ") to channel "
        //		<< theChan->getName() << endl ;

        // Add this channel to the user's channel structure.
        if (!theClient->addChannel(theChan)) {
            LOG_MSG(ERROR, "Failed to add channel {chan} to iClient {client}")
                .with("chan", theChan)
                .with("client", theClient)
                .log();

            // Non-fatal error
            continue;
        }

        // Create a ChannelUser object to represent this user's presence
        // in this channel.  Its op and voice follow, with their events.
        ChannelUser* chanUser = new (std::nothrow) ChannelUser(theClient, 0, memberHidden);
        assert(chanUser != 0);

        // Add this user to the channel's database.
        if (!addUser(theChan, chanUser)) {
            // The addition failed
            LOG_MSG(ERROR, "Unable to add user {client} to channel {chan}")
                .with("client", theClient)
                .with("chan", theChan)
                .log();

            // Prevent a memory leak by deallocating the unused
            // ChannelUser object
            destroy(chanUser);
            chanUser = 0;

            // Remove the channel info from the client
            theClient->removeChannel(theChan);

            continue;
        }

        // Notify the services clients that a user has
        // joined the channel
        theServer->postBurstJoin(theChan, theClient, chanUser);

        // Apply the current bucket's state to this client.
        if (mode_state & 1) {
            opVector.push_back(opVectorType::value_type(true, chanUser));
        }
        if (mode_state & 2) {
            voiceVector.push_back(voiceVectorType::value_type(true, chanUser));
        }

    } // while( ptr != st.end() )

    // Commit the user modes to the internal tables, and notify
    // all listening clients
    if (!opVector.empty()) {
        theServer->OnChannelModeO(theChan, 0, opVector);
    }
    if (!voiceVector.empty()) {
        theServer->OnChannelModeV(theChan, 0, voiceVector);
    }
}

/**
 * The name of the server a BURST came from, or an empty string when the
 * network table does not have it (yet).  The source is a server numeric, or
 * a name during a link's own handshake; msg_M reads its source the same way.
 */
string msg_B::sourceServerName(const xParameters& Param) const {
    const iServer* sourceServer = (NULL != strchr(Param[0], '.'))
                                      ? Network->findServerName(Param[0])
                                      : Network->findServer(Param[0]);
    if (0 == sourceServer) {
        return string();
    }
    return sourceServer->getName();
}

void msg_B::parseBurstBans(Channel* theChan, const string& theBans, const string& burstSource) {
    // This is a protected method, so the method arguments are
    // guaranteed to be valid

    // elog	<< "msg_B::parseBurstBans> Found bans for channel "
    //	<< theChan->getName()
    //	<< ": "
    //	<< theBans
    //	<< endl ;

    // Tokenize the ban string
    StringTokenizer st(theBans);

    // P10 bursts a flat list of masks.  P11 bursts each ban as a
    // "mask timestamp setter" triplet.
    const bool isP11 = theServer->getUplink()->getProtocol() >= 11;
    const StringTokenizer::size_type stride = isP11 ? 3 : 1;

    // Validate the framing of the whole section before applying any of it
    if (isP11) {
        if ((st.size() % stride) != 0) {
            const std::string problem =
                std::format("the ban section has {} tokens, not a multiple of 3", st.size());
            protocolError(std::span(&problem, 1));
        }
    }

    typedef xServer::banVectorType banVectorType;
    banVectorType banVector;
    banVector.reserve(st.size() / stride);

    // Who set each ban and when.  A P11 triplet says so; a P10 burst does
    // not, so such a ban is recorded as the bursting server's, set now,
    // which is what ircu does with one.
    vector<Channel::BanInfo> banInfo;
    banInfo.reserve(st.size() / stride);

    for (StringTokenizer::size_type i = 0; i < st.size(); i += stride) {
        banVector.push_back(banVectorType::value_type(true, st[i]));
        if (isP11) {
            // st[ i ]: mask, st[ i + 1 ]: time the ban was set,
            // st[ i + 2 ]: who set it.
            //
            // The time is what some server once wrote down and every
            // server since has passed on: one that is no number is worth
            // a warning, not the link.  The ban is as good as any other,
            // and is recorded as set now, which is when we learnt of it.
            const string& banTS = st[i + 1];
            std::optional<time_t> setAt;
            if (!banTS.empty() && banTS.find_first_not_of("0123456789") == string::npos) {
                setAt = parseNumber<time_t>(banTS);
            }
            if (!setAt) {
                LOG(WARN, "Ban {} of {}, burst by {}, has a timestamp that is no number: {}", st[i],
                    theChan->getName(), burstSource, banTS);
            }
            banInfo.push_back(Channel::BanInfo{st[i + 2], setAt.value_or(0)});
        } else {
            banInfo.push_back(Channel::BanInfo{burstSource, 0});
        }
    }

    if (!banVector.empty()) {
        theServer->OnChannelModeB(theChan, 0, banVector, banInfo);
    }
}

} // namespace gnuworld
