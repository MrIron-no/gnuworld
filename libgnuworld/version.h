/**
 * version.h
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
 */

#ifndef __VERSION_H
#define __VERSION_H

#include <string>

namespace gnuworld {

/**
 * What this build was made from: what "git describe --always --dirty" said of
 * the source when it was compiled, such as "a1b2c3d4e5" or "a1b2c3d4e5-dirty"
 * for a checkout with local changes, and "unknown" outside a checkout.
 */
const char* buildRevision();

/// "gnuworld 4.0 (a1b2c3d4e5, 2026-09-19)": the series, the revision, its date
std::string versionString();

} // namespace gnuworld

#endif // __VERSION_H
