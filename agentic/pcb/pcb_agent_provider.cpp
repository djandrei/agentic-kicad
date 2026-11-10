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
#include <agentic/core/rc_codes.h>
#include <agentic/core/services.h>
#include <agentic/pcb/pcb_agent_provider.h>
#include <agentic/pcb/pcb_library_tools.h>
#include <agentic/pcb/pcb_route_tools.h>
#include <agentic/pcb/pcb_tools.h>
#include <agentic/pcb/pcb_viewer.h>

#include <drc/drc_item.h>
#include <footprint_library_adapter.h>


namespace
{

class PCB_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "pcb" ); }
    wxString Title() const override { return wxS( "PCB" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override
    {
        aServices.Sessions().AddLibraryAdapter( LIBRARY_TABLE_TYPE::FOOTPRINT,
                                                []( LIBRARY_MANAGER& aManager )
                                                {
                                                    return std::make_unique<FOOTPRINT_LIBRARY_ADAPTER>( aManager );
                                                } );

        aServices.AddErrorCodes( "drc",
                                 []()
                                 {
                                     return AGENTIC::RcItemsJson( DRC_ITEM::GetItemsWithSeverities() );
                                 } );

        AGENTIC::RegisterFootprintLibraryTools( aServices );
        AGENTIC::RegisterPcbEditTools( aServices );
        AGENTIC::RegisterPcbCheckTools( aServices );
        AGENTIC::RegisterPcbRouteTools( aServices );
    }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreatePcbViewer( aServices, aProject );
    }
};

} // namespace


AGENT_PROVIDER* PcbAgentProvider()
{
    static PCB_AGENT_PROVIDER provider;
    return &provider;
}
