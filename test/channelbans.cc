/**
 * channelbans.cc
 * Unit test for the ban bookkeeping of Channel: the mask list and the
 * details beside it (who set the ban and when) are kept in step by every
 * path that adds or removes a ban.
 * Exits non-zero if any check fails, so it can run under "make check".
 */

#include <ctime>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Channel.h"
#include "gnuworld_config.h"
#include "server.h"

using namespace gnuworld;

namespace gnuworld {

/// libgnuworldcore has one loose end: xServer::mainLoop() is in src/main.cc,
/// with the daemon's own start-up, and every program that links the core has
/// to supply it.  Nothing here runs a server, so an empty one will do.
void xServer::mainLoop() {}

} // namespace gnuworld

namespace {

int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n';                \
        }                                                                                          \
    } while (0)

/// Setting and removing a ban is Channel's own business, and so protected.
/// A handler reaches it through ServerCommandHandler; this test is a subclass.
class TestChannel : public Channel {
  public:
    TestChannel() : Channel("#bans", 1000) {}

    using Channel::onModeB;
    using Channel::removeAllBans;
    using Channel::removeBan;
    using Channel::setBan;
};

typedef std::vector<std::pair<bool, std::string>> banVector;

/// The masks the channel holds, in list order.
std::vector<std::string> masks(const Channel& theChan) {
    return std::vector<std::string>(theChan.banList_begin(), theChan.banList_end());
}

/// "SomeNick@1700000000", or "-" where the channel has no such ban, so that
/// a check of both fields fits on one line.
std::string detail(const Channel& theChan, const std::string& banMask) {
    const Channel::BanInfo* info = theChan.getBanInfo(banMask);
    if (0 == info) {
        return "-";
    }
    return info->setBy + '@' + std::to_string(info->setAt);
}

void testSetAndRemove() {
    TestChannel theChan;

    CHECK(0 == theChan.getBanInfo("*!*@nowhere"));

    theChan.setBan("*!*@bad.host", "SomeNick", 1700000000);
    CHECK(masks(theChan) == std::vector<std::string>{"*!*@bad.host"});
    CHECK(detail(theChan, "*!*@bad.host") == "SomeNick@1700000000");

    // The comparison is removeBan()'s: case does not matter
    CHECK(detail(theChan, "*!*@BAD.HOST") == "SomeNick@1700000000");

    CHECK(theChan.removeBan("*!*@BAD.HOST"));
    CHECK(theChan.banList_size() == 0);
    CHECK(detail(theChan, "*!*@bad.host") == "-");
}

void testSetAtZeroMeansNow() {
    TestChannel theChan;

    const time_t before = ::time(0);
    theChan.setBan("*!*@now.host");
    const time_t after = ::time(0);

    const Channel::BanInfo* info = theChan.getBanInfo("*!*@now.host");
    CHECK(info != 0);
    if (info != 0) {
        // Nobody to name, and the time is the time it was seen
        CHECK(info->setBy.empty());
        CHECK(info->setAt >= before && info->setAt <= after);
    }

    // An explicit time is kept as given, however old
    theChan.setBan("*!*@old.host", "burst.example.net", 1);
    CHECK(detail(theChan, "*!*@old.host") == "burst.example.net@1");
}

void testSettingAnExistingMaskOverwrites() {
    TestChannel theChan;

    theChan.setBan("*!*@bad.host", "First", 1700000000);
    theChan.setBan("*!*@bad.host", "Second", 1700000100);

    // One ban, and the details of the ban that is there now
    CHECK(masks(theChan) == std::vector<std::string>{"*!*@bad.host"});
    CHECK(detail(theChan, "*!*@bad.host") == "Second@1700000100");
}

void testRemoveAllBans() {
    TestChannel theChan;

    theChan.setBan("*!*@one.host", "One", 1700000000);
    theChan.setBan("*!*@two.host", "Two", 1700000100);
    theChan.removeAllBans();

    CHECK(theChan.banList_size() == 0);
    CHECK(detail(theChan, "*!*@one.host") == "-");
    CHECK(detail(theChan, "*!*@two.host") == "-");
}

void testOnModeBRecordsEachBan() {
    TestChannel theChan;

    banVector bans{{true, "*!*@one.host"}, {true, "*!*@two.host"}};
    const std::vector<Channel::BanInfo> info{{"One", 1700000000}, {"Two", 1700000100}};
    theChan.onModeB(bans, info);

    CHECK(detail(theChan, "*!*@one.host") == "One@1700000000");
    CHECK(detail(theChan, "*!*@two.host") == "Two@1700000100");

    // Nothing was overlapped, so nothing was added to the vector
    CHECK(bans.size() == 2);
}

void testOnModeBWithoutDetailsRecordsNobody() {
    TestChannel theChan;

    banVector bans{{true, "*!*@one.host"}};
    const time_t before = ::time(0);
    theChan.onModeB(bans);

    const Channel::BanInfo* info = theChan.getBanInfo("*!*@one.host");
    CHECK(info != 0);
    if (info != 0) {
        CHECK(info->setBy.empty());
        CHECK(info->setAt >= before);
    }
}

void testOnModeBRemovalDropsTheDetails() {
    TestChannel theChan;

    theChan.setBan("*!*@bad.host", "SomeNick", 1700000000);

    banVector bans{{false, "*!*@bad.host"}};
    theChan.onModeB(bans);

    CHECK(theChan.banList_size() == 0);
    CHECK(detail(theChan, "*!*@bad.host") == "-");
}

void testAnOverlappedBanLosesItsDetails() {
    TestChannel theChan;

    theChan.setBan("*!*@sub.bad.host", "Narrow", 1700000000);
    theChan.setBan("*!*@elsewhere", "Untouched", 1700000050);

    // A wider ban takes the narrow one off, and its details with it
    banVector bans{{true, "*!*@*.bad.host"}};
    const std::vector<Channel::BanInfo> info{{"Wide", 1700000100}};
    theChan.onModeB(bans, info);

    CHECK(masks(theChan).size() == 2);
    CHECK(detail(theChan, "*!*@*.bad.host") == "Wide@1700000100");
    CHECK(detail(theChan, "*!*@sub.bad.host") == "-");
    CHECK(detail(theChan, "*!*@elsewhere") == "Untouched@1700000050");

    // The removal is reported back to the caller, behind the new ban
    CHECK(bans.size() == 2);
    if (bans.size() == 2) {
        CHECK(bans[1] == banVector::value_type(false, "*!*@sub.bad.host"));
    }

    // A new ban with the mask that was overlapped does not inherit anything
    banVector again{{true, "*!*@sub.bad.host"}};
    const std::vector<Channel::BanInfo> againInfo{{"Later", 1700000200}};
    theChan.onModeB(again, againInfo);
    CHECK(detail(theChan, "*!*@sub.bad.host") == "Later@1700000200");
}

} // namespace

int main() {
    testSetAndRemove();
    testSetAtZeroMeansNow();
    testSettingAnExistingMaskOverwrites();
    testRemoveAllBans();
    testOnModeBRecordsEachBan();
    testOnModeBWithoutDetailsRecordsNobody();
    testOnModeBRemovalDropsTheDetails();
    testAnOverlappedBanLosesItsDetails();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "channelbans: all checks passed\n";
    return 0;
}
