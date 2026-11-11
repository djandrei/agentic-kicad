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


#include <agentic/core/agent_project.h>
#include <agentic/sch/sym_lib_session.h>
#include <agentic/sch/sym_viewer.h>

#include <lib_symbol.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <sch_pin.h>
#include <widgets/symbol_preview_widget.h>

#include <wx/choice.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>


namespace AGENTIC
{

namespace
{

    class SYM_VIEWER : public AGENT_VIEWER
    {
    public:
        SYM_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "sym", _( "Symbol Editor" ) )
        {
            wxPanel*    center = new wxPanel( this );
            wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
            wxBoxSizer* bar = new wxBoxSizer( wxHORIZONTAL );

            m_unit = new wxChoice( center, wxID_ANY );
            m_bodyStyle = new wxChoice( center, wxID_ANY );
            bar->Add( m_unit, 0, wxALL, 2 );
            bar->Add( m_bodyStyle, 0, wxALL, 2 );
            sizer->Add( bar, 0, wxEXPAND );

            m_preview = new SYMBOL_PREVIEW_WIDGET( center, nullptr, false, EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL );
            sizer->Add( m_preview, 1, wxEXPAND );
            center->SetSizer( sizer );
            SetCentralView( center );

            m_pins = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 320, 200 ), wxLC_REPORT );
            m_pins->AppendColumn( _( "Number" ), wxLIST_FORMAT_LEFT, 60 );
            m_pins->AppendColumn( _( "Name" ), wxLIST_FORMAT_LEFT, 100 );
            m_pins->AppendColumn( _( "Type" ), wxLIST_FORMAT_LEFT, 100 );
            m_pins->AppendColumn( _( "Unit" ), wxLIST_FORMAT_LEFT, 40 );
            AddDock( m_pins, wxS( "pins" ), _( "Pins" ), wxRIGHT );
            FinishLayout();

            m_unit->Bind( wxEVT_CHOICE,
                          [this]( wxCommandEvent& )
                          {
                              redraw( false );
                          } );
            m_bodyStyle->Bind( wxEVT_CHOICE,
                               [this]( wxCommandEvent& )
                               {
                                   redraw( false );
                               } );

            for( DOC_SESSION* session : Project().Sessions() )
            {
                if( shows( *session ) )
                    m_session = static_cast<SYMBOL_LIB_SESSION*>( session );
            }

            redraw( true );
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["symbol"] = m_shownName;
            state["units"] = m_unit->GetCount();
            state["pinRows"] = m_pins->GetItemCount();
            return state;
        }

        void OnDocumentOpened( DOC_SESSION& aSession ) override
        {
            if( shows( aSession ) )
                m_session = static_cast<SYMBOL_LIB_SESSION*>( &aSession );
        }

        void OnDocumentClosing( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
            {
                m_preview->DisplayPart( nullptr, 0 );
                m_session = nullptr;
                m_shownName.clear();
            }
        }

        void OnDocumentReloading( DOC_SESSION& aSession ) override
        {
            // The library's symbols are replaced; the preview holds a copy, but drop it anyway.
            if( &aSession == m_session )
                m_preview->DisplayPart( nullptr, 0 );
        }

        void OnDocumentReloaded( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
                redraw( false );
        }

        void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) override
        {
            AGENT_VIEWER::OnCommitApplied( aSession, aChange );

            if( aSession.Program() != "sym" || &aSession.Project() != &Project() )
                return;

            m_session = static_cast<SYMBOL_LIB_SESSION*>( &aSession );
            redraw( true );
        }

        // A capture or a read makes a symbol current without a commit: show it after the call.
        void OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                        const std::string& aSummary ) override
        {
            AGENT_VIEWER::OnToolEnd( aTool, aArgs, aOk, aMillis, aSummary );

            if( !m_session )
            {
                for( DOC_SESSION* session : Project().Sessions() )
                {
                    if( shows( *session ) )
                        m_session = static_cast<SYMBOL_LIB_SESSION*>( session );
                }
            }

            if( m_session && m_session->CurrentSymbol() != m_shownName )
                redraw( true );
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "sym";
        }

    private:
        void redraw( bool aResetChoices )
        {
            m_pins->DeleteAllItems();

            LIB_SYMBOL* symbol = nullptr;

            if( m_session && !m_session->CurrentSymbol().empty() )
                symbol = m_session->Library().GetSymbol( wxString::FromUTF8( m_session->CurrentSymbol() ) );

            if( !symbol )
            {
                m_preview->DisplayPart( nullptr, 0 );
                m_shownName.clear();
                return;
            }

            if( aResetChoices || m_shownName != m_session->CurrentSymbol() )
            {
                m_unit->Clear();

                for( int unit = 1; unit <= symbol->GetUnitCount(); ++unit )
                    m_unit->Append( wxString::Format( _( "Unit %s" ), symbol->GetUnitDisplayName( unit, true ) ) );

                m_unit->SetSelection( 0 );
                m_bodyStyle->Clear();

                for( int style = 1; style <= symbol->GetBodyStyleCount(); ++style )
                    m_bodyStyle->Append( wxString::Format( _( "Body style %d" ), style ) );

                m_bodyStyle->SetSelection( 0 );
            }

            m_shownName = m_session->CurrentSymbol();
            const int unit = m_unit->GetSelection() + 1;
            const int bodyStyle = m_bodyStyle->GetSelection() + 1;
            m_preview->DisplayPart( symbol, unit, bodyStyle );

            for( const SCH_PIN* pin : symbol->GetGraphicalPins( 0, 0 ) )
            {
                long row = m_pins->InsertItem( m_pins->GetItemCount(), pin->GetNumber() );
                m_pins->SetItem( row, 1, pin->GetName() );
                m_pins->SetItem( row, 2, pin->GetElectricalTypeName() );
                m_pins->SetItem( row, 3, wxString::Format( wxS( "%d" ), pin->GetUnit() ) );
            }
        }

        SYMBOL_PREVIEW_WIDGET* m_preview = nullptr;
        wxChoice*              m_unit = nullptr;
        wxChoice*              m_bodyStyle = nullptr;
        wxListCtrl*            m_pins = nullptr;
        SYMBOL_LIB_SESSION*    m_session = nullptr;
        std::string            m_shownName;
    };

} // namespace


AGENT_VIEWER* CreateSymViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new SYM_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
