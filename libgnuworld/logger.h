/**
 * logger.h
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

#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "LogFormat.h"
#include "LogManager.h"
#include "LogRecord.h"
#include "LogRender.h"
#include "LogSink.h"

/**
 * Names the logger the LOG and LOG_MSG macros of this code write to, by defining
 * the moduleLogger() they resolve.
 *
 * It is used at GLOBAL scope - outside every namespace - once per module, in the
 * module's common header, so that every translation unit of the module logs
 * under the module's name.  Core code, which is one binary and not one module,
 * uses it once per .cc file with the sub-logger of that file, and never in a
 * header, where it would name the logger of whoever includes it.
 *
 * Usage: GNUWORLD_MODULE_LOGGER("cservice");
 *
 * The accessor has internal linkage on purpose.  Modules are separate shared
 * objects and all of their code lives in namespace gnuworld, so an exported
 * gnuworld::moduleLogger() would be one symbol for all of them: ELF
 * interposition would bind every module to whichever one was loaded first, and
 * cservice would log to dronescan's logger.  The unnamed namespace gives each
 * object file its own accessor, which nothing outside it can reach.
 *
 * Code that has a Logger* of its own - shared code acting for a module, or a
 * class holding the logger it was handed - uses LOG_TO / LOG_MSG_TO instead.
 */
#define GNUWORLD_MODULE_LOGGER(name)                                                               \
    namespace gnuworld {                                                                           \
    namespace {                                                                                    \
    [[maybe_unused]] inline Logger* moduleLogger() {                                               \
        static Logger* const l = ::gnuworld::LogManager::get(name);                                \
        return l;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    }

/**
 * Main logging macro for simple formatted messages.
 * Automatically passes the current function name and supports format strings.
 * Usage: LOG(INFO, "User {} connected", username);
 */
#define LOG(x, ...) moduleLogger()->writeFunc(x, __PRETTY_FUNCTION__, __VA_ARGS__)

/**
 * The same, to a logger named at the call site rather than the module's.
 * Usage: LOG_TO(::gnuworld::LogManager::get("core.net"), INFO, "listening");
 */
#define LOG_TO(loggerPtr, x, ...) (loggerPtr)->writeFunc(x, __PRETTY_FUNCTION__, __VA_ARGS__)

/**
 * Structured logging macro with template support and field extraction.
 * Allows mixing format arguments with named placeholders and structured fields.
 * Usage: LOG_MSG(INFO, "User {} joined {channel}", username).with("channel", chanPtr).log();
 */
#define LOG_MSG(level, template_msg, ...)                                                          \
    moduleLogger()->createMessage(level, __PRETTY_FUNCTION__, template_msg, ##__VA_ARGS__)

/**
 * The same, to a logger named at the call site rather than the module's.
 */
#define LOG_MSG_TO(loggerPtr, level, template_msg, ...)                                            \
    (loggerPtr)->createMessage(level, __PRETTY_FUNCTION__, template_msg, ##__VA_ARGS__)

namespace gnuworld {

class LogManager;

/**
 * Main logging system for GNUWorld services.
 * A logger has a name, a level and a list of sinks with a threshold each; a log
 * statement becomes one LogRecord, which the logger hands to every sink that
 * wants it.  What a sink then does with the record - a JSON line in a file, a
 * column on the console, a notice in a channel, a push notification - is the
 * sink's business and none of the logger's, so nothing here knows anything
 * about IRC.
 *
 * Loggers are not created here but asked of LogManager, which puts each of them
 * in its place in the dotted hierarchy: a record logged on "cservice.sql" goes
 * to the sinks of that logger, then of "cservice", then of the root, and the
 * level of "cservice" is the level of "cservice.sql" unless that one has a level
 * of its own.  A logger lives for as long as the process does.
 *
 * Objects of other layers reach a log message through an extractor, registered
 * process-wide for their type: it says how the object shows in a sentence and
 * what fields it contributes to a JSON record.
 */
class Logger {
    /// Only the registry creates loggers, and nothing ever destroys one
    friend class LogManager;

  public:
    /**
     * The name this logger writes under, which every record carries.
     */
    const std::string& getName() const { return name; }

    /**
     * The logger this one hangs under, and whose sinks and level it shares
     * unless it says otherwise; null for the root.
     */
    Logger* getParent() const { return parent; }

    /**
     * The level this logger logs at: its own configured level, else the level
     * its legacy module keys asked for, else the default its code supplied,
     * else whatever its parent logs at, and INFO when nothing at all was said.
     *
     * The legacy level and the code default are properties of this logger only:
     * a child inherits the effective level of its parent, not the reason for it.
     */
    Verbosity effectiveLevel() const;

    /**
     * Whether a record of this level is worth building at all.  A call site
     * asks before it formats anything, so that a message nobody will read
     * costs no more than the comparison.
     */
    bool shouldLog(Verbosity v) const;

    /**
     * Sets the most verbose level this logger accepts, which is what a
     * configuration file asks for and what beats everything else.
     */
    void setLevel(Verbosity);

    /**
     * The level of the configuration file, or nothing when the file says
     * nothing about this logger: the highest precedence there is.
     */
    void setConfigLevel(std::optional<Verbosity>);

    /**
     * The level the legacy per-module configuration keys asked for, which a
     * configured level beats and which beats the default of the code.
     */
    void setLegacyLevel(std::optional<Verbosity>);

    /**
     * The level this logger has unless something above says otherwise, as in
     * child("sql", ERROR).  The first caller decides: asking again changes
     * nothing, so that one module cannot move another's default.
     */
    void setCodeDefault(Verbosity);

    /**
     * Whether a record of this logger also reaches the sinks of its ancestors.
     * True unless it is set otherwise.  This is what the code asks for, which
     * the configuration file overrides while it says anything about it.
     */
    void setAdditive(bool);

    /**
     * What an "additivity.<name>" line of the configuration file asked for, or
     * nothing when the file says nothing about this logger, in which case the
     * answer of the code stands again.
     */
    void setConfigAdditive(std::optional<bool>);

    /// Whether the records of this logger walk up to the sinks of its parent
    bool isAdditive() const;

    /**
     * The logger of this name below this one, created if it does not exist:
     * root()->child("cservice")->child("sql") is the logger "cservice.sql".
     */
    Logger* child(const std::string& sub);

    /**
     * The same, giving the child the level it logs at unless a configuration
     * file or a legacy key says otherwise.
     */
    Logger* child(const std::string& sub, Verbosity codeDefault);

    /**
     * Adds a destination, which receives every record of this logger at or
     * below the threshold.  A sink is always held by shared_ptr: the logger
     * keeps it alive for as long as one of its records is on its way there.
     *
     * A sink attached to more than one logger of a dispatch path hears each
     * record once, and hears it if any one of those attachments lets it
     * through: the most permissive threshold of them is the one that counts.
     *
     * This is the list of the code, which a configuration reload leaves alone.
     */
    void addSink(std::shared_ptr<LogSink>, Verbosity threshold = TRACE);

    /**
     * Adds a destination the configuration file asked for.  These are the
     * sinks clearConfigSinks() takes away again when the file is read anew.
     */
    void addConfigSink(std::shared_ptr<LogSink>, Verbosity threshold);

    /**
     * Forgets every destination the configuration file asked for, leaving the
     * ones the code attached.
     */
    void clearConfigSinks();

    /**
     * Removes a destination.  A record already on its way there is delivered.
     */
    void removeSink(const std::shared_ptr<LogSink>&);

    /**
     * Changes the threshold of a destination this logger already has.
     */
    void setSinkThreshold(const std::shared_ptr<LogSink>&, Verbosity);

    /**
     * Adds a field every record of this logger carries, such as the name of
     * the bot it belongs to.  A key the logger already has is replaced.
     */
    void setContext(const std::string& key, LogValue value);

    /**
     * Fills in the logger's name, its context and the time, and hands the
     * record to every sink whose threshold it passes.
     *
     * A record the effective level of this logger does not admit goes nowhere,
     * and nothing is ever logged at OFF: the guards at the call sites spare the
     * formatting of such a record, this one is what makes the level the
     * logger's own answer rather than the caller's.
     */
    void log(LogRecord&& r);

    /**
     * What the logging system makes of one object of a registered type.
     */
    using Extractor = std::function<LogObject(const void*)>;

    /**
     * Template-based structured logging class.
     * A template string, its positional arguments and the fields added with
     * with(); log() renders the sentence and builds the record, and does
     * neither when the level rules the record out.
     */
    class MessageTemplate {
      public:
        /**
         * Constructor for MessageTemplate with optional format arguments.
         * Each positional argument is captured by copy, so that nothing it
         * refers to has to outlive the log statement.
         */
        template <typename... FormatArgs>
        MessageTemplate(Logger* logger, Verbosity lvl, const char* f, const std::string& tmpl,
                        FormatArgs&&... args)
            : templateStr(tmpl), level(lvl), func(f), loggerInstance(logger) {
            if constexpr (sizeof...(args) > 0) {
                positional.reserve(sizeof...(args));
                (positional.push_back(makeLogArg(args)), ...);
            }
        }

        /**
         * Adds a structured field.
         * A value keeps its type: a bool stays a bool, a signed integer or an
         * enum becomes an int64, an unsigned one a uint64, a floating point
         * number a double, and anything string-like a string.
         *
         * A pointer to a type with a registered extractor becomes a display
         * entry under key, which the template shows, plus one field
         * "key_<sub>" per sub-field, which JSON shows.  A null pointer of such
         * a type shows as "(null)" and emits "key":null.  Any other pointer is
         * its own address.
         *
         * Nothing at all happens when the record's level is already ruled out:
         * an extractor of a DEBUG message on an INFO logger never runs.
         */
        template <typename T> MessageTemplate& with(const std::string& key, T&& value) {
            if (!loggerInstance->shouldLog(level))
                return *this;

            using Bare = std::decay_t<T>;

            if constexpr (std::is_same_v<Bare, bool>) {
                fields.push_back(LogField{key, LogValue(value), false});
            } else if constexpr (std::is_same_v<Bare, std::string>) {
                fields.push_back(LogField{key, LogValue(std::string(value)), false});
            } else if constexpr (std::is_same_v<Bare, std::string_view>) {
                fields.push_back(LogField{key, LogValue(std::string(value)), false});
            } else if constexpr (std::is_same_v<Bare, char*> || std::is_same_v<Bare, const char*>) {
                if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
                    // A string literal, which is never null
                    fields.push_back(LogField{key, LogValue(std::string(value)), false});
                } else {
                    fields.push_back(LogField{
                        key,
                        LogValue(nullptr == value ? std::string("(null)") : std::string(value)),
                        false});
                }
            } else if constexpr (std::is_enum_v<Bare>) {
                if constexpr (std::is_signed_v<std::underlying_type_t<Bare>>)
                    fields.push_back(
                        LogField{key, LogValue(static_cast<std::int64_t>(value)), false});
                else
                    fields.push_back(
                        LogField{key, LogValue(static_cast<std::uint64_t>(value)), false});
            } else if constexpr (std::is_floating_point_v<Bare>) {
                fields.push_back(LogField{key, LogValue(static_cast<double>(value)), false});
            } else if constexpr (std::is_integral_v<Bare>) {
                if constexpr (std::is_signed_v<Bare>)
                    fields.push_back(
                        LogField{key, LogValue(static_cast<std::int64_t>(value)), false});
                else
                    fields.push_back(
                        LogField{key, LogValue(static_cast<std::uint64_t>(value)), false});
            } else if constexpr (std::is_pointer_v<Bare>) {
                withPointer<std::remove_const_t<std::remove_pointer_t<Bare>>>(key, value);
            } else {
                fields.push_back(LogField{key, LogValue(std::format("{}", value)), false});
            }

            return *this;
        }

        /**
         * Renders the sentence and hands the record to the logger.
         * Nothing is rendered when the level rules the record out.
         */
        void log() const;

        /**
         * The name log() had while the fields were still a map of strings.
         */
        void logStructured() const { log(); }

      private:
        /**
         * Adds a pointer, through the extractor of its type when there is one.
         * The extractor is looked up for the type without the pointee's const,
         * so that a T* and a const T* find the one registered for T.
         */
        template <typename Pointee, typename T> void withPointer(const std::string& key, T value) {
            const Extractor extractor = Logger::findExtractor(std::type_index(typeid(Pointee*)));

            if (nullptr == extractor) {
                // Nothing knows this type: the address is all we can say
                if (nullptr == value)
                    fields.push_back(LogField{key, LogValue(std::string("(null)")), false});
                else
                    fields.push_back(LogField{
                        key,
                        LogValue(std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(value))),
                        false});

                return;
            }

            if (nullptr == value) {
                // "(null)" in the sentence, and "key":null in the JSON line
                fields.push_back(LogField{key, LogValue(std::string("(null)")), true});
                fields.push_back(LogField{key, LogValue(), false});

                return;
            }

            const LogObject object = extractor(static_cast<const void*>(value));

            fields.push_back(LogField{key, LogValue(object.display), true});

            for (const std::pair<std::string, LogValue>& sub : object.fields)
                fields.push_back(LogField{key + '_' + sub.first, sub.second, false});
        }

        std::string templateStr;        // Original template string
        std::vector<LogArg> positional; // Positional format arguments
        std::vector<LogField> fields;   // Typed fields, in insertion order
        Verbosity level;                // Log level
        const char* func;               // __PRETTY_FUNCTION__ of the call site
        Logger* loggerInstance;         // The logger the record goes to
    };

    /**
     * Factory method to create a MessageTemplate with no format arguments.
     * Used by the LOG_MSG macro for template-based structured logging.
     */
    MessageTemplate createMessage(Verbosity level, const char* func,
                                  const std::string& templateStr) {
        return MessageTemplate(this, level, func, templateStr);
    }

    /**
     * Template factory method to create a MessageTemplate with format arguments.
     * Supports variadic templates for flexible format argument handling.
     */
    template <typename... FormatArgs>
    MessageTemplate createMessage(Verbosity level, const char* func, const std::string& templateStr,
                                  FormatArgs&&... args) {
        return MessageTemplate(this, level, func, templateStr, std::forward<FormatArgs>(args)...);
    }

  private:
    /**
     * Stream-based logging interface for << operator usage.
     * Accumulates messages in a buffer and flushes on std::endl.
     * Provides a familiar iostream-style interface for logging.
     */
    class LoggerStream {
      public:
        /**
         * Constructor for LoggerStream.
         * Associates the stream with a logger instance and verbosity level.
         */
        LoggerStream(Logger& logger, Verbosity v) : logger(logger), verbosity(v) {}

        /**
         * Template operator<< for accumulating log message content.
         * Stores all streamed values in an internal buffer.
         */
        template <typename T> LoggerStream& operator<<(const T& value) {
            messageBuffer << value;
            return *this;
        }

        /**
         * Special operator<< for stream manipulators like std::endl.
         * Flushes the accumulated message when std::endl is encountered.
         */
        LoggerStream& operator<<(std::ostream& (*fp)(std::ostream&)) {
            if (fp == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
                flush();
            }
            return *this;
        }

      private:
        Logger& logger;
        Verbosity verbosity;
        std::ostringstream messageBuffer;

        /**
         * Flushes the accumulated message buffer to the logger.
         * Clears the buffer after sending the message.
         */
        void flush() {
            std::string message = messageBuffer.str();
            logger.write(verbosity, message);
            messageBuffer.str("");
            messageBuffer.clear();
        }
    };

  public:
    /**
     * Creates a LoggerStream for iostream-style logging.
     * Allows usage like: logger->write(INFO) << "Message" << std::endl;
     */
    LoggerStream write(Verbosity v) { return LoggerStream(*this, v); }

    /**
     * Logs one message that is already a sentence, with no function and no
     * fields of its own.
     */
    void write(Verbosity, const std::string& theMessage);

    /**
     * Main logging function with caller function information.
     * Called by the LOG macro: the message template is rendered with the
     * positional arguments, which gives the sentence and the spans of it that
     * came from an argument.  Nothing is formatted when the level rules the
     * record out.
     */
    template <typename Format, typename... Args>
    void writeFunc(Verbosity v, const char* func, const Format& format, Args&&... args) {
        if (!shouldLog(v))
            return;

        std::vector<LogArg> positional;
        if constexpr (sizeof...(args) > 0) {
            positional.reserve(sizeof...(args));
            (positional.push_back(makeLogArg(args)), ...);
        }

        const RenderResult rendered =
            renderTemplate(std::string_view(format), positional, std::vector<LogField>());

        LogRecord record;
        record.level = v;
        record.function = parseFunction(nullptr == func ? std::string() : std::string(func));
        record.message = rendered.text;
        record.spans = rendered.spans;

        log(std::move(record));
    }

    /**
     * Teaches the logging system how to show objects of type T, for every
     * logger of the process.  The owner token is what removeExtractors()
     * takes back, so that a module's extractors go when the module does.
     * Registering T again replaces what was there.
     */
    template <typename T>
    static void registerExtractor(const void* owner, std::function<LogObject(const T*)> handler) {
        setExtractor(std::type_index(typeid(std::remove_const_t<T>*)), owner,
                     [handler = std::move(handler)](const void* object) {
                         return handler(static_cast<const T*>(object));
                     });
    }

    /**
     * Forgets every extractor registered under this owner token.  Called by
     * whatever registered them, before it goes away.
     */
    static void removeExtractors(const void* owner);

  private:
    /**
     * A logger writing under this name, below this parent.  It starts with no
     * sinks at all and no level of its own; the registry creates it, whoever
     * asked for it attaches what it writes to.
     */
    Logger(std::string name, Logger* parent);

    /**
     * Destructor - private, because a logger lives for as long as the process
     * does: code anywhere holds its address, and a module that is loaded again
     * finds the logger it had before.  The sinks are shared and outlive the
     * logger if anything else still holds them.
     */
    ~Logger();

    /// One destination of a record: where it goes and how much of it goes there
    using SinkEntry = std::pair<std::shared_ptr<LogSink>, Verbosity>;

    /// The extractor of this type, or an empty one when there is none
    static Extractor findExtractor(std::type_index);

    /**
     * Puts a configuration on this logger: its level, its additivity and its
     * sinks, or nothing at all for a logger the configuration does not mention.
     *
     * All three are replaced under one acquisition of this logger's mutex, which
     * is what makes a reload atomic per logger: a record logged meanwhile finds
     * the configuration that was or the configuration that is, never a logger
     * with its level and its sinks taken away and not yet given back.
     *
     * The sinks that were here are moved into released rather than let go of:
     * the caller is the registry, which holds its own lock, and a sink that is
     * destroyed closes a file.  The registry lets go of them once it has let go
     * of that lock.
     */
    void applyConfig(std::optional<Verbosity> level, std::optional<bool> newAdditive,
                     std::vector<SinkEntry> sinks, std::vector<std::shared_ptr<LogSink>>& released);

    /// Whether records walk up to the parent, with this logger's mutex held
    bool isAdditiveLocked() const { return configAdditive.value_or(additive); }

    /// Registers or replaces the extractor of one type
    static void setExtractor(std::type_index, const void* owner, Extractor);

    /**
     * What one record of the walk up the hierarchy is delivered to: the sink and
     * the threshold of the attachment that allows the most.
     */
    struct Target {
        std::shared_ptr<LogSink> sink;
        Verbosity threshold;
    };

    /**
     * Adds this logger's sinks to the list of a dispatch that is on its way up.
     * A sink already in the list stays where it is and keeps the most permissive
     * of its thresholds: it hears the record once.  The logger's mutex is held
     * by the caller.
     */
    void appendTargetsLocked(std::vector<Target>& targets) const;

    /// Adds every sink of this logger to the list, for LogManager::reopenAll()
    void appendSinks(std::vector<std::shared_ptr<LogSink>>&) const;

    std::string name;

    /// Immutable, so that walking up the hierarchy needs no lock of its own
    Logger* const parent;

    /**
     * The three levels this logger may have, highest precedence first.
     */
    std::optional<Verbosity> configLevel;
    std::optional<Verbosity> legacyLevel;
    std::optional<Verbosity> codeDefault;

    /**
     * Whether the records of this logger walk up to its ancestors: what the code
     * asked for, and over it what the configuration file did, so that reading
     * the file again gives the code's answer back.
     */
    bool additive = true;
    std::optional<bool> configAdditive;

    /**
     * The destinations: what a configuration file asked for, which is replaced
     * when the file is read anew, and what the code attached, which is not.
     */
    std::vector<SinkEntry> configSinks;
    std::vector<SinkEntry> codeSinks;
    std::vector<LogField> context;

    mutable std::mutex logMutex;
}; // class Logger

} // namespace gnuworld
