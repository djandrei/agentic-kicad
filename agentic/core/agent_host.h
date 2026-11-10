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

#ifndef AGENTIC_AGENT_HOST_H
#define AGENTIC_AGENT_HOST_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <agentic/core/observer.h>
#include <agentic/core/services.h>
#include <agentic/core/session_manager.h>
#include <agentic/protocol/mcp_server.h>

class AGENT_PROVIDER;
class KIWAY;

namespace AGENTIC
{

/**
 * Everything an agent-mode process has once: the registries, the MCP server, the open
 * projects, the observers, the core tools, the providers found in the kifaces and the Pause
 * state.  The transports and the event loop
 * belong to the launcher (kicad-cli mcp, or kicad in viewer mode).
 *
 * All methods except Server().Submit() run on the executor thread.
 */
class AGENT_HOST
{
public:
    enum class MODE
    {
        HEADLESS,
        VIEWER
    };

    /**
     * @param aKiway used to find the providers; may be nullptr (tests), then there are none.
     */
    AGENT_HOST( MODE aMode, KIWAY* aKiway, EXECUTOR aExecutor );
    ~AGENT_HOST();

    MCP_SERVER&        Server() { return *m_server; }
    TOOL_REGISTRY&     Tools() { return m_tools; }
    RESOURCE_REGISTRY& Resources() { return m_resources; }
    PROMPT_REGISTRY&   Prompts() { return m_prompts; }

    MODE Mode() const { return m_mode; }

    SESSION_MANAGER& Sessions() { return m_sessions; }
    OBSERVERS&       Observers() { return m_observers; }
    AGENT_SERVICES&  Services() { return m_services; }

    /// While paused every tool call fails; status and version still answer.
    void SetPaused( bool aPaused );
    bool IsPaused() const { return m_paused; }

    struct PROVIDER_INFO
    {
        std::string     face;     ///< Kiface name, e.g. "pcbnew".
        AGENT_PROVIDER* provider; ///< nullptr when the kiface has none or failed to load.
        std::string     error;    ///< Why the kiface could not be loaded, if it could not.
    };

    const std::vector<PROVIDER_INFO>& Providers() const { return m_providers; }

    /// Add a provider that does not come from a kiface (tests) and register its tools.
    void AddProvider( AGENT_PROVIDER* aProvider, const std::string& aFace = "test" );

    /// The provider whose tool group is \a aGroup, or nullptr.
    AGENT_PROVIDER* ProviderForGroup( const std::string& aGroup ) const;

    /**
     * Called before a tool of a provider runs (D6: viewer mode opens the program's viewer on
     * first use).  Set by the hub.
     */
    using PROVIDER_USE = std::function<void( AGENT_PROVIDER& aProvider )>;

    void SetProviderUse( PROVIDER_USE aUse ) { m_providerUse = std::move( aUse ); }

    /// The text handed to clients in the initialize result.
    static std::string Instructions();

private:
    void        findProviders( KIWAY* aKiway );
    void        registerCoreTools();
    TOOL_RESULT runTool( const std::string& aName, const JSON& aArgs, const std::function<TOOL_RESULT()>& aRun );

    MODE                        m_mode;
    TOOL_REGISTRY               m_tools;
    RESOURCE_REGISTRY           m_resources;
    PROMPT_REGISTRY             m_prompts;
    OBSERVERS                   m_observers;
    MESSAGE_LOG                 m_hostLog; ///< Messages of calls made with no project open.
    SESSION_MANAGER             m_sessions;
    AGENT_SERVICES              m_services;
    std::unique_ptr<MCP_SERVER> m_server;
    bool                        m_paused = false;
    std::vector<PROVIDER_INFO>  m_providers;
    PROVIDER_USE                m_providerUse;
};

} // namespace AGENTIC

#endif // AGENTIC_AGENT_HOST_H
