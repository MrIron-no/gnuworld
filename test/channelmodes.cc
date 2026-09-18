/**
 * channelmodes.cc
 * Unit test for the channel mode table, parser and formatter of Channel.
 * Exits non-zero if any check fails, so it can run under "make check".
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Channel.h"
#include "gnuworld_config.h"

using namespace gnuworld;

// The mode types and functions are Channel's; these keep the checks readable
using Parsed = Channel::ParsedModes;
using Change = Channel::ModeChange;
using Mode = Channel::ModeInfo;
using Type = Channel::ModeType;
constexpr auto& modes = Channel::modeTable;
constexpr auto find = &Channel::findMode;
constexpr auto isLocalOnly = &Channel::isLocalOnlyMode;
constexpr auto burstOrder = &Channel::burstOrder;
constexpr auto isValidKey = &Channel::isValidKey;
constexpr auto isValidLimit = &Channel::isValidLimit;
constexpr auto burstModeBlock = &Channel::burstModeBlock;
constexpr std::size_t maxKeyLength = MAX_KEY_LENGTH;
constexpr std::size_t maxLineLength = IRC_MAX_LINE - 2;

Parsed parse(std::string_view modeString, std::span<const std::string_view> args,
             Channel::ModeParseOptions options = {}) {
    return Channel::parseModes(modeString, args, options);
}

std::vector<std::string> formatLines(std::string_view prefix, std::span<const Change> changes,
                                     std::uint64_t timestamp) {
    return Channel::formatModeLines(prefix, changes, timestamp);
}
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
    return parse(modeString, args, {.trailingTimestamp = timestamp});
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

/// True if the parse reported exactly this one problem.
bool onlyProblem(const Parsed& parsed, string_view message) {
    return parsed.problems.size() == 1 && parsed.problems[0] == message;
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
        const bool isFlagless = (mode.type == Type::Prefix || mode.type == Type::List);
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

    // doc/P11.md 15.1: s|p m t i n r D R c C u M Z, then l k A U
    const string_view burst = "spmtinrDRcCuMZlkAU";
    for (std::size_t i = 0; i + 1 < burst.size(); ++i) {
        CHECK(burstOrder(*find(burst[i])) < burstOrder(*find(burst[i + 1])));
    }

    // Usable at compile time
    static_assert(Channel::findMode('k')->type == Type::Setting);
    static_assert(!Channel::findMode('q'));
    static_assert(MAX_CHAN_MODES == 6 && MAX_KEY_LENGTH == 23);
}

/// "kAU" and "AkU" are the same group: the order inside one carries no meaning.
std::string sorted(std::string_view s) {
    std::string out(s);
    std::ranges::sort(out);
    return out;
}

/// The letters of the table that are of this type.
std::string lettersOf(Type type) {
    std::string out;
    for (const Mode& mode : modes) {
        if (mode.type == type) {
            out += mode.letter;
        }
    }
    return out;
}

void testTypes() {
    CHECK(find('b')->type == Type::List);
    CHECK(find('k')->type == Type::Setting && find('A')->type == Type::Setting &&
          find('U')->type == Type::Setting);
    CHECK(find('l')->type == Type::SetOnly);
    CHECK(find('m')->type == Type::Flag && find('D')->type == Type::Flag);
    CHECK(find('o')->type == Type::Prefix && find('v')->type == Type::Prefix);

    // ModeType is the classification of RPL_ISUPPORT, so the table has to
    // agree, group for group, with what ircu advertises (include/supported.h,
    // with OPLEVELS on):  CHANMODES=b,AkU,l,imnpstrDdRcCuMZ  PREFIX=(ov)@+
    // give or take 'd', which is local to a server and not in the table.
    const std::string_view ircu[] = {"b", "AkU", "l", "imnpstrDdRcCuMZ"};
    const Type types[] = {Type::List, Type::Setting, Type::SetOnly, Type::Flag};
    for (std::size_t i = 0; i < 4; ++i) {
        std::string theirs(ircu[i]);
        std::erase_if(theirs, isLocalOnly);
        CHECK(sorted(lettersOf(types[i])) == sorted(theirs));
    }
    CHECK(sorted(lettersOf(Type::Prefix)) == "ov");
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

    CHECK(parseArgs("", {}).ok() && parseArgs("", {}).changes.empty());
    CHECK(parseArgs("+-+", {}).ok() && parseArgs("+-+", {}).changes.empty());

    // The typed record carries the table entry, not just a letter
    parsed = parseArgs("+R", {});
    CHECK(parsed.changes.size() == 1 && parsed.changes[0].mode.flag == Channel::MODE_REG);
    CHECK(parsed.changes[0] == (Change{true, *find('R'), ""}));
}

void testParseProblems() {
    auto parsed = parseArgs("+tx", {});
    CHECK(flat(parsed) == "+t" && onlyProblem(parsed, "unknown mode 'x'"));

    // 'd' and 'z' exist in ircu, but never between servers
    parsed = parseArgs("+nd", {});
    CHECK(flat(parsed) == "+n" && onlyProblem(parsed, "mode 'd' is local to a server"));
    CHECK(onlyProblem(parseArgs("-z", {}), "mode 'z' is local to a server"));

    CHECK(onlyProblem(parseArgs("+k", {}), "mode 'k' is missing its argument"));
    CHECK(onlyProblem(parseArgs("-k", {}), "mode 'k' is missing its argument"));
    CHECK(onlyProblem(parseArgs("+l", {}), "mode 'l' is missing its argument"));
    CHECK(onlyProblem(parseArgs("+o", {}), "mode 'o' is missing its argument"));
    CHECK(onlyProblem(parseArgs("+b", {}), "mode 'b' is missing its argument"));

    // A bad argument is consumed, so the next mode still gets the right one
    parsed = parseArgs("+lk", {"many", "sekrit"});
    CHECK(flat(parsed) == "+k:sekrit" && onlyProblem(parsed, "invalid limit for mode 'l': many"));

    // An unknown mode consumes nothing
    parsed = parseArgs("+xk", {"sekrit"});
    CHECK(flat(parsed) == "+k:sekrit" && onlyProblem(parsed, "unknown mode 'x'"));

    CHECK(onlyProblem(parseArgs("+m", {"stray"}), "unused argument: stray"));
    CHECK(onlyProblem(parseArgs("-l", {"10"}), "unused argument: 10"));

    CHECK(onlyProblem(parseArgs("+o", {"AB,AC"}), "invalid target for mode 'o': AB,AC"));
    CHECK(onlyProblem(parseArgs("+b", {"two words"}), "invalid mask for mode 'b': two words"));
    // The bug this replaces: "+b :mask <ts>" glued the timestamp onto the mask
    CHECK(onlyProblem(parseArgs("+b", {"*!bob@example.net 1789642110"}),
                      "invalid mask for mode 'b': *!bob@example.net 1789642110"));
    CHECK(onlyProblem(parseArgs("+b", {":*!*@x"}), "invalid mask for mode 'b': :*!*@x"));

    // The +A/+U passwords follow the key rules
    CHECK(onlyProblem(parseArgs("+A", {"bad,pass"}), "invalid key for mode 'A': bad,pass"));
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
    CHECK(!parsed.timestamp && onlyProblem(parsed, "unused argument: 1789642110"));

    // Only a final, all-digit argument counts
    parsed = parseArgs("+m", {"17x"}, true);
    CHECK(!parsed.timestamp && onlyProblem(parsed, "unused argument: 17x"));
    parsed = parseArgs("+m", {"1789642110", "stray"}, true);
    CHECK(!parsed.timestamp && parsed.problems.size() == 2);

    // The non-oper ClearMode defect, "M #chan -b <ts>": the timestamp is eaten as the mask
    parsed = parseArgs("-b", {"1789642110"}, true);
    CHECK(flat(parsed) == "-b:1789642110" && !parsed.timestamp);
}

void testBurstModeBlock() {
    // "AB B #p11-bans 1789637417 +tnlk 25 sekrit ACAAK:o,ACAAM :%<bans>": the
    // mode block's arguments are followed by the member list.
    const std::vector<string_view> rest{"25", "sekrit", "ACAAK:o,ACAAM"};
    auto parsed = parse("+tnlk", rest, {.allowLeftover = true});
    CHECK(parsed.ok() && flat(parsed) == "+t +n +l:25 +k:sekrit" && parsed.argsUsed == 2);

    // Without the option the member list is reported, but still not consumed
    parsed = parse("+tnlk", rest);
    CHECK(parsed.argsUsed == 2 && onlyProblem(parsed, "unused argument: ACAAK:o,ACAAM"));

    // A mode block with no arguments uses none
    parsed = parse("+tn", std::vector<string_view>{"ACAAO,ACAAN:d"}, {.allowLeftover = true});
    CHECK(parsed.ok() && parsed.argsUsed == 0);

    // A truncated burst: "+l" with nothing behind it must not read past the end
    parsed = parse("+tl", std::vector<string_view>{}, {.allowLeftover = true});
    CHECK(flat(parsed) == "+t" && parsed.argsUsed == 0 &&
          onlyProblem(parsed, "mode 'l' is missing its argument"));

    // The timestamp counts as used
    parsed = parseArgs("+l", {"10", "1789642110"}, true);
    CHECK(parsed.argsUsed == 2);
}

std::vector<Change> changesFor(string_view modeString, std::vector<string_view> args) {
    const Parsed parsed = parse(modeString, args);
    CHECK(parsed.ok());
    return parsed.changes;
}

void testFormatLines() {
    const string_view prefix = "AzAAB M #modes";
    using Lines = std::vector<std::string>;

    CHECK(formatLines(prefix, {}, 1789642110).empty());

    // One space between fields, the timestamp last.  These are the lines of
    // test_outbound_channel.py without their doubled spaces.
    CHECK(formatLines(prefix, changesFor("+m", {}), 1789642110) ==
          Lines{"AzAAB M #modes +m 1789642110"});
    CHECK(formatLines(prefix, changesFor("+l", {"10"}), 1789642110) ==
          Lines{"AzAAB M #modes +l 10 1789642110"});
    CHECK(formatLines(prefix, changesFor("+oo", {"ABAAC", "ABAAD"}), 1789642110) ==
          Lines{"AzAAB M #modes +oo ABAAC ABAAD 1789642110"});

    // One sign per run of a polarity: "+ov", not "+o+v"; "-lk", not "-l-k"
    CHECK(formatLines(prefix, changesFor("+o+v", {"ABAAC", "ABAAD"}), 1) ==
          Lines{"AzAAB M #modes +ov ABAAC ABAAD 1"});
    CHECK(formatLines(prefix, changesFor("-l-k", {"sekrit"}), 1) ==
          Lines{"AzAAB M #modes -lk sekrit 1"});
    CHECK(formatLines(prefix, changesFor("+m-i+s-t", {}), 1) == Lines{"AzAAB M #modes +m-i+s-t 1"});
    CHECK(formatLines(prefix, changesFor("-k+k", {"old", "new"}), 1) ==
          Lines{"AzAAB M #modes -k+k old new 1"});

    // At most MAX_CHAN_MODES modes with an argument on a line, as ircu counts
    // with its MAXMODEPARAMS; flags do not count against it
    auto lines = formatLines(
        prefix, changesFor("+tnooooooo", {"A1", "A2", "A3", "A4", "A5", "A6", "A7"}), 5);
    CHECK(lines ==
          (Lines{"AzAAB M #modes +tnoooooo A1 A2 A3 A4 A5 A6 5", "AzAAB M #modes +o A7 5"}));
    // Any number of flags fits on one line
    lines = formatLines(prefix, changesFor("+mtinscCuMD", {}), 5);
    CHECK(lines == Lines{"AzAAB M #modes +mtinscCuMD 5"});
    // -l takes no argument, so it does not count either
    lines = formatLines(prefix, changesFor("-l+oooooo", {"A1", "A2", "A3", "A4", "A5", "A6"}), 5);
    CHECK(lines == Lines{"AzAAB M #modes -l+oooooo A1 A2 A3 A4 A5 A6 5"});

    // The polarity is restated on the next line
    lines =
        formatLines(prefix, changesFor("-vvvvvvv", {"A1", "A2", "A3", "A4", "A5", "A6", "A7"}), 5);
    CHECK(lines.size() == 2 && lines[1] == "AzAAB M #modes -v A7 5");

    // Long masks: split before the line passes 510 bytes, and lose nothing
    const std::string mask = "*!*@" + std::string(116, 'x') + ".example.net";
    const std::vector<string_view> masks(6, mask);
    lines = formatLines(prefix, changesFor("+bbbbbb", masks), 1789642110);
    CHECK(lines.size() == 2);
    std::size_t sent = 0;
    for (const std::string& line : lines) {
        CHECK(line.size() <= maxLineLength);
        CHECK(line.ends_with(" 1789642110") && line.starts_with("AzAAB M #modes +b"));
        sent += static_cast<std::size_t>(std::ranges::count(line, '!'));
    }
    CHECK(sent == 6);

    // Whatever it sends, parse() reads back as the same changes
    const std::vector<Change> mixed =
        changesFor("+tk-l+ob-v", {"sekrit", "ABAAC", "*!*@spam.example.net", "ABAAD"});
    lines = formatLines("X", mixed, 42);
    CHECK(lines.size() == 1);
    std::vector<string_view> fields;
    for (std::size_t start = 2; start <= lines[0].size();) {
        const std::size_t end = std::min(lines[0].find(' ', start), lines[0].size());
        fields.emplace_back(string_view(lines[0]).substr(start, end - start));
        start = end + 1;
    }
    const Parsed reread = parse(fields[0], std::span<const string_view>(fields).subspan(1),
                                {.trailingTimestamp = true});
    CHECK(reread.ok() && reread.changes == mixed && reread.timestamp == std::uint64_t{42});
}

void testBurstModeBlockOutput() {
    // As recorded from ircu: "+tnlk 25 sekrit", the limit before the key
    CHECK(burstModeBlock(changesFor("+ktln", {"sekrit", "25"})) == "+tnlk 25 sekrit");
    CHECK(burstModeBlock(changesFor("+nt", {})) == "+tn");
    CHECK(burstModeBlock(changesFor("+D", {})) == "+D");

    // Nothing to set, no block: the BURST line then has none, rather than a gap
    CHECK(burstModeBlock({}).empty());
    CHECK(burstModeBlock(changesFor("-m", {})).empty());

    // Only what is being set, and never members or bans
    CHECK(burstModeBlock(changesFor("+t-m+ob", {"ABAAC", "*!*@x"})) == "+t");

    // It reads back as what went in
    const Parsed reread = parseArgs("+tnlk", {"25", "sekrit"});
    CHECK(burstModeBlock(reread.changes) == "+tnlk 25 sekrit");
}

} // namespace

int main() {
    testTable();
    testTypes();
    testParseValid();
    testParseProblems();
    testKeysAndLimits();
    testTimestamp();
    testBurstModeBlock();
    testFormatLines();
    testBurstModeBlockOutput();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "channelmodes: all checks passed\n";
    return 0;
}
