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

#include <thread>

#include <agentic/core/agent_host.h>
#include <agentic/core/main_queue.h>

#include "test_helpers.h"

using namespace AGENTIC;
using namespace AGENTIC_QA;


namespace
{

JSON callTool( AGENT_HOST& aHost, int aId, const std::string& aName )
{
    auto channel = std::make_shared<CAPTURE_CHANNEL>();
    aHost.Server().Submit( Request( aId, "tools/call", JSON{ { "name", aName }, { "arguments", JSON::object() } } ),
                           channel );
    return channel->Wait( 1 ).at( 0 );
}

} // namespace


BOOST_AUTO_TEST_SUITE( AgenticHost )


BOOST_AUTO_TEST_CASE( VersionAndStatus )
{
    AGENT_HOST host( AGENT_HOST::MODE::HEADLESS, nullptr, DirectExecutor() );
    auto       channel = std::make_shared<CAPTURE_CHANNEL>();
    host.Server().Submit( InitializeRequest(), channel );

    JSON version = callTool( host, 2, "version" );
    BOOST_CHECK_EQUAL( version["result"]["structuredContent"]["server"], "agentic-kicad" );
    BOOST_CHECK( !version["result"]["structuredContent"]["kicadVersion"].get<std::string>().empty() );
    BOOST_CHECK_EQUAL( version["result"]["structuredContent"]["protocolVersion"], "2025-06-18" );

    JSON status = callTool( host, 3, "status" );
    BOOST_CHECK_EQUAL( status["result"]["structuredContent"]["mode"], "headless" );
    BOOST_CHECK_EQUAL( status["result"]["structuredContent"]["paused"], false );
    BOOST_CHECK_EQUAL( status["result"]["structuredContent"]["client"], "qa" );
    BOOST_CHECK( status["result"]["structuredContent"]["providers"].empty() );
}


BOOST_AUTO_TEST_CASE( PauseBlocksToolsButNotStatus )
{
    AGENT_HOST host( AGENT_HOST::MODE::VIEWER, nullptr, DirectExecutor() );
    auto       channel = std::make_shared<CAPTURE_CHANNEL>();
    host.Server().Submit( InitializeRequest(), channel );

    host.Tools().Add( { "work", "Does work", "project", JSON{ { "type", "object" } }, []( const JSON&, TOOL_CONTEXT& )
                        {
                            return TOOL_RESULT::Text( "done" );
                        } } );

    host.SetPaused( true );
    JSON blocked = callTool( host, 2, "work" );
    BOOST_CHECK_EQUAL( blocked["result"]["isError"], true );
    BOOST_CHECK_EQUAL( blocked["result"]["structuredContent"]["paused"], true );

    JSON status = callTool( host, 3, "status" );
    BOOST_CHECK_EQUAL( status["result"]["isError"], false );
    BOOST_CHECK_EQUAL( status["result"]["structuredContent"]["paused"], true );
    BOOST_CHECK_EQUAL( status["result"]["structuredContent"]["mode"], "viewer" );

    host.SetPaused( false );
    JSON resumed = callTool( host, 4, "work" );
    BOOST_CHECK_EQUAL( resumed["result"]["isError"], false );
}


BOOST_AUTO_TEST_CASE( InstructionsMentionTheLoop )
{
    std::string text = AGENT_HOST::Instructions();
    BOOST_CHECK( text.find( "project_open" ) != std::string::npos );
    BOOST_CHECK( text.find( "millimetres" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( MainQueueRunsPostedWorkInOrder )
{
    MAIN_QUEUE  queue;
    std::string order;

    std::thread poster(
            [&]()
            {
                queue.Post(
                        [&]()
                        {
                            order += "a";
                        } );
                queue.Post(
                        [&]()
                        {
                            order += "b";
                        } );
            } );
    poster.join();

    BOOST_CHECK_EQUAL( queue.RunPending( std::chrono::milliseconds( 100 ) ), 2 );
    BOOST_CHECK_EQUAL( order, "ab" );

    // Empty queue: returns after the wait, or at once when woken
    queue.Wake();
    BOOST_CHECK_EQUAL( queue.RunPending( std::chrono::seconds( 5 ) ), 0 );
}


BOOST_AUTO_TEST_SUITE_END()
