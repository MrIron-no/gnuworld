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

#include <memory>
#include <string>

#include "LogRecord.h"

namespace gnuworld {

class Logger;
class LogSink;

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
}; // class LogManager

} // namespace gnuworld

#endif // __LOGMANAGER_H
