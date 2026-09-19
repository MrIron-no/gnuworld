/**
 * iServer.cc
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
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
 * $Id: iServer.cc,v 1.11 2006/12/22 06:41:45 kewlio Exp $
 */

#include <string>
#include <ctime>

#include <cstring>
#include <cstdlib>

#include "iServer.h"
#include "Numeric.h"
#include "gnuworld_config.h"
#include "NetworkTarget.h"
#include "logger.h"

GNUWORLD_CORE_LOGGER(State);

namespace gnuworld {

using std::string;

iServer::iServer(const unsigned int& _uplink, const string& _yyxxx, const string& _name,
                 const time_t& _connectTime, const string& _description)
    : NetworkTarget(_yyxxx), uplinkIntYY(_uplink), name(_name), connectTime(_connectTime),
      startTime(_connectTime), description(_description), bursting(false), flags(0), protocol(11),
      lag(0), lastLagTS(0) {}

iServer::~iServer() {}

/**
 * Interpret the protocol token of a SERVER message.
 *
 * @Param[in] token "P10"/"J10"/"P11"/"J11".  A leading 'J' means the
 * server is still bursting.  The digits are the protocol version.
 */
void iServer::setProtocolToken(const string& token) {
    if (token.size() < 2) {
        LOG(WARN, "Malformed protocol token: {}", token);
        return;
    }
    if ('J' == token[0]) {
        setBursting(true);
    }
    char* end = 0;
    unsigned long version = strtoul(token.c_str() + 1, &end, 10);
    if (end == token.c_str() + 1 || 0 == version) {
        LOG(WARN, "Unrecognised protocol version in token: {}", token);
        return;
    }
    protocol = static_cast<unsigned int>(version);
}

/**
 * Interpret a server's flags.
 *
 * @Param[in] newFlags String listing server's P10 flags.
 */
void iServer::setFlags(const string& newFlags) {
    for (string::size_type i = 0; i < newFlags.size(); i++) {
        switch (newFlags[i]) {
        case 'h':
            setHub();
            break;
        case 's':
            setService();
            break;
        case 'z':
            setTLS();
            break;
        case '+':
            break;
        default:
            // Unknown flag
            LOG(WARN, "Unknown server flag: {}, in flags string: {}", newFlags[i], newFlags);
            break;
        }
    }
}

} // namespace gnuworld
