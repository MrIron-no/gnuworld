/**
 * pushover_text.cc
 * Unit test for the two text helpers of libnotifier/pushover.h: the cut that
 * never lands inside a UTF-8 character, and the clean-up that takes the control
 * characters out of a title and a message.  Both are pure text and need no
 * libcurl, so this runs on every machine and under "make check".
 *
 * What is really being asserted is that nothing a remote IRC server or an IRC
 * user can put into a log sentence - a nick, a channel name, free text, an
 * invalid byte sequence - can make either of them read out of bounds, loop, or
 * hand the request builder a string longer than Pushover's limit or ending in
 * half a character.
 */

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>

#include "pushover.h"

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

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const auto got_ = (actual);                                                                \
        const auto want_ = (expected);                                                             \
        if (got_ != want_) {                                                                       \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #actual "\n  got  [" << got_  \
                      << "]\n  want [" << want_ << "]\n";                                          \
        }                                                                                          \
    } while (0)

/// The three characters truncateUtf8() marks a cut with
const std::string ellipsis("...");

/* The three multi-byte characters the cases below cut through, as the bytes they
 * really are: two, three and four of them */
const std::string twoByte("\xC3\xA9");          // e with an acute accent
const std::string threeByte("\xE2\x82\xAC");    // the euro sign
const std::string fourByte("\xF0\x9D\x84\x9E"); // a musical G clef

/**
 * Whether every byte of text belongs to a complete UTF-8 character.
 *
 * Written out here rather than borrowed from libgnuworld: this is the property
 * the helper promises, and a test that asserts it with the code under test's own
 * idea of a character boundary asserts nothing.
 */
bool isWholeUtf8(const std::string& text) {
    std::size_t at = 0;

    while (at < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;

        if (0x80 > lead)
            length = 1;
        else if (0xC0 == (lead & 0xE0))
            length = 2;
        else if (0xE0 == (lead & 0xF0))
            length = 3;
        else if (0xF0 == (lead & 0xF8))
            length = 4;
        else
            return false; // A continuation byte, or a byte no lead byte is

        if (at + length > text.size())
            return false; // A character the string ends in the middle of

        for (std::size_t on = 1; on < length; ++on)
            if (0x80 != (static_cast<unsigned char>(text[at + on]) & 0xC0))
                return false;

        at += length;
    }

    return true;
}

/* ------------------------------------------------------------------ *
 * truncateUtf8
 * ------------------------------------------------------------------ */

/// A string that fits, and one that fits exactly, are handed back untouched
void testWhatFitsIsUntouched() {
    CHECK_EQ(truncateUtf8("short", 10), std::string("short"));
    CHECK_EQ(truncateUtf8("exactly-8", 9), std::string("exactly-8"));
    CHECK_EQ(truncateUtf8("", 10), std::string(""));
    CHECK_EQ(truncateUtf8("", 0), std::string(""));
}

/// A string that does not fit ends in the ellipsis and is never over the limit
void testAsciiTooLong() {
    CHECK_EQ(truncateUtf8("0123456789", 8), std::string("01234..."));
    CHECK_EQ(truncateUtf8("0123456789", 8).size(), std::size_t(8));

    // One byte over: one byte of content and the mark of what was cut
    CHECK_EQ(truncateUtf8("0123456789", 9), std::string("012345..."));

    for (std::size_t limit = 4; limit < 20; ++limit) {
        const std::string cut = truncateUtf8(std::string(40, 'x'), limit);

        CHECK(cut.size() <= limit);
        CHECK(cut.size() >= ellipsis.size());
        CHECK_EQ(cut.substr(cut.size() - ellipsis.size()), ellipsis);
    }
}

/**
 * A cut that would land inside a two, three or four byte character steps back to
 * where that character begins: the result is whole UTF-8, always.
 */
void testAMultiByteCharacterStraddlingTheCut() {
    // "aaaa" then the character, so that the cut at limit - 3 falls inside it
    const std::string two("aaaa" + twoByte + "bbbb");
    const std::string three("aaaa" + threeByte + "bbbb");
    const std::string four("aaaa" + fourByte + "bbbb");

    // The cut lands on the character's last byte, its middle, or its first
    CHECK_EQ(truncateUtf8(two, 8), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(two, 7), std::string("aaaa..."));

    CHECK_EQ(truncateUtf8(three, 9), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(three, 8), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(three, 7), std::string("aaaa..."));

    CHECK_EQ(truncateUtf8(four, 10), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(four, 9), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(four, 8), std::string("aaaa..."));
    CHECK_EQ(truncateUtf8(four, 7), std::string("aaaa..."));

    // A cut past the character keeps it whole, which is the other direction
    CHECK_EQ(truncateUtf8(two, 9), std::string("aaaa" + twoByte + "..."));

    /* And at every limit at all, over a string of every width of character:
     * within the limit, and never ending in half a character */
    const std::string mixed("a" + twoByte + "b" + threeByte + "c" + fourByte + "d" + twoByte +
                            threeByte + fourByte + "e");

    for (std::size_t limit = 0; limit <= mixed.size() + 4; ++limit) {
        const std::string cut = truncateUtf8(mixed, limit);

        CHECK(cut.size() <= limit);

        if (!isWholeUtf8(cut)) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: limit " << limit
                      << " cut through a character\n";
        }
    }
}

/**
 * Bytes that are not UTF-8 at all - a string of continuation bytes, which is
 * what a cut through a character elsewhere leaves behind - end the walk back at
 * the beginning rather than before it.
 */
void testInvalidUtf8() {
    const std::string continuations(10, '\x80');

    CHECK_EQ(truncateUtf8(continuations, 8), ellipsis);
    CHECK(truncateUtf8(continuations, 8).size() <= std::size_t(8));

    // Nothing at all fits, and nothing at all is what comes back
    CHECK_EQ(truncateUtf8(continuations, 2), std::string());
    CHECK_EQ(truncateUtf8(continuations, 0), std::string());

    // A sequence that was already cut short: the walk back stops at its lead byte
    CHECK_EQ(truncateUtf8("aaaa\xF0\x9D"
                          "bbbb",
                          8),
             std::string("aaaa..."));

    // And every limit over it terminates and stays within itself
    const std::string rubbish("\x80\xFF\xC3\x80\x80\xF5\xBF\xBF\xBF\x80");

    for (std::size_t limit = 0; limit <= rubbish.size() + 4; ++limit)
        CHECK(truncateUtf8(rubbish, limit).size() <= limit);
}

/**
 * A limit no larger than the ellipsis has no room to both say something and mark
 * that something was cut, so it says what fits and marks nothing.
 */
void testALimitSmallerThanTheEllipsis() {
    CHECK_EQ(truncateUtf8("abcdef", 3), std::string("abc"));
    CHECK_EQ(truncateUtf8("abcdef", 2), std::string("ab"));
    CHECK_EQ(truncateUtf8("abcdef", 1), std::string("a"));
    CHECK_EQ(truncateUtf8("abcdef", 0), std::string());

    // And not half a character even there
    CHECK_EQ(truncateUtf8(twoByte + "x", 1), std::string());
    CHECK_EQ(truncateUtf8(twoByte + "x", 2), twoByte);
    CHECK_EQ(truncateUtf8(threeByte + "x", 2), std::string());
}

/* ------------------------------------------------------------------ *
 * withoutControlCharacters
 * ------------------------------------------------------------------ */

/**
 * A newline is a line of a multi-line record and stays; every other C0 control
 * character and DEL is taken out, and every byte of 0x80 and up - the UTF-8 a
 * nick or a channel name is made of - is left exactly as it is.
 */
void testControlCharactersGo() {
    CHECK_EQ(withoutControlCharacters("plain text"), std::string("plain text"));
    CHECK_EQ(withoutControlCharacters(""), std::string());

    // The newline stays, and both of them
    CHECK_EQ(withoutControlCharacters("one\ntwo\nthree"), std::string("one\ntwo\nthree"));

    // The rest of C0 goes: a NUL, a bell, an escape, a carriage return, a tab
    CHECK_EQ(withoutControlCharacters(std::string("a\0b", 3)), std::string("ab"));
    CHECK_EQ(withoutControlCharacters("a\x07"
                                      "b"),
             std::string("ab"));
    CHECK_EQ(withoutControlCharacters("a\x1B[31mb"), std::string("a[31mb"));
    CHECK_EQ(withoutControlCharacters("a\r\nb"), std::string("a\nb"));
    CHECK_EQ(withoutControlCharacters("a\tb"), std::string("ab"));

    // And DEL, which is not in C0 and is no more use in a notification
    CHECK_EQ(withoutControlCharacters("a\x7F"
                                      "b"),
             std::string("ab"));

    // Every byte of 0x80 and up is text, however invalid a sequence it makes
    CHECK_EQ(withoutControlCharacters(twoByte + threeByte + fourByte),
             twoByte + threeByte + fourByte);
    CHECK_EQ(withoutControlCharacters("\x80\xFF"), std::string("\x80\xFF"));

    // A string of nothing but control characters is nothing at all
    CHECK_EQ(withoutControlCharacters(std::string("\x01\x02\x03\x1F\x7F")), std::string());
}

} // namespace

int main() {
    testWhatFitsIsUntouched();
    testAsciiTooLong();
    testAMultiByteCharacterStraddlingTheCut();
    testInvalidUtf8();
    testALimitSmallerThanTheEllipsis();
    testControlCharactersGo();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "pushover_text: all checks passed\n";
    return 0;
}
