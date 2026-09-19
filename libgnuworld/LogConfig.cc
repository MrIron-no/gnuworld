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

/// The name the root logger is written under in the file
const string rootName("root");

/// The text without the spaces and tabs at either end of it
string trim(const string& text) {
    string::size_type begin = 0;
    string::size_type end = text.size();

    while (begin < end && (' ' == text[begin] || '\t' == text[begin]))
        ++begin;

    while (end > begin && (' ' == text[end - 1] || '\t' == text[end - 1]))
        --end;

    return text.substr(begin, end - begin);
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
 * see.  The file is read again here, by the same rules, to find that line: a
 * line that is neither blank nor a comment and that has no '=' or nothing after
 * it is a line EConfig refuses.  Empty when there is no such line.
 */
string findMalformedLine(const string& fileName) {
    std::ifstream in(fileName.c_str());
    string line;
    std::size_t number = 0;

    while (std::getline(in, line)) {
        ++number;

        const string trimmed = trim(line);

        if (trimmed.empty() || '#' == trimmed[0])
            continue;

        const string::size_type equals = trimmed.find('=');
        const string where = " (line " + std::to_string(number) + ")";

        if (string::npos == equals)
            return "\"" + trimmed + "\" is not a key = value line" + where;

        // EConfig takes the spaces out of a key, so the message names it as it
        // would have been keyed
        string key;

        for (const char c : trimmed.substr(0, equals))
            if (' ' != c && '\t' != c)
                key += c;

        if (trim(trimmed.substr(equals + 1)).empty())
            return key + ": the value is empty" + where;
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
        const string where = findMalformedLine(fileName);

        errors.push_back(where.empty() ? ("cannot read " + fileName) : where);

        return false;
    }

    std::map<string, SinkDraft> sinks;
    std::map<string, LoggerDraft> loggers;

    for (EConfig::const_iterator entry = file.begin(); entry != file.end(); ++entry) {
        const string& key = entry->first;
        const string value = trim(entry->second);

        /* ---- sink.<id>.<setting> ---- */
        if (hasPrefix(key, sinkPrefix)) {
            const string rest = key.substr(sinkPrefix.size());
            const string::size_type dot = rest.find_last_of('.');

            if (string::npos == dot) {
                errors.push_back(key + ": not a sink setting, expected sink.<id>.<setting>");
                continue;
            }

            const string id = rest.substr(0, dot);
            const string written = rest.substr(dot + 1);
            const string setting = lower(written);

            if (!validSinkId(id)) {
                errors.push_back(key + ": \"" + id +
                                 "\" is not a sink id (letters, digits, '_' and '-')");
                continue;
            }

            SinkDraft& draft = sinks[id];
            draft.spec.id = id;

            if (!draft.settings.insert(setting).second) {
                errors.push_back(key + ": given twice");
                continue;
            }

            if ("type" == setting) {
                if (value.empty()) {
                    errors.push_back(key + ": the sink type is empty");
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
                    errors.push_back(key + ": \"" + value +
                                     "\" is not a format, expected json or text");
            } else if ("level" == setting) {
                Verbosity level = TRACE;

                if (parseLevel(value, level))
                    draft.spec.level = level;
                else
                    errors.push_back(key + ": \"" + value + "\" is not a level");
            } else if ("colour" == setting) {
                const string colour = lower(value);
                bool wanted = true;

                if ("auto" == colour)
                    draft.spec.colour = ConsoleSink::Colour::Auto;
                else if (parseBoolean(colour, wanted))
                    draft.spec.colour = wanted ? ConsoleSink::Colour::Yes : ConsoleSink::Colour::No;
                else
                    errors.push_back(key + ": \"" + value +
                                     "\" is not a colour, expected auto, yes or no");
            } else if ("highlight" == setting) {
                bool wanted = true;

                if (parseBoolean(value, wanted))
                    draft.spec.highlight = wanted;
                else
                    errors.push_back(key + ": \"" + value + "\" is not a yes or a no");
            } else {
                errors.push_back(key + ": unknown sink setting \"" + written + "\"");
            }

            continue;
        }

        /* ---- logger.<dotted.name> = LEVEL[, sinkid ...] ---- */
        if (hasPrefix(key, loggerPrefix)) {
            const string written = key.substr(loggerPrefix.size());

            if (written.empty()) {
                errors.push_back(key + ": the logger name is empty");
                continue;
            }

            // The name keeps the case it was written in; only "root" is special
            const string name = (rootName == written) ? string() : written;

            LoggerDraft& draft = loggers[name];
            draft.spec.name = name;

            if (draft.spec.level) {
                errors.push_back(key + ": this logger is configured twice");
                continue;
            }

            const std::vector<string> parts = splitList(value);
            Verbosity level = INFO;

            if (parts[0].empty()) {
                errors.push_back(key + ": no level given");
                continue;
            }

            if (!parseLevel(parts[0], level)) {
                errors.push_back(key + ": \"" + parts[0] + "\" is not a level");
                continue;
            }

            draft.spec.level = level;
            draft.levelKey = key;

            for (std::size_t at = 1; at < parts.size(); ++at) {
                if (parts[at].empty()) {
                    errors.push_back(key + ": an empty sink id in the list");
                    continue;
                }

                bool already = false;

                for (const string& known : draft.spec.sinks)
                    if (known == parts[at]) {
                        already = true;
                        break;
                    }

                if (already) {
                    errors.push_back(key + ": the sink \"" + parts[at] + "\" is listed twice");
                    continue;
                }

                draft.spec.sinks.push_back(parts[at]);
            }

            continue;
        }

        /* ---- additivity.<dotted.name> = yes | no ---- */
        if (hasPrefix(key, additivityPrefix)) {
            const string written = key.substr(additivityPrefix.size());

            if (written.empty()) {
                errors.push_back(key + ": the logger name is empty");
                continue;
            }

            const string name = (rootName == written) ? string() : written;

            LoggerDraft& draft = loggers[name];
            draft.spec.name = name;

            if (draft.spec.additive) {
                errors.push_back(key + ": the additivity of this logger is given twice");
                continue;
            }

            bool wanted = true;

            if (!parseBoolean(value, wanted)) {
                errors.push_back(key + ": \"" + value + "\" is not a yes or a no");
                continue;
            }

            draft.spec.additive = wanted;

            continue;
        }

        errors.push_back(key + ": unknown key, expected sink., logger. or additivity.");
    }

    // What a sink of each kind cannot do without
    for (const std::pair<const string, SinkDraft>& entry : sinks) {
        const string prefix = sinkPrefix + entry.first;

        if (!entry.second.haveType) {
            errors.push_back(prefix + ": no type given, " + prefix + ".type is required");
            continue;
        }

        if ("file" == entry.second.spec.type && entry.second.spec.path.empty())
            errors.push_back(prefix + ".path: a file sink needs a path");

        if ("irc" == entry.second.spec.type && entry.second.spec.channel.empty())
            errors.push_back(prefix + ".channel: an irc sink needs a channel");
    }

    /* A logger may only be sent to a sink the file itself names.  Whether that
     * kind of sink exists is not this function's business: the registry of
     * LogManager knows the kinds, and says so when it is asked to build one */
    for (const std::pair<const string, LoggerDraft>& entry : loggers)
        for (const string& id : entry.second.spec.sinks)
            if (sinks.end() == sinks.find(id))
                errors.push_back(entry.second.levelKey + ": unknown sink \"" + id + "\"");

    if (!errors.empty()) {
        // The file is taken as a whole or not at all
        out = LogConfig();

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
