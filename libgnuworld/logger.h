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
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "LogFormat.h"
#include "LogRecord.h"
#include "LogRender.h"
#include "LogSink.h"

/**
 * Main logging macro for simple formatted messages.
 * Automatically passes the current function name and supports format strings.
 * Usage: LOG(INFO, "User {} connected", username);
 */
#define LOG(x, ...) logger->writeFunc(x, __PRETTY_FUNCTION__, __VA_ARGS__)

/**
 * SQL error logging macro for database-related errors.
 * Automatically formats SQL error messages from database objects.
 */
#define LOGSQL_ERROR(x)                                                                            \
    logger->writeFunc(ERROR, __PRETTY_FUNCTION__, "SQL Error: {}", x->ErrorMessage())

/**
 * Structured logging macro with template support and field extraction.
 * Allows mixing format arguments with named placeholders and structured fields.
 * Usage: LOG_MSG(INFO, "User {} joined {channel}", username).with("channel", chanPtr).log();
 */
#define LOG_MSG(level, template_msg, ...)                                                          \
    logger->createMessage(level, __PRETTY_FUNCTION__, template_msg, ##__VA_ARGS__)

namespace gnuworld {

/**
 * Main logging system for GNUWorld services.
 * A logger has a name, a level and a list of sinks with a threshold each; a log
 * statement becomes one LogRecord, which the logger hands to every sink that
 * wants it.  What a sink then does with the record - a JSON line in a file, a
 * column on the console, a notice in a channel, a push notification - is the
 * sink's business and none of the logger's, so nothing here knows anything
 * about IRC.
 *
 * Objects of other layers reach a log message through an extractor, registered
 * process-wide for their type: it says how the object shows in a sentence and
 * what fields it contributes to a JSON record.
 */
class Logger {
  public:
    /**
     * A logger writing under this name.  It starts with no sinks at all and at
     * the most verbose level; whoever creates it attaches what it writes to.
     */
    explicit Logger(std::string name);

    /**
     * Destructor - the sinks are shared and outlive the logger if anything
     * else still holds them.
     */
    ~Logger();

    /**
     * The name this logger writes under, which every record carries.
     */
    const std::string& getName() const { return name; }

    /**
     * Whether a record of this level is worth building at all.  A call site
     * asks before it formats anything, so that a message nobody will read
     * costs no more than the comparison.
     */
    bool shouldLog(Verbosity v) const;

    /**
     * Sets the most verbose level this logger accepts.
     */
    void setLevel(Verbosity);

    /**
     * Adds a destination, which receives every record of this logger at or
     * below the threshold.  A sink is always held by shared_ptr: the logger
     * keeps it alive for as long as one of its records is on its way there.
     */
    void addSink(std::shared_ptr<LogSink>, Verbosity threshold = TRACE);

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

    /**
     * Sets the IRC channel name for debug output.
     * Messages will be sent to this channel based on chanVerbosity setting.
     */
    void setChannel(const std::string& channelName);

    /**
     * Returns the currently configured debug channel name.
     */
    std::string getChannel() const;

    /**
     * Sets the verbosity level for IRC channel output.
     * Only messages at or below this level will be sent to the debug channel.
     */
    void setChanVerbosity(unsigned short level);

    /**
     * Sets the verbosity level for log file output.
     * Only messages at or below this level will be written to the log file.
     */
    void setLogVerbosity(unsigned short level);

    /**
     * Sets the verbosity level for console output.
     * Only log messages at or below this level will be displayed on the console.
     */
    void setConsoleVerbosity(unsigned short level);

    /**
     * Enables or disables SQL query logging.
     * When enabled, SQL queries will be logged to the file.
     */
    void setLogSQL(bool enable);

    /**
     * Enables or disables SQL query logging to the console.
     * When enabled, SQL-related log messages will be displayed on the console.
     */
    void setConsoleSQL(bool enable);

    /**
     * Names the sink the legacy log-file settings act on, and gives it the
     * threshold those settings have asked for so far.
     */
    void setLegacyFileSink(std::shared_ptr<LogSink>);

    /**
     * Names the sink the legacy console settings act on, the same way.
     */
    void setLegacyConsoleSink(std::shared_ptr<LogSink>);

    /**
     * Names the sink the legacy channel settings act on, the same way.
     */
    void setLegacyIrcSink(std::shared_ptr<LogSink>);

    /**
     * How the compatibility setter above reaches the sink that mirrors to a
     * chat room, without this file having to know what such a sink is.
     */
    void setLegacyChanSetter(std::function<void(const std::string&)>);

    /**
     * Closes and reopens every destination that has anything to reopen, for
     * external log rotation support.
     * Called from xServer::rotateLogs() when a SIGHUP is received.
     */
    void rotateLogs();

  private:
    /// The extractor of this type, or an empty one when there is none
    static Extractor findExtractor(std::type_index);

    /// Registers or replaces the extractor of one type
    static void setExtractor(std::type_index, const void* owner, Extractor);

    std::string name;
    Verbosity level = TRACE;

    std::vector<std::pair<std::shared_ptr<LogSink>, Verbosity>> sinks;
    std::vector<LogField> context;

    /**
     * The three sinks the legacy per-module logging keys configure, and the
     * settings they carry.  The thresholds are kept here as well as on the
     * sinks, because a module reads its configuration before it has an uplink
     * and so before the channel sink exists: installing a slot applies
     * whatever was asked for in the meantime.
     */
    std::shared_ptr<LogSink> legacyFileSink;
    std::shared_ptr<LogSink> legacyConsoleSink;
    std::shared_ptr<LogSink> legacyIrcSink;
    std::function<void(const std::string&)> legacyChanSetter;
    std::string legacyChanName;
    Verbosity legacyLogVerbosity = TRACE;
    Verbosity legacyConsoleVerbosity = TRACE;
    Verbosity legacyChanVerbosity = INFO;
    bool logSQL = false;
    bool consoleSQL = false;

    mutable std::mutex logMutex;
}; // class Logger

} // namespace gnuworld
