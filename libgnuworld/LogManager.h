/**
 * LogManager.h
 * The registry of the loggers of the process: it owns them, it puts each of them
 * in its place in the dotted hierarchy, and it is where anything that wants to
 * log asks for the logger it should write to.
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

#ifndef __LOGMANAGER_H
#define __LOGMANAGER_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LogRecord.h"

namespace gnuworld {

class Logger;
class LogSink;

struct LogConfig;
struct SinkSpec;

/**
 * The loggers of the process, by name, under one unnamed root.
 *
 * A name is a dotted path: "cservice.sql" is a logger below "cservice", which is
 * below the root, and asking for the deepest of them creates whatever of the
 * path is missing.  A logger, once created, lives for as long as the process
 * does and keeps the same address, so that a module which is unloaded and loaded
 * again writes to the logger it wrote to before, with the sinks and the level
 * that were configured for it.
 *
 * Everything here is static, and the registry itself is allocated on first use
 * and never destroyed: a static destructor or an atexit handler may still log
 * while the process is on its way out, and it would find a destroyed registry.
 */
class LogManager {
  public:
    /**
     * The logger of this name, creating it and every ancestor of it that does
     * not exist yet.  The same name always gives the same logger.  Thread-safe.
     *
     * The root answers to "" and to "root", the name it is written under in the
     * configuration file.  Empty segments are not segments: "a..b", ".a.b" and
     * "a.b." all name the logger "a.b".
     */
    static Logger* get(const std::string& name);

    /**
     * The root of the hierarchy, whose name is empty and which every other
     * logger descends from.
     */
    static Logger* root();

    /**
     * Sets the level this logger has unless the configuration says otherwise,
     * creating the logger if it does not exist yet.  The first caller decides;
     * a second one is ignored.
     */
    static void setCodeDefault(const std::string& name, Verbosity);

    /**
     * The logger name a module's library file stands for: the basename without
     * its directory, without a leading "lib" and without anything from its
     * first '.' onwards, so that "libcservice.la" and
     * "/usr/lib/libcservice.so.0.0.0" both give "cservice".
     */
    static std::string moduleNameFromLibrary(const std::string& file);

    /**
     * Names the module that is about to be loaded, so that the xClient the
     * module creates knows which logger is its own.  For the main thread only:
     * this is one name, not one per thread, and module loading happens there.
     */
    static void setLoadingModule(const std::string& name);

    /**
     * The name set by setLoadingModule(), which is forgotten as it is handed
     * over; empty when no module is being loaded.  Main thread only, as above.
     */
    static std::string takeLoadingModule();

    /**
     * Asks every sink of every logger to reopen itself, once each however many
     * loggers it is attached to.  This is what a SIGHUP does, so that the next
     * record goes to the new file rather than to the rotated one.
     */
    static void reopenAll();

    /**
     * The console sink the root is given before anything is configured, so that
     * a message logged during start-up is seen.  Reading the configuration
     * removes it again.
     */
    static std::shared_ptr<LogSink> bootstrapConsoleSink();

    /**
     * How a type name of logging.conf becomes a sink.  The specification holds
     * every setting the file may carry, of which a factory reads the ones its
     * own kind of sink has; a factory that cannot make its sink says why in
     * error and returns nothing, which makes the whole configuration fail.
     */
    using SinkFactory =
        std::function<std::shared_ptr<LogSink>(const SinkSpec&, std::string& error)>;

    /**
     * Teaches the configuration a kind of sink: "file" and "console" are known
     * from the outset, "irc" is registered by core, which is the only layer that
     * knows what a channel is, and a notifier registers its own.  The type name
     * is matched without regard to case; registering one again replaces it.
     */
    static void registerSinkType(const std::string& type, SinkFactory);

    /**
     * Applies a whole configuration, or none of it.
     *
     * Every sink is built first: if any of them cannot be made - an unknown
     * type, a file that will not open - nothing at all changes, the reasons are
     * in errors, and this returns false.  Only once they all exist does the
     * configuration take effect: every logger of the process loses the level,
     * the sinks and the additivity a previous configuration gave it, the specs
     * are applied to the loggers they name, and the root loses the console sink
     * it had while nothing was configured.
     *
     * What the code did is left alone: sinks attached with Logger::addSink, the
     * defaults of child(name, level), the levels of the legacy module keys and
     * an additivity the code asked for all survive.
     */
    static bool configure(const LogConfig&, std::vector<std::string>& errors);

    /**
     * Whether the configuration in force has a "logger.<name>" line of its own
     * for this logger.  A line that only speaks of its additivity does not
     * count, and the root answers to both of its names.
     */
    static bool isConfigured(const std::string& name);

    /**
     * Reads a logging.conf and applies it.  A file that cannot be read or that
     * cannot be applied leaves the configuration in force exactly as it was and
     * is reported, one record per problem, at ERROR on "core.config".
     */
    static bool loadFile(const std::string& fileName);

    /**
     * Adds a human-readable log file to the root, for a process that has no
     * logging.conf to tell it where to write.  This is a sink of the code, so a
     * configuration that is read later does not take it away; asking twice for
     * the same path changes nothing.
     */
    static void bootstrapFile(const std::string& path);

  private:
    /// Everything the registry knows, which is never destroyed
    struct State;

    /// The one registry, created on first use
    static State& state();

    /// Allocates the registry, with the root logger and its console sink
    static State* createState();

    /// The logger of this normalised name, creating it and its ancestors
    static Logger* getLocked(State&, const std::string& name);

    /// Drops the empty segments of a name, and turns "root" into ""
    static std::string normaliseName(const std::string& name);

    /// Tells the text sinks how wide the logger name column has to be
    static void updateNameWidthLocked(State&);

    /// The factory of this type name, or an empty one when none is registered
    static SinkFactory findSinkFactory(const std::string& type);
}; // class LogManager

} // namespace gnuworld

#endif // __LOGMANAGER_H
