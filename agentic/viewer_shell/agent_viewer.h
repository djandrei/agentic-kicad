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


#ifndef AGENTIC_AGENT_VIEWER_H
#define AGENTIC_AGENT_VIEWER_H

#include <string>

#include <wx/aui/aui.h>
#include <wx/frame.h>

#include <agentic/core/observer.h>

class wxListCtrl;
class wxStaticText;
class wxToggleButton;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;

/**
 * The status row shared by the hub and the viewers: Pause, optionally "View only", and a
 * text field for the last call and progress.
 */
class STATUS_ROW : public wxPanel
{
public:
    STATUS_ROW( wxWindow* aParent, AGENT_SERVICES& aServices, bool aWithViewOnly );

    void SetText( const wxString& aText );
    void SyncPause( bool aPaused );

    wxToggleButton* ViewOnlyButton() const { return m_viewOnly; }

private:
    AGENT_SERVICES& m_services;
    wxToggleButton* m_pause;
    wxToggleButton* m_viewOnly = nullptr;
    wxStaticText*   m_text;
};


/**
 * Base of every program's viewer (D6): a top-level window with a central view, the Changes
 * dock (the session's commits) and the Messages dock (the project's log), and the status
 * row with Pause and View only (F11 hides the docks).  The dock layout is kept per program
 * in the KiCad settings folder.
 *
 * A viewer observes; it never changes a document.  Closing it hides it; the hub reopens it.
 */
class AGENT_VIEWER : public wxFrame, public SESSION_OBSERVER
{
public:
    AGENT_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject, const std::string& aProgram,
                  const wxString& aTitle );
    ~AGENT_VIEWER() override;

    AGENT_PROJECT&     Project() const { return m_project; }
    const std::string& Program() const { return m_program; }

    /// Put the program's view in the centre.  Called once by the subclass constructor.
    void SetCentralView( wxWindow* aView );

    /// Close for good (project closed, program exit); a user close only hides.
    void Retire();

    /// Rows of the Changes and Messages docks, for the test hooks.
    virtual nlohmann::json UiState() const;

    // SESSION_OBSERVER
    void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) override;
    void OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                    const std::string& aSummary ) override;
    void OnPauseChanged( bool aPaused ) override;

protected:
    /// True when \a aSession is one this viewer shows (default: same project and program).
    virtual bool shows( const DOC_SESSION& aSession ) const;

    AGENT_SERVICES& services() const { return m_services; }

    /// Fill the Changes dock from the sessions' histories (a reopened viewer).
    void refillChanges();

private:
    void     onClose( wxCloseEvent& aEvent );
    void     toggleViewOnly();
    void     appendChange( const nlohmann::json& aChange );
    void     refreshMessages();
    wxString layoutFile() const;

    AGENT_SERVICES& m_services;
    AGENT_PROJECT&  m_project;
    std::string     m_program;
    wxAuiManager    m_aui;
    wxListCtrl*     m_changes;
    wxListCtrl*     m_messages;
    STATUS_ROW*     m_status;
    wxString        m_fullLayout;
    uint64_t        m_lastLogSeq = 0;
    bool            m_retiring = false;
};

} // namespace AGENTIC

#endif // AGENTIC_AGENT_VIEWER_H
