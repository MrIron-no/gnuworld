/**
 * logger_sinks.cc
 * Unit test for the sinks in libgnuworld/LogSinks.h: the file sink's append
 * and reopen behaviour, the console sink's switch and colour, the name column
 * width shared by both, and the capture sink used by the other tests.
 * Runs under "make check".
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "LogRecord.h"
#include "LogSink.h"
#include "LogSinks.h"

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

/// Reports the value as well as the failure, for the long string comparisons
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
    pattern += "/gnuworld_logsinks_XXXXXX";

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

/// How often needle occurs in haystack
std::size_t countOf(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;

    return count;
}

/// The permission bits of a file, or 07777 for a file that is not there, which
/// is a value no check below expects
mode_t fileMode(const std::string& path) {
    struct stat info{};

    if (0 != ::stat(path.c_str(), &info))
        return 07777;

    return info.st_mode & 07777;
}

/// The process umask while this lives, back to what it was when it dies
class UmaskWhile {
  public:
    explicit UmaskWhile(mode_t mask) : saved(::umask(mask)) {}
    ~UmaskWhile() { ::umask(saved); }

  private:
    mode_t saved;
};

/// 'd' matches one digit, every other character matches itself
bool shapeMatches(std::string_view text, std::string_view shape) {
    if (text.size() != shape.size())
        return false;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if ('d' == shape[i]) {
            if (text[i] < '0' || text[i] > '9')
                return false;
        } else if (text[i] != shape[i]) {
            return false;
        }
    }

    return true;
}

/// 2026-09-18 12:34:56.789 local time, so that the time column is known
std::chrono::system_clock::time_point computeFixedTime() {
    std::tm broken{};
    broken.tm_year = 2026 - 1900;
    broken.tm_mon = 8;
    broken.tm_mday = 18;
    broken.tm_hour = 12;
    broken.tm_min = 34;
    broken.tm_sec = 56;
    broken.tm_isdst = -1;

    const std::time_t when = std::mktime(&broken);

    return std::chrono::system_clock::from_time_t(when) + std::chrono::milliseconds(789);
}

/// Worked out once: mktime() reads the time zone, and the writer threads of
/// testFileSinkTwoThreads() must not do that side by side
std::chrono::system_clock::time_point fixedTime() {
    static const std::chrono::system_clock::time_point fixed = computeFixedTime();

    return fixed;
}

LogRecord makeRecord(Verbosity level, const std::string& logger, const std::string& message) {
    LogRecord record;
    record.time = fixedTime();
    record.level = level;
    record.logger = logger;
    record.message = message;

    return record;
}

/* ------------------------------------------------------------------ *
 * The file sink
 * ------------------------------------------------------------------ */

void testFileSinkJsonAppends() {
    const std::string path = scratchPath("append.log");

    // Somebody was here before us, and keeps his line
    {
        std::ofstream prior(path, std::ios::out | std::ios::trunc);
        prior << "prior content\n";
    }

    {
        FileSink sink(path, true);
        CHECK(sink.isOpen());
        CHECK_EQ(sink.path(), path);

        sink.emit(makeRecord(INFO, "core", "one"));
        sink.emit(makeRecord(WARN, "core.net", "two"));
        sink.emit(makeRecord(ERROR, "cservice", "three"));
    }

    const std::vector<std::string> lines = splitFileLines(readFile(path));
    CHECK(lines.size() == 4);
    if (lines.size() != 4)
        return;

    CHECK_EQ(lines[0], "prior content");
    for (std::size_t i = 1; i < lines.size(); ++i) {
        CHECK(!lines[i].empty() && '{' == lines[i].front() && '}' == lines[i].back());
        CHECK(countOf(lines[i], "\"message\":") == 1);
    }

    CHECK(countOf(lines[1], "\"one\"") == 1);
    CHECK(countOf(lines[2], "\"two\"") == 1);
    CHECK(countOf(lines[3], "\"three\"") == 1);
}

void testFileSinkReopen() {
    const std::string path = scratchPath("rotate.log");
    const std::string moved = scratchPath("rotate.log.1");

    FileSink sink(path, true);
    CHECK(sink.isOpen());

    sink.emit(makeRecord(INFO, "core", "before rotation"));

    CHECK(0 == ::rename(path.c_str(), moved.c_str()));
    sink.reopen();
    CHECK(sink.isOpen());
    CHECK_EQ(sink.path(), path);

    sink.emit(makeRecord(INFO, "core", "after rotation"));

    const std::vector<std::string> fresh = splitFileLines(readFile(path));
    CHECK(fresh.size() == 1);
    if (fresh.size() == 1)
        CHECK(countOf(fresh[0], "\"after rotation\"") == 1);

    const std::vector<std::string> old = splitFileLines(readFile(moved));
    CHECK(old.size() == 1);
    if (old.size() == 1)
        CHECK(countOf(old[0], "\"before rotation\"") == 1);
}

void testFileSinkText() {
    const std::string path = scratchPath("text.log");

    {
        FileSink sink(path, false);
        sink.emit(makeRecord(DEBUG, "core", "a plain sentence"));
    }

    const std::vector<std::string> lines = splitFileLines(readFile(path));
    CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return;

    // The file always shows the full date, and never a colour
    CHECK(shapeMatches(std::string_view(lines[0]).substr(0, 23), "dddd-dd-dd dd:dd:dd.ddd"));
    CHECK(lines[0].find('\x1b') == std::string::npos);
    CHECK(lines[0].find("a plain sentence") != std::string::npos);
}

void testFileSinkUnopenable() {
    const std::string path = scratchPath("no/such/directory/x.log");

    FileSink sink(path, true);
    CHECK(!sink.isOpen());
    CHECK_EQ(sink.path(), path);

    bool threw = false;
    try {
        sink.emit(makeRecord(ERROR, "core", "nowhere to go"));
        sink.reopen();
        sink.emit(makeRecord(ERROR, "core", "still nowhere"));
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(!sink.isOpen());
}

/**
 * A log file holds addresses, accounts, and the command lines of users: the
 * sink creates it readable by its own user and its own group and by nobody
 * else, whatever the umask of the process would have allowed.  A file that is
 * already there is the operator's, and keeps the mode the operator gave it.
 */
void testFileSinkCreatesPrivateFile() {
    {
        const UmaskWhile mask(0);
        const std::string path = scratchPath("mode-open.log");

        FileSink sink(path, true);
        CHECK(sink.isOpen());
        sink.emit(makeRecord(INFO, "core", "for us alone"));

        CHECK(fileMode(path) == 0640);
    }

    // The umask still takes away what it takes away
    {
        const UmaskWhile mask(077);
        const std::string path = scratchPath("mode-tight.log");

        FileSink sink(path, true);
        CHECK(sink.isOpen());
        sink.emit(makeRecord(INFO, "core", "for me alone"));

        CHECK(fileMode(path) == 0600);
    }

    {
        const UmaskWhile mask(0);
        const std::string path = scratchPath("mode-existing.log");

        {
            std::ofstream prior(path, std::ios::out | std::ios::trunc);
            prior << "prior content\n";
        }
        CHECK(0 == ::chmod(path.c_str(), 0644));

        FileSink sink(path, true);
        CHECK(sink.isOpen());
        sink.emit(makeRecord(INFO, "core", "appended to somebody else's file"));

        CHECK(fileMode(path) == 0644);

        const std::vector<std::string> lines = splitFileLines(readFile(path));
        CHECK(lines.size() == 2);
    }

    // And the same mode after a rotation, which opens the path again
    {
        const UmaskWhile mask(0);
        const std::string path = scratchPath("mode-rotated.log");
        const std::string moved = scratchPath("mode-rotated.log.1");

        FileSink sink(path, true);
        sink.emit(makeRecord(INFO, "core", "before rotation"));

        CHECK(0 == ::rename(path.c_str(), moved.c_str()));
        sink.reopen();
        sink.emit(makeRecord(INFO, "core", "after rotation"));

        CHECK(sink.isOpen());
        CHECK(fileMode(path) == 0640);
    }
}

void testFileSinkTwoThreads() {
    const std::string path = scratchPath("threads.log");
    const int perThread = 1000;

    {
        FileSink sink(path, true);
        CHECK(sink.isOpen());

        const auto writer = [&sink](const std::string& logger) {
            for (int i = 0; i < perThread; ++i)
                sink.emit(makeRecord(INFO, logger, "thread record"));
        };

        std::thread first(writer, std::string("core"));
        std::thread second(writer, std::string("cservice"));
        first.join();
        second.join();
    }

    const std::vector<std::string> lines = splitFileLines(readFile(path));
    CHECK(lines.size() == static_cast<std::size_t>(2 * perThread));

    std::size_t intact = 0;
    for (const std::string& line : lines)
        if (!line.empty() && '{' == line.front() && '}' == line.back() &&
            1 == countOf(line, "\"message\":"))
            ++intact;

    CHECK(intact == lines.size());
}

/* ------------------------------------------------------------------ *
 * The console sink
 * ------------------------------------------------------------------ */

/// std::cout while this lives, back to the terminal when it dies
class CoutCapture {
  public:
    CoutCapture() : saved(std::cout.rdbuf(captured.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(saved); }

    std::string text() const { return captured.str(); }

  private:
    std::ostringstream captured;
    std::streambuf* saved;
};

void testConsoleSinkDisabled() {
    ConsoleSink sink(ConsoleSink::Colour::No, false);

    ConsoleSink::setEnabled(false);
    CHECK(!ConsoleSink::enabled());
    {
        const CoutCapture capture;
        sink.emit(makeRecord(INFO, "core", "not a word"));
        CHECK_EQ(capture.text(), "");
    }

    ConsoleSink::setEnabled(true);
    CHECK(ConsoleSink::enabled());
}

void testConsoleSinkColour() {
    ConsoleSink::setEnabled(true);

    ConsoleSink plain(ConsoleSink::Colour::No, false);
    std::string text;
    {
        const CoutCapture capture;
        plain.emit(makeRecord(WARN, "core", "plain as day"));
        text = capture.text();
    }

    CHECK(splitFileLines(text).size() == 1);
    CHECK(!text.empty() && '\n' == text.back());
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find("plain as day") != std::string::npos);

    ConsoleSink coloured(ConsoleSink::Colour::Yes, true);
    std::string painted;
    {
        const CoutCapture capture;
        coloured.emit(makeRecord(WARN, "core", "in colour"));
        painted = capture.text();
    }

    CHECK(splitFileLines(painted).size() == 1);
    CHECK(painted.find("\x1b[") != std::string::npos);
    CHECK(painted.find("in colour") != std::string::npos);
}

/* ------------------------------------------------------------------ *
 * The name column, and the capture sink
 * ------------------------------------------------------------------ */

void testNameWidth() {
    CHECK(LogSinks::nameWidth() == 12);

    const std::string path = scratchPath("width.log");

    LogSinks::setNameWidth(20);
    CHECK(LogSinks::nameWidth() == 20);

    {
        FileSink sink(path, false);
        sink.emit(makeRecord(DEBUG, "core", "wide column"));
    }

    LogSinks::setNameWidth(12);
    CHECK(LogSinks::nameWidth() == 12);

    const std::vector<std::string> lines = splitFileLines(readFile(path));
    CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return;

    // "YYYY-MM-DD HH:MM:SS.mmm" "  " "DEBUG" "  " then the name column
    const std::size_t nameAt = 23 + 2 + 5 + 2;
    CHECK(lines[0].size() > nameAt + 20 + 2);
    if (lines[0].size() <= nameAt + 20 + 2)
        return;

    CHECK_EQ(lines[0].substr(nameAt, 20), "core" + std::string(16, ' '));
    CHECK_EQ(lines[0].substr(nameAt + 20), "  wide column");
}

void testCaptureSink() {
    CaptureSink sink;
    CHECK(sink.size() == 0);

    LogRecord record = makeRecord(INFO, "core", "the original");
    sink.emit(record);
    CHECK(sink.size() == 1);
    if (sink.size() != 1)
        return;

    // The sink kept a copy, not a handle on our record
    record.message = "changed afterwards";
    record.logger = "somebody.else";
    CHECK_EQ(sink.records[0].message, "the original");
    CHECK_EQ(sink.records[0].logger, "core");
    CHECK(sink.records[0].level == INFO);

    sink.clear();
    CHECK(sink.size() == 0);
    CHECK(sink.records.empty());
}

/// Every sink answers the two questions the logger asks before it dispatches
void testSinkDefaults() {
    CaptureSink capture;
    FileSink file(scratchPath("defaults.log"), true);
    ConsoleSink console(ConsoleSink::Colour::No, false);

    const LogSink* const sinks[3] = {&capture, &file, &console};
    for (const LogSink* const sink : sinks) {
        CHECK(!sink->mainThreadOnly());
        CHECK(!sink->suppressOnReentry());
    }
}

} // namespace

int main() {
    if (!makeScratchDir()) {
        std::cerr << "logger_sinks: cannot create a scratch directory\n";
        return 1;
    }

    testFileSinkJsonAppends();
    testFileSinkReopen();
    testFileSinkText();
    testFileSinkUnopenable();
    testFileSinkCreatesPrivateFile();
    testFileSinkTwoThreads();
    testConsoleSinkDisabled();
    testConsoleSinkColour();
    testNameWidth();
    testCaptureSink();
    testSinkDefaults();

    ConsoleSink::setEnabled(true);

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "logger_sinks: all checks passed\n";
    return 0;
}
