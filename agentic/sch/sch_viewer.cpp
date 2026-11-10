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
#include <agentic/sch/sch_doc_session.h>
#include <agentic/sch/sch_items.h>
#include <agentic/sch/sch_viewer.h>

#include <base_units.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <gal/gal_display_options.h>
#include <gal_display_options_common.h>
#include <sch_draw_panel.h>
#include <sch_item.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_view.h>
#include <schematic.h>

#include <wx/listctrl.h>
#include <wx/timer.h>
#include <wx/treectrl.h>

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
     * Shows the project's schematic.  It observes the session: SCHEMATIC_LISTENER keeps the view
     * in step with commits (as spike S0.e found), SESSION_OBSERVER events drive the sheet shown,
     * the highlight, the zoom, reloads and the docks.  It never changes the document.
     */
    class SCH_VIEWER : public AGENT_VIEWER, public SCHEMATIC_LISTENER
    {
    public:
        SCH_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "sch", _( "Schematic" ) ),
                m_highlightTimer( this )
        {
            m_canvas = new SCH_DRAW_PANEL( this, wxID_ANY, wxDefaultPosition, wxSize( 900, 700 ), m_galOptions,
                                           EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL );
            SetCentralView( m_canvas );

            m_hierarchy = new wxTreeCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 220, 200 ),
                                          wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_SINGLE );
            m_ercList = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 320, 160 ), wxLC_REPORT );
            m_ercList->AppendColumn( _( "Severity" ), wxLIST_FORMAT_LEFT, 70 );
            m_ercList->AppendColumn( _( "ERC" ), wxLIST_FORMAT_LEFT, 300 );
            m_netList = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 320, 160 ), wxLC_REPORT );
            m_netList->AppendColumn( _( "Net" ), wxLIST_FORMAT_LEFT, 120 );
            m_netList->AppendColumn( _( "Pins" ), wxLIST_FORMAT_LEFT, 200 );

            AddDock( m_hierarchy, wxS( "hierarchy" ), _( "Hierarchy" ), wxLEFT );
            AddDock( m_ercList, wxS( "erc" ), _( "ERC" ), wxRIGHT );
            AddDock( m_netList, wxS( "nets" ), _( "Nets" ), wxRIGHT );
            FinishLayout();

            m_hierarchy->Bind( wxEVT_TREE_SEL_CHANGED,
                               [this]( wxTreeEvent& aEvent )
                               {
                                   if( m_rebuilding )
                                       return;

                                   auto it = m_sheetItems.find( aEvent.GetItem().GetID() );

                                   if( it != m_sheetItems.end() )
                                       showScreen( it->second, true );
                               } );

            // The person looking around: no automatic zoom for a while (4.5).
            auto touched = [this]( wxEvent& aEvent )
            {
                m_lastPersonView = std::chrono::steady_clock::now();
                aEvent.Skip();
            };

            m_canvas->Bind( wxEVT_MOUSEWHEEL, touched );
            m_canvas->Bind( wxEVT_MIDDLE_DOWN, touched );
            m_canvas->Bind( wxEVT_RIGHT_DOWN, touched );

            // An owned timer sends its events to the owner.
            Bind(
                    wxEVT_TIMER,
                    [this]( wxTimerEvent& )
                    {
                        clearHighlight();
                    },
                    m_highlightTimer.GetId() );

            attach();
        }

        ~SCH_VIEWER() override
        {
            m_highlightTimer.Stop();
            detach();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            nlohmann::json viewItems = 0;
            size_t         count = 0;

            if( m_canvas && m_session && m_shown )
            {
                for( SCH_ITEM* item : m_shown->Items() )
                {
                    if( item->viewPrivData() )
                        count++;
                }
            }

            state["sheetShown"] = m_session && m_shown ? m_session->FileOf( m_shown ) : "";
            state["itemsInView"] = count;
            state["ercRows"] = m_ercList->GetItemCount();
            state["netRows"] = m_netList->GetItemCount();
            state["hierarchyRows"] = m_sheetItems.size();
            return state;
        }

        // SESSION_OBSERVER ---------------------------------------------------------------------

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
            // The view must let go of the items before the schematic is replaced (S0.e).
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

            // Follow the sheet the agent edits.
            SCH_SCREEN* current = m_session->CurrentSheet().LastScreen();

            if( current && current != m_shown )
                showScreen( current, false );

            highlight( aChange.value( "items", nlohmann::json::array() ) );
            rebuildHierarchy();
            rebuildNets();
        }

        void OnResults( DOC_SESSION& aSession, const nlohmann::json& aResults ) override
        {
            if( &aSession != m_session || aResults.value( "checker", "" ) != "erc" )
                return;

            m_ercList->DeleteAllItems();

            for( const nlohmann::json& v : aResults["report"].value( "violations", nlohmann::json::array() ) )
            {
                long row = m_ercList->InsertItem( m_ercList->GetItemCount(),
                                                  wxString::FromUTF8( v.value( "severity", "" ) ) );
                m_ercList->SetItem( row, 1, wxString::FromUTF8( v.value( "description", "" ) ) );
            }

            // The markers are on the screens now; draw them.
            if( m_shown )
                showScreen( m_shown, false );
        }

        void OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                        const std::string& aSummary ) override
        {
            AGENT_VIEWER::OnToolEnd( aTool, aArgs, aOk, aMillis, aSummary );

            // A capture or a query of a sheet brings the view back to what the agent looks at.
            if( m_session && ( aTool == "sch_capture" ) )
            {
                m_lastPersonView = {};
                SCH_SCREEN* current = m_session->CurrentSheet().LastScreen();

                if( current )
                    showScreen( current, true );
            }
        }

        // SCHEMATIC_LISTENER: mirror commits into the view -------------------------------------

        void OnSchItemsAdded( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
        {
            for( SCH_ITEM* item : aItems )
            {
                if( m_shown && m_shown->CheckIfOnDrawList( item ) )
                    m_canvas->GetView()->Add( item );
            }

            m_canvas->ForceRefresh();
        }

        void OnSchItemsRemoved( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
        {
            for( SCH_ITEM* item : aItems )
            {
                if( item->viewPrivData() )
                    m_canvas->GetView()->Remove( item );
            }

            m_canvas->ForceRefresh();
        }

        void OnSchItemsChanged( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
        {
            for( SCH_ITEM* item : aItems )
            {
                if( item->viewPrivData() )
                    m_canvas->GetView()->Update( item );
            }

            m_canvas->ForceRefresh();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "sch";
        }

    private:
        void attach()
        {
            for( DOC_SESSION* session : Project().Sessions() )
            {
                if( shows( *session ) )
                {
                    m_session = static_cast<SCH_DOC_SESSION*>( session );
                    break;
                }
            }

            if( !m_session )
                return;

            m_session->Schematic().AddListener( this );
            SCH_SCREEN* screen = m_session->CurrentSheet().LastScreen();
            showScreen( screen ? screen : m_session->Schematic().RootScreen(), true );
            rebuildHierarchy();
            rebuildNets();
            refillChanges();
        }

        void detach()
        {
            if( !m_session )
                return;

            m_highlightTimer.Stop();
            m_highlighted.clear();

            // The view's drawing sheet and its text variable listener refer to the schematic's
            // text variable tracker: let go of both, then of the items (Cleanup, not Clear).
            m_canvas->GetView()->DetachTextVarTracker();
            m_canvas->GetView()->Cleanup();
            m_session->Schematic().RemoveListener( this );
            m_session = nullptr;
            m_shown = nullptr;
            m_sheetItems.clear();
            m_hierarchy->DeleteAllItems();
        }

        void showScreen( SCH_SCREEN* aScreen, bool aFit )
        {
            if( !aScreen )
                return;

            m_canvas->GetView()->Clear();
            m_canvas->DisplaySheet( aScreen );
            m_shown = aScreen;

            if( aFit || !recentlyLookedAround() )
            {
                const VECTOR2I size = aScreen->GetPageSettings().GetSizeIU( schIUScale.IU_PER_MILS );
                m_canvas->GetView()->SetViewport( BOX2D( VECTOR2D( 0, 0 ), VECTOR2D( size ) ) );
            }

            m_canvas->StartDrawing();
            m_canvas->ForceRefresh();
        }

        bool recentlyLookedAround() const
        {
            return std::chrono::steady_clock::now() - m_lastPersonView < std::chrono::seconds( 3 );
        }

        void highlight( const nlohmann::json& aKiids )
        {
            clearHighlight();

            if( !m_shown )
                return;

            std::set<std::string> wanted;

            for( const nlohmann::json& kiid : aKiids )
                wanted.insert( kiid.get<std::string>() );

            BOX2I area;
            bool  any = false;

            for( SCH_ITEM* item : m_shown->Items() )
            {
                if( !wanted.count( utf8( item->m_Uuid.AsString() ) ) )
                    continue;

                item->SetBrightened();
                m_canvas->GetView()->Update( item );
                m_highlighted.push_back( item );

                if( any )
                    area.Merge( item->GetBoundingBox() );
                else
                    area = item->GetBoundingBox();

                any = true;
            }

            // Zoom to the change unless the person moved the view a moment ago.
            if( any && !recentlyLookedAround() )
            {
                area.Inflate( schIUScale.mmToIU( 25 ) );
                m_canvas->GetView()->SetViewport( BOX2D( area.GetOrigin(), area.GetSize() ) );
            }

            m_canvas->ForceRefresh();
            m_highlightTimer.StartOnce( 1500 );
        }

        void clearHighlight()
        {
            for( SCH_ITEM* item : m_highlighted )
            {
                item->ClearBrightened();

                if( item->viewPrivData() )
                    m_canvas->GetView()->Update( item );
            }

            m_highlighted.clear();
            m_canvas->ForceRefresh();
        }

        void rebuildHierarchy()
        {
            if( !m_session )
                return;

            m_rebuilding = true;
            m_hierarchy->DeleteAllItems();
            m_sheetItems.clear();
            wxTreeItemId root = m_hierarchy->AddRoot( wxS( "root" ) );

            for( const SCH_SHEET_PATH& path : m_session->Schematic().Hierarchy() )
            {
                wxString label = path.PathHumanReadable( false );

                if( path.size() > 1 )
                    label = path.Last()->GetName() + wxS( "  (" ) + label + wxS( ")" );

                wxTreeItemId item = m_hierarchy->AppendItem( root, label );
                m_sheetItems[item.GetID()] = path.LastScreen();

                if( path.LastScreen() == m_shown )
                    m_hierarchy->SelectItem( item );
            }

            m_rebuilding = false;
        }

        void rebuildNets()
        {
            if( !m_session )
                return;

            m_netList->DeleteAllItems();
            SCH_NET_SNAPSHOT snapshot = SCH_NET_SNAPSHOT::Take( m_session->Schematic() );

            for( const auto& [name, pins] : snapshot.nets )
            {
                wxString list;

                for( const std::string& pin : pins )
                    list += ( list.IsEmpty() ? wxString() : wxString( wxS( " " ) ) ) + wxString::FromUTF8( pin );

                long row = m_netList->InsertItem( m_netList->GetItemCount(), wxString::FromUTF8( name ) );
                m_netList->SetItem( row, 1, list );
            }
        }

        GAL_DISPLAY_OPTIONS_IMPL              m_galOptions;
        SCH_DRAW_PANEL*                       m_canvas = nullptr;
        wxTreeCtrl*                           m_hierarchy = nullptr;
        wxListCtrl*                           m_ercList = nullptr;
        wxListCtrl*                           m_netList = nullptr;
        SCH_DOC_SESSION*                      m_session = nullptr;
        SCH_SCREEN*                           m_shown = nullptr;
        std::map<void*, SCH_SCREEN*>          m_sheetItems;
        std::vector<SCH_ITEM*>                m_highlighted;
        wxTimer                               m_highlightTimer;
        bool                                  m_rebuilding = false;
        std::chrono::steady_clock::time_point m_lastPersonView;
    };

} // namespace


AGENT_VIEWER* CreateSchViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new SCH_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
