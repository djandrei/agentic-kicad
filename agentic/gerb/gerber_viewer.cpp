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
#include <agentic/gerb/gerber_tools.h>
#include <agentic/gerb/gerber_viewer.h>
#include <agentic/protocol/registry.h>

#include <gerber_file_image.h>
#include <gerber_file_image_list.h>

#include <wx/checklst.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>


namespace AGENTIC
{

namespace
{

    /**
     * Shows the loaded layers as the agent's capture draws them (the same headless renderer),
     * with a layer list to choose what is drawn, and the selected layer's file information.
     */
    class GERBER_VIEWER : public AGENT_VIEWER
    {
    public:
        GERBER_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "gerber", _( "Gerber Viewer" ) )
        {
            m_scroll = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxSize( 800, 600 ) );
            m_scroll->SetScrollRate( 10, 10 );
            m_scroll->SetBackgroundColour( *wxWHITE );
            m_bitmap = new wxStaticBitmap( m_scroll, wxID_ANY, wxBitmap() );
            wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
            sizer->Add( m_bitmap, 0, wxALL, 4 );
            m_scroll->SetSizer( sizer );
            SetCentralView( m_scroll );

            m_layers = new wxCheckListBox( this, wxID_ANY, wxDefaultPosition, wxSize( 260, 240 ) );
            m_info = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize( 260, 160 ),
                                     wxTE_MULTILINE | wxTE_READONLY );
            AddDock( m_layers, wxS( "layers" ), _( "Layers" ), wxLEFT );
            AddDock( m_info, wxS( "info" ), _( "File" ), wxLEFT );
            FinishLayout();

            m_layers->Bind( wxEVT_CHECKLISTBOX,
                            [this]( wxCommandEvent& )
                            {
                                draw();
                            } );
            m_layers->Bind( wxEVT_LISTBOX,
                            [this]( wxCommandEvent& aEvent )
                            {
                                showInfo( aEvent.GetSelection() );
                            } );

            reload();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["layerRows"] = m_layers->GetCount();
            state["pictureWidth"] = m_bitmap->GetBitmap().IsOk() ? m_bitmap->GetBitmap().GetWidth() : 0;
            state["loads"] = m_loads;
            return state;
        }

        void OnFilesChanged( AGENT_PROJECT& aProject ) override
        {
            if( &aProject == &Project() && GerberState( Project() ).loads != m_loads )
                reload();
        }

    protected:
        bool shows( const DOC_SESSION& ) const override { return false; }

    private:
        void reload()
        {
            GERBER_STATE& state = GerberState( Project() );
            m_loads = state.loads;
            m_layers->Clear();

            for( const std::string& file : state.files )
            {
                m_layers->Append( wxString::FromUTF8( file ) );
                m_layers->Check( m_layers->GetCount() - 1, true );
            }

            m_info->Clear();
            draw();

            if( m_layers->GetCount() )
                showInfo( 0 );
        }

        void draw()
        {
            std::vector<int> shown;

            for( unsigned i = 0; i < m_layers->GetCount(); ++i )
            {
                if( m_layers->IsChecked( i ) )
                    shown.push_back( static_cast<int>( i ) );
            }

            if( shown.empty() )
            {
                m_bitmap->SetBitmap( wxBitmap() );
                return;
            }

            try
            {
                const int width = std::max( 300, m_scroll->GetClientSize().GetWidth() - 10 );
                m_bitmap->SetBitmap( wxBitmap( GerberPicture( Project(), shown, width ) ) );
            }
            catch( const TOOL_ERROR& )
            {
                m_bitmap->SetBitmap( wxBitmap() );
            }

            m_scroll->FitInside();
            m_scroll->Refresh();
        }

        void showInfo( int aIndex )
        {
            GERBER_STATE& state = GerberState( Project() );

            if( aIndex < 0 || aIndex >= (int) state.files.size() )
                return;

            GERBER_FILE_IMAGE* img = state.images->GetGbrImage( aIndex );

            if( !img )
                return;

            wxString text = wxString::FromUTF8( state.files[aIndex] ) + wxS( "\n" );
            text += wxString::Format( _( "Items: %d\nApertures: %d\n" ), img->GetItemsCount(), img->GetDcodesCount() );

            if( img->m_ImageName.size() )
                text += _( "Image: " ) + img->m_ImageName + wxS( "\n" );

            m_info->SetValue( text );
        }

        wxScrolledWindow* m_scroll = nullptr;
        wxStaticBitmap*   m_bitmap = nullptr;
        wxCheckListBox*   m_layers = nullptr;
        wxTextCtrl*       m_info = nullptr;
        int               m_loads = -1;
    };

} // namespace


AGENT_VIEWER* CreateGerberViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new GERBER_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
