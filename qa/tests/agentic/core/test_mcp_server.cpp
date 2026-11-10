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

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <thread>

#include <agentic/protocol/json_rpc.h>
#include <agentic/protocol/mcp_server.h>

#include "test_helpers.h"

using namespace AGENTIC;
using namespace AGENTIC_QA;


namespace
{

struct SERVER_FIXTURE
{
    SERVER_FIXTURE()
    {
        tools.Add( { "echo", "Echo the text", "project",
                     JSON::parse( R"({"type":"object","properties":{"text":{"type":"string"}},
                                      "required":["text"],"additionalProperties":false})" ),
                     []( const JSON& aArgs, TOOL_CONTEXT& )
                     {
                         return TOOL_RESULT::Data( JSON{ { "text", aArgs["text"] } } );
                     } } );

        tools.Add( { "fails", "Always fails", "project", JSON{ { "type", "object" } },
                     []( const JSON&, TOOL_CONTEXT& ) -> TOOL_RESULT
                     {
                         throw TOOL_ERROR( "version conflict", JSON{ { "version", "abc" } } );
                     } } );

        tools.Add( { "pcb_thing", "A PCB tool", "pcb", JSON{ { "type", "object" } }, []( const JSON&, TOOL_CONTEXT& )
                     {
                         return TOOL_RESULT::Text( "pcb" );
                     } } );

        tools.Add( { "wait_cancel", "Runs until cancelled, with progress", "project", JSON{ { "type", "object" } },
                     [this]( const JSON&, TOOL_CONTEXT& aContext )
                     {
                         started = true;
                         int i = 0;

                         while( !aContext.IsCancelled() )
                         {
                             if( i < 3 )
                                 aContext.Progress( ++i, 3, "step" );

                             std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
                         }

                         return TOOL_RESULT::Error( aContext.TimedOut() ? "timed out" : "cancelled" );
                     } } );

        tools.Add( { "timeout", "Times out", "project", JSON{ { "type", "object" } },
                     []( const JSON&, TOOL_CONTEXT& aContext )
                     {
                         aContext.SetTimeout( 0.05 );

                         while( !aContext.IsCancelled() )
                             std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );

                         return TOOL_RESULT::Error( aContext.TimedOut() ? "timed out" : "cancelled" );
                     } } );

        resources.Add( { "kicad://docs/test", "test", "A test resource", "text/plain", []()
                         {
                             return RESOURCE_CONTENT{ "", "text/plain", "hello", "" };
                         } } );

        prompts.Add( { "loop", "The working loop", JSON::parse( R"([{"name":"area","required":true}])" ),
                       []( const JSON& aArgs )
                       {
                           return JSON{ { "description", "loop" }, { "messages", JSON::array( { aArgs["area"] } ) } };
                       } } );
    }

    std::unique_ptr<MCP_SERVER> Make( EXECUTOR aExecutor )
    {
        return std::make_unique<MCP_SERVER>( SERVER_INFO{ "agentic-kicad", "0.1.0", "Use it well." }, tools, resources,
                                             prompts, std::move( aExecutor ) );
    }

    /// Submit one message and wait for \a aCount messages on a fresh channel.
    static std::vector<JSON> Call( MCP_SERVER& aServer, const std::string& aText, size_t aCount = 1 )
    {
        auto channel = std::make_shared<CAPTURE_CHANNEL>();
        aServer.Submit( aText, channel );
        return channel->Wait( aCount );
    }

    TOOL_REGISTRY     tools;
    RESOURCE_REGISTRY resources;
    PROMPT_REGISTRY   prompts;
    std::atomic<bool> started{ false };
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( AgenticMcpServer, SERVER_FIXTURE )


BOOST_AUTO_TEST_CASE( RequiresInitialize )
{
    auto              server = Make( DirectExecutor() );
    std::vector<JSON> out = Call( *server, Request( 1, "tools/list" ) );
    BOOST_REQUIRE_EQUAL( out.size(), 1u );
    BOOST_CHECK_EQUAL( out[0]["error"]["code"], JSON_RPC::INVALID_REQUEST );

    // ping is allowed before initialize
    out = Call( *server, Request( 2, "ping" ) );
    BOOST_CHECK( out[0].contains( "result" ) );
}


BOOST_AUTO_TEST_CASE( InitializeNegotiatesVersion )
{
    auto              server = Make( DirectExecutor() );
    std::vector<JSON> out = Call( *server, InitializeRequest() );
    const JSON&       result = out.at( 0 )["result"];
    BOOST_CHECK_EQUAL( result["protocolVersion"], "2025-06-18" );
    BOOST_CHECK_EQUAL( result["serverInfo"]["name"], "agentic-kicad" );
    BOOST_CHECK_EQUAL( result["instructions"], "Use it well." );
    BOOST_CHECK_EQUAL( result["capabilities"]["tools"]["listChanged"], true );
    BOOST_CHECK_EQUAL( server->ClientName(), "qa" );

    auto server2 = Make( DirectExecutor() );
    out = Call( *server2, Request( 1, "initialize", JSON{ { "protocolVersion", "1999-01-01" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["protocolVersion"], MCP_SERVER::SupportedProtocolVersions().front() );
}


BOOST_AUTO_TEST_CASE( ToolsListAndCall )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );

    std::vector<JSON> out = Call( *server, Request( 2, "tools/list" ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["tools"].size(), tools.Size() );

    out = Call( *server,
                Request( 3, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "text", "hi" } } } } ) );
    const JSON& result = out.at( 0 )["result"];
    BOOST_CHECK_EQUAL( result["isError"], false );
    BOOST_CHECK_EQUAL( result["structuredContent"]["text"], "hi" );
}


BOOST_AUTO_TEST_CASE( ToolArgumentErrors )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );

    std::vector<JSON> out = Call( *server, Request( 2, "tools/call", JSON{ { "name", "nope" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::INVALID_PARAMS );

    out = Call( *server, Request( 3, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "text", 5 } } } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::INVALID_PARAMS );
    BOOST_CHECK( out.at( 0 )["error"]["message"].get<std::string>().find( "$.text" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( ToolErrorIsAToolResult )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );

    std::vector<JSON> out = Call( *server, Request( 2, "tools/call", JSON{ { "name", "fails" } } ) );
    const JSON&       result = out.at( 0 )["result"];
    BOOST_CHECK_EQUAL( result["isError"], true );
    BOOST_CHECK_EQUAL( result["content"][0]["text"], "version conflict" );
    BOOST_CHECK_EQUAL( result["structuredContent"]["version"], "abc" );
}


BOOST_AUTO_TEST_CASE( GateBlocksCalls )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );
    server->SetToolGate(
            []( const std::string& )
            {
                return std::optional<std::string>( "paused" );
            } );

    std::vector<JSON> out = Call(
            *server, Request( 2, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "text", "x" } } } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["isError"], true );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["structuredContent"]["paused"], true );
}


BOOST_AUTO_TEST_CASE( ObserverSeesEveryCall )
{
    auto        server = Make( DirectExecutor() );
    std::string seen;
    bool        sawError = false;

    server->SetToolObserver(
            [&]( const std::string& aName, const JSON&, const TOOL_RESULT& aResult, double aMillis )
            {
                seen += aName + ";";
                sawError |= aResult.isError;
                BOOST_CHECK( aMillis >= 0 );
            } );

    Call( *server, InitializeRequest() );
    Call( *server, Request( 2, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "text", "x" } } } } ) );
    Call( *server, Request( 3, "tools/call", JSON{ { "name", "fails" } } ) );
    BOOST_CHECK_EQUAL( seen, "echo;fails;" );
    BOOST_CHECK( sawError );
}


BOOST_AUTO_TEST_CASE( DisabledGroups )
{
    auto              server = Make( DirectExecutor() );
    std::vector<JSON> broadcasts;
    server->SetBroadcast(
            [&]( const JSON& aMessage )
            {
                broadcasts.push_back( aMessage );
            } );
    Call( *server, InitializeRequest() );

    server->SetDisabledGroups( { "pcb" } );
    BOOST_REQUIRE_EQUAL( broadcasts.size(), 1u );
    BOOST_CHECK_EQUAL( broadcasts[0]["method"], "notifications/tools/list_changed" );

    // Same set again: no new broadcast
    server->SetDisabledGroups( { "pcb" } );
    BOOST_CHECK_EQUAL( broadcasts.size(), 1u );

    std::vector<JSON> out = Call( *server, Request( 2, "tools/list" ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["tools"].size(), tools.Size() - 1 );

    out = Call( *server, Request( 3, "tools/call", JSON{ { "name", "pcb_thing" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::INVALID_PARAMS );
}


BOOST_AUTO_TEST_CASE( NotificationsGetNoResponse )
{
    auto server = Make( DirectExecutor() );
    auto channel = std::make_shared<CAPTURE_CHANNEL>();
    BOOST_CHECK( !server->Submit( R"({"jsonrpc":"2.0","method":"notifications/initialized"})", channel ) );
}


BOOST_AUTO_TEST_CASE( ParseErrorResponse )
{
    auto              server = Make( DirectExecutor() );
    std::vector<JSON> out = Call( *server, "{broken" );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::PARSE_ERROR );
    BOOST_CHECK( out.at( 0 )["id"].is_null() );
}


BOOST_AUTO_TEST_CASE( Batch )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );

    std::string batch = "[" + Request( 2, "ping" ) + R"(,{"jsonrpc":"2.0","method":"notifications/x"},)"
                        + Request( 3, "tools/list" ) + "]";
    std::vector<JSON> out = Call( *server, batch );
    BOOST_REQUIRE( out.at( 0 ).is_array() );
    BOOST_CHECK_EQUAL( out[0].size(), 2u );
}


BOOST_AUTO_TEST_CASE( CancelRunningTool )
{
    QUEUE_EXECUTOR queue;
    auto           server = Make( queue.Executor() );
    Call( *server, InitializeRequest() );

    auto channel = std::make_shared<CAPTURE_CHANNEL>();
    server->Submit( Request( 7, "tools/call", JSON{ { "name", "wait_cancel" } } ), channel );

    while( !started )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    // The cancel is handled on this (transport) thread while the tool runs on the queue
    server->Submit( R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":7}})", channel );

    std::vector<JSON> out = channel->Wait( 1 );
    BOOST_REQUIRE_EQUAL( out.size(), 1u );
    BOOST_CHECK_EQUAL( out[0]["result"]["content"][0]["text"], "cancelled" );
    server->WaitIdle();
}


BOOST_AUTO_TEST_CASE( CancelBeforeStart )
{
    QUEUE_EXECUTOR queue;
    auto           server = Make( queue.Executor() );
    Call( *server, InitializeRequest() );

    // Occupy the executor, then queue a second request and cancel it before it starts
    auto busy = std::make_shared<CAPTURE_CHANNEL>();
    server->Submit( Request( 1, "tools/call", JSON{ { "name", "wait_cancel" } } ), busy );

    while( !started )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    auto queued = std::make_shared<CAPTURE_CHANNEL>();
    server->Submit( Request( 2, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "text", "x" } } } } ),
                    queued );
    server->Submit( R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":2}})", queued );
    server->Submit( R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})", busy );

    std::vector<JSON> out = queued->Wait( 1 );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::REQUEST_CANCELLED );
    server->WaitIdle();
}


BOOST_AUTO_TEST_CASE( ToolTimeout )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );
    std::vector<JSON> out = Call( *server, Request( 2, "tools/call", JSON{ { "name", "timeout" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["content"][0]["text"], "timed out" );
}


BOOST_AUTO_TEST_CASE( ProgressNotifications )
{
    QUEUE_EXECUTOR queue;
    auto           server = Make( queue.Executor() );
    Call( *server, InitializeRequest() );

    auto channel = std::make_shared<CAPTURE_CHANNEL>();
    server->Submit(
            Request( 5, "tools/call", JSON{ { "name", "wait_cancel" }, { "_meta", { { "progressToken", "tok" } } } } ),
            channel );

    // Three progress notifications arrive before we cancel
    std::vector<JSON> out = channel->Wait( 3 );
    BOOST_REQUIRE_EQUAL( out.size(), 3u );
    BOOST_CHECK_EQUAL( out[0]["method"], "notifications/progress" );
    BOOST_CHECK_EQUAL( out[0]["params"]["progressToken"], "tok" );
    BOOST_CHECK_EQUAL( out[2]["params"]["progress"], 3 );
    BOOST_CHECK_EQUAL( out[2]["params"]["total"], 3 );

    server->Submit( R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5}})", channel );
    out = channel->Wait( 4 );
    BOOST_CHECK( out.at( 3 ).contains( "result" ) );
    server->WaitIdle();
}


BOOST_AUTO_TEST_CASE( ResourcesAndPrompts )
{
    auto server = Make( DirectExecutor() );
    Call( *server, InitializeRequest() );

    std::vector<JSON> out = Call( *server, Request( 2, "resources/read", JSON{ { "uri", "kicad://docs/test" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["contents"][0]["text"], "hello" );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["contents"][0]["uri"], "kicad://docs/test" );

    out = Call( *server, Request( 3, "resources/read", JSON{ { "uri", "kicad://nope" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::RESOURCE_NOT_FOUND );

    out = Call( *server, Request( 4, "prompts/get", JSON{ { "name", "loop" } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::INVALID_PARAMS );

    out = Call( *server,
                Request( 5, "prompts/get", JSON{ { "name", "loop" }, { "arguments", { { "area", "pcb" } } } } ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["result"]["messages"][0], "pcb" );

    out = Call( *server, Request( 6, "no/such" ) );
    BOOST_CHECK_EQUAL( out.at( 0 )["error"]["code"], JSON_RPC::METHOD_NOT_FOUND );
}


BOOST_AUTO_TEST_SUITE_END()
