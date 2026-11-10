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

#include <agentic/hub/agent_viewer_mode.h>
#include <agentic/hub/hub_frame.h>

#include <wx/crt.h>


namespace AGENTIC
{

wxFrame* StartAgentViewerMode( KIWAY* aKiway, const AGENT_LAUNCH_OPTIONS& aOptions )
{
    HUB_FRAME* hub = new HUB_FRAME( aKiway );

    if( !hub->StartServer( aOptions.port ) )
    {
        hub->Destroy();
        return nullptr;
    }

    hub->Show();
    return hub;
}

} // namespace AGENTIC
