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
#include <agentic/pcb/footprint_lib_session.h>
#include <agentic/pcb/fp_viewer.h>
#include <agentic/pcb/pcb_items.h>

#include <board.h>
#include <footprint.h>
#include <gal_display_options_common.h>
#include <pad.h>
#include <pcb_draw_panel_gal.h>
#include <view/view.h>

#include <wx/listctrl.h>


namespace AGENTIC
{

namespace
{

    /**
     * Shows a copy of the footprint the agent worked on last, on a board of its own, so the
     * session's footprints are never in a view (a reload replaces them).
     */
    class FP_VIEWER : public AGENT_VIEWER
    {
    public:
        FP_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "fp", _( "Footprint Editor" ) )
        {
            m_canvas = new PCB_DRAW_PANEL_GAL( this, wxID_ANY, wxDefaultPosition, wxSize( 700, 600 ), m_galOptions,
                                               EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL );
            SetCentralView( m_canvas );

            m_pads = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 360, 240 ), wxLC_REPORT );
            m_pads->AppendColumn( _( "Pad" ), wxLIST_FORMAT_LEFT, 50 );
            m_pads->AppendColumn( _( "Type" ), wxLIST_FORMAT_LEFT, 70 );
            m_pads->AppendColumn( _( "X" ), wxLIST_FORMAT_RIGHT, 70 );
            m_pads->AppendColumn( _( "Y" ), wxLIST_FORMAT_RIGHT, 70 );
            m_pads->AppendColumn( _( "Size" ), wxLIST_FORMAT_LEFT, 100 );
            AddDock( m_pads, wxS( "pads" ), _( "Pads" ), wxRIGHT );
            FinishLayout();

            for( DOC_SESSION* session : Project().Sessions() )
            {
                if( shows( *session ) )
                    m_session = static_cast<FOOTPRINT_LIB_SESSION*>( session );
            }

            redraw();
        }

        ~FP_VIEWER() override
        {
            m_canvas->GetView()->Clear();
            m_shown.reset();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["footprint"] = m_shownName;
            state["padRows"] = m_pads->GetItemCount();
            state["padsInView"] = 0;

            if( m_shown && m_shown->GetFirstFootprint() )
            {
                int inView = 0;

                for( PAD* pad : m_shown->GetFirstFootprint()->Pads() )
                    inView += pad->viewPrivData() != nullptr;

                state["padsInView"] = inView;
            }

            return state;
        }

        void OnDocumentOpened( DOC_SESSION& aSession ) override
        {
            if( shows( aSession ) )
                m_session = static_cast<FOOTPRINT_LIB_SESSION*>( &aSession );
        }

        void OnDocumentClosing( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
            {
                m_session = nullptr;
                redraw();
            }
        }

        void OnDocumentReloaded( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
                redraw();
        }

        void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) override
        {
            AGENT_VIEWER::OnCommitApplied( aSession, aChange );

            if( !shows( aSession ) )
                return;

            m_session = static_cast<FOOTPRINT_LIB_SESSION*>( &aSession );
            redraw();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "fp";
        }

    private:
        void redraw()
        {
            m_pads->DeleteAllItems();

            FOOTPRINT* footprint = nullptr;

            if( m_session && !m_session->Current().empty() && m_session->Has( m_session->Current() ) )
                footprint = &m_session->Footprint( m_session->Current() );

            // A new board each time: the canvas drops its view of the old one first.
            m_canvas->GetView()->Clear();
            m_shown = std::make_unique<BOARD>();
            m_shown->SetBoardUse( BOARD_USE::FPHOLDER );
            m_shownName.clear();

            if( footprint )
            {
                FOOTPRINT* copy = static_cast<FOOTPRINT*>( footprint->Duplicate( IGNORE_PARENT_GROUP ) );
                copy->SetParent( m_shown.get() );
                m_shown->Add( copy );
                m_shownName = m_session->Current();

                for( const PAD* pad : copy->Pads() )
                {
                    const VECTOR2I pos = pad->GetFPRelativePosition();
                    const VECTOR2I size = pad->GetSize( PADSTACK::ALL_LAYERS );
                    long           row = m_pads->InsertItem( m_pads->GetItemCount(), pad->GetNumber() );
                    m_pads->SetItem( row, 1, pad->ShowPadAttr() );
                    m_pads->SetItem( row, 2, wxString::Format( wxS( "%.3f" ), pcbIUScale.IUTomm( pos.x ) ) );
                    m_pads->SetItem( row, 3, wxString::Format( wxS( "%.3f" ), pcbIUScale.IUTomm( pos.y ) ) );
                    m_pads->SetItem( row, 4,
                                     wxString::Format( wxS( "%.3f x %.3f" ), pcbIUScale.IUTomm( size.x ),
                                                       pcbIUScale.IUTomm( size.y ) ) );
                }
            }

            m_canvas->DisplayBoard( m_shown.get() );
            m_canvas->UpdateColors();

            BOX2I box = footprint ? m_shown->GetFirstFootprint()->GetBoundingBox( true )
                                  : BOX2I( VECTOR2I( PcbIu( -5 ), PcbIu( -5 ) ), VECTOR2I( PcbIu( 10 ), PcbIu( 10 ) ) );
            box.Inflate( std::max( box.GetWidth(), box.GetHeight() ) / 5 );
            m_canvas->GetView()->SetViewport( BOX2D( box.GetOrigin(), box.GetSize() ) );
            m_canvas->StartDrawing();
            m_canvas->ForceRefresh();
        }

        GAL_DISPLAY_OPTIONS_IMPL m_galOptions;
        PCB_DRAW_PANEL_GAL*      m_canvas = nullptr;
        wxListCtrl*              m_pads = nullptr;
        FOOTPRINT_LIB_SESSION*   m_session = nullptr;
        std::unique_ptr<BOARD>   m_shown;
        std::string              m_shownName;
    };

} // namespace


AGENT_VIEWER* CreateFpViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new FP_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
