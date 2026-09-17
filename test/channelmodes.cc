/**
 * channelmodes.cc
 * Unit test for libgnuworld/ChannelModes.
 * Exits non-zero if any check fails, so it can run under "make check".
 */

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ChannelModes.h"

using namespace gnuworld::chanmode;
using std::string_view;

namespace {

int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n';                \
        }                                                                                          \
    } while (0)

Parsed parseArgs(string_view modeString, std::vector<string_view> args, bool timestamp = false) {
    return parse(modeString, args, timestamp);
}

/// "+t -l +k:sekrit": the changes, flattened so that a check fits on one line.
std::string flat(const Parsed& parsed) {
    std::string out;
    for (const Change& change : parsed.changes) {
        if (!out.empty()) {
            out += ' ';
        }
        out += change.set ? '+' : '-';
        out += change.mode.letter;
        if (!change.arg.empty()) {
            out += ':' + change.arg;
        }
    }
    return out;
}

bool onlyProblem(const Parsed& parsed, Error error, char letter, string_view detail = {}) {
    return parsed.problems.size() == 1 &&
           parsed.problems[0] == Problem{error, letter, std::string(detail)};
}

void testTable() {
    // Every letter is unique, and so is every channel flag bit
    for (std::size_t i = 0; i < modes.size(); ++i) {
        for (std::size_t j = i + 1; j < modes.size(); ++j) {
            CHECK(modes[i].letter != modes[j].letter);
            CHECK(modes[i].flag == 0 || modes[i].flag != modes[j].flag);
        }
    }

    // Members and bans are not channel flags; everything else is exactly one bit
    for (const Mode& mode : modes) {
        const bool isFlagless = (mode.kind == Kind::Member || mode.kind == Kind::Ban);
        CHECK(isFlagless == (mode.flag == 0));
        CHECK(isFlagless || (mode.flag & (mode.flag - 1)) == 0);
    }

    // The values Channel::MODE_* have had for twenty years; modules store them
    CHECK(find('t')->flag == 0x00001);
    CHECK(find('n')->flag == 0x00002);
    CHECK(find('k')->flag == 0x00010);
    CHECK(find('l')->flag == 0x00020);
    CHECK(find('D')->flag == 0x00200);
    CHECK(find('R')->flag == 0x01000);
    CHECK(find('Z')->flag == 0x20000);

    CHECK(!find('x'));
    CHECK(!find('+'));
    CHECK(!find('d') && isLocalOnly('d'));
    CHECK(!find('z') && isLocalOnly('z'));
    CHECK(!isLocalOnly('D'));

    // When an argument is present
    CHECK(!find('m')->takesArg(true) && !find('m')->takesArg(false));
    CHECK(find('l')->takesArg(true) && !find('l')->takesArg(false));
    CHECK(find('k')->takesArg(true) && find('k')->takesArg(false));
    CHECK(find('A')->takesArg(false) && find('U')->takesArg(false));
    CHECK(find('o')->takesArg(false) && find('v')->takesArg(false) && find('b')->takesArg(false));

    // Only +R is reserved for servers
    for (const Mode& mode : modes) {
        CHECK(mode.serverOnly == (mode.letter == 'R'));
    }

    // doc/P11.md 15.1: s|p m t i n r D R c C u M Z, then l k A U
    const string_view burst = "spmtinrDRcCuMZlkAU";
    for (std::size_t i = 0; i + 1 < burst.size(); ++i) {
        CHECK(burstOrder(*find(burst[i])) < burstOrder(*find(burst[i + 1])));
    }

    // Usable at compile time
    static_assert(find('k')->kind == Kind::Key);
    static_assert(!find('q'));
    static_assert(maxParamsPerLine == 6 && maxKeyLength == 23);
}

void testParseValid() {
    // Mode blocks taken from the recorded P11 burst
    CHECK(flat(parseArgs("+tn", {})) == "+t +n");
    CHECK(flat(parseArgs("+tnD", {})) == "+t +n +D");
    auto parsed = parseArgs("+tnlk", {"25", "sekrit"});
    CHECK(parsed.ok() && flat(parsed) == "+t +n +l:25 +k:sekrit");

    // Polarity switches, and -l takes no argument while -k does
    parsed = parseArgs("-lk", {"sekrit"});
    CHECK(parsed.ok() && flat(parsed) == "-l -k:sekrit");
    parsed = parseArgs("+m-i+s", {});
    CHECK(parsed.ok() && flat(parsed) == "+m -i +s");
    parsed = parseArgs("-k+k", {"old", "new"});
    CHECK(parsed.ok() && flat(parsed) == "-k:old +k:new");

    // No leading sign means '+'
    CHECK(flat(parseArgs("tn", {})) == "+t +n");

    // Members and bans, in the order given
    parsed = parseArgs("+ov-b", {"ABAAC", "ABAAD", "*!*@spam.example.net"});
    CHECK(parsed.ok() && flat(parsed) == "+o:ABAAC +v:ABAAD -b:*!*@spam.example.net");
    parsed = parseArgs("+o", {"ABAAA:999"}); // with an oplevel
    CHECK(parsed.ok() && flat(parsed) == "+o:ABAAA:999");

    CHECK(parseArgs("", {}).ok() && parseArgs("", {}).changes.empty());
    CHECK(parseArgs("+-+", {}).ok() && parseArgs("+-+", {}).changes.empty());

    // The typed record carries the table entry, not just a letter
    parsed = parseArgs("+R", {});
    CHECK(parsed.changes.size() == 1 && parsed.changes[0].mode.serverOnly);
    CHECK(parsed.changes[0] == (Change{true, *find('R'), ""}));
}

void testParseProblems() {
    auto parsed = parseArgs("+tx", {});
    CHECK(flat(parsed) == "+t" && onlyProblem(parsed, Error::UnknownMode, 'x'));

    // 'd' and 'z' exist in ircu, but never between servers
    parsed = parseArgs("+nd", {});
    CHECK(flat(parsed) == "+n" && onlyProblem(parsed, Error::LocalOnlyMode, 'd'));
    CHECK(onlyProblem(parseArgs("-z", {}), Error::LocalOnlyMode, 'z'));

    CHECK(onlyProblem(parseArgs("+k", {}), Error::MissingArgument, 'k'));
    CHECK(onlyProblem(parseArgs("-k", {}), Error::MissingArgument, 'k'));
    CHECK(onlyProblem(parseArgs("+l", {}), Error::MissingArgument, 'l'));
    CHECK(onlyProblem(parseArgs("+o", {}), Error::MissingArgument, 'o'));
    CHECK(onlyProblem(parseArgs("+b", {}), Error::MissingArgument, 'b'));

    // A bad argument is consumed, so the next mode still gets the right one
    parsed = parseArgs("+lk", {"many", "sekrit"});
    CHECK(flat(parsed) == "+k:sekrit" && onlyProblem(parsed, Error::InvalidLimit, 'l', "many"));

    // An unknown mode consumes nothing
    parsed = parseArgs("+xk", {"sekrit"});
    CHECK(flat(parsed) == "+k:sekrit" && onlyProblem(parsed, Error::UnknownMode, 'x'));

    CHECK(onlyProblem(parseArgs("+m", {"stray"}), Error::UnusedArgument, 0, "stray"));
    CHECK(onlyProblem(parseArgs("-l", {"10"}), Error::UnusedArgument, 0, "10"));

    CHECK(onlyProblem(parseArgs("+o", {"AB,AC"}), Error::InvalidTarget, 'o', "AB,AC"));
    CHECK(onlyProblem(parseArgs("+b", {"two words"}), Error::InvalidMask, 'b', "two words"));
    // The bug this replaces: "+b :mask <ts>" glued the timestamp onto the mask
    CHECK(onlyProblem(parseArgs("+b", {"*!bob@example.net 1789642110"}), Error::InvalidMask, 'b',
                      "*!bob@example.net 1789642110"));
    CHECK(onlyProblem(parseArgs("+b", {":*!*@x"}), Error::InvalidMask, 'b', ":*!*@x"));

    // The +A/+U passwords follow the key rules
    CHECK(onlyProblem(parseArgs("+A", {"bad,pass"}), Error::InvalidKey, 'A', "bad,pass"));
    CHECK(parseArgs("+AU", {"apass", "upass"}).ok());
}

void testKeysAndLimits() {
    CHECK(isValidKey("sekrit"));
    CHECK(isValidKey(std::string(maxKeyLength, 'k')));
    CHECK(!isValidKey(std::string(maxKeyLength + 1, 'k')));
    CHECK(!isValidKey(""));
    CHECK(!isValidKey(":key"));
    CHECK(isValidKey("k:ey"));
    CHECK(!isValidKey("a,b"));
    CHECK(!isValidKey("a b"));
    CHECK(!isValidKey(string_view("a\0b", 3))); // the NUL that reached the wire via std::ends
    CHECK(!isValidKey("a\tb"));

    CHECK(isValidLimit("1"));
    CHECK(isValidLimit("25"));
    CHECK(isValidLimit("2147483647"));
    CHECK(!isValidLimit("2147483648"));
    CHECK(!isValidLimit("99999999999999999999999"));
    CHECK(!isValidLimit("0"));
    CHECK(!isValidLimit("-5"));
    CHECK(!isValidLimit("+5"));
    CHECK(!isValidLimit("10x"));
    CHECK(!isValidLimit(" 10"));
    CHECK(!isValidLimit(""));
    CHECK(!isValidLimit(string_view("10\0", 3)));
}

void testTimestamp() {
    // Lines from the outbound recording: "<src> M <chan> <modes> <args> <ts>"
    auto parsed = parseArgs("+m", {"1789642110"}, true);
    CHECK(parsed.ok() && flat(parsed) == "+m" && parsed.timestamp == std::uint64_t{1789642110});

    // The limit is not mistaken for the timestamp, with or without one behind it
    parsed = parseArgs("+l", {"10"}, true);
    CHECK(parsed.ok() && flat(parsed) == "+l:10" && !parsed.timestamp);
    parsed = parseArgs("+l", {"10", "1789642110"}, true);
    CHECK(parsed.ok() && flat(parsed) == "+l:10" && parsed.timestamp == std::uint64_t{1789642110});

    parsed = parseArgs("-l-k", {"sekrit", "1789642110"}, true);
    CHECK(parsed.ok() && flat(parsed) == "-l -k:sekrit" &&
          parsed.timestamp == std::uint64_t{1789642110});
    parsed = parseArgs("+oo", {"ABAAC", "ABAAD", "1789642110"}, true);
    CHECK(parsed.ok() && flat(parsed) == "+o:ABAAC +o:ABAAD" && parsed.timestamp);

    // A P10 peer may leave it out
    parsed = parseArgs("+o", {"ABAAC"}, true);
    CHECK(parsed.ok() && !parsed.timestamp);

    // Not asked for: a leftover number is just a stray argument
    parsed = parseArgs("+m", {"1789642110"}, false);
    CHECK(!parsed.timestamp && onlyProblem(parsed, Error::UnusedArgument, 0, "1789642110"));

    // Only a final, all-digit argument counts
    parsed = parseArgs("+m", {"17x"}, true);
    CHECK(!parsed.timestamp && onlyProblem(parsed, Error::UnusedArgument, 0, "17x"));
    parsed = parseArgs("+m", {"1789642110", "stray"}, true);
    CHECK(!parsed.timestamp && parsed.problems.size() == 2);

    // The non-oper ClearMode defect, "M #chan -b <ts>": the timestamp is eaten as the mask
    parsed = parseArgs("-b", {"1789642110"}, true);
    CHECK(flat(parsed) == "-b:1789642110" && !parsed.timestamp);
}

} // namespace

int main() {
    testTable();
    testParseValid();
    testParseProblems();
    testKeysAndLimits();
    testTimestamp();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "channelmodes: all checks passed\n";
    return 0;
}
