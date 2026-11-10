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

#include <agentic/hub/hub_frame.h>
#include <agentic/core/agent_host.h>
#include <agentic/transport/http_transport.h>

#include <wx/crt.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>


namespace AGENTIC
{

HUB_FRAME::HUB_FRAME( KIWAY* aKiway ) :
        wxFrame( nullptr, wxID_ANY, wxS( "KiCad - Agent" ), wxDefaultPosition, wxSize( 720, 480 ) ),
        m_kiway( aKiway )
{
    m_host = std::make_unique<AGENT_HOST>( AGENT_HOST::MODE::VIEWER, aKiway,
                                           [this]( std::function<void()> aWork )
                                           {
                                               CallAfter( std::move( aWork ) );
                                           } );

    m_info = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxTE_READONLY );

    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    sizer->Add( m_info, 1, wxEXPAND );
    SetSizer( sizer );

    Bind( wxEVT_CLOSE_WINDOW, &HUB_FRAME::onClose, this );
}


HUB_FRAME::~HUB_FRAME()
{
    // Stop the transport first: its threads must not reach the host while it is destroyed.
    if( m_transport )
        m_transport->Stop();
}


bool HUB_FRAME::StartServer( int aPort )
{
    HTTP_OPTIONS options;
    options.port = static_cast<unsigned short>( aPort );
    options.token = GenerateToken();

    m_transport = std::make_unique<HTTP_TRANSPORT>( m_host->Server(), options );

    try
    {
        m_transport->Start();
    }
    catch( const std::exception& e )
    {
        wxFprintf( stderr, "%s\n", e.what() );
        return false;
    }

    wxString command = wxString::Format( wxS( "claude mcp add --transport http kicad %s "
                                              "--header \"Authorization: Bearer %s\"" ),
                                         m_transport->Url(), options.token );

    wxFprintf( stderr, "Agentic-KiCad MCP server listening at %s\n", m_transport->Url() );
    wxFprintf( stderr, "Bearer token: %s\n", options.token );
    wxFprintf( stderr, "Connect Claude Code with:\n  %s\n", command );

    m_info->SetValue( wxString::Format( wxS( "Agentic-KiCad MCP server\n\nURL: %s\n\n"
                                             "Connect Claude Code with:\n%s\n" ),
                                        m_transport->Url(), command ) );
    return true;
}


void HUB_FRAME::onClose( wxCloseEvent& aEvent )
{
    if( m_transport )
        m_transport->Stop();

    Destroy();
}

} // namespace AGENTIC
