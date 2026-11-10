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


#ifndef AGENTIC_RC_CODES_H
#define AGENTIC_RC_CODES_H

#include <functional>
#include <vector>

#include <nlohmann/json.hpp>

#include <rc_item.h>

namespace AGENTIC
{

/**
 * ERC or DRC codes as JSON, straight from a checker's item registry, so the list never
 * drifts from KiCad: [{code, key, title, group}].  Group headings (code 0) name the group
 * of the items after them.
 */
inline nlohmann::json RcItemsJson( const std::vector<std::reference_wrapper<RC_ITEM>>& aItems )
{
    nlohmann::json out = nlohmann::json::array();
    std::string    group;

    for( const RC_ITEM& item : aItems )
    {
        const std::string title = item.GetErrorText( false ).ToStdString( wxConvUTF8 );

        if( item.GetErrorCode() == 0 )
        {
            group = title;
            continue;
        }

        nlohmann::json entry{ { "code", item.GetErrorCode() },
                              { "key", item.GetSettingsKey().ToStdString( wxConvUTF8 ) },
                              { "title", title } };

        if( !group.empty() )
            entry["group"] = group;

        out.push_back( entry );
    }

    return out;
}

} // namespace AGENTIC

#endif // AGENTIC_RC_CODES_H
