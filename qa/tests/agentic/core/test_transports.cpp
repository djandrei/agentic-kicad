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

#include <sstream>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <agentic/protocol/json_rpc.h>
#include <agentic/transport/http_transport.h>
#include <agentic/transport/stdio_transport.h>

#include "test_helpers.h"

using namespace AGENTIC;
using namespace AGENTIC_QA;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;


namespace
{

struct TRANSPORT_FIXTURE
{
    TRANSPORT_FIXTURE()
    {
        tools.Add( { "echo", "Echo", "project", JSON{ { "type", "object" } }, []( const JSON& aArgs, TOOL_CONTEXT& )
                     {
                         return TOOL_RESULT::Data( aArgs );
                     } } );

        tools.Add( { "wait_cancel", "Runs until cancelled", "project", JSON{ { "type", "object" } },
                     [this]( const JSON&, TOOL_CONTEXT& aContext )
                     {
                         started = true;
                         aContext.Progress( 1, 2, "half" );

                         while( !aContext.IsCancelled() )
                             std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );

                         return TOOL_RESULT::Text( "cancelled" );
                     } } );
    }

    TOOL_REGISTRY     tools;
    RESOURCE_REGISTRY resources;
    PROMPT_REGISTRY   prompts;
    QUEUE_EXECUTOR    queue;
    std::atomic<bool> started{ false };

    MCP_SERVER server{ SERVER_INFO{ "agentic-kicad", "0.1.0", "" }, tools, resources, prompts, queue.Executor() };
};


/// One POST over a real socket; returns status, headers and body.
struct HTTP_REPLY
{
    unsigned                           status = 0;
    std::map<std::string, std::string> headers;
    std::string                        body;
};


HTTP_REPLY post( unsigned short aPort, const std::string& aBody, const std::string& aToken,
                 const std::string& aAccept = "application/json, text/event-stream" )
{
    net::io_context ioc;
    tcp::socket     socket( ioc );
    socket.connect( tcp::endpoint( net::ip::make_address( "127.0.0.1" ), aPort ) );

    http::request<http::string_body> req{ http::verb::post, "/mcp", 11 };
    req.set( http::field::host, "127.0.0.1" );
    req.set( http::field::content_type, "application/json" );
    req.set( http::field::accept, aAccept );

    if( !aToken.empty() )
        req.set( http::field::authorization, "Bearer " + aToken );

    req.body() = aBody;
    req.prepare_payload();
    http::write( socket, req );

    beast::flat_buffer                buffer;
    http::response<http::string_body> res;
    http::read( socket, buffer, res );

    HTTP_REPLY reply;
    reply.status = res.result_int();
    reply.body = res.body();

    for( const auto& field : res )
        reply.headers[std::string( field.name_string() )] = std::string( field.value() );

    beast::error_code ec;
    socket.shutdown( tcp::socket::shutdown_both, ec );
    return reply;
}

} // namespace


BOOST_FIXTURE_TEST_SUITE( AgenticTransports, TRANSPORT_FIXTURE )


BOOST_AUTO_TEST_CASE( StdioRoundTrip )
{
    std::istringstream in( InitializeRequest() + "\n\n"
                           + Request( 2, "tools/call", JSON{ { "name", "echo" }, { "arguments", { { "a", 1 } } } } )
                           + "\n" + R"({"jsonrpc":"2.0","method":"notifications/initialized"})" + "\n" );
    std::ostringstream out;

    STDIO_TRANSPORT transport( server, in, out );
    transport.Run();

    std::istringstream lines( out.str() );
    std::string        line;
    std::vector<JSON>  messages;

    while( std::getline( lines, line ) )
        messages.push_back( JSON::parse( line ) );

    BOOST_REQUIRE_EQUAL( messages.size(), 2u );
    BOOST_CHECK_EQUAL( messages[0]["id"], 1 );
    BOOST_CHECK_EQUAL( messages[1]["result"]["structuredContent"]["a"], 1 );
}


BOOST_AUTO_TEST_CASE( StdioCancel )
{
    // The cancel line is read while the tool runs on the executor
    std::istringstream in( InitializeRequest() + "\n" + Request( 2, "tools/call", JSON{ { "name", "wait_cancel" } } )
                           + "\n" + R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":2}})"
                           + "\n" );
    std::ostringstream out;

    STDIO_TRANSPORT transport( server, in, out );
    transport.Run();

    BOOST_CHECK( out.str().find( "ancelled" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( HttpHandleChecks )
{
    HTTP_TRANSPORT                           transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "secret", "/mcp" } );
    const std::map<std::string, std::string> auth{ { "authorization", "Bearer secret" } };

    BOOST_CHECK_EQUAL( transport.Handle( "POST", "/other", auth, "" ).status, 404u );

    std::map<std::string, std::string> foreign = auth;
    foreign["origin"] = "http://evil.example.com";
    BOOST_CHECK_EQUAL( transport.Handle( "POST", "/mcp", foreign, InitializeRequest() ).status, 403u );

    std::map<std::string, std::string> local = auth;
    local["origin"] = "http://localhost:3000";
    BOOST_CHECK_EQUAL( transport.Handle( "POST", "/mcp", local, InitializeRequest() ).status, 200u );

    BOOST_CHECK_EQUAL( transport.Handle( "POST", "/mcp", {}, InitializeRequest() ).status, 401u );
    BOOST_CHECK_EQUAL(
            transport.Handle( "POST", "/mcp", { { "authorization", "Bearer wrong" } }, InitializeRequest() ).status,
            401u );
    BOOST_CHECK_EQUAL( transport.Handle( "GET", "/mcp", auth, "" ).status, 405u );

    HTTP_TRANSPORT::RESPONSE note =
            transport.Handle( "POST", "/mcp", auth, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" );
    BOOST_CHECK_EQUAL( note.status, 202u );

    std::map<std::string, std::string> badSession = auth;
    badSession["mcp-session-id"] = "nope";
    BOOST_CHECK_EQUAL( transport.Handle( "POST", "/mcp", badSession, Request( 9, "ping" ) ).status, 404u );
}


BOOST_AUTO_TEST_CASE( HttpInitializeSetsSession )
{
    HTTP_TRANSPORT           transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "", "/mcp" } );
    HTTP_TRANSPORT::RESPONSE r = transport.Handle( "POST", "/mcp", {}, InitializeRequest() );
    BOOST_CHECK_EQUAL( r.status, 200u );
    BOOST_CHECK_EQUAL( r.headers["Mcp-Session-Id"], transport.SessionId() );
    BOOST_CHECK_EQUAL( JSON::parse( r.body )["result"]["serverInfo"]["name"], "agentic-kicad" );
}


BOOST_AUTO_TEST_CASE( HttpRefusesNonLoopback )
{
    HTTP_TRANSPORT transport( server, HTTP_OPTIONS{ "0.0.0.0", 0, "t", "/mcp" } );
    BOOST_CHECK_THROW( transport.Start(), std::runtime_error );
}


BOOST_AUTO_TEST_CASE( HttpOverSocket )
{
    HTTP_TRANSPORT transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "tok", "/mcp" } );
    transport.Start();
    BOOST_CHECK( transport.Running() );
    BOOST_CHECK( transport.Url().find( "http://127.0.0.1:" ) == 0 );

    HTTP_REPLY reply = post( transport.Port(), InitializeRequest(), "tok" );
    BOOST_CHECK_EQUAL( reply.status, 200u );
    BOOST_CHECK_EQUAL( JSON::parse( reply.body )["id"], 1 );

    reply = post( transport.Port(), InitializeRequest(), "" );
    BOOST_CHECK_EQUAL( reply.status, 401u );

    transport.Stop();
    BOOST_CHECK( !transport.Running() );
}


BOOST_AUTO_TEST_CASE( HttpCancelOverSecondConnection )
{
    HTTP_TRANSPORT transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "tok", "/mcp" } );
    transport.Start();
    post( transport.Port(), InitializeRequest(), "tok" );

    HTTP_REPLY  longReply;
    std::thread caller(
            [&]()
            {
                longReply = post( transport.Port(), Request( 3, "tools/call", JSON{ { "name", "wait_cancel" } } ),
                                  "tok", "application/json" );
            } );

    while( !started )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    HTTP_REPLY cancel =
            post( transport.Port(), R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3}})",
                  "tok" );
    BOOST_CHECK_EQUAL( cancel.status, 202u );

    caller.join();
    BOOST_CHECK_EQUAL( longReply.status, 200u );
    BOOST_CHECK_EQUAL( JSON::parse( longReply.body )["result"]["content"][0]["text"], "cancelled" );

    transport.Stop();
}


BOOST_AUTO_TEST_CASE( HttpEventStreamCarriesProgress )
{
    HTTP_TRANSPORT transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "tok", "/mcp" } );
    transport.Start();
    post( transport.Port(), InitializeRequest(), "tok" );

    HTTP_REPLY  streamed;
    std::thread caller(
            [&]()
            {
                streamed = post( transport.Port(),
                                 Request( 4, "tools/call",
                                          JSON{ { "name", "wait_cancel" }, { "_meta", { { "progressToken", 11 } } } } ),
                                 "tok" );
            } );

    while( !started )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    post( transport.Port(), R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":4}})", "tok" );
    caller.join();

    BOOST_CHECK_EQUAL( streamed.status, 200u );
    BOOST_CHECK_EQUAL( streamed.headers["Content-Type"], "text/event-stream" );

    size_t progress = streamed.body.find( "notifications/progress" );
    size_t result = streamed.body.find( "\"result\"" );
    BOOST_CHECK( progress != std::string::npos );
    BOOST_CHECK( result != std::string::npos );
    BOOST_CHECK( progress < result );

    transport.Stop();
}


BOOST_AUTO_TEST_CASE( HttpStopAbortsWaitingRequest )
{
    HTTP_TRANSPORT transport( server, HTTP_OPTIONS{ "127.0.0.1", 0, "", "/mcp" } );
    transport.Start();
    post( transport.Port(), InitializeRequest(), "" );

    std::thread caller(
            [&]()
            {
                try
                {
                    post( transport.Port(), Request( 5, "tools/call", JSON{ { "name", "wait_cancel" } } ), "",
                          "application/json" );
                }
                catch( const std::exception& )
                {
                    // The connection may be closed under us
                }
            } );

    while( !started )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    // Stop must return although the tool is still running
    transport.Stop();
    caller.join();

    // Let the tool finish so the executor can be destroyed
    auto channel = std::make_shared<CAPTURE_CHANNEL>();
    server.Submit( R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5}})", channel );
    server.WaitIdle();
}


BOOST_AUTO_TEST_CASE( TokensAreRandom )
{
    std::string a = GenerateToken();
    std::string b = GenerateToken();
    BOOST_CHECK_EQUAL( a.size(), 32u );
    BOOST_CHECK_NE( a, b );
}


BOOST_AUTO_TEST_SUITE_END()
