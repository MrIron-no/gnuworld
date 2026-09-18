/**
 * notifier.h
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

#include <string>

#include "LogRecord.h"
#include "LogSink.h"

namespace gnuworld {

/**
 * Base class for all notification systems
 * Provides a common interface for sending messages
 *
 * A notifier is a sink of the logging system: attach one to a logger with
 * addSink() and every record the threshold lets through arrives at
 * sendMessage().
 */
class notifier : public LogSink {
  public:
    notifier() = default;
    ~notifier() override = default;

    /**
     * Send a notification message with verbosity level
     * @param level The verbosity/severity level
     * @param message The message body
     * @return true if message was sent successfully, false otherwise
     */
    virtual bool sendMessage(int level, const std::string message) = 0;

    /**
     * Delivers one log record as a notification: the sentence, behind the
     * function it was logged from at every level but INFO.
     */
    void emit(const LogRecord& r) override {
        sendMessage(static_cast<int>(r.level),
                    (r.level == INFO ? std::string() : r.function + "> ") + r.message);
    }

    /**
     * Sending a notification logs on its own account, so a notifier takes no
     * record that was logged from inside a sink dispatch.
     */
    bool suppressOnReentry() const override { return true; }

    /**
     * Get the number of successful notifications sent
     * @return Number of successful notifications
     */
    virtual size_t getSuccessful() const = 0;

    /**
     * Get the number of failed notifications
     * @return Number of failed notifications
     */
    virtual size_t getErrors() const = 0;
};

} // namespace gnuworld