/*
 * Channel.h
 * Author: Daniel Karrels (dan@karerls.com)
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
 * $Id: Channel.h,v 1.37 2008/04/16 20:29:36 danielaustin Exp $
 */

#ifndef __CHANNEL_H
#define __CHANNEL_H "$Id: Channel.h,v 1.37 2008/04/16 20:29:36 danielaustin Exp $"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <map>
#include <vector>
#include <list>
#include <utility>

#include <ctime>

#include "ChannelUser.h"
#include "xparameters.h"
#include "ELog.h"
#include "gnuworld_config.h"

#ifdef USE_THREAD
#include <mutex>
#include <shared_mutex>
#endif

namespace gnuworld {

/// Forward declaration of class iClient.
class iClient;

/**
 * This class represents a single network channel.
 * Channel users are also maintained in this structure.
 */
class Channel {

  protected:
    /**
     * The type used to hold this channel's users.
     */
    typedef std::map<unsigned int, ChannelUser*> userListType;

    /**
     * The type to be used to store channel bans.
     */
    typedef std::list<std::string> banListType;

    /// Make class xServer a friend of this class.
    friend class xServer;

  public:
    /**
     * The type used to store this channel's current channel
     * modes.
     */
    typedef unsigned int modeType;

    /// Bit representing channel mode +t
    static constexpr modeType MODE_T = 0x00001;

    /// Bit representing channel mode +n
    static constexpr modeType MODE_N = 0x00002;

    /// Bit representing channel mode +s
    static constexpr modeType MODE_S = 0x00004;

    /// Bit representing channel mode +p
    static constexpr modeType MODE_P = 0x00008;

    /// Bit representing channel mode +k
    static constexpr modeType MODE_K = 0x00010;

    /// Bit representing channel mode +l
    static constexpr modeType MODE_L = 0x00020;

    /// Bit representing channel mode +m
    static constexpr modeType MODE_M = 0x00040;

    /// Bit representing channel mode +i
    static constexpr modeType MODE_I = 0x00080;

    /// Bit representing channel mode +r
    static constexpr modeType MODE_R = 0x00100;

    /// Bit representing channel mode +R
    static constexpr modeType MODE_REG = 0x01000;

    /// Bit representing channel mode +D
    static constexpr modeType MODE_D = 0x00200;

    /// Bit representing channel mode +c
    static constexpr modeType MODE_C = 0x02000;

    /// Bit representing channel mode +C
    static constexpr modeType MODE_CTCP = 0x04000;

    /// Bit representing channel mode +P
    static constexpr modeType MODE_PART = 0x08000;

    /// Bit representing channel mode +M
    static constexpr modeType MODE_MNOREG = 0x10000;

    /// Bit representing channel mode +A
    static constexpr modeType MODE_A = 0x00400;

    /// Bit representing channel mode +U
    static constexpr modeType MODE_U = 0x00800;

    /// Bit representing channel mode +Z
    static constexpr modeType MODE_Z = 0x20000;

    /*
     * The channel modes: which letters exist, what argument each takes and
     * when, and how a mode string is read and written.  This is the single
     * description of them; the handlers in libircu read modes through
     * parseModes(), and every MODE line gnuworld sends is put together by
     * formatModeLines().  The reference is ircu's doc/P11.md, section 15.1.
     */

    /// What a mode is, which decides how its argument is validated.
    enum class ModeKind : unsigned char {
        Flag,     ///< +m: no argument
        Key,      ///< +k <key>, -k <key>
        Limit,    ///< +l <n>, -l
        Password, ///< +A/+U <pass>, -A/-U <pass> (OPLEVELS)
        Member,   ///< +o/+v <member>
        Ban       ///< +b <mask>
    };

    /**
     * The group a mode belongs to, which says when it carries an argument.
     * A to D are the four groups of the CHANMODES token that a server
     * advertises in RPL_ISUPPORT (005), "CHANMODES=A,B,C,D".  That is the
     * ISUPPORT draft's classification, not RFC 1459's.  ircu sends
     * "CHANMODES=b,AkU,l,imnpstrDdRcCuMZ" and "PREFIX=(ov)@+".
     */
    enum class ModeGroup : unsigned char {
        A,     ///< a list: always an argument (b)
        B,     ///< a setting: always an argument, set or unset (k, A, U)
        C,     ///< a setting: an argument only when set (l)
        D,     ///< a flag: never an argument (m, t, n, ...)
        Prefix ///< a member's status: always an argument.  Not part of
               ///< CHANMODES, but of the PREFIX token (o, v)
    };

    struct ModeInfo {
        char letter;
        ModeKind kind;
        /// The MODE_* bit; 0 for Member and Ban, which are not channel flags.
        modeType flag;
        /// Only a server may set or clear it (+R, registered with services).
        bool serverOnly;

        constexpr ModeGroup group() const noexcept {
            switch (kind) {
            case ModeKind::Ban:
                return ModeGroup::A;
            case ModeKind::Key:
            case ModeKind::Password:
                return ModeGroup::B;
            case ModeKind::Limit:
                return ModeGroup::C;
            case ModeKind::Flag:
                return ModeGroup::D;
            case ModeKind::Member:
                return ModeGroup::Prefix;
            }
            return ModeGroup::D;
        }

        constexpr bool takesArg(bool set) const noexcept {
            return group() != ModeGroup::D && (group() != ModeGroup::C || set);
        }

        friend constexpr bool operator==(const ModeInfo&, const ModeInfo&) = default;
    };

    /**
     * Every channel mode that travels between servers.  The flags and
     * settings are in the order a BURST sends them: s|p m t i n r D R c C u
     * M Z, then l k A U.
     */
    static constexpr std::array<ModeInfo, 21> modeTable{{
        {'s', ModeKind::Flag, MODE_S, false},
        {'p', ModeKind::Flag, MODE_P, false},
        {'m', ModeKind::Flag, MODE_M, false},
        {'t', ModeKind::Flag, MODE_T, false},
        {'i', ModeKind::Flag, MODE_I, false},
        {'n', ModeKind::Flag, MODE_N, false},
        {'r', ModeKind::Flag, MODE_R, false},
        {'D', ModeKind::Flag, MODE_D, false},
        {'R', ModeKind::Flag, MODE_REG, true},
        {'c', ModeKind::Flag, MODE_C, false},
        {'C', ModeKind::Flag, MODE_CTCP, false},
        {'u', ModeKind::Flag, MODE_PART, false},
        {'M', ModeKind::Flag, MODE_MNOREG, false},
        {'Z', ModeKind::Flag, MODE_Z, false},
        {'l', ModeKind::Limit, MODE_L, false},
        {'k', ModeKind::Key, MODE_K, false},
        {'A', ModeKind::Password, MODE_A, false},
        {'U', ModeKind::Password, MODE_U, false},
        {'o', ModeKind::Member, 0, false},
        {'v', ModeKind::Member, 0, false},
        {'b', ModeKind::Ban, 0, false},
    }};

    /// Look a mode up by its letter.
    static constexpr std::optional<ModeInfo> findMode(char letter) noexcept {
        for (const ModeInfo& mode : modeTable) {
            if (mode.letter == letter) {
                return mode;
            }
        }
        return std::nullopt;
    }

    /// 'd' (hidden members remain after -D) and 'z' (a member is not on
    /// TLS) are valid in ircu but local to a server: never sent to, or
    /// accepted from, another one.
    static constexpr bool isLocalOnlyMode(char letter) noexcept {
        return letter == 'd' || letter == 'z';
    }

    /// The position of a mode in a BURST mode block; lower goes first.
    static constexpr std::size_t burstOrder(const ModeInfo& mode) noexcept {
        for (std::size_t i = 0; i < modeTable.size(); ++i) {
            if (modeTable[i].letter == mode.letter) {
                return i;
            }
        }
        return modeTable.size();
    }

    /// ircu's KEYLEN: the longest key, and the longest +A/+U password.
    static constexpr std::size_t maxKeyLength = 23;

    /// One validated mode change.
    struct ModeChange {
        bool set; ///< true for '+', false for '-'
        ModeInfo mode;
        std::string arg; ///< empty when the mode takes none in this direction

        friend bool operator==(const ModeChange&, const ModeChange&) = default;
    };

    enum class ModeError : unsigned char {
        UnknownMode,
        LocalOnlyMode, ///< 'd' or 'z'
        MissingArgument,
        InvalidKey, ///< also a +A/+U password; they share the key rules
        InvalidLimit,
        InvalidTarget, ///< the member of a +o/+v
        InvalidMask,
        UnusedArgument ///< an argument no mode asked for
    };

    struct ModeProblem {
        ModeError error;
        char letter;        ///< the mode concerned; 0 for UnusedArgument
        std::string detail; ///< the offending argument, if there was one

        friend bool operator==(const ModeProblem&, const ModeProblem&) = default;
    };

    struct ParsedModes {
        /// The changes that were valid, in the order given.
        std::vector<ModeChange> changes;
        /// What was wrong with the rest; such a mode is left out of changes.
        std::vector<ModeProblem> problems;
        /// The channel timestamp, if one was asked for and found.
        std::optional<std::uint64_t> timestamp;
        /// How many of the arguments were consumed, the timestamp included.
        std::size_t argsUsed = 0;

        bool ok() const noexcept { return problems.empty(); }
    };

    struct ModeParseOptions {
        /// One all-digit argument left over after every mode has taken its
        /// own is the channel timestamp that ends a MODE line.  Counting
        /// from the modes is what tells "+l 10" from "+m 1700000000".
        bool trailingTimestamp = false;
        /// Arguments nobody asked for are not a problem: a BURST has its
        /// member list right behind the mode block's arguments.
        bool allowLeftover = false;
    };

    /**
     * Parse a mode string, such as "+tnk-l", and its arguments.  No leading
     * sign means '+'.  A mode with a problem is reported and skipped and the
     * parse carries on, so the caller decides what a problem means: a line
     * from the network is applied as far as it is valid, while a change we
     * are about to send should not go out unless ok().
     */
    static ParsedModes parseModes(std::string_view modeString,
                                  std::span<const std::string_view> args, ModeParseOptions options);
    static ParsedModes parseModes(std::string_view modeString,
                                  std::span<const std::string_view> args);

    /// ircu's is_clean_key(): not empty, at most maxKeyLength, no leading
    /// ':', and no comma, space or control character.
    static bool isValidKey(std::string_view key) noexcept;

    /// A channel limit: decimal digits only, from 1 to INT_MAX.
    static bool isValidLimit(std::string_view limit) noexcept;

    /**
     * Format changes as complete MODE lines,
     * "<prefix> <modes> [<args>] <timestamp>", where prefix is
     * "<source> M <#channel>".  One sign per run of a polarity; at most
     * MAX_CHAN_MODES modes on a line, flags included, and never past the
     * line limit; the channel timestamp last, which a P11 peer requires;
     * one space between fields.
     */
    static std::vector<std::string> formatModeLines(std::string_view prefix,
                                                    std::span<const ModeChange> changes,
                                                    std::uint64_t timestamp);

    /// The mode block of a BURST, "+tnlk 25 sekrit": what is being set, in
    /// burst order.  Empty if nothing remains.
    static std::string burstModeBlock(std::span<const ModeChange> changes);

    /// The modes of each CHANMODES group as one string, "A,B,C,D".
    static std::string isupportChanmodes();

    /// A short name for a ModeError, for logs.
    static std::string_view describe(ModeError error) noexcept;

    /// Type used to store number of clients in channel
    typedef userListType::size_type size_type;

    /**
     * The type used for mutable iteration through this
     * channel's user structure.
     */
    typedef userListType::iterator userIterator;

    /**
     * The type used for immutable iteration through this
     * channel's user structure.
     */
    typedef userListType::const_iterator const_userIterator;

    /**
     * The type used for immutable iteration through this
     * channel's ban structure.
     */
    typedef banListType::const_iterator const_banIterator;

    /**
     * The type used for mutable iteration through this
     * channel's ban structure.
     */
    typedef banListType::iterator banIterator;

    /**
     * Construct a channel of the given name, constructed at the
     * given creation time.
     */
    Channel(const std::string& _name, const time_t& _creationTime);

    /**
     * Destroy this channel.
     */
    virtual ~Channel();

    /**
     * Test if a given channel mode is set.
     */
    inline bool getMode(const modeType& whichMode) const {
        return (whichMode == (modes & whichMode));
    }

    /**
     * Set the given channel mode.
     */
    inline void setMode(const modeType& whichMode) { modes |= whichMode; }

    /**
     * Remove the given channel mode.
     */
    inline void removeMode(const modeType& whichMode) { modes &= ~whichMode; }

    /**
     * Return true if the given mode is set for the the ChannelUser
     * corresponding to the given iClient.
     */
    bool getUserMode(const ChannelUser::modeType& whichMode, iClient*) const;

    /**
     * Remove the given mode from the ChannelUser associated with
     * the given iClient.
     */
    bool removeUserMode(const ChannelUser::modeType& whichMode, iClient*);

    /**
     * Set the given mode for the ChannelUser associated with the
     * given iClient.
     */
    bool setUserMode(const ChannelUser::modeType& whichMode, iClient*);

    /**
     * Set a limit on the channel.  This method will set the
     * channel mode and set the limit.
     */
    inline void setLimit(const unsigned int& newLimit) { limit = newLimit; }

    /**
     * Set a key on the channel.  This method will set the
     * channel mode and the key as well.
     */
    inline void setKey(const std::string& newKey) { key = newKey; }

    /**
     * Set an Apass on the channel.  This method will set the
     * channel mode and the Apass as well.
     */
    inline void setApass(const std::string& newApass) { Apass = newApass; }

    /**
     * Set an Upass on the channel.  This method will set the
     * channel mode and the Upass as well.
     */
    inline void setUpass(const std::string& newUpass) { Upass = newUpass; }

    /**
     * Reveal a delayed-join (hidden) member, if the given client is on
     * this channel and hidden.  Returns true if the member was hidden.
     */
    bool revealUser(const iClient* theClient);

    /**
     * Add a ban to this Channel's ban list.
     */
    void setBan(const std::string& banMask);

    /**
     * Remove a ban from this channel's ban list.  This does a lexical
     * comparison, not a wildcard match.
     * Returns true if ban found (and removed), false if not
     * found.
     */
    bool removeBan(const std::string& banMask);

    /**
     * Remove all the bans in the channels ban list.
     */
    inline void removeAllBans() { banList.clear(); }

    /**
     * Remove all channel modes on users and the channel itself.
     * Use with caution.
     */
    void removeAllModes();

    /**
     * Find a ban in the channel's ban list which lexically matches
     * the given banMask.
     */
    bool findBan(const std::string& banMask) const;

    /**
     * Find a ban in the channel's ban list which wildcard matches
     * the given banMask.
     */
    bool matchBan(const std::string& banMask) const;

    /**
     * Search for a ban that matches the mask in (banMask).
     * If a matching ban is found, it is stored in (matchingBan),
     * and true is returned.
     * Otherwise, (matchingBan) is unmodified, and false is
     * returned.
     */
    inline bool getMatchingBan(const std::string& banMask, std::string& matchingBan) const;

    /**
     * Retrieve the current channel modes.
     */
    inline const modeType& getModes() const { return modes; }

    /**
     * Retrieve a string of the current channel modes.
     */
    const std::string getModeString() const;

    /**
     * Type used to store the size of the banlist.
     */
    typedef banListType::size_type banListSizeType;

    /**
     * Retrieve the number of elements in the ban list.
     */
    inline banListSizeType banList_size() const { return banList.size(); }

    /**
     * Retrieve the name of this channel.
     */
    inline const std::string& getName() const { return name; }

    /**
     * Retrieve the creation time of this channel.
     */
    inline const time_t& getCreationTime() const { return creationTime; }

    /**
     * Set the creation time of this channel.  This is protected
     * so that only class xServer may access it externally.
     */
    inline virtual void setCreationTime(const time_t& newCT) { creationTime = newCT; }

    /**
     * Retrieve this channel's key.  Note that the
     * existence of a key does not mean that channel
     * mode +k is set.
     */
    inline const std::string& getKey() const { return key; }

    /**
     * Retrieve this channel's Apass.  Note that the
     * existence of an Apass does not mean that channel
     * mode +A is set.
     */
    inline const std::string& getApass() const { return Apass; }

    /**
     * Retrieve this channel's Upass.  Note that the
     * existence of an Upass does not mean that channel
     * mode +U is set.
     */
    inline const std::string& getUpass() const { return Upass; }

    /**
     * Retrieve this channel's limit.  Note that the
     * existence of a limit here does not mean that
     * channel mode +l is set.
     */
    inline const unsigned int& getLimit() const { return limit; }

#ifdef USE_THREAD
    /**
     * Returns the mutex for this channel object.
     */
    inline std::shared_mutex& getMutex() const { return chanMutex; }
#endif

    /**
     * Return true if no clients remain in this channel,
     * false otherwise.
     */
    inline bool empty() const { return userList.empty(); }

    /**
     * Return the number of clients in this channel.
     */
    inline size_type size() const { return userList.size(); }

    /**
     * Retrieve a const_iterator to the beginning of the ban list.
     */
    inline const_banIterator banList_begin() const { return banList.begin(); }

    /**
     * Retrieve a const_iterator to the end of the channel ban list.
     */
    inline const_banIterator banList_end() const { return banList.end(); }

    /**
     * Retrieve an iterator to the beginning of the ban list.
     */
    inline banIterator banList_begin() { return banList.begin(); }

    /**
     * Retrieve an iterator to the end of the channel ban list.
     */
    inline banIterator banList_end() { return banList.end(); }

    /**
     * Retrieve an iterator to the beginning of
     * this channel's user structure.
     */
    inline userIterator userList_begin() { return userList.begin(); }

    /**
     * Retrieve an iterator to the end of
     * this channel's user structure.
     */
    inline userIterator userList_end() { return userList.end(); }

    inline size_type userList_size() { return userList.size(); }

    /**
     *  Return the Channel's userlist.
     */
    inline const userListType& users() const { return userList; }

    /**
     * Retrieve a const iterator to the beginning of
     * this channel's user structure.
     */
    inline const_userIterator userList_begin() const { return userList.begin(); }

    /**
     * Retrieve a const iterator to the end of this
     * channel's user structure.
     */
    inline const_userIterator userList_end() const { return userList.end(); }

    /**
     * Add a ChannelUser to this channel's internal user
     * structure.
     */
    bool addUser(ChannelUser* newUser);

    /**
     * Add an iClient to this chanenl's internal user
     * structure.
     */
    bool addUser(iClient* theClient);

    /**
     * Remove a ChannelUser from this channel's internal
     * user structure.  Note that the memory associated
     * with the ChannelUser is NOT deallocated, it is
     * returned to the caller.
     */
    ChannelUser* removeUser(iClient* theClient);

    /**
     * Remove the given ChannelUser from this Channel.
     */
    ChannelUser* removeUser(ChannelUser* theUser);

    /**
     * Remove a ChannelUser from this channel's internal
     * user structure.  Note that the memory associated
     * with the ChannelUser is NOT deallocated, it is
     * returned to the caller.
     */
    ChannelUser* removeUser(const unsigned int& intYYXXX);

    /**
     * Return the ChannelUser associated with the given iClient,
     * NULL if not found.
     */
    ChannelUser* findUser(const iClient* theClient) const;

#ifdef TOPIC_TRACK

    /**
     * Returns the channel topic (if TOPIC_TRACK is defined)
     */
    const std::string& getTopic() const { return topic; }

    const std::string& getTopicWhoSet() const { return topic_whoset; }

    const long& getTopicTS() const { return topic_ts; }

    /**
     * Sets this channel's topic value to the value passed in.
     * This method exists only if TOPIC_TRACK is defined.
     */
    void setTopic(const std::string& _Topic) { topic = _Topic; }

    void setTopicWhoSet(const std::string& _TopicWhoSet) { topic_whoset = _TopicWhoSet; }

    void setTopicTS(const time_t& _TopicTS) { topic_ts = _TopicTS; }

#endif

    /**
     * Convenience operator for outputting Channel information
     * to a C++ output stream.
     */
    friend ELog& operator<<(ELog& out, const Channel& rhs) {
        out << "Name: " << rhs.name << ", creation time: " << rhs.creationTime;
        return out;
    }

    /**
     * Return a level 2 ban for the given user.
     */
    static std::string createBan(const iClient*);

  protected:
    /**
     * Handle one or more "simple" mode changes.
     */
    virtual void onMode(const std::vector<std::pair<bool, modeType>>&);

    /**
     * This method is called when channel mode 'l' is set
     * or unset.
     */
    virtual void onModeL(bool, const unsigned int&);

    /**
     * This method is called when channel mode 'k' is set
     * or unset.
     */
    virtual void onModeK(bool, const std::string&);

    /**
     * This method is called when channel mode 'A' is set
     * or unset.
     */
    virtual void onModeA(bool, const std::string&);

    /**
     * This method is called when channel mode 'U' is set
     * or unset.
     */
    virtual void onModeU(bool, const std::string&);

    /**
     * This method is called when one or more channel
     * mode (t)'s is/are set or unset.
     */
    virtual void onModeO(const std::vector<std::pair<bool, ChannelUser*>>&);

    /**
     * This method is called when one or more channel
     * mode (v)'s is/are set or unset.
     */
    virtual void onModeV(const std::vector<std::pair<bool, ChannelUser*>>&);

    /**
     * This method is called when one or more channel
     * mode (b)'s is/are set or unset.
     * This method will add to the vector passed to it any
     * bans that have been removed as a result of newly added
     * overlapping bans.
     */
    virtual void onModeB(std::vector<std::pair<bool, std::string>>&);

    /**
     * The name of this channel.
     */
    std::string name;

    /**
     * The time at which this channel was created.
     */
    time_t creationTime;

    /**
     * This channel's current modes.
     */
    modeType modes;

    /**
     * The limit associated with this channel.
     * Note that this variable may hold values
     * even when channel mode +l is NOT set.
     */
    unsigned int limit;

    /**
     * The key associated with this channel.
     * Note that this variable may hold values
     * even when channel mode +k is NOT set.
     */
    std::string key;

    /**
     * The Apass associated with this channel.
     * Note that this variable may hold values
     * even when channel mode +A is NOT set.
     */
    std::string Apass;

    /**
     * The Upass associated with this channel.
     * Note that this variable may hold values
     * even when channel mode +U is NOT set.
     */
    std::string Upass;

    /**
     * The structure used to hold the ChannelUser
     * instances.
     */
    userListType userList;

    /**
     * The structure used to store the channel bans.
     */
    banListType banList;

#ifdef USE_THREAD
    /**
     * A mutex to lock the channel object.
     */
    mutable std::shared_mutex chanMutex;
#endif

#ifdef TOPIC_TRACK
    /**
     * This channel's topic, only if TOPIC_TRACK is defined.
     */
    std::string topic;

    std::string topic_whoset;

    time_t topic_ts;
#endif
};

} // namespace gnuworld

#endif // __CHANNEL_H
