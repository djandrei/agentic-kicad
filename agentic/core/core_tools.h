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


#ifndef AGENTIC_CORE_TOOLS_H
#define AGENTIC_CORE_TOOLS_H

#include <nlohmann/json.hpp>

namespace AGENTIC
{

class AGENT_SERVICES;

/// Project, file, history, log and project-settings tools (stage 2), group "project".
void RegisterProjectTools( AGENT_SERVICES& aServices );

/// Schema helper: an object schema with \a aProperties, \a aRequired and the optional
/// "project" argument every project tool takes.
nlohmann::json ProjectToolSchema( nlohmann::json aProperties, nlohmann::json aRequired = nlohmann::json::array() );

} // namespace AGENTIC

#endif // AGENTIC_CORE_TOOLS_H
