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
#include <agentic/ds/ds_session.h>
#include <agentic/ds/ds_tools.h>
#include <agentic/ds/ds_viewer.h>
#include <agentic/protocol/registry.h>

#include <drawing_sheet/ds_data_item.h>
#include <drawing_sheet/ds_data_model.h>

#include <wx/listctrl.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>


namespace AGENTIC
{

namespace
{

    /// Shows the sheet the agent changed last, drawn as ds_capture draws it, and its items.
    class DS_VIEWER : public AGENT_VIEWER
    {
    public:
        DS_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "ds", _( "Drawing Sheet Editor" ) )
        {
            m_scroll = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxSize( 900, 650 ) );
            m_scroll->SetScrollRate( 10, 10 );
            m_bitmap = new wxStaticBitmap( m_scroll, wxID_ANY, wxBitmap() );
            wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
            sizer->Add( m_bitmap, 0, wxALL, 4 );
            m_scroll->SetSizer( sizer );
            SetCentralView( m_scroll );

            m_items = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 320, 300 ), wxLC_REPORT );
            m_items->AppendColumn( _( "Type" ), wxLIST_FORMAT_LEFT, 60 );
            m_items->AppendColumn( _( "Name" ), wxLIST_FORMAT_LEFT, 100 );
            m_items->AppendColumn( _( "Text" ), wxLIST_FORMAT_LEFT, 150 );
            AddDock( m_items, wxS( "items" ), _( "Items" ), wxRIGHT );
            FinishLayout();

            for( DOC_SESSION* session : Project().Sessions() )
            {
                if( shows( *session ) )
                    m_session = static_cast<SHEET_SESSION*>( session );
            }

            redraw();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["file"] = m_session ? m_session->Path() : std::string();
            state["itemRows"] = m_items->GetItemCount();
            state["pictureWidth"] = m_bitmap->GetBitmap().IsOk() ? m_bitmap->GetBitmap().GetWidth() : 0;
            return state;
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

            m_session = static_cast<SHEET_SESSION*>( &aSession );
            redraw();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "ds";
        }

    private:
        void redraw()
        {
            m_items->DeleteAllItems();

            if( !m_session )
            {
                m_bitmap->SetBitmap( wxBitmap() );
                return;
            }

            for( DS_DATA_ITEM* item : m_session->Model().GetItems() )
            {
                static const wxString types[] = { wxS( "text" ), wxS( "line" ), wxS( "rect" ), wxS( "polygon" ),
                                                  wxS( "bitmap" ) };
                long                  row = m_items->InsertItem( m_items->GetItemCount(), types[item->GetType()] );
                m_items->SetItem( row, 1, item->m_Name );

                if( item->GetType() == DS_DATA_ITEM::DS_TEXT )
                    m_items->SetItem( row, 2, static_cast<DS_DATA_ITEM_TEXT*>( item )->m_TextBase );
            }

            const std::filesystem::path file = Project().Root() / ".agentic" / "captures" / "sheet-view.png";

            try
            {
                std::filesystem::create_directories( file.parent_path() );
                m_bitmap->SetBitmap( wxBitmap( DrawingSheetPicture( m_session->Model(), Project().KicadProject(), "A4",
                                                                    860, file.string() ) ) );
            }
            catch( const TOOL_ERROR& )
            {
                m_bitmap->SetBitmap( wxBitmap() );
            }

            m_scroll->FitInside();
            m_scroll->Refresh();
        }

        wxScrolledWindow* m_scroll = nullptr;
        wxStaticBitmap*   m_bitmap = nullptr;
        wxListCtrl*       m_items = nullptr;
        SHEET_SESSION*    m_session = nullptr;
    };

} // namespace


AGENT_VIEWER* CreateDrawingSheetViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new DS_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
