/*
 * Channel.cc
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
 * $Id: Channel.cc,v 1.55 2008/04/16 20:29:37 danielaustin Exp $
 */
#include <new>
#include <map>
#include <string>
#include <vector>
#include <string_view>
#include <span>
#include <optional>
#include <algorithm>
#include <iostream>
#include <sstream>
#include "Channel.h"
#include "iClient.h"
#include "ChannelUser.h"
#include "Network.h"
#include "xparameters.h"
#include "StringTokenizer.h"
#include "match.h"
#include "misc.h"
#include "server.h"
#include "logger.h"

GNUWORLD_MODULE_LOGGER("core.state");

namespace gnuworld {
using std::string;
using std::stringstream;
using std::vector;

Channel::Channel(const string& _name, const time_t& _creationTime)
    : name(_name), creationTime(_creationTime), modes(0), limit(0)
#ifdef TOPIC_TRACK
      ,
      topic_ts(0)
#endif
{
}

Channel::~Channel() {
    // Deallocate all ChannelUser's that are left
    userIterator currentPtr = userList_begin();
    userIterator endPtr = userList_end();
    for (; currentPtr != endPtr; ++currentPtr) {
        delete currentPtr->second;
    }
    userList.clear();
}

bool Channel::addUser(ChannelUser* newUser) {
    assert(newUser != 0);

#ifdef USE_THREAD
    std::unique_lock<std::shared_mutex> lock(chanMutex);
#endif

    // elog	<< "Channel::addUser> ("
    //	<< getName()
    //	<< ") Number of users: "
    //	<< userList.size()
    //	<< endl ;

    if (!userList.insert(userListType::value_type(newUser->getIntYYXXX(), newUser)).second) {
        LOG_MSG(WARN, "({chan}): Unable to add user: {client}")
            .with("chan", this)
            .with("client", newUser)
            .log();
        return false;
    }

    // elog	<< "Channel::addUser> " << name << " added user: "
    //	<< *newUser << endl ;

    return true;
}

bool Channel::addUser(iClient* theClient) {
    ChannelUser* addMe = new (std::nothrow) ChannelUser(theClient);

    // The signature of addUser() here will verify the pointer
    return addUser(addMe);
}

ChannelUser* Channel::removeUser(ChannelUser* theUser) {
    assert(theUser != 0);

    return removeUser(theUser->getClient()->getIntYYXXX());
}

ChannelUser* Channel::removeUser(iClient* theClient) {
    // This method is public, so the pointer must be validated
    assert(theClient != 0);

    return removeUser(theClient->getIntYYXXX());
}

ChannelUser* Channel::removeUser(const unsigned int& intYYXXX) {
#ifdef USE_THREAD
    std::unique_lock<std::shared_mutex> lock(chanMutex);
#endif

    ChannelUser* tmpUser;

    // Attempt to find the user in question
    userIterator ptr = userList.find(intYYXXX);

    // Was the user found?
    if (ptr != userList.end()) {
        // Yup, go ahead and remove the user from the userList
        tmpUser = ptr->second;

        userList.erase(ptr);

        // Return a pointer to the ChannelUser
        return tmpUser;
    }

    // Otherwise, the user was NOT found
    // Log the error
    // Happens when not handling zombies
    // elog	<< "Channel::removeUser> ("
    //	<< getName() << ") "
    //	<< "Unable to find user: "
    //	<< intYYXXX
    //	<< std::endl ;

    // Return error state
    return 0;
}

ChannelUser* Channel::findUser(const iClient* theClient) const {
    assert(theClient != 0);

    const_userIterator ptr = userList.find(theClient->getIntYYXXX());
    if (ptr == userList_end()) {
        // User not found
        return 0;
    }
    return ptr->second;
}

bool Channel::removeUserMode(const ChannelUser::modeType& whichMode, iClient* theClient) {
    ChannelUser* theChanUser = findUser(theClient);
    if (NULL == theChanUser) {
        LOG_MSG(WARN, "({chan}) Unable to find user: {client_numeric}")
            .with("chan", this)
            .with("client", theClient)
            .log();
        return false;
    }
    theChanUser->removeMode(whichMode);
    return true;
}

bool Channel::setUserMode(const ChannelUser::modeType& whichMode, iClient* theClient) {
    // findUser() is also public, and so will verify theClient's pointer
    ChannelUser* theChanUser = findUser(theClient);
    if (NULL == theChanUser) {
        LOG_MSG(WARN, "({chan}) Unable to find user: {client_numeric}")
            .with("chan", this)
            .with("client", theClient)
            .log();
        return false;
    }
    theChanUser->setMode(whichMode);
    return true;
}

bool Channel::getUserMode(const ChannelUser::modeType& whichMode, iClient* theClient) const {
    // findUser() is also public, and so will verify theClient's pointer
    ChannelUser* theChanUser = findUser(theClient);
    if (NULL == theChanUser) {
        LOG_MSG(WARN, "({chan}) Unable to find user: {client_numeric}")
            .with("chan", this)
            .with("client", theClient)
            .log();
        return false;
    }
    return theChanUser->getMode(whichMode);
}

std::vector<Channel::ModeChange> Channel::changesToClear(std::string_view letters,
                                                         std::vector<std::string>& problems) const {
    std::vector<ModeChange> changes;

    for (const char letter : letters) {
        const std::optional<ModeInfo> mode = findMode(letter);
        if (!mode) {
            problems.push_back(describeNonMode(letter));
            continue;
        }

        switch (mode->type) {
        case ModeType::Flag:
        case ModeType::SetOnly:
            if (getMode(mode->flag)) {
                changes.push_back({false, *mode, std::string()});
            }
            break;
        case ModeType::Setting:
            // Taking one off names its current value
            if (getMode(mode->flag)) {
                changes.push_back({false, *mode,
                                   ('k' == letter)   ? getKey()
                                   : ('A' == letter) ? getApass()
                                                     : getUpass()});
            }
            break;
        case ModeType::Prefix:
            for (const auto& [id, member] : users()) {
                (void)id;
                if (('o' == letter) ? member->isModeO() : member->isModeV()) {
                    changes.push_back({false, *mode, member->getCharYYXXX()});
                }
            }
            break;
        case ModeType::List:
            for (const_banIterator ban = banList_begin(); ban != banList_end(); ++ban) {
                changes.push_back({false, *mode, *ban});
            }
            break;
        }
    }
    return changes;
}

bool Channel::revealUser(const iClient* theClient) {
    ChannelUser* theUser = findUser(theClient);
    if (0 == theUser || !theUser->isHidden()) {
        return false;
    }
    theUser->reveal();
    return true;
}

void Channel::setBan(const string& newBan) {
    // xServer will worry about removing conflicting bans
    banList.push_front(newBan);
}

bool Channel::removeBan(const string& banMask) {
    for (banIterator ptr = banList_begin(), end = banList_end(); ptr != end; ++ptr) {
        if (!strcasecmp(*ptr, banMask)) {
            banList.erase(ptr);
            // Ban found, return true
            return true;
        }
    }
    // Ban not found
    return false;
}

bool Channel::findBan(const string& banMask) const {
    for (const_banIterator ptr = banList_begin(), end = banList_end(); ptr != end; ++ptr) {
        if (!strcasecmp(*ptr, banMask)) {
            return true;
        }
    }
    return false;
}

bool Channel::matchBan(const string& banMask) const {
    for (const_banIterator ptr = banList_begin(), end = banList_end(); ptr != end; ++ptr) {
        if (!match(banMask, *ptr)) {
            // Found a match
            return true;
        }
    }
    return false;
}

bool Channel::getMatchingBan(const string& banMask, string& matchingBan) const {
    for (const_banIterator ptr = banList_begin(), end = banList_end(); ptr != end; ++ptr) {
        if (!match(banMask, *ptr)) {
            matchingBan = *ptr;
            return true;
        }
    }
    return false;
}

void Channel::onMode(const vector<std::pair<bool, Channel::modeType>>& modeVector) {
    typedef vector<std::pair<bool, Channel::modeType>> modeVectorType;
    for (modeVectorType::const_iterator mItr = modeVector.begin(); mItr != modeVector.end();
         ++mItr) {
        bool polarity = (*mItr).first;

        if (polarity)
            setMode((*mItr).second);
        else
            removeMode((*mItr).second);
    } // for()
}

void Channel::onModeL(bool polarity, const unsigned int& newLimit) {
    if (polarity) {
        setMode(MODE_LIMIT);
        setLimit(newLimit);
    } else {
        removeMode(MODE_LIMIT);
        setLimit(0);
    }
}

void Channel::onModeK(bool polarity, const string& newKey) {
    if (polarity) {
        setMode(MODE_KEY);
        setKey(newKey);
    } else {
        removeMode(MODE_KEY);
        setKey(string());
    }
}

void Channel::onModeA(bool polarity, const string& newApass) {
    if (polarity) {
        setMode(MODE_APASS);
        setApass(newApass);
    } else {
        removeMode(MODE_APASS);
        setApass(string());
    }
}

void Channel::onModeU(bool polarity, const string& newUpass) {
    if (polarity) {
        setMode(MODE_UPASS);
        setUpass(newUpass);
    } else {
        removeMode(MODE_UPASS);
        setUpass(string());
    }
}

void Channel::onModeO(const vector<std::pair<bool, ChannelUser*>>& opVector) {
    typedef vector<std::pair<bool, ChannelUser*>> opVectorType;
    for (opVectorType::const_iterator ptr = opVector.begin(); ptr != opVector.end(); ++ptr) {
        if (ptr->first) {
            ptr->second->setModeO();
        } else {
            ptr->second->removeModeO();
        }
    }
}

void Channel::onModeV(const vector<std::pair<bool, ChannelUser*>>& voiceVector) {
    typedef vector<std::pair<bool, ChannelUser*>> voiceVectorType;
    for (voiceVectorType::const_iterator ptr = voiceVector.begin(); ptr != voiceVector.end();
         ++ptr) {
        if (ptr->first) {
            ptr->second->setModeV();
        } else {
            ptr->second->removeModeV();
        }
    }
}

/**
 * The banVector passed to this method will be updated to
 * include any bans that have been removed as a result
 * of overlapping bans being added.
 * The order of these additions will be as expected:
 *  an overlapping ban will be put into the banVector,
 *  followed by all bans that it overrides.
 */
void Channel::onModeB(xServer::banVectorType& banVector) {
    typedef xServer::banVectorType banVectorType;

    banVectorType origBans(banVector);
    banVector.clear();

    // Walk through the list of bans being removed/added
    for (banVectorType::const_iterator newBanPtr = origBans.begin(); newBanPtr != origBans.end();
         ++newBanPtr) {
        banVector.push_back(*newBanPtr);

        // Is the ban being set or removed?
        if (!newBanPtr->first) {
            // Removing a ban
            removeBan(newBanPtr->second);
            continue;
        }

        // Setting a ban
        // Need to check the list of current bans for overlaps
        // This is grossly inefficient, Im open to suggestions

        // Next, search for overlaps
        banIterator currentBanPtr = banList_begin();
        for (; currentBanPtr != banList_end();) {
            if (!match(newBanPtr->second, *currentBanPtr)) {
                // Add the removed ban to the banVector
                // so that the caller can notify the rest
                // of the system of the removal
                banVector.push_back(banVectorType::value_type(false, *currentBanPtr));

                //			elog	<< "Channel::onModeB> Removing "
                //				<< "overlapping ban: "
                //				<< *currentBanPtr
                //				<< endl ;

                // Overlap, remove the old ban
                currentBanPtr = banList.erase(currentBanPtr);

            } else {
                // Update the iterator
                ++currentBanPtr;
            }
        } // inner for()

        // Now set the new ban
        // Setting this ban will add the ban into
        // later comparisons, but not this comparison,
        // which is what we want.
        setBan(newBanPtr->second);

    } // outer for()
}

const string Channel::getModeString() const {
    string modeString("+");
    string argString;

    if (modes & MODE_TOPICLIMIT)
        modeString += 't';
    if (modes & MODE_NOPRIVMSGS)
        modeString += 'n';
    if (modes & MODE_SECRET)
        modeString += 's';
    if (modes & MODE_PRIVATE)
        modeString += 'p';
    if (modes & MODE_MODERATED)
        modeString += 'm';
    if (modes & MODE_INVITEONLY)
        modeString += 'i';
    if (modes & MODE_REGONLY)
        modeString += 'r';
    if (modes & MODE_REGISTERED)
        modeString += 'R';
    if (modes & MODE_DELJOINS)
        modeString += 'D';
    if (modes & MODE_NOCOLOR)
        modeString += 'c';
    if (modes & MODE_NOCTCP)
        modeString += 'C';
    if (modes & MODE_NOPARTMSGS)
        modeString += 'u';
    if (modes & MODE_MODERATENOREG)
        modeString += 'M';
    if (modes & MODE_TLSONLY)
        modeString += 'Z';

    if (modes & MODE_KEY) {
        modeString += 'k';
        argString += getKey() + ' ';
    }

    if (modes & MODE_LIMIT) {
        modeString += 'l';

        // Can't put numerical variables into a string
        stringstream s;
        s << getLimit();

        argString += s.str();
    }

    if (modes & MODE_APASS) {
        modeString += 'A';
        argString += getApass() + ' ';
    }

    if (modes & MODE_UPASS) {
        modeString += 'U';
        argString += getUpass() + ' ';
    }

    return (modeString + ' ' + argString);
}

string Channel::createBan(const iClient* theClient) {
    assert(theClient != 0);

    if ((theClient->isModeX()) && (theClient->isModeR()))
        return "*!*@" + theClient->getInsecureHost();
    else
        return createBanMask(theClient->getNickUserHost());
}

void Channel::removeAllModes() {
    for (userIterator uItr = userList_begin(); uItr != userList_end(); ++uItr) {
        ChannelUser* theUser = uItr->second;
        assert(theUser != 0);

        theUser->removeModeO();
        theUser->removeModeV();
    }
    modes = 0;
    limit = 0;
#if __GNUC__ == 2
    key = "";
    Apass = "";
    Upass = "";
#else
    key.clear();
    Apass.clear();
    Upass.clear();
#endif
}

} // namespace gnuworld
