/**
 * SendAs.h
 * Copyright (C) 2026 The GNUWorld developers
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
 */

#ifndef GNUWORLD_SENDAS_H
#define GNUWORLD_SENDAS_H

#include <cstdint>

namespace gnuworld {

/**
 * Who a change sent to the network comes from.  Every xClient method that
 * changes something on the network takes one, so that a call site says
 * which it is: Op(chan, user, SendAs::Server), not Op(chan, user, true).
 */
enum class SendAs : std::uint8_t {
    /// The xClient itself.  To change a channel it has to be on the channel
    /// and opped.  If it is not on the channel, it joins, is opped by the
    /// server, makes the change and parts again; if it is on the channel
    /// without ops, the change fails.
    Client,

    /// The gnuworld server.  The network applies such a change without
    /// asking who is opped, and the xClient need not be on the channel.
    Server
};

} // namespace gnuworld

#endif // GNUWORLD_SENDAS_H
