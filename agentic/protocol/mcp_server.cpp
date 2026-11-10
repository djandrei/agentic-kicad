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

#include <agentic/protocol/mcp_server.h>
#include <agentic/protocol/json_rpc.h>
#include <agentic/protocol/schema.h>

#include <algorithm>
#include <chrono>
#include <exception>


namespace AGENTIC
{

namespace
{

    /// Raised inside dispatch() to produce a JSON-RPC error response.
    struct RPC_FAILURE
    {
        JSON_RPC::ERROR error;
    };


    [[noreturn]] void fail( int aCode, const std::string& aMessage, JSON aData = nullptr )
    {
        throw RPC_FAILURE{ { aCode, aMessage, std::move( aData ) } };
    }


    std::string requestKey( const JSON& aId )
    {
        return aId.dump();
    }

} // namespace


MCP_SERVER::MCP_SERVER( SERVER_INFO aInfo, TOOL_REGISTRY& aTools, RESOURCE_REGISTRY& aResources,
                        PROMPT_REGISTRY& aPrompts, EXECUTOR aExecutor ) :
        m_info( std::move( aInfo ) ),
        m_tools( aTools ),
        m_resources( aResources ),
        m_prompts( aPrompts ),
        m_executor( std::move( aExecutor ) ),
        m_protocolVersion( SupportedProtocolVersions().front() )
{
}


const std::vector<std::string>& MCP_SERVER::SupportedProtocolVersions()
{
    static const std::vector<std::string> versions{ "2025-06-18", "2025-03-26", "2024-11-05" };
    return versions;
}


JSON MCP_SERVER::Capabilities() const
{
    return JSON{ { "tools", JSON{ { "listChanged", true } } },
                 { "resources", JSON{ { "subscribe", false }, { "listChanged", false } } },
                 { "prompts", JSON{ { "listChanged", false } } },
                 { "logging", JSON::object() } };
}


void MCP_SERVER::SetBroadcast( std::function<void( const JSON& )> aBroadcast )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_broadcast = std::move( aBroadcast );
}


void MCP_SERVER::Broadcast( const std::string& aMethod, JSON aParams )
{
    std::function<void( const JSON& )> broadcast;

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        broadcast = m_broadcast;
    }

    if( broadcast )
        broadcast( JSON_RPC::MakeNotification( aMethod, std::move( aParams ) ) );
}


void MCP_SERVER::SetDisabledGroups( const std::set<std::string>& aGroups )
{
    if( aGroups == m_disabledGroups )
        return;

    m_disabledGroups = aGroups;
    Broadcast( "notifications/tools/list_changed" );
}


bool MCP_SERVER::Submit( const std::string& aText, std::shared_ptr<REPLY_CHANNEL> aChannel )
{
    JSON_RPC::ERROR     error;
    std::optional<JSON> parsed = JSON_RPC::Parse( aText, error );

    if( !parsed )
    {
        aChannel->Send( JSON_RPC::MakeError( nullptr, error ) );
        return true;
    }

    const bool        isBatch = parsed->is_array();
    std::vector<JSON> items;

    if( isBatch )
        items.assign( parsed->begin(), parsed->end() );
    else
        items.push_back( *parsed );

    // Notifications are handled here, on the transport thread, so that a cancellation reaches
    // a request that is running on the executor thread.
    std::vector<JSON>                           requests;
    std::vector<std::shared_ptr<REQUEST_STATE>> states;

    for( const JSON& item : items )
    {
        if( JSON_RPC::IsNotification( item ) )
        {
            handleNotification( item );
        }
        else if( JSON_RPC::IsRequest( item ) )
        {
            JSON params = item.value( "params", JSON::object() );
            JSON token = nullptr;

            if( params.is_object() && params.contains( "_meta" ) && params["_meta"].is_object() )
                token = params["_meta"].value( "progressToken", JSON( nullptr ) );

            std::weak_ptr<REPLY_CHANNEL> weakChannel = aChannel;

            auto send = [weakChannel]( const JSON& aMessage )
            {
                if( std::shared_ptr<REPLY_CHANNEL> channel = weakChannel.lock() )
                    channel->Send( aMessage );
            };

            auto state = std::make_shared<REQUEST_STATE>( item["id"], token, std::move( send ) );

            registerRequest( state );
            requests.push_back( item );
            states.push_back( state );
        }

        // Responses from the client are ignored: this server sends no requests.
    }

    if( requests.empty() )
        return false;

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        ++m_pending;
    }

    m_executor(
            [this, requests, states, isBatch, aChannel]()
            {
                JSON responses = JSON::array();

                for( size_t i = 0; i < requests.size(); ++i )
                {
                    std::optional<JSON> response;

                    try
                    {
                        response = handleSingle( requests[i], states[i] );
                    }
                    catch( const std::exception& e )
                    {
                        JSON_RPC::ERROR err{ JSON_RPC::INTERNAL_ERROR, std::string( "Internal error: " ) + e.what() };
                        response = JSON_RPC::MakeError( requests[i]["id"], err );
                    }

                    unregisterRequest( states[i] );

                    if( response )
                        responses.push_back( *response );
                }

                if( isBatch )
                    aChannel->Send( responses );
                else if( !responses.empty() )
                    aChannel->Send( responses[0] );

                // Notify under the lock: once WaitIdle() returns, this lambda no longer
                // touches the server, so the server may be destroyed.
                std::lock_guard<std::mutex> lock( m_mutex );
                --m_pending;
                m_idle.notify_all();
            } );

    return true;
}


void MCP_SERVER::WaitIdle()
{
    std::unique_lock<std::mutex> lock( m_mutex );
    m_idle.wait( lock,
                 [this]()
                 {
                     return m_pending == 0;
                 } );
}


void MCP_SERVER::handleNotification( const JSON& aMessage )
{
    const std::string method = aMessage["method"].get<std::string>();

    if( method == "notifications/cancelled" )
    {
        JSON params = aMessage.value( "params", JSON::object() );

        if( !params.is_object() || !params.contains( "requestId" ) )
            return;

        std::lock_guard<std::mutex> lock( m_mutex );
        auto                        it = m_inFlight.find( requestKey( params["requestId"] ) );

        if( it != m_inFlight.end() )
            it->second->Cancel();
    }

    // notifications/initialized and others need no action.
}


void MCP_SERVER::registerRequest( const std::shared_ptr<REQUEST_STATE>& aState )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_inFlight[requestKey( aState->Id() )] = aState;
}


void MCP_SERVER::unregisterRequest( const std::shared_ptr<REQUEST_STATE>& aState )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    auto                        it = m_inFlight.find( requestKey( aState->Id() ) );

    if( it != m_inFlight.end() && it->second == aState )
        m_inFlight.erase( it );
}


std::optional<JSON> MCP_SERVER::handleSingle( const JSON& aMessage, const std::shared_ptr<REQUEST_STATE>& aState )
{
    const JSON id = aMessage["id"];

    if( aMessage.value( "jsonrpc", "" ) != "2.0" )
    {
        return JSON_RPC::MakeError( id, { JSON_RPC::INVALID_REQUEST, "Invalid Request: jsonrpc must be \"2.0\"" } );
    }

    if( aState->WasCancelledByClient() )
        return JSON_RPC::MakeError( id, { JSON_RPC::REQUEST_CANCELLED, "Request cancelled" } );

    const std::string method = aMessage["method"].get<std::string>();
    JSON              params = aMessage.value( "params", JSON::object() );

    if( params.is_null() )
        params = JSON::object();

    try
    {
        return JSON_RPC::MakeResult( id, dispatch( method, params, aState ) );
    }
    catch( const RPC_FAILURE& failure )
    {
        return JSON_RPC::MakeError( id, failure.error );
    }
}


JSON MCP_SERVER::dispatch( const std::string& aMethod, const JSON& aParams,
                           const std::shared_ptr<REQUEST_STATE>& aState )
{
    if( aMethod == "initialize" )
        return onInitialize( aParams );

    if( aMethod == "ping" )
        return JSON::object();

    if( !m_initialized )
        fail( JSON_RPC::INVALID_REQUEST, "Server not initialized: send initialize first" );

    if( aMethod == "tools/list" )
        return JSON{ { "tools", m_tools.ListJson( m_disabledGroups ) } };

    if( aMethod == "tools/call" )
        return onToolsCall( aParams, aState );

    if( aMethod == "resources/list" )
        return JSON{ { "resources", m_resources.ListJson() } };

    if( aMethod == "resources/templates/list" )
        return JSON{ { "resourceTemplates", m_resources.ListTemplatesJson() } };

    if( aMethod == "resources/read" )
        return onResourcesRead( aParams );

    if( aMethod == "prompts/list" )
        return JSON{ { "prompts", m_prompts.ListJson() } };

    if( aMethod == "prompts/get" )
        return onPromptsGet( aParams );

    if( aMethod == "logging/setLevel" )
        return JSON::object();

    fail( JSON_RPC::METHOD_NOT_FOUND, "Method not found: " + aMethod );
}


JSON MCP_SERVER::onInitialize( const JSON& aParams )
{
    const std::string               requested = aParams.value( "protocolVersion", "" );
    const std::vector<std::string>& supported = SupportedProtocolVersions();

    if( std::find( supported.begin(), supported.end(), requested ) != supported.end() )
        m_protocolVersion = requested;
    else
        m_protocolVersion = supported.front();

    if( aParams.contains( "clientInfo" ) && aParams["clientInfo"].is_object() )
        m_clientName = aParams["clientInfo"].value( "name", "" );

    m_initialized = true;

    JSON result{ { "protocolVersion", m_protocolVersion },
                 { "capabilities", Capabilities() },
                 { "serverInfo", JSON{ { "name", m_info.name }, { "version", m_info.version } } } };

    if( !m_info.instructions.empty() )
        result["instructions"] = m_info.instructions;

    return result;
}


JSON MCP_SERVER::onToolsCall( const JSON& aParams, const std::shared_ptr<REQUEST_STATE>& aState )
{
    if( !aParams.contains( "name" ) || !aParams["name"].is_string() )
        fail( JSON_RPC::INVALID_PARAMS, "tools/call: missing tool name" );

    const std::string name = aParams["name"].get<std::string>();
    const TOOL_DEF*   tool = m_tools.Find( name );

    if( !tool || m_disabledGroups.count( tool->group ) )
        fail( JSON_RPC::INVALID_PARAMS, "Unknown tool: " + name );

    JSON args = aParams.value( "arguments", JSON::object() );

    if( args.is_null() )
        args = JSON::object();

    if( std::optional<std::string> err = SCHEMA::Validate( tool->inputSchema, args ) )
        fail( JSON_RPC::INVALID_PARAMS, "Invalid arguments for tool '" + name + "': " + *err );

    const auto  started = std::chrono::steady_clock::now();
    TOOL_RESULT result;
    bool        blocked = false;

    if( m_gate )
    {
        if( std::optional<std::string> reason = m_gate( name ) )
        {
            result = TOOL_RESULT::Error( *reason, JSON{ { "paused", true } } );
            blocked = true;
        }
    }

    if( !blocked )
    {
        if( m_start )
            m_start( name, args );

        TOOL_CONTEXT context( aState );

        try
        {
            result = tool->handler( args, context );
        }
        catch( const TOOL_ERROR& e )
        {
            result = TOOL_RESULT::Error( e.what(), e.Data() );
        }
        catch( const std::exception& e )
        {
            result = TOOL_RESULT::Error( std::string( "Tool '" ) + name + "' failed: " + e.what() );
        }
    }

    if( m_observer )
    {
        const double millis =
                std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        m_observer( name, args, result, millis );
    }

    return result.ToJson();
}


JSON MCP_SERVER::onResourcesRead( const JSON& aParams )
{
    if( !aParams.contains( "uri" ) || !aParams["uri"].is_string() )
        fail( JSON_RPC::INVALID_PARAMS, "resources/read: missing uri" );

    const std::string               uri = aParams["uri"].get<std::string>();
    std::optional<RESOURCE_CONTENT> content = m_resources.Read( uri );

    if( !content )
        fail( JSON_RPC::RESOURCE_NOT_FOUND, "Resource not found: " + uri, JSON{ { "uri", uri } } );

    if( content->uri.empty() )
        content->uri = uri;

    return JSON{ { "contents", JSON::array( { content->ToJson() } ) } };
}


JSON MCP_SERVER::onPromptsGet( const JSON& aParams )
{
    if( !aParams.contains( "name" ) || !aParams["name"].is_string() )
        fail( JSON_RPC::INVALID_PARAMS, "prompts/get: missing name" );

    const std::string name = aParams["name"].get<std::string>();
    const PROMPT_DEF* prompt = m_prompts.Find( name );

    if( !prompt )
        fail( JSON_RPC::INVALID_PARAMS, "Unknown prompt: " + name );

    JSON args = aParams.value( "arguments", JSON::object() );

    for( const JSON& argDef : prompt->arguments )
    {
        const std::string argName = argDef["name"].get<std::string>();

        if( argDef.value( "required", false ) && !args.contains( argName ) )
            fail( JSON_RPC::INVALID_PARAMS, "prompts/get: missing required argument '" + argName + "'" );
    }

    return prompt->get( args );
}

} // namespace AGENTIC
