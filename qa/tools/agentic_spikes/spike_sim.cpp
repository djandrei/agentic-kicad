// Agentic-KiCad spike S0.g: run a transient simulation in-process, no simulator frame.
//
// Usage: spike_sim <root.kicad_sch> [sim-command]
//
// Loads the schematic, builds the circuit model the simulator frame uses (SPICE_CIRCUIT_MODEL),
// takes the simulation command from the sheet's text directives unless one is given, attaches
// it to SPICE_SIMULATOR::CreateInstance( "ngspice" ), runs it on ngspice's background thread and
// waits, then reads the result vectors.

#include "spike_init.h"

#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <pgm_base.h>
#include <reporter.h>
#include <project/project_file.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <sim/spice_settings.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <sim/ngspice.h>
#include <sim/spice_circuit_model.h>
#include <sim/spice_simulator.h>
#include <symbol_editor/symbol_editor_settings.h>

#include <wx/filename.h>
#include <wx/utils.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>


int main( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::fprintf( stderr, "usage: spike_sim <root.kicad_sch> [sim-command]\n" );
        return 2;
    }

    if( !SpikeInit( argc, argv ) )
        return 1;

    SETTINGS_MANAGER&  mgr = Pgm().GetSettingsManager();
    EESCHEMA_SETTINGS* settings = new EESCHEMA_SETTINGS;
    mgr.RegisterSettings( new KICAD_SETTINGS, false );
    mgr.RegisterSettings( settings, false );
    mgr.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
    mgr.Load();
    Kiface().InitSettings( settings );

    wxFileName rootFile( wxString::FromUTF8( argv[1] ) );
    rootFile.MakeAbsolute();

    std::unique_ptr<SCHEMATIC> schematic(
            EESCHEMA_HELPERS::LoadSchematic( rootFile.GetFullPath(), true, false ) );

    if( !schematic )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto model = std::make_shared<SPICE_CIRCUIT_MODEL>( schematic.get(), nullptr );

    // The model collects its sheets and directives when it reads the schematic; until then
    // GetSchTextSimCommand() sees no sheets.  The simulator frame does the same.
    WX_STRING_REPORTER readReporter;
    model->ReadSchematicAndLibraries( NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS, readReporter );
    wxString command = argc > 2 ? wxString::FromUTF8( argv[2] ) : model->GetSchTextSimCommand();

    std::printf( "command %s\n", command.ToUTF8().data() );

    std::shared_ptr<SPICE_SIMULATOR> simulator = SPICE_SIMULATOR::CreateInstance( "ngspice" );

    if( !simulator )
    {
        std::fprintf( stderr, "no ngspice\n" );
        return 1;
    }

    // ngspice reads its compatibility mode from the project's simulator settings; the frame
    // assigns them before attaching (and defaults to LTspice/PSpice compatibility without a
    // workbook).  Without them NGSPICE::GetSettingCommands asserts.
    std::shared_ptr<NGSPICE_SETTINGS> ngspiceSettings =
            schematic->Project().GetProjectFile().m_SchematicSettings->m_NgspiceSettings;

    if( ngspiceSettings->GetWorkbookFilename().IsEmpty() )
        ngspiceSettings->SetCompatibilityMode( NGSPICE_COMPATIBILITY_MODE::LT_PSPICE );

    simulator->Settings() = ngspiceSettings;
    simulator->Init();

    WX_STRING_REPORTER reporter;
    WX_STRING_REPORTER console;   // ngspice's own output, written from its thread
    simulator->SetReporter( &console );
    double             t = SpikeNowMs();

    if( !simulator->Attach( model, command, NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS,
                            schematic->Project().GetProjectPath(), reporter ) )
    {
        std::fprintf( stderr, "attach failed: %s\n", reporter.GetMessages().ToUTF8().data() );
        return 1;
    }

    const double attachMs = SpikeNowMs() - t;

    t = SpikeNowMs();

    if( !simulator->Run() )
    {
        std::fprintf( stderr, "run failed\n" );
        return 1;
    }

    // ngspice runs on its own thread; the MCP host will wait the same way (with progress and
    // cancel), without blocking its event loop for the whole run.
    do
    {
        wxMilliSleep( 20 );
    } while( simulator->IsRunning() );

    const double runMs = SpikeNowMs() - t;

    std::vector<std::string> vectors = simulator->AllVectors();
    std::printf( "attach_ms %.1f run_ms %.1f vectors %zu\n", attachMs, runMs, vectors.size() );

    size_t points = 0;

    for( const std::string& name : vectors )
    {
        // AC results are complex; read their gain (dB).
        std::vector<double> values = simulator->GetRealVector( name );

        if( values.empty() )
            values = simulator->GetGainVector( name );

        if( values.empty() )
            continue;

        points = std::max( points, values.size() );
        const auto [lo, hi] = std::minmax_element( values.begin(), values.end() );
        std::printf( "  %-24s points %6zu min %12.6g max %12.6g\n", name.c_str(), values.size(), *lo, *hi );
    }

    const wxString netlistErrors = reporter.GetMessages();

    if( !netlistErrors.IsEmpty() )
        std::printf( "netlist messages:\n%s\n", netlistErrors.ToUTF8().data() );

    simulator->SetReporter( nullptr );

    if( std::getenv( "SPIKE_CONSOLE" ) || vectors.size() <= 1 || points <= 10 )
        std::printf( "ngspice console:\n%s\n", console.GetMessages().ToUTF8().data() );

    std::printf( "asserts %d\n", g_spikeAsserts );

    const bool pass = vectors.size() > 1 && points > 10;
    std::printf( "%s\n", pass ? "PASS" : "FAIL" );

    return pass ? 0 : 1;
}
