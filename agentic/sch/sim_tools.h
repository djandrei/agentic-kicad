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


#ifndef AGENTIC_SIM_TOOLS_H
#define AGENTIC_SIM_TOOLS_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;

/// A signal the agent asked for, as the viewer plots it.
struct SIM_SIGNAL
{
    std::string         name;
    std::string         quantity; ///< "value", "gain_db", "phase", "magnitude"
    std::vector<double> x;
    std::vector<double> y;
};


/// The simulator state of a project (kept for the process, not saved).
struct SIM_STATE
{
    std::string             analysis;    ///< set by sim_analysis_set; empty: the schematic's directive
    std::string             lastCommand; ///< the analysis of the last run
    std::string             xAxis;       ///< "time", "frequency", a sweep, or empty (op)
    std::string             netlist;     ///< the netlist of the last run
    std::string             console;     ///< ngspice's output of the last run
    std::vector<SIM_SIGNAL> signals;     ///< what the agent looked at since the last run
    nlohmann::json          measurements = nlohmann::json::array();
    int                     runs = 0;
};


SIM_STATE& SimState( AGENT_PROJECT& aProject );

/// sim_model_get|set, sim_analysis_set, sim_netlist, sim_run, sim_vectors, sim_signal,
/// sim_measure, sim_plot_capture (stage 10), group "sim".
void RegisterSimulatorTools( AGENT_SERVICES& aServices );

} // namespace AGENTIC

#endif // AGENTIC_SIM_TOOLS_H
