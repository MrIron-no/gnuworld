/**
 * ChannelModes.h
 * Everything gnuworld knows about channel mode letters, in one place.
 *
 * Copyright (C) 2026 The GNUWorld developers
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
 */

#ifndef GNUWORLD_CHANNELMODES_H
#define GNUWORLD_CHANNELMODES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * The table below is the single description of the channel modes: which
 * letters exist, what kind of argument each takes and when, and which may
 * travel between servers.  parse() turns a mode string and its arguments
 * into typed, validated changes.
 *
 * This unit knows nothing about Channel, the network tables or the
 * connection, so it can be tested in isolation.  Checks that need channel
 * state ("is the target on the channel?") belong to whoever applies the
 * changes.
 *
 * The reference is ircu's doc/P11.md, section 15.1.
 */
namespace gnuworld::chanmode {

/// What a mode is, which decides how its argument is validated.  Finer
/// grained than Type: a key and a limit are both settings, but only one of
/// them is a number.
enum class Kind : std::uint8_t {
    Flag,     ///< +m: no argument
    Key,      ///< +k <key>, -k <key>
    Limit,    ///< +l <n>, -l
    Password, ///< +A/+U <pass>, -A/-U <pass> (OPLEVELS)
    Member,   ///< +o/+v <member>
    Ban       ///< +b <mask>
};

/**
 * The group a mode belongs to, which says when it carries an argument.
 *
 * A to D are the four groups of the CHANMODES token that a server advertises
 * in RPL_ISUPPORT (005), "CHANMODES=A,B,C,D".  This is the ISUPPORT draft's
 * classification, not RFC 1459's, and it is what clients use to parse a MODE
 * line without knowing the letters.  ircu sends
 * "CHANMODES=b,AkU,l,imnpstrDdRcCuMZ" and "PREFIX=(ov)@+".
 */
enum class Type : std::uint8_t {
    A,     ///< a list: always an argument, and it adds to or removes from the list (b)
    B,     ///< a setting: always an argument, when set and when unset (k, A, U)
    C,     ///< a setting: an argument only when set (l)
    D,     ///< a flag: never an argument (m, t, n, ...)
    Prefix ///< a member's status: always an argument.  Not part of CHANMODES;
           ///< advertised in the PREFIX token instead (o, v)
};

struct Mode {
    char letter;
    Kind kind;

    /// The bit for this mode in Channel::modeType.  0 for Member and Ban,
    /// which are not channel flags.  The values match Channel::MODE_*.
    std::uint32_t flag;

    /// Only a server may set or clear it (+R, registered with services).
    bool serverOnly = false;

    constexpr Type type() const noexcept {
        switch (kind) {
        case Kind::Ban:
            return Type::A;
        case Kind::Key:
        case Kind::Password:
            return Type::B;
        case Kind::Limit:
            return Type::C;
        case Kind::Flag:
            return Type::D;
        case Kind::Member:
            return Type::Prefix;
        }
        return Type::D;
    }

    constexpr bool takesArg(bool set) const noexcept {
        switch (type()) {
        case Type::D:
            return false;
        case Type::C:
            return set;
        default:
            return true;
        }
    }

    friend constexpr bool operator==(const Mode&, const Mode&) = default;
};

/**
 * Every channel mode that travels between servers.  The flag and parameter
 * modes are listed in the order a BURST sends them; burstOrder() relies on it.
 */
inline constexpr std::array modes{
    Mode{'s', Kind::Flag, 0x00004},
    Mode{'p', Kind::Flag, 0x00008},
    Mode{'m', Kind::Flag, 0x00040},
    Mode{'t', Kind::Flag, 0x00001},
    Mode{'i', Kind::Flag, 0x00080},
    Mode{'n', Kind::Flag, 0x00002},
    Mode{'r', Kind::Flag, 0x00100},
    Mode{'D', Kind::Flag, 0x00200},
    Mode{'R', Kind::Flag, 0x01000, /* serverOnly */ true},
    Mode{'c', Kind::Flag, 0x02000},
    Mode{'C', Kind::Flag, 0x04000},
    Mode{'u', Kind::Flag, 0x08000},
    Mode{'M', Kind::Flag, 0x10000},
    Mode{'Z', Kind::Flag, 0x20000},
    Mode{'l', Kind::Limit, 0x00020},
    Mode{'k', Kind::Key, 0x00010},
    Mode{'A', Kind::Password, 0x00400},
    Mode{'U', Kind::Password, 0x00800},
    Mode{'o', Kind::Member, 0},
    Mode{'v', Kind::Member, 0},
    Mode{'b', Kind::Ban, 0},
};

/// Look a mode up by its letter.
constexpr std::optional<Mode> find(char letter) noexcept {
    for (const Mode& mode : modes) {
        if (mode.letter == letter) {
            return mode;
        }
    }
    return std::nullopt;
}

/**
 * Modes an ircu server keeps to itself: 'd' (the channel still has hidden
 * members after -D) and 'z' (a member is not on TLS).  They are valid mode
 * letters, but must never be sent to, or accepted from, another server.
 */
constexpr bool isLocalOnly(char letter) noexcept { return letter == 'd' || letter == 'z'; }

/// The position of a mode in a BURST mode block; lower goes first.
constexpr std::size_t burstOrder(const Mode& mode) noexcept {
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].letter == mode.letter) {
            return i;
        }
    }
    return modes.size();
}

/// ircu's MAXMODEPARAMS: a single MODE line carries at most this many
/// modes that take an argument.
inline constexpr std::size_t maxParamsPerLine = 6;

/// ircu's KEYLEN: the longest key, and the longest +A/+U password.
inline constexpr std::size_t maxKeyLength = 23;

/// One validated mode change.
struct Change {
    bool set; ///< true for '+', false for '-'
    Mode mode;
    std::string arg; ///< empty when the mode takes none in this direction

    friend bool operator==(const Change&, const Change&) = default;
};

enum class Error : std::uint8_t {
    UnknownMode,
    LocalOnlyMode, ///< 'd' or 'z', which never travel between servers
    MissingArgument,
    InvalidKey, ///< also a +A/+U password; they share the key rules
    InvalidLimit,
    InvalidTarget, ///< the member of a +o/+v
    InvalidMask,
    UnusedArgument ///< an argument no mode asked for
};

struct Problem {
    Error error;
    char letter;        ///< the mode concerned; 0 for UnusedArgument
    std::string detail; ///< the offending argument, if there was one

    friend bool operator==(const Problem&, const Problem&) = default;
};

struct Parsed {
    /// The changes that were valid, in the order given.
    std::vector<Change> changes;

    /// What was wrong with the rest.  A mode with a problem is left out of
    /// changes; the others are unaffected.
    std::vector<Problem> problems;

    /// The channel timestamp, if one was asked for and found.
    std::optional<std::uint64_t> timestamp;

    /// How many of the arguments were consumed, the timestamp included.
    std::size_t argsUsed = 0;

    bool ok() const noexcept { return problems.empty(); }
};

struct ParseOptions {
    /// One all-digit argument left over after every mode has taken its own
    /// is the channel timestamp that ends a MODE line between servers.
    /// Counting from the modes, rather than looking at the last argument,
    /// is what tells "+l 10" from "+m 1700000000".
    bool trailingTimestamp = false;

    /// Arguments nobody asked for are not a problem.  A BURST has its
    /// member list right behind the arguments of its mode block; see
    /// Parsed::argsUsed.
    bool allowLeftover = false;
};

/**
 * Parse a mode string, such as "+tnk-l", and its arguments.
 *
 * A string with no leading sign starts out as '+', as ircu reads it.
 * A mode with a problem is reported and skipped, and the parse carries on,
 * so the caller decides what a problem means: a line from the network is
 * applied as far as it is valid, while a change we are about to send should
 * not go out at all unless ok().
 */
Parsed parse(std::string_view modeString, std::span<const std::string_view> args,
             ParseOptions options = {});

/// ircu's is_clean_key(): not empty, at most maxKeyLength, no leading ':',
/// and no comma, space or control character.
bool isValidKey(std::string_view key) noexcept;

/// A channel limit: decimal digits only, from 1 to INT_MAX.
bool isValidLimit(std::string_view limit) noexcept;

/// The longest line we send, without its CR LF: RFC 1459's 512 less the two.
inline constexpr std::size_t maxLineLength = 510;

/**
 * Format changes as complete MODE lines: "<prefix> <modes> [<args>] <timestamp>".
 *
 * prefix is everything in front of the mode string, "<source> M <#channel>"
 * (or "... OM ..." for an OPMODE).  Consecutive changes of one polarity share
 * a sign, so +o and +v come out as "+ov" and not "+o+v".  A new line is
 * started when the next change would be the seventh with an argument
 * (maxParamsPerLine) or would take the line past maxLineLength; every line
 * ends in the channel timestamp, which a P11 peer requires.  There is
 * exactly one space between any two fields.
 *
 * The changes are sent as given: run them through parse() first if they
 * come from outside.  No changes, no lines.
 */
std::vector<std::string> formatLines(std::string_view prefix, std::span<const Change> changes,
                                     std::uint64_t timestamp);

/**
 * The mode block of a BURST line, "+tnlk 25 sekrit": the modes being set, in
 * the order ircu bursts them, with their arguments behind.  Changes that
 * clear a mode, and members and bans, have no place in a mode block and are
 * left out.  Empty if nothing remains, in which case the BURST has no block.
 */
std::string burstModeBlock(std::span<const Change> changes);

/// The modes of each CHANMODES group as one string, "A,B,C,D".  The
/// server-local 'd' and 'z' are not in the table, so not in here either.
std::string isupportChanmodes();

/// A short name for an Error, for logs.
std::string_view describe(Error error) noexcept;

} // namespace gnuworld::chanmode

#endif // GNUWORLD_CHANNELMODES_H
