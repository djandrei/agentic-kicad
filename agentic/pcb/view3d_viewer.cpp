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
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/view3d_tools.h>
#include <agentic/pcb/view3d_viewer.h>

#include <3d_canvas/board_adapter.h>
#include <3d_canvas/eda_3d_canvas.h>
#include <3d_rendering/track_ball.h>
#include <3d_viewer/eda_3d_viewer_settings.h>
#include <board.h>
#include <common_ogl/ogl_attr_list.h>
#include <project_pcb.h>
#include <settings/settings_manager.h>

#include <wx/timer.h>


namespace AGENTIC
{

namespace
{

    /// Debounce for reloading the 3D board after commits (stage 9).
    constexpr int RELOAD_DELAY_MS = 500;


    class VIEW3D_VIEWER : public AGENT_VIEWER
    {
    public:
        VIEW3D_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "view3d", _( "3D Viewer" ) ),
                m_camera( 2 * RANGE_SCALE_3D ),
                m_reloadTimer( this )
        {
            if( EDA_3D_VIEWER_SETTINGS* user = GetAppSettings<EDA_3D_VIEWER_SETTINGS>( "3d_viewer" ) )
                m_cfg.m_Render = user->m_Render;

            m_cfg.m_UseStackupColors = true;
            m_adapter.m_Cfg = &m_cfg;
            m_adapter.m_IsBoardView = false;

            m_canvas =
                    new EDA_3D_CANVAS( this, OGL_ATT_LIST::GetAttributesList( ANTIALIASING_MODE::AA_NONE ), m_adapter,
                                       m_camera, PROJECT_PCB::Get3DCacheManager( &aProject.KicadProject() ) );
            m_canvas->SetMinSize( wxSize( 700, 500 ) );
            SetCentralView( m_canvas );
            FinishLayout();

            // An owned timer sends its events to the owner.
            Bind(
                    wxEVT_TIMER,
                    [this]( wxTimerEvent& )
                    {
                        reload();
                    },
                    m_reloadTimer.GetId() );

            attach();
        }

        ~VIEW3D_VIEWER() override
        {
            m_reloadTimer.Stop();
            m_canvas->JoinBgWorker();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["boardLoaded"] = m_session != nullptr;
            state["reloads"] = m_reloads;
            state["view"] = m_view;
            state["reloadPending"] = m_reloadTimer.IsRunning();
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
            // The board is replaced; the renderer must not touch the old one.
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

            if( &aSession == m_session )
                m_reloadTimer.StartOnce( RELOAD_DELAY_MS );
        }

        void OnResults( DOC_SESSION& aSession, const nlohmann::json& aResults ) override
        {
            if( &aSession != m_session || aResults.value( "checker", "" ) != "view3d" )
                return;

            // The agent's camera is the reference: the view snaps back to it on each camera call.
            followAgent();
            m_reloadTimer.StartOnce( RELOAD_DELAY_MS );
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

            m_adapter.SetBoard( &m_session->Board() );
            reload();
            followAgent();
        }

        void detach()
        {
            m_reloadTimer.Stop();
            m_canvas->JoinBgWorker();
            m_session = nullptr;
        }

        void followAgent()
        {
            const VIEW3D_STATE& state = View3dState( Project() );
            View3dApplyOptions( m_adapter, state );
            View3dApplyCamera( m_camera, state );
            m_view = state.view;
            m_canvas->Request_refresh();
        }

        void reload()
        {
            if( !m_session )
                return;

            m_canvas->ReloadRequest( &m_session->Board(), PROJECT_PCB::Get3DCacheManager( &Project().KicadProject() ) );
            m_canvas->Request_refresh();
            m_reloads++;
        }

        EDA_3D_VIEWER_SETTINGS m_cfg;
        BOARD_ADAPTER          m_adapter;
        TRACK_BALL             m_camera;
        EDA_3D_CANVAS*         m_canvas = nullptr;
        BOARD_DOC_SESSION*     m_session = nullptr;
        wxTimer                m_reloadTimer;
        int                    m_reloads = 0;
        std::string            m_view;
    };

} // namespace


AGENT_VIEWER* CreateView3dViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new VIEW3D_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
