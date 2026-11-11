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
#include <agentic/viewer_shell/agent_viewer.h>

#include <paths.h>

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>


namespace AGENTIC
{

STATUS_ROW::STATUS_ROW( wxWindow* aParent, AGENT_SERVICES& aServices, bool aWithViewOnly ) :
        wxPanel( aParent ),
        m_services( aServices )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxHORIZONTAL );

    m_pause = new wxToggleButton( this, wxID_ANY, _( "Pause" ) );
    m_pause->SetToolTip( _( "Pause the agent: its calls fail until you resume" ) );
    m_pause->SetValue( m_services.IsPaused() );
    m_pause->Bind( wxEVT_TOGGLEBUTTON,
                   [this]( wxCommandEvent& )
                   {
                       m_services.SetPaused( m_pause->GetValue() );
                   } );
    sizer->Add( m_pause, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2 );

    if( aWithViewOnly )
    {
        m_viewOnly = new wxToggleButton( this, wxID_ANY, _( "View only" ) );
        m_viewOnly->SetToolTip( _( "Hide the docks (F11)" ) );
        sizer->Add( m_viewOnly, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2 );
    }

    m_text = new wxStaticText( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END );
    sizer->Add( m_text, 1, wxALL | wxALIGN_CENTER_VERTICAL, 4 );
    SetSizer( sizer );
}


void STATUS_ROW::SetText( const wxString& aText )
{
    m_text->SetLabel( aText );
}


void STATUS_ROW::SyncPause( bool aPaused )
{
    m_pause->SetValue( aPaused );
    m_pause->SetLabel( aPaused ? _( "Resume" ) : _( "Pause" ) );
}


AGENT_VIEWER::AGENT_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject, const std::string& aProgram,
                            const wxString& aTitle ) :
        wxFrame( nullptr, wxID_ANY, aTitle + wxS( " - " ) + wxString::FromUTF8( aProject.Name() ), wxDefaultPosition,
                 wxSize( 1200, 800 ) ),
        m_services( aServices ),
        m_project( aProject ),
        m_program( aProgram )
{
    m_aui.SetManagedWindow( this );

    m_changes = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 320, 200 ), wxLC_REPORT | wxLC_SINGLE_SEL );
    m_changes->AppendColumn( _( "Change" ), wxLIST_FORMAT_LEFT, 300 );

    m_messages = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 600, 120 ), wxLC_REPORT | wxLC_SINGLE_SEL );
    m_messages->AppendColumn( _( "Severity" ), wxLIST_FORMAT_LEFT, 80 );
    m_messages->AppendColumn( _( "Message" ), wxLIST_FORMAT_LEFT, 600 );
    m_messages->AppendColumn( _( "Tool" ), wxLIST_FORMAT_LEFT, 140 );

    m_status = new STATUS_ROW( this, aServices, true );
    m_status->SyncPause( aServices.IsPaused() );

    m_aui.AddPane( m_changes, wxAuiPaneInfo()
                                      .Name( wxS( "changes" ) )
                                      .Caption( _( "Changes" ) )
                                      .Right()
                                      .BestSize( 320, -1 )
                                      .CloseButton( false ) );
    m_aui.AddPane( m_messages, wxAuiPaneInfo()
                                       .Name( wxS( "messages" ) )
                                       .Caption( _( "Messages" ) )
                                       .Bottom()
                                       .BestSize( -1, 140 )
                                       .CloseButton( false ) );
    m_aui.AddPane( m_status, wxAuiPaneInfo()
                                     .Name( wxS( "status" ) )
                                     .Bottom()
                                     .Layer( 1 )
                                     .CaptionVisible( false )
                                     .PaneBorder( false )
                                     .Gripper( false )
                                     .CloseButton( false )
                                     .MinSize( -1, m_status->GetBestSize().y ) );

    m_status->ViewOnlyButton()->Bind( wxEVT_TOGGLEBUTTON,
                                      [this]( wxCommandEvent& )
                                      {
                                          toggleViewOnly();
                                      } );

    wxAcceleratorEntry entries[1];
    const int          viewOnlyId = wxNewId();
    entries[0].Set( wxACCEL_NORMAL, WXK_F11, viewOnlyId );
    SetAcceleratorTable( wxAcceleratorTable( 1, entries ) );
    Bind(
            wxEVT_MENU,
            [this]( wxCommandEvent& )
            {
                m_status->ViewOnlyButton()->SetValue( !m_status->ViewOnlyButton()->GetValue() );
                toggleViewOnly();
            },
            viewOnlyId );

    Bind( wxEVT_CLOSE_WINDOW, &AGENT_VIEWER::onClose, this );

    m_services.Observers().Add( this );
    m_lastLogSeq = m_project.Log().LastSeq();
}


AGENT_VIEWER::~AGENT_VIEWER()
{
    m_services.Observers().Remove( this );

    // Keep the layout with the docks shown, even when closed in View only.
    wxString   perspective = m_fullLayout.IsEmpty() ? m_aui.SavePerspective() : m_fullLayout;
    wxFileName fn( layoutFile() );
    fn.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );
    wxFFile out( fn.GetFullPath(), wxS( "w" ) );

    if( out.IsOpened() )
        out.Write( perspective );

    m_aui.UnInit();
}


wxString AGENT_VIEWER::layoutFile() const
{
    wxFileName fn( PATHS::GetUserSettingsPath(), wxEmptyString );
    fn.AppendDir( wxS( "agentic" ) );
    fn.SetFullName( wxString::FromUTF8( m_program ) + wxS( "-viewer.layout" ) );
    return fn.GetFullPath();
}


void AGENT_VIEWER::SetCentralView( wxWindow* aView )
{
    m_aui.AddPane( aView, wxAuiPaneInfo().Name( wxS( "view" ) ).CenterPane() );
    m_aui.Update();
}


void AGENT_VIEWER::AddDock( wxWindow* aWindow, const wxString& aName, const wxString& aCaption, int aSide )
{
    wxAuiPaneInfo info = wxAuiPaneInfo().Name( aName ).Caption( aCaption ).CloseButton( false );

    if( aSide == wxLEFT )
        info.Left().BestSize( 220, -1 );
    else if( aSide == wxBOTTOM )
        info.Bottom().BestSize( -1, 140 );
    else
        info.Right().BestSize( 320, -1 );

    m_aui.AddPane( aWindow, info );
    m_programDocks.push_back( aName );
}


void AGENT_VIEWER::FinishLayout()
{
    // Restore the dock layout saved for this program.
    wxFFile  layout( layoutFile(), wxS( "r" ) );
    wxString saved;

    if( layout.IsOpened() && layout.ReadAll( &saved ) && !saved.IsEmpty() )
        m_aui.LoadPerspective( saved, false );

    m_aui.Update();
}


void AGENT_VIEWER::Retire()
{
    m_retiring = true;

    // Destroy() only schedules the deletion, and the project may be freed before it runs: let go
    // of its documents now (each viewer detaches as when a document closes), stop observing, and
    // stop handling events (timers, paints) that would read the project.
    for( DOC_SESSION* session : m_project.Sessions() )
        OnDocumentClosing( *session );

    m_services.Observers().Remove( this );
    SetEvtHandlerEnabled( false );
    Hide();
    Destroy();
}


void AGENT_VIEWER::onClose( wxCloseEvent& aEvent )
{
    if( m_retiring || !aEvent.CanVeto() )
    {
        Destroy();
        return;
    }

    // D6: closing a viewer hides it; the hub button shows it again.
    aEvent.Veto();
    Hide();
}


void AGENT_VIEWER::toggleViewOnly()
{
    const bool viewOnly = m_status->ViewOnlyButton()->GetValue();

    if( viewOnly )
    {
        m_fullLayout = m_aui.SavePerspective();
        m_aui.GetPane( wxS( "changes" ) ).Hide();
        m_aui.GetPane( wxS( "messages" ) ).Hide();

        for( const wxString& dock : m_programDocks )
            m_aui.GetPane( dock ).Hide();
    }
    else
    {
        m_aui.GetPane( wxS( "changes" ) ).Show();
        m_aui.GetPane( wxS( "messages" ) ).Show();

        for( const wxString& dock : m_programDocks )
            m_aui.GetPane( dock ).Show();
        m_fullLayout.Clear();
    }

    m_aui.Update();
}


bool AGENT_VIEWER::shows( const DOC_SESSION& aSession ) const
{
    return &aSession.Project() == &m_project && aSession.Program() == m_program;
}


void AGENT_VIEWER::appendChange( const nlohmann::json& aChange )
{
    wxString text = wxString::FromUTF8( aChange.value( "summary", aChange.dump() ) );
    long     row = m_changes->InsertItem( m_changes->GetItemCount(), text );
    m_changes->EnsureVisible( row );
}


void AGENT_VIEWER::refillChanges()
{
    m_changes->DeleteAllItems();

    for( DOC_SESSION* session : m_project.Sessions() )
    {
        if( !shows( *session ) )
            continue;

        for( const nlohmann::json& change : session->Changes() )
            appendChange( change );
    }
}


void AGENT_VIEWER::refreshMessages()
{
    for( const LOG_RECORD& record : m_project.Log().Since( m_lastLogSeq, 500 ) )
    {
        long row = m_messages->InsertItem( m_messages->GetItemCount(), wxString::FromUTF8( record.severity ) );
        m_messages->SetItem( row, 1, wxString::FromUTF8( record.text ) );
        m_messages->SetItem( row, 2, wxString::FromUTF8( record.tool ) );
        m_messages->EnsureVisible( row );
        m_lastLogSeq = record.seq;
    }
}


void AGENT_VIEWER::OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange )
{
    if( shows( aSession ) )
        appendChange( aChange );
}


void AGENT_VIEWER::OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                              const std::string& aSummary )
{
    refreshMessages();
    m_status->SetText( wxString::Format( wxS( "%s %s (%.0f ms)" ), wxString::FromUTF8( aTool ),
                                         aOk ? _( "done" ) : _( "failed" ), aMillis ) );
}


void AGENT_VIEWER::OnPauseChanged( bool aPaused )
{
    m_status->SyncPause( aPaused );
}


nlohmann::json AGENT_VIEWER::UiState() const
{
    nlohmann::json changes = nlohmann::json::array();
    nlohmann::json messages = nlohmann::json::array();

    for( long i = 0; i < m_changes->GetItemCount(); ++i )
        changes.push_back( m_changes->GetItemText( i ).ToStdString( wxConvUTF8 ) );

    for( long i = 0; i < m_messages->GetItemCount(); ++i )
        messages.push_back( m_messages->GetItemText( i, 1 ).ToStdString( wxConvUTF8 ) );

    return nlohmann::json{ { "program", m_program },
                           { "project", m_project.Name() },
                           { "shown", IsShown() },
                           { "changes", changes },
                           { "messages", messages } };
}

} // namespace AGENTIC
