/**
 * LogConfig.cc
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
 */
#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "EConfig.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogSinks.h"

#include "LogConfig.h"

namespace gnuworld {

using std::string;

namespace {

/// What a key of logging.conf may begin with, and nothing else may
const string sinkPrefix("sink.");
const string loggerPrefix("logger.");
const string additivityPrefix("additivity.");

/**
 * What is not part of a key or a value at either end of it.
 *
 * The '\r' is in there because a file saved on Windows ends every line with one,
 * and neither EConfig nor anything else takes it off: without this a level reads
 * as "warn\r" and a sink id as "main\r", and a perfectly good file is refused
 * line by line.
 */
const string blanks(" \t\r\n\v\f");

/// The three bytes an editor may put in front of the first line of a file
const string byteOrderMark("\xEF\xBB\xBF");

/**
 * The most a logger name may be.
 *
 * A name is a path through a hierarchy, and the registry creates a logger for
 * every prefix of one: a name of tens of thousands of segments is not a deep
 * hierarchy but a file asking the process for the square of what the file cost
 * to write.  Anything past either of these is an ordinary parse error, which is
 * what a file that is not a configuration has always been.
 */
const string::size_type maximumNameLength = 255;
const std::size_t maximumNameSegments = 16;

/// The most a sink id may be, for the same reason and in the same spirit
const string::size_type maximumSinkIdLength = 64;

/// The most a message says of a key or a value a file wrote
const string::size_type maximumShownLength = 64;

/**
 * The most errors a report about one file carries.  A single line may name
 * half a million sinks, and every one of them that is unknown is an error of
 * its own: what comes back is the first few of them and a count of the rest,
 * which is as much as anyone reads anyway.
 */
const std::size_t maximumErrors = 20;

/// The ellipsis a truncated key or a shortened list of errors ends with
const string ellipsis("\xE2\x80\xA6");

/**
 * What a message shows of something a file wrote: nothing raw, and no more of
 * it than a reader wants to see.  A key or a value may be of any length at
 * all, and an error about one is read by a person, not parsed by anything.
 */
string shown(const string& text) {
    const string escaped = escapeControl(text);

    if (escaped.size() <= maximumShownLength)
        return escaped;

    return escaped.substr(0, maximumShownLength) + ellipsis;
}

/**
 * The errors, no more than maximumErrors of them, the last saying how many of
 * them are not being shown.  suppressed is how many were never written down in
 * the first place, which is what a loop over one monstrous line counts instead
 * of allocating a string for every step of it.
 */
void capErrors(std::vector<string>& errors, std::size_t suppressed = 0) {
    if (0 == suppressed && errors.size() <= maximumErrors)
        return;

    std::size_t hidden = suppressed;

    if (errors.size() > maximumErrors - 1) {
        hidden += errors.size() - (maximumErrors - 1);
        errors.resize(maximumErrors - 1);
    }

    errors.push_back(ellipsis + " and " + std::to_string(hidden) + " more");
}

/// The text without the blank characters at either end of it
string trim(const string& text) {
    const string::size_type begin = text.find_first_not_of(blanks);

    if (string::npos == begin)
        return string();

    return text.substr(begin, text.find_last_not_of(blanks) - begin + 1);
}

/// The text without a byte order mark in front of it
string withoutByteOrderMark(const string& text) {
    return 0 == text.compare(0, byteOrderMark.size(), byteOrderMark)
               ? text.substr(byteOrderMark.size())
               : text;
}

/// Whether the file begins with a byte order mark
bool fileStartsWithByteOrderMark(const string& fileName) {
    std::ifstream in(fileName.c_str());
    char head[3] = {0, 0, 0};

    in.read(head, sizeof(head));

    return in.gcount() == static_cast<std::streamsize>(sizeof(head)) &&
           byteOrderMark == string(head, sizeof(head));
}

/// The text in lower case, for the values and setting names that are read so
string lower(const string& text) {
    string folded;
    folded.reserve(text.size());

    for (const char c : text)
        folded += static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);

    return folded;
}

/// Whether the key begins with this prefix, whatever case it was written in
bool hasPrefix(const string& key, const string& prefix) {
    return key.size() >= prefix.size() && lower(key.substr(0, prefix.size())) == prefix;
}

/// The parts of a comma separated list, each without the space around it
std::vector<string> splitList(const string& value) {
    std::vector<string> parts;
    string::size_type at = 0;

    for (;;) {
        const string::size_type comma = value.find(',', at);

        if (string::npos == comma) {
            parts.push_back(trim(value.substr(at)));
            break;
        }

        parts.push_back(trim(value.substr(at, comma - at)));
        at = comma + 1;
    }

    return parts;
}

/// A yes or a no of the file, however it was spelled
bool parseBoolean(const string& value, bool& out) {
    const string folded = lower(value);

    if ("yes" == folded || "true" == folded || "on" == folded || "1" == folded) {
        out = true;
        return true;
    }

    if ("no" == folded || "false" == folded || "off" == folded || "0" == folded) {
        out = false;
        return true;
    }

    return false;
}

/**
 * The logger a "logger." or "additivity." key is about, false when it is about
 * none.
 *
 * The name is normalised before anything else is done with it, because that is
 * the name the registry knows the logger under: "logger.x" and "logger..x" are
 * two lines about one logger and not two, which is what makes the second of them
 * the duplicate it is.
 */
bool loggerName(const string& written, string& name) {
    name = LogManager::normaliseName(written);

    // The registry gives the empty name to the root, however "root" was spelt
    // ("root", "root.", ".root"), and to a name that is nothing but dots - which
    // names no logger at all and is the one thing refused here
    if (name.empty())
        return string::npos != written.find_first_not_of('.');

    return true;
}

/**
 * Whether a normalised logger name is one a hierarchy could have, and why not
 * when it is not.
 */
bool nameWithinLimits(const string& name, string& why) {
    if (name.size() > maximumNameLength) {
        why = "the logger name is longer than " + std::to_string(maximumNameLength) + " bytes";

        return false;
    }

    std::size_t segments = name.empty() ? 0 : 1;

    for (const char c : name)
        if ('.' == c)
            ++segments;

    if (segments > maximumNameSegments) {
        why = "the logger name has more than " + std::to_string(maximumNameSegments) + " segments";

        return false;
    }

    return true;
}

/// Whether this is a sink id: letters, digits, '_' and '-', and at least one
bool validSinkId(const string& id) {
    if (id.empty())
        return false;

    for (const char c : id) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || '_' == c || '-' == c;

        if (!allowed)
            return false;
    }

    return true;
}

/**
 * Names the line EConfig would not read.
 *
 * EConfig keeps nothing of a file it could not read to the end, and says which
 * line it stumbled over through elog rather than through anything a caller can
 * see.  The file is read again here, by the same rules, to find that line:
 * EConfig splits a line on '=' and drops the empty pieces, so a line that is
 * neither blank nor a comment and that has no '=', nothing before it or nothing
 * after it is a line EConfig refuses.  Empty when there is no such line.
 */
string findMalformedLine(const string& fileName) {
    std::ifstream in(fileName.c_str());
    string line;
    std::size_t number = 0;

    while (std::getline(in, line)) {
        ++number;

        // A mark in front of the first line is not what is wrong with that line
        if (1 == number)
            line = withoutByteOrderMark(line);

        const string trimmed = trim(line);

        if (trimmed.empty() || '#' == trimmed[0])
            continue;

        const string::size_type equals = trimmed.find('=');
        const string where = " (line " + std::to_string(number) + ")";

        if (string::npos == equals)
            return "\"" + shown(trimmed) + "\" is not a key = value line" + where;

        // EConfig takes the spaces out of a key, so the message names it as it
        // would have been keyed
        string key;

        for (const char c : trimmed.substr(0, equals))
            if (' ' != c && '\t' != c)
                key += c;

        key = trim(key);

        if (key.empty())
            return "line " + std::to_string(number) + ": no key before '='";

        if (trim(trimmed.substr(equals + 1)).empty())
            return shown(key) + ": the value is empty" + where;
    }

    return string();
}

/// One sink of the file while it is being read, and what has been said about it
struct SinkDraft {
    SinkSpec spec;
    bool haveType = false;
    std::set<string> settings;
};

/// One logger of the file while it is being read
struct LoggerDraft {
    LoggerSpec spec;
    string levelKey;
};

} // namespace

/**
 * The text with every control character written as an escape, so that nothing
 * quoted out of a file can paint a terminal or break a log line in two.
 */
string escapeControl(const string& text) {
    static const char* const digits = "0123456789abcdef";

    string shown;
    shown.reserve(text.size());

    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);

        if (byte < 0x20 || 0x7f == byte) {
            shown += "\\x";
            shown += digits[byte >> 4];
            shown += digits[byte & 0x0f];
        } else
            shown += c;
    }

    return shown;
}

/**
 * Reads logging.conf and turns it into sinks and loggers.
 *
 * Every key of the file is looked at, and a key that is not right is one error
 * added to the list; the file is applied only if the list is empty at the end,
 * so that a typo in one line never leaves the process half configured.  The
 * loggers and the sinks come back sorted, by name and by id, so that two reads
 * of one file are the same thing.
 */
bool parseLogConfig(const string& fileName, LogConfig& out, std::vector<string>& errors) {
    out = LogConfig();
    errors.clear();

    /* EConfig reports a file it cannot open the same way it reports a file it
     * cannot parse, so the two are told apart here, before it is asked */
    {
        std::ifstream probe(fileName.c_str());

        if (!probe.is_open()) {
            errors.push_back("cannot open " + fileName);
            return false;
        }
    }

    // EConfig::Require() is the only thing in EConfig that exits, and this
    // never calls it: a bad logging.conf is reported, never fatal
    EConfig file(fileName);

    if (file.hasError()) {
        /* A line that is wrong for a reason of its own is named first, mark or
         * no mark.  A mark in front of the first key is taken off that key
         * below; a mark in front of a comment is one EConfig stumbles over
         * although nothing else is wrong with the file, and then the mark is
         * the thing to say */
        const string where = findMalformedLine(fileName);

        if (!where.empty())
            errors.push_back(where);
        else if (fileStartsWithByteOrderMark(fileName))
            errors.push_back(fileName +
                             ": starts with a UTF-8 byte order mark; save it without one");
        else
            errors.push_back("cannot read " + fileName);

        return false;
    }

    std::map<string, SinkDraft> sinks;
    std::map<string, LoggerDraft> loggers;

    /* One line may list half a million sinks, and each of them may be wrong in
     * a way of its own.  Past the cap those are counted rather than written
     * down: the report says how many there were, and a file cannot make this
     * function allocate a string for every comma in it */
    std::size_t suppressed = 0;

    const auto addListError = [&errors, &suppressed](const string& text) {
        if (errors.size() < maximumErrors)
            errors.push_back(text);
        else
            ++suppressed;
    };

    for (EConfig::const_iterator entry = file.begin(); entry != file.end(); ++entry) {
        /* The key as the file meant it: without the blanks EConfig leaves at
         * either end of it, and without the byte order mark an editor may have
         * put in front of the first one */
        const string key = withoutByteOrderMark(trim(entry->first));
        const string value = trim(entry->second);

        // What a message may say about this key, with nothing raw left in it
        const string shownKey = shown(key);

        /* ---- sink.<id>.<setting> ---- */
        if (hasPrefix(key, sinkPrefix)) {
            const string rest = key.substr(sinkPrefix.size());
            const string::size_type dot = rest.find_last_of('.');

            if (string::npos == dot) {
                errors.push_back(shownKey + ": not a sink setting, expected sink.<id>.<setting>");
                continue;
            }

            const string id = rest.substr(0, dot);
            const string written = rest.substr(dot + 1);
            const string setting = lower(written);

            if (id.size() > maximumSinkIdLength) {
                errors.push_back(shownKey + ": the sink id is longer than " +
                                 std::to_string(maximumSinkIdLength) + " bytes");
                continue;
            }

            if (!validSinkId(id)) {
                errors.push_back(shownKey + ": \"" + shown(id) +
                                 "\" is not a sink id (letters, digits, '_' and '-')");
                continue;
            }

            SinkDraft& draft = sinks[id];
            draft.spec.id = id;

            if (!draft.settings.insert(setting).second) {
                errors.push_back(shownKey + ": given twice");
                continue;
            }

            if ("type" == setting) {
                if (value.empty()) {
                    errors.push_back(shownKey + ": the sink type is empty");
                    continue;
                }

                draft.spec.type = lower(value);
                draft.haveType = true;
            } else if ("path" == setting) {
                draft.spec.path = value;
            } else if ("channel" == setting) {
                draft.spec.channel = value;
            } else if ("format" == setting) {
                const string format = lower(value);

                /* A console and a channel are always read by a human, so this
                 * says nothing to them - but it still has to be a format */
                if ("json" == format)
                    draft.spec.json = true;
                else if ("text" == format)
                    draft.spec.json = false;
                else
                    errors.push_back(shownKey + ": \"" + shown(value) +
                                     "\" is not a format, expected json or text");
            } else if ("level" == setting) {
                Verbosity level = TRACE;

                if (parseLevel(value, level))
                    draft.spec.level = level;
                else
                    errors.push_back(shownKey + ": \"" + shown(value) + "\" is not a level");
            } else if ("colour" == setting) {
                const string colour = lower(value);
                bool wanted = true;

                if ("auto" == colour)
                    draft.spec.colour = ConsoleSink::Colour::Auto;
                else if (parseBoolean(colour, wanted))
                    draft.spec.colour = wanted ? ConsoleSink::Colour::Yes : ConsoleSink::Colour::No;
                else
                    errors.push_back(shownKey + ": \"" + shown(value) +
                                     "\" is not a colour, expected auto, yes or no");
            } else if ("highlight" == setting) {
                bool wanted = true;

                if (parseBoolean(value, wanted))
                    draft.spec.highlight = wanted;
                else
                    errors.push_back(shownKey + ": \"" + shown(value) + "\" is not a yes or a no");
            } else {
                errors.push_back(shownKey + ": unknown sink setting \"" + shown(written) + "\"");
            }

            continue;
        }

        /* ---- logger.<dotted.name> = LEVEL[, sinkid ...] ---- */
        if (hasPrefix(key, loggerPrefix)) {
            const string written = key.substr(loggerPrefix.size());
            string name;

            /* The name the registry knows this logger under, which is what the
             * duplicate below is a duplicate of.  A name of nothing but dots
             * names no logger, and only the exact word "root" is the root: a
             * name is read case for case, so "ROOT" is a logger of its own */
            if (!loggerName(written, name)) {
                errors.push_back(shownKey + ": empty logger name");
                continue;
            }

            string why;

            if (!nameWithinLimits(name, why)) {
                errors.push_back(shownKey + ": " + why);
                continue;
            }

            LoggerDraft& draft = loggers[name];
            draft.spec.name = name;

            if (draft.spec.level) {
                errors.push_back(shownKey + ": this logger is configured twice");
                continue;
            }

            const std::vector<string> parts = splitList(value);
            Verbosity level = INFO;

            if (parts[0].empty()) {
                errors.push_back(shownKey + ": no level given");
                continue;
            }

            if (!parseLevel(parts[0], level)) {
                errors.push_back(shownKey + ": \"" + shown(parts[0]) + "\" is not a level");
                continue;
            }

            draft.spec.level = level;
            draft.levelKey = shownKey;

            // Kept beside the list: half a million distinct ids must not be
            // compared with each other one by one
            std::set<string> listed;

            for (std::size_t at = 1; at < parts.size(); ++at) {
                if (parts[at].empty()) {
                    addListError(shownKey + ": an empty sink id in the list");
                    continue;
                }

                if (!listed.insert(parts[at]).second) {
                    addListError(shownKey + ": the sink \"" + shown(parts[at]) +
                                 "\" is listed twice");
                    continue;
                }

                draft.spec.sinks.push_back(parts[at]);
            }

            continue;
        }

        /* ---- additivity.<dotted.name> = yes | no ---- */
        if (hasPrefix(key, additivityPrefix)) {
            const string written = key.substr(additivityPrefix.size());
            string name;

            // Normalised first here as well, and for the same reason
            if (!loggerName(written, name)) {
                errors.push_back(shownKey + ": empty logger name");
                continue;
            }

            string why;

            if (!nameWithinLimits(name, why)) {
                errors.push_back(shownKey + ": " + why);
                continue;
            }

            LoggerDraft& draft = loggers[name];
            draft.spec.name = name;

            if (draft.spec.additive) {
                errors.push_back(shownKey + ": the additivity of this logger is given twice");
                continue;
            }

            bool wanted = true;

            if (!parseBoolean(value, wanted)) {
                errors.push_back(shownKey + ": \"" + shown(value) + "\" is not a yes or a no");
                continue;
            }

            draft.spec.additive = wanted;

            continue;
        }

        errors.push_back(shownKey + ": unknown key, expected sink., logger. or additivity.");
    }

    /* Which key claimed a file first, by the path as it was written: two file
     * sinks on one file are two streams with a lock each appending to it, and a
     * record of the one could land in the middle of a record of the other.  The
     * paths are compared as they stand - resolving one is the business of
     * whoever opens it, not of this */
    std::map<string, string> filePaths;

    // What a sink of each kind cannot do without
    for (const std::pair<const string, SinkDraft>& entry : sinks) {
        const string prefix = sinkPrefix + entry.first;

        if (!entry.second.haveType) {
            errors.push_back(prefix + ": no type given, " + prefix + ".type is required");
            continue;
        }

        if ("file" == entry.second.spec.type) {
            if (entry.second.spec.path.empty())
                errors.push_back(prefix + ".path: a file sink needs a path");
            else {
                const std::pair<std::map<string, string>::iterator, bool> claimed =
                    filePaths.emplace(entry.second.spec.path, prefix + ".path");

                if (!claimed.second)
                    errors.push_back(prefix + ".path: same file as " + claimed.first->second);
            }
        }

        if ("irc" == entry.second.spec.type && entry.second.spec.channel.empty())
            errors.push_back(prefix + ".channel: an irc sink needs a channel");
    }

    /* A logger may only be sent to a sink the file itself names.  Whether that
     * kind of sink exists is not this function's business: the registry of
     * LogManager knows the kinds, and says so when it is asked to build one */
    for (const std::pair<const string, LoggerDraft>& entry : loggers)
        for (const string& id : entry.second.spec.sinks)
            if (sinks.end() == sinks.find(id))
                addListError(entry.second.levelKey + ": unknown sink \"" + shown(id) + "\"");

    if (!errors.empty() || 0 != suppressed) {
        // The file is taken as a whole or not at all
        out = LogConfig();

        capErrors(errors, suppressed);

        return false;
    }

    out.sinks.reserve(sinks.size());
    out.loggers.reserve(loggers.size());

    for (const std::pair<const string, SinkDraft>& entry : sinks)
        out.sinks.push_back(entry.second.spec);

    for (const std::pair<const string, LoggerDraft>& entry : loggers)
        out.loggers.push_back(entry.second.spec);

    return true;
}

} // namespace gnuworld
