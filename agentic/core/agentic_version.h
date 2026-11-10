/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef AGENTIC_VERSION_H
#define AGENTIC_VERSION_H

#include <string>

namespace AGENTIC
{

/// Version of the Agentic-KiCad server, reported by the `version` tool and in `initialize`.
std::string ServerVersion();

/// Name the server reports in `initialize` (serverInfo.name).
std::string ServerName();

} // namespace AGENTIC

#endif // AGENTIC_VERSION_H
