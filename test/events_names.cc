/**
 * events_names.cc
 * Unit test for the event names of include/events.h: eventName() for each of
 * the two enums, and the eventNames[] table that is built from them.
 *
 * The table used to be written out by hand, was one entry short of the enums
 * and was therefore misaligned from EVT_RAW up - and reading its last entry
 * read past its end.  These cases are what says it cannot happen again.
 * Runs under "make check".
 */

#include <iostream>
#include <set>
#include <string_view>

#include "events.h"

using namespace gnuworld;

namespace {

int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n';                \
        }                                                                                          \
    } while (0)

/// Every event has a name, and the table has it at that event's own index
void testEveryEventIsNamed() {
    for (std::size_t whichEvent = 0; whichEvent < networkEventCount; ++whichEvent) {
        const std::string_view name = eventName(static_cast<NetworkEvent>(whichEvent));
        if (name.empty()) {
            ++failures;
            std::cerr << __FILE__ << ": network event " << whichEvent << " has no name\n";
            continue;
        }
        CHECK(eventNames[whichEvent] == name);
    }

    for (std::size_t whichEvent = EVT_JOIN; whichEvent <= EVT_BURST; ++whichEvent) {
        const std::string_view name = eventName(static_cast<ChannelEvent>(whichEvent));
        if (name.empty()) {
            ++failures;
            std::cerr << __FILE__ << ": channel event " << whichEvent << " has no name\n";
            continue;
        }
        CHECK(eventNames[whichEvent] == name);
    }
}

/// The table is exactly as long as the two enums together, so that no event is
/// without an entry and no entry is without an event
void testTheTableIsTheSizeOfTheEnums() {
    CHECK(eventNames.size() == networkEventCount + channelEventCount);
    CHECK(networkEventCount == static_cast<std::size_t>(EVT_JOIN));
    CHECK(eventNames.size() == static_cast<std::size_t>(EVT_BURST) + 1);

    for (const std::string_view name : eventNames) {
        CHECK(!name.empty());
    }
}

/// No two events answer to the same name: a module that looks an event up by
/// name gets the one it asked for
void testTheNamesAreUnique() {
    std::set<std::string_view> seen;
    for (const std::string_view name : eventNames) {
        CHECK(seen.insert(name).second);
    }
    CHECK(seen.size() == eventNames.size());
}

/// The entries that were wrong before: EVT_ACCOUNT_FLAGS had none at all, and
/// everything from EVT_RAW up sat one place too low
void testTheEntriesThatUsedToBeMisaligned() {
    CHECK(eventNames[EVT_ACCOUNT] == "Account Login");
    CHECK(eventNames[EVT_ACCOUNT_FLAGS] == "Account Flags");
    CHECK(eventNames[EVT_RAW] == "Raw");
    CHECK(eventNames[EVT_REMNETCONF] == "Netconf Remove");
    CHECK(eventNames[EVT_JOIN] == "Channel Join");
    CHECK(eventNames[EVT_BURST] == "Channel Burst");
}

/// Each way a membership can arrive maps to its own channel event: one virtual
/// delivers the three, but mod.stats still counts and mod.gnutest still names
/// them apart
void testEveryJoinKindHasItsOwnEvent() {
    CHECK(channelEventOf(JoinKind::Join) == EVT_JOIN);
    CHECK(channelEventOf(JoinKind::Create) == EVT_CREATE);
    CHECK(channelEventOf(JoinKind::Burst) == EVT_BURST);

    CHECK(eventName(channelEventOf(JoinKind::Join)) == "Channel Join");
    CHECK(eventName(channelEventOf(JoinKind::Create)) == "Channel Create");
    CHECK(eventName(channelEventOf(JoinKind::Burst)) == "Channel Burst");
}

} // anonymous namespace

int main() {
    testEveryEventIsNamed();
    testTheTableIsTheSizeOfTheEnums();
    testTheNamesAreUnique();
    testTheEntriesThatUsedToBeMisaligned();
    testEveryJoinKindHasItsOwnEvent();

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "events_names: all checks passed\n";
    return 0;
}
