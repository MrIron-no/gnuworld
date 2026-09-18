/**
 * LogSink.h
 * The interface every destination of a log record implements: a file, the
 * console, an IRC channel, a notifier.
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

#ifndef __LOGSINK_H
#define __LOGSINK_H

#include "LogRecord.h"

namespace gnuworld {

/**
 * One destination of the logging system.  A logger hands the same record to
 * each of the sinks attached to it and to its ancestors, so a sink formats
 * the record itself and decides what to do with the line.
 *
 * CaptureSink in LogSinks.h is the short example of how to write one.
 */
class LogSink {
  public:
    virtual ~LogSink() = default;

    /**
     * Delivers one record.  Called from any thread, so an implementation
     * takes its own lock; it never throws and never logs anything itself.
     */
    virtual void emit(const LogRecord&) = 0;

    /**
     * Picks up whatever the outside world did to the destination: a file sink
     * closes and reopens its path, so that log rotation works.  Called on
     * SIGHUP, from the main loop.
     */
    virtual void reopen() {}

    /**
     * True when this sink may only be written from the main thread.  The
     * logger then queues records that arrive on another thread and delivers
     * them from the main loop instead, in order.
     */
    virtual bool mainThreadOnly() const { return false; }

    /**
     * True when this sink wants nothing to do with a re-entrant record: one
     * logged while the calling thread is already inside a sink dispatch is
     * not delivered to this sink.  That is how a sink that logs on its own
     * account, such as the IRC one writing to the network, is kept from
     * feeding itself.
     */
    virtual bool suppressOnReentry() const { return false; }
};

} // namespace gnuworld

#endif // __LOGSINK_H
