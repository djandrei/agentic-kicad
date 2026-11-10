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


#ifndef AGENTIC_HUB_FRAME_H
#define AGENTIC_HUB_FRAME_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <wx/frame.h>

#include <agentic/core/observer.h>

class AGENT_PROVIDER;
class KIWAY;
class wxButton;
class wxListBox;
class wxListCtrl;
class wxTreeCtrl;
class wxTreeItemId;

namespace AGENTIC
{

class AGENT_HOST;
class AGENT_VIEWER;
class HTTP_TRANSPORT;
class STATUS_ROW;

/**
 * The hub window of viewer mode.  It owns the agent host and the HTTP transport, and runs
 * tool calls on the GUI thread through CallAfter on itself, so no call outlives the frame.
 *
 * It shows the open projects, the current project's files, the Activity list of calls, one
 * button per program (with a dot while that program's viewer is hidden and active: red after
 * a failed call), and the status row with Pause, the server URL and the last call.  It opens
 * each program's viewer on the first call of that program in a project (D6).
 */
class HUB_FRAME : public wxFrame, public SESSION_OBSERVER
{
public:
    explicit HUB_FRAME( KIWAY* aKiway );
    ~HUB_FRAME() override;

    /// Start the HTTP server; prints the connection details on stderr.  False on failure.
    bool StartServer( int aPort );

    AGENT_HOST& Host() { return *m_host; }

    // SESSION_OBSERVER
    void OnProjectOpened( AGENT_PROJECT& aProject ) override;
    void OnProjectClosed( AGENT_PROJECT& aProject ) override;
    void OnFilesChanged( AGENT_PROJECT& aProject ) override;
    void OnToolStart( const std::string& aTool, const nlohmann::json& aArgs ) override;
    void OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                    const std::string& aSummary ) override;
    void OnPauseChanged( bool aPaused ) override;

private:
    struct PROGRAM_BUTTON
    {
        AGENT_PROVIDER* provider = nullptr;
        wxButton*       button = nullptr;
        wxWindow*       dot = nullptr;
    };

    void onClose( wxCloseEvent& aEvent );
    void rebuildProjects();
    void rebuildFiles();
    void ensureViewer( AGENT_PROVIDER& aProvider );
    void showViewer( AGENT_PROVIDER& aProvider );
    void setDot( const std::string& aGroup, const wxColour& aColour );
    void registerTestHooks();

    KIWAY*                                m_kiway;
    std::unique_ptr<AGENT_HOST>           m_host;
    std::unique_ptr<HTTP_TRANSPORT>       m_transport;
    wxListBox*                            m_projects;
    wxTreeCtrl*                           m_files;
    wxListCtrl*                           m_activity;
    STATUS_ROW*                           m_status;
    wxString                              m_url;
    std::map<std::string, PROGRAM_BUTTON> m_buttons; ///< By tool group.
    std::string                           m_runningGroup;
    std::set<std::string>                 m_expanded; ///< Expanded folders of the file tree.

    /// Viewers by (program, project); hidden viewers stay here until their project closes.
    std::map<std::pair<std::string, AGENT_PROJECT*>, AGENT_VIEWER*> m_viewers;
};

} // namespace AGENTIC

#endif // AGENTIC_HUB_FRAME_H
