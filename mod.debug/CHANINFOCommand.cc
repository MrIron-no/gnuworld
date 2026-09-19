/**
 * CHANINFOCommand.cc
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
 */

#include <ctime>
#include <format>
#include <string>

#include "Network.h"
#include "StringTokenizer.h"
#include "gnuworld_config.h"
#include "misc.h"

#include "debug.h"
#include "debug-commands.h"

namespace gnuworld {

void CHANINFOCommand::Exec(const iClient* theClient, const std::string& Message) {
    StringTokenizer st(Message);
    if (st.size() < 2) {
        Usage(theClient);
        return;
    }

    Channel* theChan = Network->findChannel(st[1]);
    if (theChan == nullptr) {
        bot->Notice(theClient, "Unable to find channel {}", st[1]);
        return;
    }

    bot->Notice(theClient, "Channel: {}", theChan->getName());
    bot->Notice(theClient, "Modes: {} ({:#x})", theChan->getModeString(), theChan->getModes());
    bot->Notice(theClient, "Created at time: {} ({} ago)", theChan->getCreationTime(),
                prettyDuration(theChan->getCreationTime()));

    if (theChan->getMode(Channel::MODE_KEY) || !theChan->getKey().empty()) {
        bot->Notice(theClient, "Key: {}", theChan->getKey());
    }

    if (theChan->getMode(Channel::MODE_LIMIT) || theChan->getLimit() != 0) {
        bot->Notice(theClient, "Limit: {}", theChan->getLimit());
    }

    if (theChan->getMode(Channel::MODE_APASS) || !theChan->getApass().empty()) {
        bot->Notice(theClient, "Apass: {}", theChan->getApass());
    }

    if (theChan->getMode(Channel::MODE_UPASS) || !theChan->getUpass().empty()) {
        bot->Notice(theClient, "Upass: {}", theChan->getUpass());
    }

#ifdef TOPIC_TRACK
    bot->Notice(theClient, "Topic: {}", theChan->getTopic());
    if (theChan->getTopicTS() != 0) {
        bot->Notice(theClient, "Topic set {} ago [{}] by {}", prettyDuration(theChan->getTopicTS()),
                    theChan->getTopicTS(), theChan->getTopicWhoSet());
    }
#endif

    int totalOps = 0;
    int totalVoice = 0;
    int totalHidden = 0;

    bot->Notice(theClient, "Users:");
    for (const auto& [id, theUser] : theChan->users()) {
        (void)id;
        if (theUser->isModeO())
            ++totalOps;
        if (theUser->isModeV())
            ++totalVoice;
        if (theUser->isHidden())
            ++totalHidden;

        // A hidden (delayed join) member never holds op or voice
        const char* tmpMode = theUser->isHidden() ? "hide: " : "none: ";
        if (theUser->isModeO() && theUser->isModeV())
            tmpMode = "+o+v: ";
        else if (theUser->isModeO())
            tmpMode = "+o:   ";
        else if (theUser->isModeV())
            tmpMode = "+v:   ";

        bot->Notice(theClient, "  {}{}!{}@{} (numeric: {})", tmpMode, theUser->getNickName(),
                    theUser->getUserName(), theUser->getHostName(), theUser->getCharYYXXX());
    }

    bot->Notice(theClient, "Number of channel users: {} ({} ops, {} voice, {} hidden)",
                theChan->size(), totalOps, totalVoice, totalHidden);

    if (theChan->banList_size() == 0) {
        bot->Notice(theClient, "Ban list: (empty)");
        return;
    }

    // One notice per ban, so that each has room for who set it and when.
    // A channel with many bans is many notices; a mask and a setter are both
    // bounded by the network's name lengths, so a line stays well inside the
    // 400 bytes a notice used to be packed up to.
    bot->Notice(theClient, "Ban list ({}):", theChan->banList_size());
    for (auto banItr = theChan->banList_begin(); banItr != theChan->banList_end(); ++banItr) {
        // The core records both for every ban it holds.  A path that could
        // not know who set one leaves the name empty; no details at all
        // would be a bug in the bookkeeping, not a ban nobody set.
        const Channel::BanInfo* banInfo = theChan->getBanInfo(*banItr);

        // A P11 burst carries the name as another server wrote it: nothing
        // of it that would draw on a terminal goes into the notice
        std::string setBy;
        if (banInfo != nullptr) {
            for (const char c : banInfo->setBy) {
                if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
                    setBy += c;
                }
            }
        }
        if (setBy.empty()) {
            setBy = "(unknown)";
        }

        // The time of a burst ban is the wire's too, and a clock that runs
        // ahead is no duration prettyDuration() can say
        std::string setAt = "(unknown)";
        if (banInfo != nullptr && banInfo->setAt != 0) {
            setAt = (banInfo->setAt > ::time(nullptr))
                        ? std::format("{} (in the future)", prettyTime(banInfo->setAt))
                        : std::format("{} ({} ago)", prettyTime(banInfo->setAt),
                                      prettyDuration(banInfo->setAt));
        }

        bot->Notice(theClient, "  {}  set by {}  at {}", *banItr, setBy, setAt);
    }
}

} // namespace gnuworld
