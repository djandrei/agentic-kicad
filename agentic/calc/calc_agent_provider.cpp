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


#include <agentic/calc/calc_agent_provider.h>
#include <agentic/calc/calc_tools.h>
#include <agentic/core/agent_project.h>
#include <agentic/core/provider.h>
#include <agentic/viewer_shell/agent_viewer.h>

#include <wx/listctrl.h>


namespace
{

/// A table of the project's calculations, newest first (D6: opened on the first calculation).
class CALC_VIEWER : public AGENTIC::AGENT_VIEWER
{
public:
    CALC_VIEWER( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) :
            AGENTIC::AGENT_VIEWER( aServices, aProject, "calc", _( "Calculator" ) )
    {
        m_table = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 900, 500 ), wxLC_REPORT );
        m_table->AppendColumn( _( "Calculator" ), wxLIST_FORMAT_LEFT, 160 );
        m_table->AppendColumn( _( "Inputs" ), wxLIST_FORMAT_LEFT, 340 );
        m_table->AppendColumn( _( "Outputs" ), wxLIST_FORMAT_LEFT, 400 );
        SetCentralView( m_table );
        FinishLayout();
        refill();
    }

    nlohmann::json UiState() const override
    {
        nlohmann::json state = AGENTIC::AGENT_VIEWER::UiState();
        state["rows"] = m_table->GetItemCount();
        state["first"] = m_table->GetItemCount() ? m_table->GetItemText( 0 ).ToStdString( wxConvUTF8 ) : std::string();
        return state;
    }

    void OnFilesChanged( AGENTIC::AGENT_PROJECT& aProject ) override
    {
        if( &aProject == &Project() )
            refill();
    }

protected:
    bool shows( const AGENTIC::DOC_SESSION& ) const override { return false; }

private:
    void refill()
    {
        m_table->DeleteAllItems();

        for( const nlohmann::json& entry : AGENTIC::CalcHistory( Project() ) )
        {
            long row = m_table->InsertItem( m_table->GetItemCount(),
                                            wxString::FromUTF8( entry.value( "calculator", "" ) ) );
            m_table->SetItem( row, 1, wxString::FromUTF8( entry["inputs"].dump() ) );
            m_table->SetItem( row, 2, wxString::FromUTF8( entry["outputs"].dump() ) );
        }
    }

    wxListCtrl* m_table = nullptr;
};


class CALC_AGENT_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "calc" ); }
    wxString Title() const override { return wxS( "Calculator" ); }

    void Register( AGENTIC::AGENT_SERVICES& aServices ) override { AGENTIC::RegisterCalculatorTools( aServices ); }

    AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject ) override
    {
        return new CALC_VIEWER( aServices, aProject );
    }
};

} // namespace


AGENT_PROVIDER* CalculatorAgentProvider()
{
    static CALC_AGENT_PROVIDER provider;
    return &provider;
}
