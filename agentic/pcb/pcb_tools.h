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


#ifndef AGENTIC_PCB_TOOLS_H
#define AGENTIC_PCB_TOOLS_H

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

class BOARD_COMMIT;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;
class BOARD_DOC_SESSION;

/// Board setup, queries, placement and copper (stage 6.1, 6.2), group "pcb".
void RegisterPcbEditTools( AGENT_SERVICES& aServices );

/// DRC, unrouted, statistics, captures, exports and the title block (stage 6.3), group "pcb".
void RegisterPcbCheckTools( AGENT_SERVICES& aServices );

/**
 * Run one board change (D4, D5): \a aEdit fills one BOARD_COMMIT, which is pushed; the result
 * gets the ratsnest delta when \a aRatsnest and the saved files; the change is recorded for
 * the viewer, with the KIIDs \a aEdit lists in "items".
 */
nlohmann::json PcbMutate( AGENT_PROJECT& aProject, BOARD_DOC_SESSION& aSession, const std::string& aLabel,
                          const nlohmann::json& aArgs, bool aRatsnest,
                          const std::function<nlohmann::json( BOARD_COMMIT& )>& aEdit );

} // namespace AGENTIC

#endif // AGENTIC_PCB_TOOLS_H
