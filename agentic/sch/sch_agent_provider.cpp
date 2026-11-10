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


#include <agentic/core/core_tools.h>
#include <agentic/core/library_tools.h>
#include <agentic/core/provider.h>
#include <agentic/core/rc_codes.h>
#include <agentic/core/services.h>
#include <agentic/sch/sch_agent_provider.h>
#include <agentic/sch/sch_library_tools.h>
#include <agentic/sch/sch_tools.h>
#include <agentic/sch/sch_viewer.h>

#include <erc/erc_item.h>
#include <libraries/symbol_library_adapter.h>


namespace
{

class SCH_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "sch" ); }
    wxString Title() const override { return wxS( "Schematic" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override
    {
        aServices.Sessions().AddLibraryAdapter( LIBRARY_TABLE_TYPE::SYMBOL,
                                                []( LIBRARY_MANAGER& aManager )
                                                {
                                                    return std::make_unique<SYMBOL_LIBRARY_ADAPTER>( aManager );
                                                } );

        aServices.AddErrorCodes( "erc",
                                 []()
                                 {
                                     return AGENTIC::RcItemsJson( ERC_ITEM::GetItemsWithSeverities() );
                                 } );

        AGENTIC::RegisterSymbolLibraryTools( aServices );
        AGENTIC::RegisterSchEditTools( aServices );
        AGENTIC::RegisterSchCheckTools( aServices );
    }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreateSchViewer( aServices, aProject );
    }
};

} // namespace


AGENT_PROVIDER* SchAgentProvider()
{
    static SCH_AGENT_PROVIDER provider;
    return &provider;
}
