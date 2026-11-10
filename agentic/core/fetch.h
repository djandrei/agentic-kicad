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


#ifndef AGENTIC_FETCH_H
#define AGENTIC_FETCH_H

#include <functional>
#include <string>

namespace AGENTIC
{

/**
 * Download \a aUrl into a string.  http(s) goes through KiCad's curl wrapper; file:// reads a
 * local file (fixture repositories and manuals in tests).  \a aProgress receives the
 * fraction done and returns false to abort.  At most \a aMaxBytes are accepted.
 *
 * @throw TOOL_ERROR on failure, with the reason.
 */
std::string Fetch( const std::string& aUrl, size_t aMaxBytes,
                   const std::function<bool( double aFraction )>& aProgress = nullptr );

/// The local path of a file:// URL, or empty for other schemes.
std::string FileUrlPath( const std::string& aUrl );

} // namespace AGENTIC

#endif // AGENTIC_FETCH_H
