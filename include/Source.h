/**
 * Source.h
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

#ifndef GNUWORLD_SOURCE_H
#define GNUWORLD_SOURCE_H

#include <cstddef>

namespace gnuworld {

class iClient;
class iServer;

/**
 * Who a change sent to the network comes from.
 *
 * Normally the object the method is called on says it: MyUplink->Op(...) is
 * the gnuworld server opping someone, bot->Op(...) is that xClient doing it.
 * A fake client or a spawned or juped server has no such object: it is only
 * an iClient or an iServer, which cannot do anything by itself.  For those,
 * the xServer methods take a Source as their last argument:
 *
 *   MyUplink->Op(theChan, target);              // the gnuworld server
 *   MyUplink->Op(theChan, target, fakeClient);  // a fake client
 *   MyUplink->Op(theChan, target, fakeServer);  // a server we introduced
 *
 * A client source has to be on the channel, opped, or the call fails: the
 * network would bounce the change.  A server source needs neither.
 */
class Source {
  public:
    /// The gnuworld server itself.
    Source() = default;

    /// A client: a fake one, or an xClient's own iClient.  Implicit on
    /// purpose, so that an iClient* can be passed where a Source is expected.
    Source(const iClient* client) : theClient(client) {}

    /// A server other than ourselves: one we spawned, or a jupe.
    Source(const iServer* server) : theServer(server) {}

    /// A null pointer says nothing about which of the two was meant.
    Source(std::nullptr_t) = delete;

    /// The client, or null if the source is a server.
    const iClient* client() const noexcept { return theClient; }

    /// The server, or null if it is a client or the gnuworld server itself.
    const iServer* server() const noexcept { return theServer; }

    bool isClient() const noexcept { return theClient != nullptr; }

  private:
    const iClient* theClient = nullptr;
    const iServer* theServer = nullptr;
};

} // namespace gnuworld

#endif // GNUWORLD_SOURCE_H
