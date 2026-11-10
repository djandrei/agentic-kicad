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


/**
 * Stage 2: the project sandbox, document sessions, save after every call (D5), undo through
 * the file history (D7), messages (D11), Pause, and several projects at once.
 */

#include <boost/test/unit_test.hpp>

#include <agentic/core/agent_host.h>
#include <agentic/core/core_tools.h>
#include <agentic/core/file_store.h>
#include <agentic/core/handles.h>
#include <agentic/core/progress.h>
#include <agentic/core/provider.h>

#include <lockfile.h>

#include <wx/log.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "test_helpers.h"

using namespace AGENTIC;
using namespace AGENTIC_QA;
namespace fs = std::filesystem;


namespace
{

/// A fresh, empty folder under the test temp dir.
fs::path freshDir( const std::string& aName )
{
    static int counter = 0;
    fs::path   dir = fs::temp_directory_path() / "kicad_qa_agentic_stage2"
                   / ( aName + "-" + std::to_string( ::getpid() ) + "-" + std::to_string( counter++ ) );
    fs::remove_all( dir );
    fs::create_directories( dir );
    return fs::canonical( dir );
}


void writeFile( const fs::path& aPath, const std::string& aContent )
{
    fs::create_directories( aPath.parent_path() );
    std::ofstream out( aPath, std::ios::binary | std::ios::trunc );
    out << aContent;
}


std::string readFile( const fs::path& aPath )
{
    return FILE_STORE::ReadFile( aPath );
}


/// A document that is a text file: the smallest DOC_SESSION.
class STUB_SESSION : public DOC_SESSION
{
public:
    STUB_SESSION( AGENT_PROJECT& aProject, const std::string& aPath ) :
            DOC_SESSION( aProject, "stub", aPath )
    {
    }

    std::string text;
    bool        failSave = false;

protected:
    void load() override
    {
        const fs::path path = Project().Files().Resolve( Path() );
        text = fs::exists( path ) ? readFile( path ) : std::string();
    }

    void save( const std::vector<std::string>& aFiles ) override
    {
        if( failSave )
            throw TOOL_ERROR( "disk full (simulated)" );

        FILE_STORE::WriteAtomic( Project().Files().Resolve( Path() ), text );
    }
};


/// A provider with one mutating tool, stub_append, on the document stub.txt.
class STUB_PROVIDER : public AGENT_PROVIDER
{
public:
    wxString Name() const override { return wxS( "stub" ); }

    void Register( AGENT_SERVICES& aServices ) override
    {
        aServices.Tools().Add( { "stub_append", "Append text to stub.txt.", "stub",
                                 ProjectToolSchema( { { "text", { { "type", "string" } } },
                                                      { "warn", { { "type", "boolean" } } },
                                                      { "fail", { { "type", "boolean" } } },
                                                      { "force", { { "type", "boolean" } } } },
                                                    { "text" } ),
                                 [&aServices, this]( const JSON& aArgs, TOOL_CONTEXT& )
                                 {
                                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                                     STUB_SESSION*  session = session_for( project );
                                     session->failSave = aArgs.value( "fail", false );

                                     JSON result =
                                             project.Mutate( "stub_append", aArgs.value( "force", false ),
                                                             [&]() -> JSON
                                                             {
                                                                 if( aArgs.value( "warn", false ) )
                                                                     wxLogWarning( "stub warning" );

                                                                 session->text += aArgs["text"].get<std::string>();
                                                                 session->MarkModified();
                                                                 return JSON{ { "length", session->text.size() } };
                                                             } );

                                     return TOOL_RESULT::Data( result );
                                 } } );
    }

    STUB_SESSION* session_for( AGENT_PROJECT& aProject )
    {
        if( DOC_SESSION* existing = aProject.FindSession( "stub", "stub.txt" ) )
            return static_cast<STUB_SESSION*>( existing );

        auto session = std::make_unique<STUB_SESSION>( aProject, "stub.txt" );
        session->Open( false );
        return static_cast<STUB_SESSION*>( &aProject.AddSession( std::move( session ) ) );
    }
};


/// A host with the stub provider, talking JSON-RPC like a client.
class HOST_FIXTURE
{
public:
    HOST_FIXTURE() :
            host( AGENT_HOST::MODE::HEADLESS, nullptr, DirectExecutor() )
    {
        host.AddProvider( &provider );
        auto channel = std::make_shared<CAPTURE_CHANNEL>();
        host.Server().Submit( InitializeRequest(), channel );
        channel->Wait( 1 );
    }

    /// The tools/call result (content, structuredContent, isError).
    JSON Call( const std::string& aName, JSON aArgs = JSON::object() )
    {
        auto channel = std::make_shared<CAPTURE_CHANNEL>();
        host.Server().Submit( Request( ++id, "tools/call", JSON{ { "name", aName }, { "arguments", aArgs } } ),
                              channel );
        JSON response = channel->Wait( 1 ).at( 0 );
        BOOST_REQUIRE_MESSAGE( response.contains( "result" ), response.dump() );
        return response["result"];
    }

    JSON Ok( const std::string& aName, JSON aArgs = JSON::object() )
    {
        JSON result = Call( aName, aArgs );
        BOOST_REQUIRE_MESSAGE( !result["isError"].get<bool>(), aName + ": " + result.dump() );
        return result["structuredContent"];
    }

    std::string Fail( const std::string& aName, JSON aArgs = JSON::object() )
    {
        JSON result = Call( aName, aArgs );
        BOOST_REQUIRE_MESSAGE( result["isError"].get<bool>(), aName + " should fail: " + result.dump() );
        return result["content"][0]["text"].get<std::string>();
    }

    STUB_PROVIDER provider;
    AGENT_HOST    host;
    int           id = 10;
};

} // namespace


BOOST_AUTO_TEST_SUITE( AgenticHandles )


BOOST_AUTO_TEST_CASE( Lengths )
{
    BOOST_CHECK_CLOSE( ParseLengthMm( 1.27, "x" ), 1.27, 1e-9 );
    BOOST_CHECK_CLOSE( ParseLengthMm( "2.54mm", "x" ), 2.54, 1e-9 );
    BOOST_CHECK_CLOSE( ParseLengthMm( "50mil", "x" ), 1.27, 1e-9 );
    BOOST_CHECK_CLOSE( ParseLengthMm( "0.1 in", "x" ), 2.54, 1e-9 );
    BOOST_CHECK_CLOSE( ParseLengthMm( "100um", "x" ), 0.1, 1e-9 );
    BOOST_CHECK_THROW( ParseLengthMm( "ten", "x" ), TOOL_ERROR );
    BOOST_CHECK_THROW( ParseLengthMm( "3 parsecs", "x" ), TOOL_ERROR );
    BOOST_CHECK_CLOSE( ParseAngleDeg( "90deg", "a" ), 90.0, 1e-9 );
    BOOST_CHECK_CLOSE( SnapMm( 1.3, 1.27 ), 1.27, 1e-9 );
}


BOOST_AUTO_TEST_CASE( Handles )
{
    BOOST_CHECK( ParseHandle( "R5" ).kind == HANDLE::KIND::REFERENCE );

    HANDLE pin = ParseHandle( "U1.A12" );
    BOOST_CHECK( pin.kind == HANDLE::KIND::PIN );
    BOOST_CHECK_EQUAL( pin.reference, "U1" );
    BOOST_CHECK_EQUAL( pin.pin, "A12" );

    BOOST_CHECK( ParseHandle( "/SDA" ).kind == HANDLE::KIND::NET );
    BOOST_CHECK( ParseHandle( "/power/" ).kind == HANDLE::KIND::SHEET );
    BOOST_CHECK( ParseHandle( "layer:F.Cu" ).kind == HANDLE::KIND::LAYER );
    BOOST_CHECK( ParseHandle( "0f9c6c1e-1234-4abc-9def-0123456789ab" ).kind == HANDLE::KIND::KIID );
    BOOST_CHECK_THROW( ParseHandle( "" ), TOOL_ERROR );
    BOOST_CHECK_THROW( ParseHandle( "bogus:thing" ), TOOL_ERROR );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( AgenticFileStore )


BOOST_AUTO_TEST_CASE( Sandbox )
{
    fs::path root = freshDir( "sandbox" );
    fs::path outside = freshDir( "outside" );
    writeFile( outside / "secret.txt", "secret" );
    writeFile( root / "inside.txt", "inside" );
    fs::create_directory_symlink( outside, root / "link" );

    FILE_STORE store( root );

    BOOST_CHECK_EQUAL( store.Read( "inside.txt" ), "inside" );
    BOOST_CHECK_THROW( store.Resolve( "../outside/secret.txt" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Resolve( "a/../../x" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Resolve( ( outside / "secret.txt" ).string() ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Read( "link/secret.txt" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Write( "link/new.txt", "x" ), TOOL_ERROR );
    BOOST_CHECK( !fs::exists( outside / "new.txt" ) );

    // Design files are read but never written by file tools.
    writeFile( root / "board.kicad_pcb", "(kicad_pcb)" );
    BOOST_CHECK_EQUAL( store.Read( "board.kicad_pcb" ), "(kicad_pcb)" );
    BOOST_CHECK_THROW( store.Write( "board.kicad_pcb", "x" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Write( "fp-lib-table", "x" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Remove( "board.kicad_pcb" ), TOOL_ERROR );
    BOOST_CHECK_THROW( store.Rename( "inside.txt", "inside.kicad_sch", false ), TOOL_ERROR );
    BOOST_CHECK_EQUAL( readFile( root / "board.kicad_pcb" ), "(kicad_pcb)" );

    // The history is not reachable.
    BOOST_CHECK_THROW( store.Write( ".agentic/history/x", "x" ), TOOL_ERROR );
}


BOOST_AUTO_TEST_CASE( AtomicWriteAndVersionConflict )
{
    fs::path   root = freshDir( "versions" );
    FILE_STORE store( root );

    const std::string v1 = store.Write( "notes/a.txt", "one", std::string() );
    BOOST_CHECK_EQUAL( v1, FILE_STORE::VersionOf( "one" ) );
    BOOST_CHECK_EQUAL( store.Version( "notes/a.txt" ), v1 );

    // "" means "must not exist"
    BOOST_CHECK_THROW( store.Write( "notes/a.txt", "again", std::string() ), TOOL_ERROR );

    const std::string v2 = store.Write( "notes/a.txt", "two", v1 );
    BOOST_CHECK_NE( v1, v2 );

    try
    {
        store.Write( "notes/a.txt", "three", v1 );
        BOOST_FAIL( "a stale baseVersion must conflict" );
    }
    catch( const TOOL_ERROR& e )
    {
        BOOST_CHECK( e.Data()["conflict"].get<bool>() );
        BOOST_CHECK_EQUAL( e.Data()["version"].get<std::string>(), v2 );
    }

    BOOST_CHECK_EQUAL( store.Read( "notes/a.txt" ), "two" );

    // No temporary files are left behind.
    int files = 0;

    for( const auto& entry : fs::directory_iterator( root / "notes" ) )
    {
        (void) entry;
        files++;
    }

    BOOST_CHECK_EQUAL( files, 1 );
}


BOOST_AUTO_TEST_CASE( KilledDuringSaveKeepsAWholeFile )
{
    // A process killed while saving leaves either the old or the new file, never a mix.
    fs::path          root = freshDir( "killed" );
    const fs::path    target = root / "big.txt";
    const std::string a( 4 * 1024 * 1024, 'a' );
    const std::string b( 4 * 1024 * 1024, 'b' );
    FILE_STORE::WriteAtomic( target, a );

    std::mt19937 rng( 1234 );

    for( int round = 0; round < 12; ++round )
    {
        pid_t child = ::fork();
        BOOST_REQUIRE( child >= 0 );

        if( child == 0 )
        {
            // Never return into the test runner from the child.
            try
            {
                for( int i = 0;; ++i )
                    FILE_STORE::WriteAtomic( target, i % 2 ? a : b );
            }
            catch( ... )
            {
            }

            ::_exit( 1 );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 5 + rng() % 40 ) );
        ::kill( child, SIGKILL );
        ::waitpid( child, nullptr, 0 );

        const std::string content = readFile( target );
        BOOST_CHECK_MESSAGE( content == a || content == b, "round " << round << ": the file is torn" );
    }
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( AgenticProject )


BOOST_AUTO_TEST_CASE( CreateOpenAndProjectSettings )
{
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "create" );

    JSON info = f.Ok( "project_create", { { "path", ( dir / "alpha" ).string() } } );
    BOOST_CHECK_EQUAL( info["name"], "alpha" );
    BOOST_CHECK( fs::exists( dir / "alpha" / "alpha.kicad_pro" ) );

    // A second create in the same folder is refused.
    f.Fail( "project_create", { { "path", ( dir / "alpha" ).string() } } );

    JSON vars = f.Ok( "text_vars_set", { { "vars", { { "REV", "B" }, { "AUTHOR", "qa" } } } } );
    BOOST_CHECK_EQUAL( vars["vars"]["REV"], "B" );
    BOOST_CHECK_EQUAL( vars["saved"][0]["path"], "alpha.kicad_pro" );
    BOOST_CHECK( readFile( dir / "alpha" / "alpha.kicad_pro" ).find( "\"REV\": \"B\"" ) != std::string::npos );

    JSON classes = f.Ok( "netclasses_set",
                         { { "netclasses", JSON::array( { { { "name", "Power" }, { "trackWidth", "20mil" } } } ) },
                           { "patterns", JSON::array( { { { "pattern", "/VCC*" }, { "netclass", "Power" } } } ) } } );
    bool found = false;

    for( const JSON& netclass : classes["netclasses"] )
    {
        if( netclass["name"] == "Power" )
        {
            found = true;
            BOOST_CHECK_CLOSE( netclass["trackWidth"].get<double>(), 0.508, 1e-6 );
        }
    }

    BOOST_CHECK( found );
    f.Fail( "netclasses_set", { { "netclasses", JSON::array( { { { "name", "Default" }, { "remove", true } } } ) } } );

    // Undo takes back the net classes, then the text variables.
    f.Ok( "undo" );
    BOOST_CHECK( f.Ok( "netclasses_get" )["netclasses"].size() == 1 );
    f.Ok( "undo" );
    BOOST_CHECK( f.Ok( "text_vars_get" )["vars"].empty() );
    f.Ok( "redo" );
    BOOST_CHECK_EQUAL( f.Ok( "text_vars_get" )["vars"]["REV"], "B" );
}


BOOST_AUTO_TEST_CASE( StubDocumentSavedAfterEveryCallAndUndone )
{
    // Stage 2 acceptance: a stub document changed by three calls is on disk after each call,
    // and three undo calls bring the file back to its original content byte for byte.
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "stub" );
    f.Ok( "project_create", { { "path", ( dir / "stubby" ).string() } } );

    const fs::path    file = dir / "stubby" / "stub.txt";
    const std::string original = "original\r\n\x01 bytes\n";
    writeFile( file, original );

    std::string expected = original;

    for( const char* text : { "one\n", "two\n", "three\n" } )
    {
        JSON result = f.Ok( "stub_append", { { "text", text } } );
        expected += text;
        BOOST_CHECK_EQUAL( readFile( file ), expected );
        BOOST_CHECK_EQUAL( result["saved"][0]["path"], "stub.txt" );
        BOOST_CHECK_EQUAL( result["saved"][0]["version"].get<std::string>(), FILE_STORE::VersionOf( expected ) );
    }

    f.Ok( "undo" );
    f.Ok( "undo" );
    f.Ok( "undo" );
    BOOST_CHECK( readFile( file ) == original );
    f.Fail( "undo" );

    // The session was reloaded: the next call appends to the original.
    f.Ok( "redo" );
    BOOST_CHECK_EQUAL( readFile( file ), original + "one\n" );
    f.Ok( "stub_append", { { "text", "new\n" } } );
    BOOST_CHECK_EQUAL( readFile( file ), original + "one\nnew\n" );

    // A new change clears redo.
    f.Fail( "redo" );
}


BOOST_AUTO_TEST_CASE( ConflictAndFailedSave )
{
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "conflict" );
    f.Ok( "project_create", { { "path", ( dir / "c" ).string() } } );
    const fs::path file = dir / "c" / "stub.txt";

    f.Ok( "stub_append", { { "text", "agent\n" } } );

    // Someone else edits the file: the next call fails and writes nothing.
    writeFile( file, "person\n" );
    std::string error = f.Fail( "stub_append", { { "text", "more\n" } } );
    BOOST_CHECK( error.find( "changed outside the agent" ) != std::string::npos );
    BOOST_CHECK_EQUAL( readFile( file ), "person\n" );

    // The session reloaded the person's version; the agent continues from it.
    f.Ok( "stub_append", { { "text", "agent2\n" } } );
    BOOST_CHECK_EQUAL( readFile( file ), "person\nagent2\n" );

    // force:true overwrites another change.
    writeFile( file, "person again\n" );
    f.Ok( "stub_append", { { "text", "x\n" }, { "force", true } } );
    BOOST_CHECK_EQUAL( readFile( file ), "person\nagent2\nx\n" );

    // A failed save fails the call and memory goes back to the disk content.
    f.Fail( "stub_append", { { "text", "lost\n" }, { "fail", true } } );
    BOOST_CHECK_EQUAL( readFile( file ), "person\nagent2\nx\n" );
    f.Ok( "stub_append", { { "text", "y\n" } } );
    BOOST_CHECK_EQUAL( readFile( file ), "person\nagent2\nx\ny\n" );
}


BOOST_AUTO_TEST_CASE( MessagesReturnedAndLogged )
{
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "messages" );
    f.Ok( "project_create", { { "path", ( dir / "m" ).string() } } );

    JSON result = f.Ok( "stub_append", { { "text", "a" }, { "warn", true } } );
    BOOST_REQUIRE( result.contains( "messages" ) );
    BOOST_CHECK_EQUAL( result["messages"][0]["severity"], "warning" );
    BOOST_CHECK_EQUAL( result["messages"][0]["text"], "stub warning" );

    // A call without messages returns none; the log has the earlier one.
    BOOST_CHECK( !f.Ok( "stub_append", { { "text", "b" } } ).contains( "messages" ) );

    JSON log = f.Ok( "log_get" );
    bool sawWarning = false;

    for( const JSON& record : log["records"] )
        sawWarning |= record["text"] == "stub warning" && record["tool"] == "stub_append";

    BOOST_CHECK( sawWarning );
}


BOOST_AUTO_TEST_CASE( TwoProjectsAtOnce )
{
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "two" );
    f.Ok( "project_create", { { "path", ( dir / "left" ).string() } } );
    f.Ok( "project_create", { { "path", ( dir / "right" ).string() } } );

    BOOST_CHECK_EQUAL( f.Ok( "project_list" )["projects"].size(), 2 );

    f.Ok( "text_vars_set", { { "project", "left" }, { "vars", { { "SIDE", "L" } } } } );
    f.Ok( "text_vars_set", { { "project", "right" }, { "vars", { { "SIDE", "R" } } } } );
    f.Ok( "stub_append", { { "project", "left" }, { "text", "L" }, { "warn", true } } );

    BOOST_CHECK_EQUAL( f.Ok( "text_vars_get", { { "project", "left" } } )["vars"]["SIDE"], "L" );
    BOOST_CHECK_EQUAL( f.Ok( "text_vars_get", { { "project", "right" } } )["vars"]["SIDE"], "R" );
    BOOST_CHECK_EQUAL( readFile( dir / "left" / "stub.txt" ), "L" );
    BOOST_CHECK( !fs::exists( dir / "right" / "stub.txt" ) );

    // Separate logs and undo histories: the warning is only in the left log.
    auto warnings = [&]( const std::string& aProject )
    {
        int count = 0;

        JSON log = f.Ok( "log_get", { { "project", aProject } } );

        for( const JSON& record : log["records"] )
        {
            if( record["severity"].get<std::string>() == "warning" )
                count++;
        }

        return count;
    };

    BOOST_CHECK_EQUAL( warnings( "left" ), 1 );
    BOOST_CHECK_EQUAL( warnings( "right" ), 0 );
    f.Ok( "undo", { { "project", "right" } } );
    BOOST_CHECK( f.Ok( "text_vars_get", { { "project", "right" } } )["vars"].empty() );
    BOOST_CHECK_EQUAL( f.Ok( "text_vars_get", { { "project", "left" } } )["vars"]["SIDE"], "L" );

    // Closing one leaves the other current.
    JSON closed = f.Ok( "project_close", { { "project", "right" } } );
    BOOST_CHECK_EQUAL( closed["current"], "left" );
}


BOOST_AUTO_TEST_CASE( PauseFailsCalls )
{
    HOST_FIXTURE f;
    f.host.SetPaused( true );
    std::string error = f.Fail( "project_list" );
    BOOST_CHECK( error.find( "paused" ) != std::string::npos );
    BOOST_CHECK( !f.Call( "status" )["isError"].get<bool>() );
    f.host.SetPaused( false );
    f.Ok( "project_list" );
}


BOOST_AUTO_TEST_CASE( LockHeldByAnotherProcess )
{
    HOST_FIXTURE f;
    fs::path     dir = freshDir( "lock" );
    f.Ok( "project_create", { { "path", ( dir / "locked" ).string() } } );
    f.Ok( "project_close" );

    const fs::path proFile = dir / "locked" / "locked.kicad_pro";
    int            toChild[2], toParent[2];
    BOOST_REQUIRE( ::pipe( toChild ) == 0 && ::pipe( toParent ) == 0 );

    pid_t child = ::fork();
    BOOST_REQUIRE( child >= 0 );

    if( child == 0 )
    {
        // A second KiCad: hold the project lock until told to stop.
        LOCKFILE lock( wxString::FromUTF8( proFile.string() ) );
        char     ok = lock.Valid() ? '1' : '0';
        (void) !::write( toParent[1], &ok, 1 );
        char done;
        (void) !::read( toChild[0], &done, 1 );
        ::_exit( 0 );
    }

    char locked = 0;
    BOOST_REQUIRE( ::read( toParent[0], &locked, 1 ) == 1 );
    BOOST_REQUIRE_EQUAL( locked, '1' );

    std::string error = f.Fail( "project_open", { { "path", ( dir / "locked" ).string() } } );
    BOOST_CHECK( error.find( "open in another KiCad" ) != std::string::npos );

    // force:true takes it over.
    f.Ok( "project_open", { { "path", ( dir / "locked" ).string() }, { "force", true } } );

    char done = 'x';
    (void) !::write( toChild[1], &done, 1 );
    ::waitpid( child, nullptr, 0 );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( AgenticProgress )


BOOST_AUTO_TEST_CASE( ForwardsProgressAndCancel )
{
    std::vector<JSON> sent;
    auto              state = std::make_shared<REQUEST_STATE>( JSON( 7 ), JSON( "tok" ),
                                                               [&]( const JSON& aMessage )
                                                               {
                                                      sent.push_back( aMessage );
                                                  } );
    TOOL_CONTEXT      context( state );
    double            uiFraction = -1;

    AGENT_PROGRESS_REPORTER reporter( context,
                                      [&]( double aFraction, const std::string& )
                                      {
                                          uiFraction = aFraction;
                                      } );
    reporter.SetNumPhases( 2 );
    reporter.Report( wxS( "filling zones" ) );
    reporter.BeginPhase( 0 );
    reporter.SetCurrentProgress( 0.5 );
    BOOST_CHECK( reporter.KeepRefreshing() );
    BOOST_CHECK_CLOSE( uiFraction, 0.25, 1e-6 );
    reporter.AdvancePhase();
    BOOST_CHECK_CLOSE( reporter.Fraction(), 0.5, 1e-6 );

    BOOST_REQUIRE( !sent.empty() );
    BOOST_CHECK_EQUAL( sent.back()["method"], "notifications/progress" );
    BOOST_CHECK_EQUAL( sent.back()["params"]["progressToken"], "tok" );

    BOOST_CHECK( !reporter.IsCancelled() );
    state->Cancel();
    BOOST_CHECK( reporter.IsCancelled() );
    BOOST_CHECK( !reporter.KeepRefreshing() );
}


BOOST_AUTO_TEST_SUITE_END()
