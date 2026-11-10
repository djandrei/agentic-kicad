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
#include <agentic/core/agentic_version.h>
#include <agentic/core/provider.h>

#include <build_version.h>
#include <kiface_ids.h>
#include <kiway.h>
#include <ki_exception.h>


namespace AGENTIC
{

namespace
{

    const JSON EMPTY_OBJECT_SCHEMA =
            JSON{ { "type", "object" }, { "properties", JSON::object() }, { "additionalProperties", false } };

} // namespace


AGENT_HOST::AGENT_HOST( MODE aMode, KIWAY* aKiway, EXECUTOR aExecutor ) :
        m_mode( aMode )
{
    m_server = std::make_unique<MCP_SERVER>( SERVER_INFO{ ServerName(), ServerVersion(), Instructions() }, m_tools,
                                             m_resources, m_prompts, std::move( aExecutor ) );

    m_server->SetToolGate(
            [this]( const std::string& aName ) -> std::optional<std::string>
            {
                if( m_paused && aName != "status" && aName != "version" )
                    return std::string( "The person watching paused the agent. Wait and retry, or call "
                                        "'status' to see when the agent is resumed." );

                return std::nullopt;
            } );

    registerCoreTools();

    if( aKiway )
        findProviders( aKiway );
}


AGENT_HOST::~AGENT_HOST() = default;


void AGENT_HOST::SetPaused( bool aPaused )
{
    m_paused = aPaused;
}


std::string AGENT_HOST::Instructions()
{
    return "This server drives KiCad, the electronics design suite. A project is a folder with a "
           ".kicad_pro file. Every KiCad program has a tool group: schematic (sch_*), symbol editor "
           "(sym_*), PCB (pcb_*), footprint editor (fp_*), 3D (view3d_*), simulator (sim_*), Gerber "
           "viewer (gerber_*), drawing sheets (ds_*), calculator (calc_*) and footprint assignment "
           "(cvpcb_*). Lengths are millimetres, angles degrees, Y points down. Changes are made "
           "through structured tools, never by writing KiCad files; each change is saved at once "
           "and can be undone with 'undo'. Start with 'status' and 'project_open'. Check your "
           "work with ERC/DRC and captures before moving on.";
}


void AGENT_HOST::findProviders( KIWAY* aKiway )
{
    // clang-format off
    static const std::vector<std::pair<KIWAY::FACE_T, const char*>> faces = {
        { KIWAY::FACE_SCH, "eeschema" },
        { KIWAY::FACE_PCB, "pcbnew" },
        { KIWAY::FACE_CVPCB, "cvpcb" },
        { KIWAY::FACE_GERBVIEW, "gerbview" },
        { KIWAY::FACE_PL_EDITOR, "pl_editor" },
        { KIWAY::FACE_PCB_CALCULATOR, "pcb_calculator" }
    };
    // clang-format on

    for( const auto& [face, name] : faces )
    {
        PROVIDER_INFO info{ name, nullptr, "" };

        try
        {
            if( KIFACE* kiface = aKiway->KiFACE( face, true ) )
                info.provider = static_cast<AGENT_PROVIDER*>( kiface->IfaceOrAddress( KIFACE_AGENT_PROVIDER ) );
            else
                info.error = "kiface not available";
        }
        catch( const IO_ERROR& e )
        {
            info.error = e.What().ToStdString();
        }

        m_providers.push_back( info );
    }
}


void AGENT_HOST::registerCoreTools()
{
    m_tools.Add( { "version", "Versions of this server, of KiCad and of the MCP protocol in use.", "project",
                   EMPTY_OBJECT_SCHEMA, [this]( const JSON&, TOOL_CONTEXT& )
                   {
                       return TOOL_RESULT::Data( JSON{ { "server", ServerName() },
                                                       { "serverVersion", ServerVersion() },
                                                       { "kicadVersion", GetBuildVersion().ToStdString() },
                                                       { "protocolVersion", m_server->ProtocolVersion() } } );
                   } } );

    m_tools.Add( { "status",
                   "State of the server: mode (headless or viewer), whether the person watching paused "
                   "the agent, the connected client and the KiCad programs available.",
                   "project", EMPTY_OBJECT_SCHEMA, [this]( const JSON&, TOOL_CONTEXT& )
                   {
                       JSON providers = JSON::array();

                       for( const PROVIDER_INFO& info : m_providers )
                       {
                           JSON entry{ { "kiface", info.face }, { "available", info.provider != nullptr } };

                           if( info.provider )
                               entry["group"] = info.provider->Name().ToStdString();

                           if( !info.error.empty() )
                               entry["error"] = info.error;

                           providers.push_back( entry );
                       }

                       return TOOL_RESULT::Data( JSON{ { "mode", m_mode == MODE::VIEWER ? "viewer" : "headless" },
                                                       { "paused", m_paused },
                                                       { "client", m_server->ClientName() },
                                                       { "providers", providers } } );
                   } } );
}

} // namespace AGENTIC
