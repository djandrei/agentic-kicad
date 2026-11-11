// Agentic-KiCad spike S0.a: edit a schematic with no frame.
//
// Usage: spike_frameless_sch <root.kicad_sch> <lib-symbol-name> <net-name>
//
// Works on the file in place, so pass a copy.  Loads the schematic, takes <lib-symbol-name>
// (for example "ecc83-pp:R") from the root sheet's embedded symbol cache, places a new instance
// to the right of the existing drawing, draws a wire from its first pin and puts a local label
// <net-name> on the wire end, all in one SCH_COMMIT on a frameless TOOL_MANAGER.  The commit
// recomputes connectivity; the spike checks that the net exists and holds the new pin, then
// saves the root sheet.  The caller checks the saved file with kicad-cli.

#include "spike_init.h"

#include <api/sch_api_save.h>
#include <connectivity/conn_facade.h>
#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <lib_symbol.h>
#include <pgm_base.h>
#include <sch_commit.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_settings.h>
#include <tool/tool_manager.h>

#include <wx/filename.h>

#include <cstdio>
#include <cstdlib>
#include <memory>


static int snap( int aValue )
{
    const int grid = schIUScale.MilsToIU( 50 );
    return ( aValue / grid ) * grid;
}


int main( int argc, char** argv )
{
    if( argc < 4 )
    {
        std::fprintf( stderr, "usage: spike_frameless_sch <root.kicad_sch> <lib-symbol> <net>\n" );
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
    const wxString libName = wxString::FromUTF8( argv[2] );
    const wxString netName = wxString::FromUTF8( argv[3] );

    std::unique_ptr<SCHEMATIC> schematic(
            EESCHEMA_HELPERS::LoadSchematic( rootFile.GetFullPath(), true, false ) );

    if( !schematic )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto toolMgr = std::make_unique<TOOL_MANAGER>();
    toolMgr->SetEnvironment( schematic.get(), nullptr, nullptr, settings, nullptr );

    SCH_SHEET_LIST hierarchy = schematic->Hierarchy();
    SCH_SHEET_PATH path = hierarchy.at( 0 );
    SCH_SCREEN*    screen = path.LastScreen();

    auto libIt = screen->GetLibSymbols().find( libName );

    if( libIt == screen->GetLibSymbols().end() )
    {
        std::fprintf( stderr, "no cached library symbol %s\n", libName.ToUTF8().data() );
        return 1;
    }

    // Place to the right of everything already on the sheet, on the 50 mil grid.
    BOX2I extents;

    for( SCH_ITEM* item : screen->Items() )
        extents.Merge( item->GetBoundingBox() );

    const VECTOR2I origin( snap( extents.GetRight() + schIUScale.mmToIU( 20 ) ),
                           snap( extents.GetCenter().y ) );

    LIB_ID      libId;
    libId.Parse( libName );
    SCH_SYMBOL* symbol = new SCH_SYMBOL( *libIt->second, libId, &path, 1, 0, origin, screen );
    symbol->SetRef( &path, wxS( "R900" ) );
    symbol->SetValueFieldText( wxS( "10k" ) );

    std::vector<SCH_PIN*> pins = symbol->GetPins( &path );

    if( pins.empty() )
    {
        std::fprintf( stderr, "symbol has no pins\n" );
        return 1;
    }

    const VECTOR2I pinPos = pins[0]->GetPosition();
    const VECTOR2I wireEnd = pinPos + VECTOR2I( 0, -schIUScale.MilsToIU( 300 ) );

    SCH_LINE* wire = new SCH_LINE( pinPos, LAYER_WIRE );
    wire->SetEndPoint( wireEnd );
    wire->SetParent( screen );

    SCH_LABEL* label = new SCH_LABEL( wireEnd, netName );
    label->SetParent( screen );

    double     t = SpikeNowMs();
    SCH_COMMIT commit( toolMgr.get() );
    commit.Add( symbol, screen );
    commit.Add( wire, screen );
    commit.Add( label, screen );
    commit.Push( wxS( "S0.a symbol, wire and label" ) );
    const double pushMs = SpikeNowMs() - t;

    // Connectivity was recomputed by the commit.  Upstream's default engine is the
    // SCH_CONNECTIVITY facade (ADVANCED_CFG m_ConnectivityEngine), not CONNECTION_GRAPH, so the
    // pin's net is read through SCHEMATIC::Connectivity().
    const auto     pinConn = schematic->Connectivity().Connection( pins[0]->m_Uuid, path.Path() );
    const wxString pinNet = pinConn ? pinConn->Name() : wxString( wxS( "<none>" ) );
    const auto     net = schematic->Connectivity().NetByName( wxS( "/" ) + netName );

    std::printf( "push_ms %.1f pin_net %s net_found %d nets %zu\n", pushMs, pinNet.ToUTF8().data(),
                 net.has_value(), schematic->Connectivity().NetNames().size() );

    t = SpikeNowMs();

    if( !SCH_API_SAVE::SaveSheetToFile( path.Last(), *schematic, rootFile.GetFullPath() ) )
    {
        std::fprintf( stderr, "save failed\n" );
        return 1;
    }

    std::printf( "save_ms %.1f asserts %d\n", SpikeNowMs() - t, g_spikeAsserts );

    const bool pass = net.has_value() && pinNet.EndsWith( netName ) && g_spikeAsserts == 0;
    std::printf( "%s\n", pass ? "PASS (in process; check the file with kicad-cli)" : "FAIL" );
    return pass ? 0 : 1;
}
