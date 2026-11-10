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

#ifndef AGENTIC_AGENT_VIEWER_MODE_H
#define AGENTIC_AGENT_VIEWER_MODE_H

#include <wx/string.h>

class KIWAY;
class wxFrame;

namespace AGENTIC
{

struct AGENT_LAUNCH_OPTIONS
{
    int      port = 0; ///< MCP HTTP port; 0 picks a free one.
    wxString project;  ///< Project folder to open at start, or empty.
};

/**
 * Start viewer mode: the hub window, the agent host and the HTTP server.  Called by
 * PGM_KICAD::OnPgmInit instead of creating the project manager frame.
 *
 * @return the hub frame, or nullptr when the server could not start (the reason is printed
 *         on stderr).
 */
wxFrame* StartAgentViewerMode( KIWAY* aKiway, const AGENT_LAUNCH_OPTIONS& aOptions );

} // namespace AGENTIC

#endif // AGENTIC_AGENT_VIEWER_MODE_H
