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
#include <agentic/core/core_tools.h>
#include <agentic/core/library_tools.h>
#include <agentic/core/pcm_tools.h>
#include <agentic/docs/docs_tools.h>
#include <agentic/core/message_log.h>
#include <agentic/core/provider.h>

#include <build_version.h>
#include <kiface_ids.h>
#include <kiway.h>
#include <ki_exception.h>

#include <chrono>
#include <set>


namespace AGENTIC
{

namespace
{

    const JSON EMPTY_OBJECT_SCHEMA =
            JSON{ { "type", "object" }, { "properties", JSON::object() }, { "additionalProperties", false } };

} // namespace


AGENT_HOST::AGENT_HOST( MODE aMode, KIWAY* aKiway, EXECUTOR aExecutor ) :
        m_mode( aMode ),
        m_sessions( m_observers ),
        m_services( m_tools, m_resources, m_prompts, m_sessions, m_observers, aMode == MODE::VIEWER )
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

    m_server->SetToolWrapper(
            [this]( const std::string& aName, const JSON& aArgs, TOOL_CONTEXT&,
                    const std::function<TOOL_RESULT()>& aRun )
            {
                return runTool( aName, aArgs, aRun );
            } );

    m_services.SetPauseControl(
            [this]()
            {
                return m_paused;
            },
            [this]( bool aPaused )
            {
                SetPaused( aPaused );
            } );

    InstallAgentLogTarget();
    registerCoreTools();
    RegisterProjectTools( m_services );
    RegisterLibraryTools( m_services );
    RegisterDocsTools( m_services, Instructions() );
    RegisterPcmTools( m_services );

    if( aKiway )
        findProviders( aKiway );
}


AGENT_HOST::~AGENT_HOST()
{
    // Close the projects (and their sessions) while the providers' code is still loaded.
    for( AGENT_PROJECT* project : m_sessions.Projects() )
        m_sessions.Close( *project );
}


void AGENT_HOST::SetPaused( bool aPaused )
{
    if( m_paused == aPaused )
        return;

    m_paused = aPaused;

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnPauseChanged( aPaused );
            } );
}


void AGENT_HOST::AddProvider( AGENT_PROVIDER* aProvider, const std::string& aFace )
{
    m_providers.push_back( PROVIDER_INFO{ aFace, aProvider, "" } );
    aProvider->Register( m_services );
}


AGENT_PROVIDER* AGENT_HOST::ProviderForGroup( const std::string& aGroup ) const
{
    for( const PROVIDER_INFO& info : m_providers )
    {
        if( info.provider && info.provider->Name().ToStdString() == aGroup )
            return info.provider;
    }

    return nullptr;
}


TOOL_RESULT AGENT_HOST::runTool( const std::string& aName, const JSON& aArgs, const std::function<TOOL_RESULT()>& aRun )
{
    const TOOL_DEF* tool = m_tools.Find( aName );

    if( tool && m_providerUse )
    {
        if( AGENT_PROVIDER* provider = ProviderForGroup( tool->group ) )
            m_providerUse( *provider );
    }

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnToolStart( aName, aArgs );
            } );

    const auto    started = std::chrono::steady_clock::now();
    CALL_MESSAGES messages( aName );
    TOOL_RESULT   result;

    m_services.SetMessages( &messages );
    result = aRun();
    m_services.SetMessages( nullptr );

    // The current project's settings choose the visible tool groups.
    if( AGENT_PROJECT* current = m_sessions.Current() )
    {
        std::set<std::string> disabled;

        for( const JSON& group : current->Settings().value( "disabledGroups", JSON::array() ) )
        {
            if( group.is_string() && group.get<std::string>() != "project" )
                disabled.insert( group.get<std::string>() );
        }

        if( disabled != m_server->DisabledGroups() )
            m_server->SetDisabledGroups( disabled );
    }

    // The call's messages go to the log of the project it used and back to the agent (D11).
    AGENT_PROJECT* project = m_sessions.Current();
    JSON           records = messages.Flush( project ? project->Log() : m_hostLog );

    if( !records.empty() )
    {
        if( result.isError )
        {
            std::string text = "\nKiCad reported:";

            for( const JSON& record : records )
                text += "\n  " + record["severity"].get<std::string>() + ": " + record["text"].get<std::string>();

            result.content.push_back( TextContent( text ) );

            if( result.structuredContent.is_object() )
                result.structuredContent["messages"] = records;
        }
        else if( result.structuredContent.is_object() )
        {
            // Keep other content (images); refresh the text copy of the structured result.
            const std::string oldText = result.structuredContent.dump( 2, ' ', false, JSON::error_handler_t::replace );
            result.structuredContent["messages"] = records;

            for( JSON& item : result.content )
            {
                if( item.value( "type", "" ) == "text" && item.value( "text", "" ) == oldText )
                    item["text"] = result.structuredContent.dump( 2, ' ', false, JSON::error_handler_t::replace );
            }
        }
    }

    const double millis =
            std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();

    std::string summary;

    if( result.isError && !result.content.empty() && result.content[0].contains( "text" ) )
        summary = result.content[0]["text"].get<std::string>();

    // The call itself is a record too, so log_get shows what ran (after the call's own
    // records, which the result already carries).
    if( project && aName != "log_get" )
    {
        LOG_RECORD call;
        call.severity = "call";
        call.tool = aName;
        call.text = aName + ( result.isError ? " failed" : " ok" ) + ( summary.empty() ? "" : ": " + summary );
        project->Log().Add( call );
    }

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnToolEnd( aName, aArgs, !result.isError, millis, summary );
            } );

    return result;
}


std::string AGENT_HOST::Instructions()
{
    return "This server drives KiCad, the electronics design suite. A project is a folder with a "
           ".kicad_pro file: open one with 'project_open' or make one with 'project_create'. Each KiCad "
           "program available adds a tool group named by its prefix (sch_ for the schematic, pcb_ for "
           "the board, and so on); 'status' lists the programs this server has. Lengths are "
           "millimetres (or strings with units such as \"50mil\"), angles degrees, Y points down. "
           "Change designs only through the structured tools, never by writing KiCad files; every "
           "change is saved at once and 'undo' takes back the last one. Project files that are not "
           "design files (notes, scripts) are read and written with the file_ tools. Check your work "
           "(ERC, DRC, captures) before moving on.";
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

        if( info.provider )
        {
            info.provider->Register( m_services );

            for( AGENT_PROVIDER* companion : info.provider->Companions() )
            {
                m_providers.push_back( PROVIDER_INFO{ name, companion, "" } );
                companion->Register( m_services );
            }
        }
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
                   "the agent, the open projects, the connected client and the KiCad programs available.",
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

                       JSON projects = JSON::array();

                       for( AGENT_PROJECT* project : m_sessions.Projects() )
                       {
                           projects.push_back( { { "name", project->Name() },
                                                 { "root", project->Root().string() },
                                                 { "current", project == m_sessions.Current() } } );
                       }

                       JSON out{ { "mode", m_mode == MODE::VIEWER ? "viewer" : "headless" },
                                 { "paused", m_paused },
                                 { "projects", projects },
                                 { "client", m_server->ClientName() },
                                 { "providers", providers } };

                       for( const auto& [name, status] : m_services.Status() )
                           out[name] = status();

                       return TOOL_RESULT::Data( out );
                   } } );
}

} // namespace AGENTIC
