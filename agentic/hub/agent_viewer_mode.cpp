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

#include <agentic/hub/agent_viewer_mode.h>
#include <agentic/core/agent_host.h>
#include <agentic/hub/hub_frame.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>

#include <wx/crt.h>
#include <wx/timer.h>


namespace AGENTIC
{

namespace
{

    std::atomic<bool> g_stopRequested{ false };


    extern "C" void viewerSignalHandler( int )
    {
        // Only a flag here; the GUI thread acts on it between calls.
        g_stopRequested = true;
    }


    /**
     * SIGTERM and SIGINT close the hub as its close box does, on the GUI thread, so a call in
     * progress finishes first (tools run on the GUI thread) and the process exits normally.
     */
    class SIGNAL_WATCH : public wxTimer
    {
    public:
        explicit SIGNAL_WATCH( wxFrame* aHub ) :
                m_hub( aHub )
        {
        }

        void Notify() override
        {
            if( !g_stopRequested )
                return;

            Stop();
            m_hub->Close( true );
        }

    private:
        wxFrame* m_hub;
    };


    std::unique_ptr<SIGNAL_WATCH> g_signalWatch;

} // namespace

wxFrame* StartAgentViewerMode( KIWAY* aKiway, const AGENT_LAUNCH_OPTIONS& aOptions )
{
    HUB_FRAME* hub = new HUB_FRAME( aKiway );

    if( !hub->StartServer( aOptions.port ) )
    {
        hub->Destroy();
        return nullptr;
    }

    if( !aOptions.project.IsEmpty() )
    {
        try
        {
            hub->Host().Sessions().Open( std::filesystem::path( aOptions.project.ToStdString( wxConvUTF8 ) ), false );
        }
        catch( const std::exception& e )
        {
            wxFprintf( stderr, "Cannot open the project %s: %s\n", aOptions.project, e.what() );
        }
    }

    std::signal( SIGINT, viewerSignalHandler );
#ifdef SIGTERM
    std::signal( SIGTERM, viewerSignalHandler );
#endif

    g_signalWatch = std::make_unique<SIGNAL_WATCH>( hub );
    g_signalWatch->Start( 100 );

    // The watch must not outlive the hub it closes.
    hub->Bind( wxEVT_DESTROY,
               [hub]( wxWindowDestroyEvent& aEvent )
               {
                   if( aEvent.GetEventObject() == hub && g_signalWatch )
                       g_signalWatch->Stop();

                   aEvent.Skip();
               } );

    hub->Show();
    return hub;
}

} // namespace AGENTIC
