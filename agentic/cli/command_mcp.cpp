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

#include <agentic/cli/command_mcp.h>
#include <agentic/cli/fd_streams.h>
#include <agentic/core/agent_host.h>
#include <agentic/core/main_queue.h>
#include <agentic/transport/http_transport.h>
#include <agentic/transport/stdio_transport.h>

#include <cli/exit_codes.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <thread>

#include <wx/app.h>
#include <wx/crt.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define ARG_STDIO "--stdio"
#define ARG_PORT "--port"
#define ARG_PROJECT "--project"


namespace
{

std::atomic<bool> g_mcpExitRequested{ false };

void mcpSignalHandler( int )
{
    g_mcpExitRequested.store( true );
}

} // namespace


CLI::MCP_COMMAND::MCP_COMMAND() :
        COMMAND( "mcp" )
{
    m_argParser.add_description( UTF8STDSTR( _( "Run the Agentic-KiCad MCP server headless" ) ) );

    m_argParser.add_argument( ARG_STDIO )
            .help( UTF8STDSTR( _( "Serve MCP over stdin/stdout (the client spawns kicad-cli)" ) ) )
            .flag();

    m_argParser.add_argument( ARG_PORT )
            .default_value( -1 )
            .scan<'i', int>()
            .help( UTF8STDSTR( _( "Serve MCP over HTTP on 127.0.0.1 at this port (0 picks a free port)" ) ) )
            .metavar( "PORT" );

    m_argParser.add_argument( ARG_PROJECT )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Project folder to open at start" ) ) )
            .metavar( "DIR" );
}


int CLI::MCP_COMMAND::doPerform( KIWAY& aKiway )
{
    const bool useStdio = m_argParser.get<bool>( ARG_STDIO );
    const int  port = m_argParser.get<int>( ARG_PORT );

    if( useStdio == ( port >= 0 ) )
    {
        wxFprintf( stderr, _( "Give exactly one of --stdio or --port.\n" ) );
        return EXIT_CODES::ERR_ARGS;
    }

    if( port > 65535 )
    {
        wxFprintf( stderr, _( "Invalid port %d.\n" ), port );
        return EXIT_CODES::ERR_ARGS;
    }

#ifndef _WIN32
    // Keep the protocol stream clean: stdout now goes to stderr, the protocol gets the
    // original stdout descriptor.
    int protocolFd = -1;

    if( useStdio )
    {
        std::fflush( stdout );
        protocolFd = ::dup( STDOUT_FILENO );
        ::dup2( STDERR_FILENO, STDOUT_FILENO );
    }
#endif

    AGENTIC::MAIN_QUEUE queue;
    AGENTIC::AGENT_HOST host( AGENTIC::AGENT_HOST::MODE::HEADLESS, &aKiway, queue.Executor() );

    const std::string project = m_argParser.get<std::string>( ARG_PROJECT );

    if( !project.empty() )
    {
        try
        {
            host.Sessions().Open( std::filesystem::path( project ), false );
        }
        catch( const std::exception& e )
        {
            wxFprintf( stderr, _( "Cannot open the project %s: %s\n" ), project, e.what() );
            return EXIT_CODES::ERR_INVALID_INPUT_FILE;
        }
    }

    g_mcpExitRequested.store( false );
    auto oldSigInt = std::signal( SIGINT, mcpSignalHandler );
#ifdef SIGTERM
    auto oldSigTerm = std::signal( SIGTERM, mcpSignalHandler );
#endif

    auto runLoop = [&]( const std::atomic<bool>& aDone )
    {
        while( !aDone.load() && !g_mcpExitRequested.load() )
        {
            queue.RunPending( std::chrono::milliseconds( 10 ) );

            if( wxTheApp )
                wxTheApp->ProcessPendingEvents();
        }
    };

    int result = EXIT_CODES::OK;

    if( useStdio )
    {
#ifndef _WIN32
        std::atomic<bool> stopReading{ false };
        std::atomic<bool> readerDone{ false };

        AGENTIC::FD_IN_BUF  inBuf( STDIN_FILENO, stopReading );
        AGENTIC::FD_OUT_BUF outBuf( protocolFd );
        std::istream        in( &inBuf );
        std::ostream        out( &outBuf );

        AGENTIC::STDIO_TRANSPORT transport( host.Server(), in, out );

        std::thread reader(
                [&]()
                {
                    transport.Run();
                    readerDone = true;
                    queue.Wake();
                } );

        runLoop( readerDone );

        // On a signal, stop reading, then keep serving until the reader has answered
        // every request it submitted.
        stopReading = true;

        while( !readerDone.load() )
        {
            queue.RunPending( std::chrono::milliseconds( 10 ) );

            if( wxTheApp )
                wxTheApp->ProcessPendingEvents();
        }

        reader.join();
        ::close( protocolFd );
#else
        wxFprintf( stderr, _( "--stdio is not supported on this platform.\n" ) );
        result = EXIT_CODES::ERR_ARGS;
#endif
    }
    else
    {
        AGENTIC::HTTP_OPTIONS options;
        options.port = static_cast<unsigned short>( port );
        options.token = AGENTIC::GenerateToken();

        AGENTIC::HTTP_TRANSPORT transport( host.Server(), options );

        try
        {
            transport.Start();
        }
        catch( const std::exception& e )
        {
            wxFprintf( stderr, "%s\n", e.what() );
            result = EXIT_CODES::ERR_UNKNOWN;
        }

        if( transport.Running() )
        {
            wxFprintf( stderr, "Agentic-KiCad MCP server listening at %s\n", transport.Url() );
            wxFprintf( stderr, "Bearer token: %s\n", options.token );
            wxFprintf( stderr,
                       "Connect Claude Code with:\n  claude mcp add --transport http kicad %s "
                       "--header \"Authorization: Bearer %s\"\n",
                       transport.Url(), options.token );

            std::atomic<bool> never{ false };
            runLoop( never );

            // Answer what is in flight, then stop.  Stop() aborts requests still waiting.
            transport.Stop();
            queue.RunPending( std::chrono::milliseconds( 0 ) );
        }
    }

    std::signal( SIGINT, oldSigInt );
#ifdef SIGTERM
    std::signal( SIGTERM, oldSigTerm );
#endif

    return result;
}
