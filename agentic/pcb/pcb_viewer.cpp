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
#include <agentic/core/services.h>
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/pcb_items.h>
#include <agentic/pcb/pcb_viewer.h>

#include <board.h>
#include <connectivity/connectivity_data.h>
#include <gal_display_options_common.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_view.h>

#include <wx/checklst.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <chrono>
#include <set>


namespace AGENTIC
{

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    /**
     * Shows the project's board.  BOARD_LISTENER keeps the view in step with commits (S0.e);
     * SESSION_OBSERVER events drive the highlight, the zoom, reloads and the docks.
     */
    class PCB_VIEWER : public AGENT_VIEWER, public BOARD_LISTENER
    {
    public:
        PCB_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "pcb", _( "PCB" ) ),
                m_highlightTimer( this )
        {
            m_canvas = new PCB_DRAW_PANEL_GAL( this, wxID_ANY, wxDefaultPosition, wxSize( 900, 700 ), m_galOptions,
                                               EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL );
            SetCentralView( m_canvas );

            m_layers = new wxCheckListBox( this, wxID_ANY, wxDefaultPosition, wxSize( 180, 200 ) );
            m_nets = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 300, 160 ), wxLC_REPORT );
            m_nets->AppendColumn( _( "Net" ), wxLIST_FORMAT_LEFT, 160 );
            m_nets->AppendColumn( _( "Unrouted" ), wxLIST_FORMAT_RIGHT, 80 );
            m_drc = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 300, 160 ), wxLC_REPORT );
            m_drc->AppendColumn( _( "Severity" ), wxLIST_FORMAT_LEFT, 70 );
            m_drc->AppendColumn( _( "DRC" ), wxLIST_FORMAT_LEFT, 280 );
            m_unrouted = new wxStaticText( this, wxID_ANY, wxEmptyString );

            AddDock( m_layers, wxS( "layers" ), _( "Layers" ), wxLEFT );
            AddDock( m_nets, wxS( "nets" ), _( "Nets and ratsnest" ), wxRIGHT );
            AddDock( m_drc, wxS( "drc" ), _( "DRC" ), wxRIGHT );
            AddDock( m_unrouted, wxS( "unrouted" ), _( "Unrouted" ), wxBOTTOM );
            FinishLayout();

            m_layers->Bind( wxEVT_CHECKLISTBOX,
                            [this]( wxCommandEvent& aEvent )
                            {
                                // Visibility in this view only: the board's own settings are not changed.
                                const int index = aEvent.GetInt();

                                if( index >= 0 && index < (int) m_layerIds.size() )
                                {
                                    m_canvas->GetView()->SetLayerVisible( m_layerIds[index],
                                                                          m_layers->IsChecked( index ) );
                                    m_canvas->ForceRefresh();
                                }
                            } );

            auto touched = [this]( wxEvent& aEvent )
            {
                m_lastPersonView = std::chrono::steady_clock::now();
                aEvent.Skip();
            };

            m_canvas->Bind( wxEVT_MOUSEWHEEL, touched );
            m_canvas->Bind( wxEVT_MIDDLE_DOWN, touched );
            m_canvas->Bind( wxEVT_RIGHT_DOWN, touched );

            m_highlightTimer.Bind( wxEVT_TIMER,
                                   [this]( wxTimerEvent& )
                                   {
                                       clearHighlight();
                                   } );

            attach();
        }

        ~PCB_VIEWER() override
        {
            m_highlightTimer.Stop();
            detach();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            size_t         footprints = 0;

            if( m_session )
            {
                for( FOOTPRINT* fp : m_session->Board().Footprints() )
                    footprints += fp->viewPrivData() != nullptr;
            }

            state["footprintsInView"] = footprints;
            state["layerRows"] = m_layers->GetCount();
            state["netRows"] = m_nets->GetItemCount();
            state["drcRows"] = m_drc->GetItemCount();
            state["unrouted"] = utf8( m_unrouted->GetLabel() );
            return state;
        }

        void OnDocumentOpened( DOC_SESSION& aSession ) override
        {
            if( shows( aSession ) && !m_session )
                attach();
        }

        void OnDocumentClosing( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
                detach();
        }

        void OnDocumentReloading( DOC_SESSION& aSession ) override
        {
            if( &aSession == m_session )
                detach();
        }

        void OnDocumentReloaded( DOC_SESSION& aSession ) override
        {
            if( shows( aSession ) )
                attach();
        }

        void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) override
        {
            AGENT_VIEWER::OnCommitApplied( aSession, aChange );

            if( &aSession != m_session )
                return;

            highlight( aChange.value( "items", nlohmann::json::array() ) );
            refreshDocks();
        }

        void OnResults( DOC_SESSION& aSession, const nlohmann::json& aResults ) override
        {
            if( &aSession != m_session || aResults.value( "checker", "" ) != "drc" )
                return;

            m_drc->DeleteAllItems();

            for( const nlohmann::json& v : aResults["report"].value( "violations", nlohmann::json::array() ) )
            {
                long row = m_drc->InsertItem( m_drc->GetItemCount(), wxString::FromUTF8( v.value( "severity", "" ) ) );
                m_drc->SetItem( row, 1, wxString::FromUTF8( v.value( "description", "" ) ) );
            }

            // The markers were committed to the board; the listener added them to the view.
            m_canvas->ForceRefresh();
            refreshDocks();
        }

        void OnBoardCompositeUpdate( BOARD&, std::vector<BOARD_ITEM*>& aAdded, std::vector<BOARD_ITEM*>& aRemoved,
                                     std::vector<BOARD_ITEM*>& aChanged ) override
        {
            KIGFX::VIEW* view = m_canvas->GetView();

            for( BOARD_ITEM* item : aRemoved )
                view->Remove( item );

            for( BOARD_ITEM* item : aAdded )
                view->Add( item );

            for( BOARD_ITEM* item : aChanged )
                view->Update( item );

            m_canvas->RedrawRatsnest();
            m_canvas->ForceRefresh();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "pcb";
        }

    private:
        void attach()
        {
            for( DOC_SESSION* session : Project().Sessions() )
            {
                if( shows( *session ) )
                {
                    m_session = static_cast<BOARD_DOC_SESSION*>( session );
                    break;
                }
            }

            if( !m_session )
                return;

            BOARD& board = m_session->Board();
            board.AddListener( this );
            m_canvas->DisplayBoard( &board );
            m_canvas->SyncLayersVisibility( &board );
            m_canvas->UpdateColors();
            fit();
            m_canvas->StartDrawing();

            m_layers->Clear();
            m_layerIds.clear();

            for( PCB_LAYER_ID layer : board.GetEnabledLayers().UIOrder() )
            {
                m_layerIds.push_back( layer );
                m_layers->Append( board.GetLayerName( layer ) );
                m_layers->Check( m_layers->GetCount() - 1, true );
            }

            refreshDocks();
            refillChanges();
        }

        void detach()
        {
            if( !m_session )
                return;

            m_highlightTimer.Stop();
            m_highlighted.clear();
            m_canvas->GetView()->Clear();
            m_session->Board().RemoveListener( this );
            m_session = nullptr;
        }

        void fit()
        {
            BOARD& board = m_session->Board();
            BOX2I  box = board.GetBoardEdgesBoundingBox();

            if( box.GetWidth() <= 0 )
                box = board.GetBoundingBox();

            if( box.GetWidth() <= 0 )
                box = BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( PcbIu( 100 ), PcbIu( 80 ) ) );

            box.Inflate( box.GetWidth() / 20 );
            m_canvas->GetView()->SetViewport( BOX2D( box.GetOrigin(), box.GetSize() ) );
        }

        bool recentlyLookedAround() const
        {
            return std::chrono::steady_clock::now() - m_lastPersonView < std::chrono::seconds( 3 );
        }

        void highlight( const nlohmann::json& aKiids )
        {
            clearHighlight();

            BOARD& board = m_session->Board();
            BOX2I  area;
            bool   any = false;

            for( const nlohmann::json& kiid : aKiids )
            {
                BOARD_ITEM* item = board.ResolveItem( KIID( wxString::FromUTF8( kiid.get<std::string>() ) ), true );

                if( !item )
                    continue;

                item->SetBrightened();
                m_canvas->GetView()->Update( item );
                m_highlighted.push_back( item->m_Uuid );

                if( any )
                    area.Merge( item->GetBoundingBox() );
                else
                    area = item->GetBoundingBox();

                any = true;
            }

            if( any && !recentlyLookedAround() )
            {
                area.Inflate( std::max( area.GetWidth(), area.GetHeight() ) + PcbIu( 5 ) );
                m_canvas->GetView()->SetViewport( BOX2D( area.GetOrigin(), area.GetSize() ) );
            }

            m_canvas->ForceRefresh();
            m_highlightTimer.StartOnce( 1500 );
        }

        void clearHighlight()
        {
            if( !m_session )
                return;

            for( const KIID& kiid : m_highlighted )
            {
                if( BOARD_ITEM* item = m_session->Board().ResolveItem( kiid, true ) )
                {
                    item->ClearBrightened();
                    m_canvas->GetView()->Update( item );
                }
            }

            m_highlighted.clear();
            m_canvas->ForceRefresh();
        }

        void refreshDocks()
        {
            if( !m_session )
                return;

            PCB_RATSNEST ratsnest = PCB_RATSNEST::Take( m_session->Board() );
            m_nets->DeleteAllItems();

            for( NETINFO_ITEM* net : m_session->Board().GetNetInfo() )
            {
                if( net->GetNetCode() <= 0 )
                    continue;

                long row = m_nets->InsertItem( m_nets->GetItemCount(), net->GetNetname() );
                auto it = ratsnest.unrouted.find( utf8( net->GetNetname() ) );
                m_nets->SetItem( row, 1,
                                 wxString::Format( wxS( "%d" ), it == ratsnest.unrouted.end() ? 0 : it->second ) );
            }

            m_unrouted->SetLabel( wxString::Format( _( "%d unrouted connections" ), ratsnest.total ) );
        }

        GAL_DISPLAY_OPTIONS_IMPL              m_galOptions;
        PCB_DRAW_PANEL_GAL*                   m_canvas = nullptr;
        wxCheckListBox*                       m_layers = nullptr;
        wxListCtrl*                           m_nets = nullptr;
        wxListCtrl*                           m_drc = nullptr;
        wxStaticText*                         m_unrouted = nullptr;
        BOARD_DOC_SESSION*                    m_session = nullptr;
        std::vector<PCB_LAYER_ID>             m_layerIds;
        std::vector<KIID>                     m_highlighted;
        wxTimer                               m_highlightTimer;
        std::chrono::steady_clock::time_point m_lastPersonView;
    };

} // namespace


AGENT_VIEWER* CreatePcbViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new PCB_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
