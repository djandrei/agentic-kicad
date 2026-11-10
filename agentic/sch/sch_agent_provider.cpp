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
#include <agentic/sch/sim_tools.h>
#include <agentic/sch/sim_viewer.h>
#include <agentic/sch/sym_tools.h>
#include <agentic/sch/sym_viewer.h>

#include <erc/erc_item.h>
#include <libraries/symbol_library_adapter.h>


namespace
{

/// The symbol editor: compiled into eeschema, its own tool group and viewer.
class SYM_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "sym" ); }
    wxString Title() const override { return wxS( "Symbol Editor" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override { AGENTIC::RegisterSymbolEditorTools( aServices ); }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreateSymViewer( aServices, aProject );
    }
};


/// The simulator: ngspice on the project's schematic, its own tool group and viewer.
class SIM_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "sim" ); }
    wxString Title() const override { return wxS( "Simulator" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override { AGENTIC::RegisterSimulatorTools( aServices ); }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreateSimViewer( aServices, aProject );
    }
};


class SCH_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "sch" ); }
    wxString Title() const override { return wxS( "Schematic" ); }

    std::vector<AGENT_PROVIDER*> Companions() override { return { &m_symbolEditor, &m_simulator }; }

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

        aServices.SetNetlistSource(
                []( AGENTIC::AGENT_PROJECT& aProject )
                {
                    return AGENTIC::SchNetlist( aProject );
                } );

        AGENTIC::RegisterSymbolLibraryTools( aServices );
        AGENTIC::RegisterSchEditTools( aServices );
        AGENTIC::RegisterSchCheckTools( aServices );
    }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return AGENTIC::CreateSchViewer( aServices, aProject );
    }

private:
    SYM_AGENT_PROVIDER m_symbolEditor;
    SIM_AGENT_PROVIDER m_simulator;
};

} // namespace


AGENT_PROVIDER* SchAgentProvider()
{
    static SCH_AGENT_PROVIDER provider;
    return &provider;
}
