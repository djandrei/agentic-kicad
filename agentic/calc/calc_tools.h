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


#ifndef AGENTIC_CALC_TOOLS_H
#define AGENTIC_CALC_TOOLS_H

#include <deque>

#include <nlohmann/json.hpp>

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;

/// The calculations of a project, newest first (the viewer's table; kept for the process).
std::deque<nlohmann::json>& CalcHistory( AGENT_PROJECT& aProject );

/// Run calculator \a aName on \a aInputs (the calc_run tool without the history).
nlohmann::json RunCalculator( const std::string& aName, const nlohmann::json& aInputs );

/// calc_list, calc_run (stage 13), group "calc"; debug_calc_panel with AGENTIC_TEST_HOOKS.
void RegisterCalculatorTools( AGENT_SERVICES& aServices );

/// The upstream panel's answer for the same inputs (calc_pin.cpp; viewer mode only).
nlohmann::json RunCalculatorPanel( const std::string& aName, const nlohmann::json& aInputs );

} // namespace AGENTIC

#endif // AGENTIC_CALC_TOOLS_H
