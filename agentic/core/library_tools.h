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


#ifndef AGENTIC_LIBRARY_TOOLS_H
#define AGENTIC_LIBRARY_TOOLS_H

#include <string>

#include <nlohmann/json.hpp>

#include <libraries/library_table.h>

class LIBRARY_MANAGER_ADAPTER;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;
class TOOL_CONTEXT;

/// lib_tables, lib_add and lib_create (stage 3), group "project".
void RegisterLibraryTools( AGENT_SERVICES& aServices );

/// "symbol", "footprint" or "design_block" to the table type.  @throw TOOL_ERROR otherwise.
LIBRARY_TABLE_TYPE ParseLibraryType( const std::string& aType );

/**
 * Load every enabled library of \a aType for the project, reporting progress (D10) and
 * honouring a cancel.  Returns the adapter; libraries that failed to load are listed in
 * \a aErrors as {library, error}.
 *
 * @throw TOOL_ERROR when no program provides that library type, or when cancelled.
 */
LIBRARY_MANAGER_ADAPTER& LoadLibraries( AGENT_PROJECT& aProject, LIBRARY_TABLE_TYPE aType, TOOL_CONTEXT& aContext,
                                        nlohmann::json& aErrors );

/**
 * Score of a candidate for a free-text query: each query word found in the name, keywords or
 * description adds to it, a whole-word match more than a prefix match, the name most.
 * Numbers in the query ("8 pin") also match \a aPinCount.  0 means no word matched.
 */
double SearchScore( const std::string& aQuery, const std::string& aName, const std::string& aKeywords,
                    const std::string& aDescription, int aPinCount );

} // namespace AGENTIC

#endif // AGENTIC_LIBRARY_TOOLS_H
