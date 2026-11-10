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


#include <agentic/core/agent_host.h>
#include <agentic/core/provider.h>
#include <agentic/hub/hub_frame.h>
#include <agentic/transport/http_transport.h>
#include <agentic/viewer_shell/agent_viewer.h>

#include <wx/button.h>
#include <wx/crt.h>
#include <wx/datetime.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>
#include <wx/utils.h>

#include <cstdlib>


namespace AGENTIC
{

namespace
{

    const int MAX_ACTIVITY_ROWS = 1000;

    /// Folder of each project-relative path, e.g. "a/b/c.txt" -> "a/b".
    std::string parentOf( const std::string& aPath )
    {
        const size_t slash = aPath.find_last_of( '/' );
        return slash == std::string::npos ? std::string() : aPath.substr( 0, slash );
    }


    std::string nameOf( const std::string& aPath )
    {
        const size_t slash = aPath.find_last_of( '/' );
        return slash == std::string::npos ? aPath : aPath.substr( slash + 1 );
    }


    class PATH_DATA : public wxTreeItemData
    {
    public:
        explicit PATH_DATA( std::string aPath ) :
                m_path( std::move( aPath ) )
        {
        }
        const std::string& Path() const { return m_path; }

    private:
        std::string m_path;
    };

} // namespace


HUB_FRAME::HUB_FRAME( KIWAY* aKiway ) :
        wxFrame( nullptr, wxID_ANY, wxS( "KiCad - Agent" ), wxDefaultPosition, wxSize( 980, 640 ) ),
        m_kiway( aKiway )
{
    m_host = std::make_unique<AGENT_HOST>( AGENT_HOST::MODE::VIEWER, aKiway,
                                           [this]( std::function<void()> aWork )
                                           {
                                               CallAfter( std::move( aWork ) );
                                           } );

    wxBoxSizer* top = new wxBoxSizer( wxVERTICAL );

    // Program buttons
    wxBoxSizer* programs = new wxBoxSizer( wxHORIZONTAL );

    for( const AGENT_HOST::PROVIDER_INFO& info : m_host->Providers() )
    {
        if( !info.provider )
            continue;

        PROGRAM_BUTTON entry;
        entry.provider = info.provider;
        entry.button = new wxButton( this, wxID_ANY, info.provider->Title() );
        entry.button->SetToolTip( wxString::Format( _( "Show the %s viewer" ), info.provider->Title() ) );
        entry.dot = new wxWindow( this, wxID_ANY, wxDefaultPosition, wxSize( 10, 10 ) );
        entry.dot->SetBackgroundColour( GetBackgroundColour() );

        AGENT_PROVIDER* provider = info.provider;
        entry.button->Bind( wxEVT_BUTTON,
                            [this, provider]( wxCommandEvent& )
                            {
                                showViewer( *provider );
                            } );

        programs->Add( entry.button, 0, wxALL, 2 );
        programs->Add( entry.dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8 );
        m_buttons[provider->Name().ToStdString()] = entry;
    }

    top->Add( programs, 0, wxEXPAND | wxALL, 2 );

    // Projects and files on the left, activity on the right
    wxSplitterWindow* split =
            new wxSplitterWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D );
    wxPanel*    left = new wxPanel( split );
    wxBoxSizer* leftSizer = new wxBoxSizer( wxVERTICAL );

    leftSizer->Add( new wxStaticText( left, wxID_ANY, _( "Projects" ) ), 0, wxALL, 2 );
    m_projects = new wxListBox( left, wxID_ANY, wxDefaultPosition, wxSize( -1, 80 ) );
    leftSizer->Add( m_projects, 0, wxEXPAND | wxALL, 2 );
    leftSizer->Add( new wxStaticText( left, wxID_ANY, _( "Files" ) ), 0, wxALL, 2 );
    m_files = new wxTreeCtrl( left, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT );
    leftSizer->Add( m_files, 1, wxEXPAND | wxALL, 2 );
    left->SetSizer( leftSizer );

    m_activity = new wxListCtrl( split, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL );
    m_activity->AppendColumn( _( "Time" ), wxLIST_FORMAT_LEFT, 80 );
    m_activity->AppendColumn( _( "Tool" ), wxLIST_FORMAT_LEFT, 160 );
    m_activity->AppendColumn( _( "Result" ), wxLIST_FORMAT_LEFT, 80 );
    m_activity->AppendColumn( _( "ms" ), wxLIST_FORMAT_RIGHT, 60 );
    m_activity->AppendColumn( _( "Detail" ), wxLIST_FORMAT_LEFT, 300 );

    split->SplitVertically( left, m_activity, 300 );
    top->Add( split, 1, wxEXPAND );

    m_status = new STATUS_ROW( this, m_host->Services(), false );
    top->Add( m_status, 0, wxEXPAND );
    SetSizer( top );

    m_projects->Bind( wxEVT_LISTBOX,
                      [this]( wxCommandEvent& )
                      {
                          int                         sel = m_projects->GetSelection();
                          std::vector<AGENT_PROJECT*> projects = m_host->Sessions().Projects();

                          if( sel >= 0 && sel < (int) projects.size() )
                          {
                              m_host->Sessions().SetCurrent( *projects[sel] );
                              rebuildFiles();
                          }
                      } );

    m_files->Bind( wxEVT_TREE_ITEM_EXPANDED,
                   [this]( wxTreeEvent& aEvent )
                   {
                       if( auto* data = dynamic_cast<PATH_DATA*>( m_files->GetItemData( aEvent.GetItem() ) ) )
                           m_expanded.insert( data->Path() );
                   } );

    m_files->Bind( wxEVT_TREE_ITEM_COLLAPSED,
                   [this]( wxTreeEvent& aEvent )
                   {
                       if( auto* data = dynamic_cast<PATH_DATA*>( m_files->GetItemData( aEvent.GetItem() ) ) )
                           m_expanded.erase( data->Path() );
                   } );

    m_host->Observers().Add( this );
    m_host->SetProviderUse(
            [this]( AGENT_PROVIDER& aProvider )
            {
                m_runningGroup = aProvider.Name().ToStdString();
                ensureViewer( aProvider );
            } );

    registerTestHooks();

    Bind( wxEVT_CLOSE_WINDOW, &HUB_FRAME::onClose, this );
}


HUB_FRAME::~HUB_FRAME()
{
    // Stop the transport first: its threads must not reach the host while it is destroyed.
    if( m_transport )
        m_transport->Stop();

    for( auto& [key, viewer] : m_viewers )
        viewer->Retire();

    m_viewers.clear();
    m_host->Observers().Remove( this );
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

    m_url = m_transport->Url();
    m_status->SetText( wxString::Format( _( "Listening at %s" ), m_url ) );
    m_status->SetToolTip( command );
    return true;
}


void HUB_FRAME::onClose( wxCloseEvent& aEvent )
{
    if( m_transport )
        m_transport->Stop();

    Destroy();
}


void HUB_FRAME::rebuildProjects()
{
    m_projects->Clear();
    int index = 0;

    for( AGENT_PROJECT* project : m_host->Sessions().Projects() )
    {
        m_projects->Append( wxString::FromUTF8( project->Name() + "  (" + project->Root().string() + ")" ) );

        if( project == m_host->Sessions().Current() )
            m_projects->SetSelection( index );

        index++;
    }
}


void HUB_FRAME::rebuildFiles()
{
    m_files->Freeze();
    m_files->DeleteAllItems();

    wxTreeItemId root = m_files->AddRoot( wxS( "root" ) );

    if( AGENT_PROJECT* project = m_host->Sessions().Current() )
    {
        std::map<std::string, wxTreeItemId> folders;
        folders[""] = root;

        try
        {
            for( const FILE_STORE::ENTRY& entry : project->Files().List( "", true ) )
            {
                auto parent = folders.find( parentOf( entry.path ) );

                if( parent == folders.end() )
                    continue;

                wxTreeItemId item = m_files->AppendItem( parent->second, wxString::FromUTF8( nameOf( entry.path ) ), -1,
                                                         -1, new PATH_DATA( entry.path ) );

                if( entry.directory )
                    folders[entry.path] = item;
            }
        }
        catch( const std::exception& )
        {
            // The folder can vanish under us; the next change rebuilds the tree.
        }

        // Folders are collapsed unless the person opened them.
        for( const auto& [path, item] : folders )
        {
            if( !path.empty() && m_expanded.contains( path ) )
                m_files->Expand( item );
        }
    }

    m_files->Thaw();
}


void HUB_FRAME::OnProjectOpened( AGENT_PROJECT& aProject )
{
    rebuildProjects();
    rebuildFiles();
}


void HUB_FRAME::OnProjectClosed( AGENT_PROJECT& aProject )
{
    for( auto it = m_viewers.begin(); it != m_viewers.end(); )
    {
        if( it->first.second == &aProject )
        {
            it->second->Retire();
            it = m_viewers.erase( it );
        }
        else
        {
            ++it;
        }
    }

    // The project is still in the list while it closes; rebuild once it is gone.
    CallAfter(
            [this]()
            {
                rebuildProjects();
                rebuildFiles();
            } );
}


void HUB_FRAME::OnFilesChanged( AGENT_PROJECT& aProject )
{
    if( &aProject == m_host->Sessions().Current() )
        rebuildFiles();
}


void HUB_FRAME::OnToolStart( const std::string& aTool, const nlohmann::json& aArgs )
{
    m_status->SetText( wxString::Format( _( "Running %s ..." ), wxString::FromUTF8( aTool ) ) );
}


void HUB_FRAME::OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                           const std::string& aSummary )
{
    if( m_activity->GetItemCount() >= MAX_ACTIVITY_ROWS )
        m_activity->DeleteItem( 0 );

    long row = m_activity->InsertItem( m_activity->GetItemCount(), wxDateTime::Now().FormatISOTime() );
    m_activity->SetItem( row, 1, wxString::FromUTF8( aTool ) );
    m_activity->SetItem( row, 2, aOk ? _( "ok" ) : _( "failed" ) );
    m_activity->SetItem( row, 3, wxString::Format( wxS( "%.0f" ), aMillis ) );
    m_activity->SetItem( row, 4, wxString::FromUTF8( aSummary.substr( 0, 300 ) ) );
    m_activity->EnsureVisible( row );

    if( !aOk )
        m_activity->SetItemTextColour( row, *wxRED );

    // The activity dot of a program whose viewer is hidden (D6).
    if( !m_runningGroup.empty() )
    {
        AGENT_PROJECT* project = m_host->Sessions().Current();
        auto           viewer = m_viewers.find( { m_runningGroup, project } );

        if( viewer == m_viewers.end() || !viewer->second->IsShown() )
            setDot( m_runningGroup, aOk ? wxColour( 0, 160, 0 ) : *wxRED );
    }

    m_runningGroup.clear();

    m_status->SetText( wxString::Format( _( "%s  |  last call: %s %s (%.0f ms)" ), m_url, wxString::FromUTF8( aTool ),
                                         aOk ? _( "ok" ) : _( "failed" ), aMillis ) );

    rebuildProjects();
}


void HUB_FRAME::OnPauseChanged( bool aPaused )
{
    m_status->SyncPause( aPaused );
}


void HUB_FRAME::setDot( const std::string& aGroup, const wxColour& aColour )
{
    auto it = m_buttons.find( aGroup );

    if( it == m_buttons.end() )
        return;

    it->second.dot->SetBackgroundColour( aColour );
    it->second.dot->Refresh();
}


void HUB_FRAME::ensureViewer( AGENT_PROVIDER& aProvider )
{
    AGENT_PROJECT* project = m_host->Sessions().Current();

    if( !project )
        return;

    auto key = std::make_pair( aProvider.Name().ToStdString(), project );

    if( m_viewers.count( key ) )
        return;

    AGENT_VIEWER* viewer = aProvider.CreateViewer( m_host->Services(), *project );

    if( !viewer )
        return;

    m_viewers[key] = viewer;

    // First use opens the viewer without taking focus from wherever the person is working.
    viewer->ShowWithoutActivating();
}


void HUB_FRAME::showViewer( AGENT_PROVIDER& aProvider )
{
    AGENT_PROJECT* project = m_host->Sessions().Current();

    if( !project )
        return;

    ensureViewer( aProvider );

    auto it = m_viewers.find( { aProvider.Name().ToStdString(), project } );

    if( it == m_viewers.end() )
        return;

    it->second->Show();
    it->second->Raise();
    setDot( aProvider.Name().ToStdString(), GetBackgroundColour() );
}


void HUB_FRAME::registerTestHooks()
{
    // UI introspection for the end-to-end tests, never registered otherwise.
    const char* hooks = std::getenv( "AGENTIC_TEST_HOOKS" );

    if( !hooks || std::string( hooks ) != "1" )
        return;

    m_host->Tools().Add(
            { "debug_ui_state", "Test hook: what the hub and the viewers show.", "debug",
              JSON{ { "type", "object" }, { "properties", JSON::object() } }, [this]( const JSON&, TOOL_CONTEXT& )
              {
                  JSON activity = JSON::array();

                  // The row of this call is added after it returns, so it is not listed.
                  for( long i = 0; i < m_activity->GetItemCount(); ++i )
                  {
                      activity.push_back( { { "tool", m_activity->GetItemText( i, 1 ).ToStdString( wxConvUTF8 ) },
                                            { "result", m_activity->GetItemText( i, 2 ).ToStdString( wxConvUTF8 ) } } );
                  }

                  JSON projects = JSON::array();

                  for( unsigned i = 0; i < m_projects->GetCount(); ++i )
                      projects.push_back( m_projects->GetString( i ).ToStdString( wxConvUTF8 ) );

                  JSON                                       files = JSON::array();
                  std::function<void( const wxTreeItemId& )> walk = [&]( const wxTreeItemId& aItem )
                  {
                      wxTreeItemIdValue cookie;

                      for( wxTreeItemId child = m_files->GetFirstChild( aItem, cookie ); child.IsOk();
                           child = m_files->GetNextChild( aItem, cookie ) )
                      {
                          if( auto* data = dynamic_cast<PATH_DATA*>( m_files->GetItemData( child ) ) )
                              files.push_back( data->Path() );

                          walk( child );
                      }
                  };

                  if( m_files->GetRootItem().IsOk() )
                      walk( m_files->GetRootItem() );

                  JSON viewers = JSON::array();

                  for( const auto& [key, viewer] : m_viewers )
                      viewers.push_back( viewer->UiState() );

                  JSON dots = JSON::object();

                  for( const auto& [group, button] : m_buttons )
                  {
                      wxColour colour = button.dot->GetBackgroundColour();
                      dots[group] = colour == GetBackgroundColour() ? "none" : colour == *wxRED ? "red" : "green";
                  }

                  return TOOL_RESULT::Data( JSON{ { "activity", activity },
                                                  { "projects", projects },
                                                  { "files", files },
                                                  { "viewers", viewers },
                                                  { "dots", dots },
                                                  { "paused", m_host->IsPaused() } } );
              } } );
}

} // namespace AGENTIC
