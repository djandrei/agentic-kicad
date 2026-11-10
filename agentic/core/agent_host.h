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

#include <memory>
#include <string>
#include <vector>

#include <agentic/protocol/mcp_server.h>

class AGENT_PROVIDER;
class KIWAY;

namespace AGENTIC
{

/**
 * Everything an agent-mode process has once: the registries, the MCP server, the core tools,
 * the providers found in the kifaces and the Pause state.  The transports and the event loop
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

    /// The text handed to clients in the initialize result.
    static std::string Instructions();

private:
    void findProviders( KIWAY* aKiway );
    void registerCoreTools();

    MODE                        m_mode;
    TOOL_REGISTRY               m_tools;
    RESOURCE_REGISTRY           m_resources;
    PROMPT_REGISTRY             m_prompts;
    std::unique_ptr<MCP_SERVER> m_server;
    bool                        m_paused = false;
    std::vector<PROVIDER_INFO>  m_providers;
};

} // namespace AGENTIC

#endif // AGENTIC_AGENT_HOST_H
