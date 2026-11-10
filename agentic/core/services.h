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


#ifndef AGENTIC_SERVICES_H
#define AGENTIC_SERVICES_H

#include <functional>
#include <string>

#include <agentic/core/observer.h>
#include <agentic/core/session_manager.h>
#include <agentic/protocol/registry.h>

class AGENT_PROVIDER;
class REPORTER;

namespace AGENTIC
{

class CALL_MESSAGES;

/**
 * What the host gives every provider: the registries to add tools to, the open projects, the
 * observers, and the state of the call being served.  Owned by AGENT_HOST; providers keep a
 * reference for the life of the process.
 */
class AGENT_SERVICES
{
public:
    AGENT_SERVICES( TOOL_REGISTRY& aTools, RESOURCE_REGISTRY& aResources, PROMPT_REGISTRY& aPrompts,
                    SESSION_MANAGER& aSessions, OBSERVERS& aObservers, bool aViewerMode ) :
            m_tools( aTools ),
            m_resources( aResources ),
            m_prompts( aPrompts ),
            m_sessions( aSessions ),
            m_observers( aObservers ),
            m_viewerMode( aViewerMode )
    {
    }

    TOOL_REGISTRY&     Tools() { return m_tools; }
    RESOURCE_REGISTRY& Resources() { return m_resources; }
    PROMPT_REGISTRY&   Prompts() { return m_prompts; }
    SESSION_MANAGER&   Sessions() { return m_sessions; }
    OBSERVERS&         Observers() { return m_observers; }

    /// True in viewer mode (kicad), false headless (kicad-cli).
    bool ViewerMode() const { return m_viewerMode; }

    /// The messages of the call being served; engine calls report into it (D11).
    CALL_MESSAGES* Messages() const { return m_messages; }

    /// A REPORTER for engine calls: the current call's messages, or one that drops everything.
    REPORTER& Reporter();

    /// Set by the host around each call.
    void SetMessages( CALL_MESSAGES* aMessages ) { m_messages = aMessages; }

    /// Progress forwarded to the hub and viewer status bars (D10); set by the hub.
    using PROGRESS_SINK = std::function<void( double aFraction, const std::string& aMessage )>;

    const PROGRESS_SINK& ProgressSink() const { return m_progressSink; }
    void                 SetProgressSink( PROGRESS_SINK aSink ) { m_progressSink = std::move( aSink ); }

    /// The Pause control (owned by the host); viewers show and switch it.
    bool IsPaused() const { return m_isPaused ? m_isPaused() : false; }
    void SetPaused( bool aPaused )
    {
        if( m_setPaused )
            m_setPaused( aPaused );
    }

    void SetPauseControl( std::function<bool()> aIsPaused, std::function<void( bool )> aSetPaused )
    {
        m_isPaused = std::move( aIsPaused );
        m_setPaused = std::move( aSetPaused );
    }

private:
    TOOL_REGISTRY&     m_tools;
    RESOURCE_REGISTRY& m_resources;
    PROMPT_REGISTRY&   m_prompts;
    SESSION_MANAGER&   m_sessions;
    OBSERVERS&         m_observers;
    bool               m_viewerMode;
    CALL_MESSAGES*     m_messages = nullptr;
    PROGRESS_SINK      m_progressSink;

    std::function<bool()>       m_isPaused;
    std::function<void( bool )> m_setPaused;
};

} // namespace AGENTIC

#endif // AGENTIC_SERVICES_H
