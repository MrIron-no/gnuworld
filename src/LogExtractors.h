/**
 * LogExtractors.h
 * What the logging system makes of the objects of the core: the form an
 * iClient, an iServer, a Channel or a ChannelUser takes in a message, and the
 * fields it contributes to a JSON record.
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

#ifndef __LOGEXTRACTORS_H
#define __LOGEXTRACTORS_H

#include "LogRecord.h"

namespace gnuworld {

class Channel;
class ChannelUser;
class iClient;
class iServer;

/**
 * Registers the extractors of the core objects with the logging system, under
 * the owner token nullptr: they belong to the process, not to a module, and
 * are never unregistered.  Called once, from the main thread, at start-up.
 */
void registerCoreLogExtractors();

/**
 * The display form is the nick; the fields are nick, userhost, ip and numeric,
 * then account and account_id for a client that is logged in, then is_oper.
 */
LogObject logObjectFor(const iClient*);

/**
 * The display form is the server name; the fields are name, numeric, uplink
 * and is_bursting.
 */
LogObject logObjectFor(const iServer*);

/**
 * The display form is the channel name; the fields are name and modes.
 */
LogObject logObjectFor(const Channel*);

/**
 * The display form is the nick of the client this membership belongs to; the
 * fields are is_op and is_voice.
 */
LogObject logObjectFor(const ChannelUser*);

} // namespace gnuworld

#endif // __LOGEXTRACTORS_H
