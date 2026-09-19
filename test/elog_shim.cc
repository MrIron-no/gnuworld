/**
 * elog_shim.cc
 * Unit test for libgnuworld/ELog.h: the deprecated elog is a front end of the
 * logger now, so every line it is given becomes one DEBUG record on the logger
 * "legacy" - one line per std::endl, one buffer per thread, the format state of
 * a real stream, and the log file, its layout and its reopening that core still
 * asks for through openFile().  It links libgnuworld alone.
 * Runs under "make check".
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include "ELog.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"

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

/// Reports the value as well as the failure, for the string comparisons
#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const std::string got_((actual));                                                          \
        const std::string want_((expected));                                                       \
        if (got_ != want_) {                                                                       \
            ++failures;                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #actual "\n  got  [" << got_  \
                      << "]\n  want [" << want_ << "]\n";                                          \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ *
 * A scratch directory of our own, gone again when the test ends
 * ------------------------------------------------------------------ */

std::string scratchDir;

void removeScratchDir() {
    if (scratchDir.empty())
        return;

    DIR* const dir = opendir(scratchDir.c_str());
    if (nullptr != dir) {
        for (const struct dirent* entry = readdir(dir); nullptr != entry; entry = readdir(dir)) {
            if (0 == std::strcmp(entry->d_name, ".") || 0 == std::strcmp(entry->d_name, ".."))
                continue;
            const std::string victim = scratchDir + '/' + entry->d_name;
            ::unlink(victim.c_str());
        }
        closedir(dir);
    }

    ::rmdir(scratchDir.c_str());
    scratchDir.clear();
}

/// Creates the scratch directory; returns false when the system will not have it
bool makeScratchDir() {
    const char* const tmp = std::getenv("TMPDIR");
    std::string pattern = (nullptr != tmp && '\0' != tmp[0]) ? tmp : "/tmp";
    pattern += "/gnuworld_elogshim_XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');

    if (nullptr == mkdtemp(buffer.data()))
        return false;

    scratchDir = buffer.data();
    std::atexit(removeScratchDir);

    return true;
}

std::string scratchPath(const std::string& name) { return scratchDir + '/' + name; }

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/// Everything elog has logged since the last forget(), oldest first
std::shared_ptr<CaptureSink> capture;

/// Forgets the records of the cases before this one
void forget() { capture->clear(); }

/// How many records elog has logged since the last forget()
std::size_t logged() { return capture->size(); }

/// The message of one record, or a complaint that says which one is missing
std::string messageOf(std::size_t which) {
    if (which >= capture->records.size())
        return "<no record " + std::to_string(which) + ">";

    return capture->records[which].message;
}

/// The whole content of a file, empty when it is not there
std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();

    return out.str();
}

/// The lines of a text, without the trailing empty one a final newline makes
std::vector<std::string> splitFileLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;

    for (const char c : text) {
        if ('\n' == c) {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }

    if (!current.empty())
        lines.push_back(current);

    return lines;
}

/**
 * Whether a line of the log file is this message, in the text layout: a full
 * date, the level, the logger name padded to the width of the longest name,
 * and the message, every column separated by two spaces.
 */
bool isLegacyTextLine(const std::string& line, const std::string& message) {
    // "YYYY-MM-DD HH:MM:SS.mmm" and then the columns
    if (line.size() < 23 || '-' != line[4] || '-' != line[7] || ' ' != line[10])
        return false;

    const std::string columns = "  DEBUG  legacy";
    const std::string::size_type at = line.find(columns);
    if (23 != at)
        return false;

    // The name column is padded to the width every text sink agrees on, and
    // then two spaces separate it from the message
    std::string::size_type rest = at + columns.size();
    while (rest < line.size() && ' ' == line[rest])
        ++rest;

    if (rest < at + columns.size() + 2)
        return false;

    return line.substr(rest) == message;
}

/* ------------------------------------------------------------------ *
 * One line, one record
 * ------------------------------------------------------------------ */

/**
 * A completed elog line is one record on "legacy", at DEBUG, with the pieces
 * it was streamed and no function: nothing of the statement says which one it
 * would be.
 */
void testOneLine() {
    forget();

    elog << "a" << 1 << std::endl;

    CHECK(1 == logged());
    if (1 != logged())
        return;

    const LogRecord& record = capture->records[0];

    CHECK(DEBUG == record.level);
    CHECK_EQ(record.logger, "legacy");
    CHECK_EQ(record.message, "a1");
    CHECK(record.function.empty());
}

/**
 * A statement that does not end its line logs nothing; the line is logged when
 * some later statement ends it, as one record with everything in it.
 */
void testLineAcrossStatements() {
    forget();

    elog << "half a line, ";
    CHECK(0 == logged());

    elog << "and the rest" << std::endl;

    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "half a line, and the rest");
}

/**
 * Newlines: an endl on its own has no line to log, a newline the caller
 * streamed at the end of the line is the sink's to add, and a newline inside
 * the line is part of the message.
 */
void testNewlines() {
    forget();

    elog << std::endl;
    CHECK(0 == logged());

    elog << "x\n" << std::endl;
    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "x");

    elog << "l1\nl2" << std::endl;
    CHECK(2 == logged());
    CHECK_EQ(messageOf(1), "l1\nl2");
}

/**
 * The buffer is a stream: a manipulator applies to it and stays applied until
 * the thread says otherwise, and std::flush has nothing to flush.
 */
void testManipulators() {
    forget();

    elog << std::hex << 255 << std::endl;
    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "ff");

    elog << std::dec << 255 << std::endl;
    CHECK(2 == logged());
    CHECK_EQ(messageOf(1), "255");

    elog << std::flush;
    CHECK(2 == logged());

    elog << "nothing was flushed away" << std::endl;
    CHECK(3 == logged());
    CHECK_EQ(messageOf(2), "nothing was flushed away");
}

/**
 * What std::endl is where a module's copy of it is not libgnuworld's: libc++
 * (FreeBSD, macOS) keeps std::endl out of its ABI, so every shared object has
 * one of its own, at an address of its own.  It does what std::endl does.
 */
std::ostream& anotherObjectsEndl(std::ostream& stream) {
    stream.put('\n');
    stream.flush();

    return stream;
}

/// And a manipulator that is not the end of any line
std::ostream& writesAStar(std::ostream& stream) { return stream << '*'; }

/**
 * The end of a line is known by what the manipulator does, not by where it
 * lives: a module built against libc++ ends its lines too.
 */
void testAnEndlOfAnotherSharedObject() {
    forget();

    elog << "a module's line" << anotherObjectsEndl;

    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "a module's line");

    // More than once: the second time is answered from what the first learnt
    elog << "and another" << anotherObjectsEndl;

    CHECK(2 == logged());
    CHECK_EQ(messageOf(1), "and another");

    // Any other manipulator is the buffer's business
    elog << "a" << writesAStar << "b" << std::endl;

    CHECK(3 == logged());
    CHECK_EQ(messageOf(2), "a*b");

    // The same through a module logger's stream interface
    const std::shared_ptr<CaptureSink> streamed = std::make_shared<CaptureSink>();
    Logger* const logger = LogManager::get("elogshimtest");
    logger->addSink(streamed, TRACE);

    logger->write(INFO) << "streamed by a module" << anotherObjectsEndl;

    CHECK(1 == streamed->size());
    if (1 == streamed->size())
        CHECK_EQ(streamed->records[0].message, "streamed by a module");
}

/* ------------------------------------------------------------------ *
 * Threads
 * ------------------------------------------------------------------ */

/// Whether a message is "t1-<n>" or "t2-<n>", and nothing else
bool isThreadLine(const std::string& message, char& which) {
    if (message.size() < 4 || 't' != message[0] || '-' != message[2])
        return false;

    if ('1' != message[1] && '2' != message[1])
        return false;

    for (std::size_t at = 3; at < message.size(); ++at)
        if (message[at] < '0' || message[at] > '9')
            return false;

    which = message[1];

    return true;
}

/**
 * Two threads writing at once: the buffer is one per thread, so no line is
 * lost and no half of one turns up inside another.
 */
void testTwoThreads() {
    forget();

    const int lines = 500;

    const auto writer = [lines](const char* tag) {
        for (int n = 0; n < lines; ++n)
            elog << tag << '-' << n << std::endl;
    };

    std::thread one(writer, "t1");
    std::thread two(writer, "t2");

    one.join();
    two.join();

    CHECK(static_cast<std::size_t>(2 * lines) == logged());

    int fromOne = 0;
    int fromTwo = 0;

    for (const LogRecord& record : capture->records) {
        char which = '\0';

        if (!isThreadLine(record.message, which)) {
            ++failures;
            std::cerr << __FILE__ << ':' << __LINE__ << ": failed: not a thread line ["
                      << record.message << "]\n";
            continue;
        }

        if ('1' == which)
            ++fromOne;
        else
            ++fromTwo;
    }

    CHECK(lines == fromOne);
    CHECK(lines == fromTwo);
}

/**
 * A thread that ends in the middle of a line takes the half of it with it: the
 * buffer is the thread's own, and its end is not a crash and not a line of
 * somebody else's.
 */
void testThreadEndsMidLine() {
    forget();

    std::thread orphan([]() { elog << "never finished by its thread"; });
    orphan.join();

    CHECK(0 == logged());

    elog << "a line of the main thread" << std::endl;

    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "a line of the main thread");
}

/* ------------------------------------------------------------------ *
 * The level of "legacy", and what core asks of elog
 * ------------------------------------------------------------------ */

/**
 * "logger.legacy = INFO" really silences elog, and taking that line away
 * brings it back at the DEBUG its own code asks for.  A silenced line is
 * dropped, not kept: it must not turn up in front of the next one.
 */
void testConfiguredLevelSilences() {
    Logger* const legacy = LogManager::get("legacy");

    forget();
    legacy->setConfigLevel(INFO);

    elog << "not at this level" << std::endl;
    CHECK(0 == logged());

    legacy->setConfigLevel(std::optional<Verbosity>());

    elog << "and here we are again" << std::endl;
    CHECK(1 == logged());
    CHECK_EQ(messageOf(0), "and here we are again");
}

/**
 * setStream() is how the console is turned on and off now, and getStream() and
 * isOpen() still answer what core asks them, which is whether anything of a
 * message is seen anywhere.
 */
void testStreamAndConsole() {
    elog.setStream(nullptr);
    CHECK(!ConsoleSink::enabled());
    CHECK(nullptr == elog.getStream());

    elog.setStream(&std::cout);
    CHECK(ConsoleSink::enabled());
    CHECK(&std::cout == elog.getStream());

    // The rest of this test logs a good deal; none of it belongs on a terminal
    elog.setStream(nullptr);
    CHECK(!ConsoleSink::enabled());
}

/**
 * openFile() gives the logger a log file of the old name, in the new layout,
 * appended to rather than truncated; asking again for the same path adds no
 * second file and reopens the one there is, which is how a SIGHUP picks the
 * new file up after logrotate moved the old one away.
 */
void testLogFile() {
    CHECK(!elog.isOpen());

    const std::string path = scratchPath("debug.log");

    // What a previous run of the process left behind, which is kept
    {
        std::ofstream before(path, std::ios::out | std::ios::trunc);
        before << "a line of the run before this one\n";
    }

    CHECK(elog.openFile(path));
    CHECK(elog.isOpen());

    forget();
    elog << "into the file" << std::endl;
    CHECK(1 == logged());

    const std::vector<std::string> appended = splitFileLines(readFile(path));

    CHECK(2 == appended.size());
    if (2 == appended.size()) {
        CHECK_EQ(appended[0], "a line of the run before this one");
        CHECK(isLegacyTextLine(appended[1], "into the file"));
        if (!isLegacyTextLine(appended[1], "into the file"))
            std::cerr << "  the line was [" << appended[1] << "]\n";
    }

    // logrotate moves the file away and SIGHUP reaches openFile() again
    const std::string rotated = scratchPath("debug.log.1");
    CHECK(0 == ::rename(path.c_str(), rotated.c_str()));
    CHECK(elog.openFile(path));

    forget();
    elog << "after the rotation" << std::endl;
    CHECK(1 == logged());

    // One line, so the second openFile() added no second file of the same path
    const std::vector<std::string> fresh = splitFileLines(readFile(path));

    CHECK(1 == fresh.size());
    if (1 == fresh.size()) {
        CHECK(isLegacyTextLine(fresh[0], "after the rotation"));
        if (!isLegacyTextLine(fresh[0], "after the rotation"))
            std::cerr << "  the line was [" << fresh[0] << "]\n";
    }

    // And what was logged before the rotation stayed where it was written
    const std::vector<std::string> kept = splitFileLines(readFile(rotated));

    CHECK(2 == kept.size());
}

/* ------------------------------------------------------------------ *
 * The way out of the process
 * ------------------------------------------------------------------ */

/**
 * One line at exit, from an atexit handler registered before the main thread
 * ever used elog: its thread_local buffer has been destroyed by the time this
 * runs, and streaming a line here must not be a use of it after that.  The
 * record itself is nobody's to read any more; the exit status is the check.
 */
void logAtExit() { elog << "an atexit handler logs on the way out" << std::endl; }

} // namespace

int main() {
    /* Nothing of this test belongs on a terminal: the console sink the root is
     * given until something is configured would print every line of it */
    LogManager::root()->removeSink(LogManager::bootstrapConsoleSink());

    if (!makeScratchDir()) {
        std::cerr << "elog_shim: cannot create a scratch directory\n";
        return 1;
    }

    // Registered before the first elog line, so that the buffer of this thread
    // is destroyed before this handler runs
    std::atexit(logAtExit);

    capture = std::make_shared<CaptureSink>();
    LogManager::get("legacy")->addSink(capture, TRACE);

    testOneLine();
    testLineAcrossStatements();
    testNewlines();
    testManipulators();
    testAnEndlOfAnotherSharedObject();
    testTwoThreads();
    testThreadEndsMidLine();
    testConfiguredLevelSilences();
    testStreamAndConsole();
    testLogFile();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "elog_shim: all checks passed\n";
    return 0;
}
