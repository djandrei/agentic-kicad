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


#include <agentic/core/provider.h>
#include <agentic/ds/ds_agent_provider.h>
#include <agentic/ds/ds_tools.h>
#include <agentic/ds/ds_viewer.h>


namespace
{

class DS_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "ds" ); }
    wxString Title() const override { return wxS( "Drawing Sheet Editor" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override { AGENTIC::RegisterDrawingSheetTools( aServices ); }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreateDrawingSheetViewer( aServices, aProject );
    }
};

} // namespace


AGENT_PROVIDER* PlEditorAgentProvider()
{
    static DS_AGENT_PROVIDER provider;
    return &provider;
}
