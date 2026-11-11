// Agentic-KiCad spike S0.b: edit a board with no frame, fill zones, run DRC, save.
//
// Usage: spike_frameless_pcb <board.kicad_pcb> <report.json>
//
// Works on the file in place, so pass a copy.  On a frameless TOOL_MANAGER: adds a 2 mm track
// on the copper layer of the first SMD or through-hole pad, starting at that pad, on its net;
// refills every zone through ZONE_FILLER_TOOL (headless, as kicad-cli does); runs DRC_ENGINE and
// writes a JSON report; saves the board.  The caller runs `kicad-cli pcb drc` on the saved file
// and compares the two reports.

#include "spike_init.h"

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <board_loader.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_report.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <kiface_base.h>
#include <pad.h>
#include <pcb_marker.h>
#include <pcb_track.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project.h>
#include <project_pcb.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <tools/zone_filler_tool.h>
#include <wildcards_and_files_ext.h>
#include <widgets/report_severity.h>

#include <wx/filename.h>

#include <cstdio>
#include <memory>


int main( int argc, char** argv )
{
    if( argc < 3 )
    {
        std::fprintf( stderr, "usage: spike_frameless_pcb <board.kicad_pcb> <report.json>\n" );
        return 2;
    }

    if( !SpikeInit( argc, argv ) )
        return 1;

    wxFileName boardFile( wxString::FromUTF8( argv[1] ) );
    boardFile.MakeAbsolute();
    const wxString reportPath = wxString::FromUTF8( argv[2] );

    wxFileName proFile( boardFile );
    proFile.SetExt( FILEEXT::ProjectFileExtension );

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    PCBNEW_SETTINGS*  settings = new PCBNEW_SETTINGS;
    mgr.RegisterSettings( settings, false );
    mgr.Load();
    Kiface().InitSettings( settings );
    mgr.LoadProject( proFile.GetFullPath() );
    PROJECT* project = &mgr.Prj();

    const wxString         path = boardFile.GetFullPath();
    std::unique_ptr<BOARD> board = BOARD_LOADER::Load( path, PCB_IO_MGR::KICAD_SEXP, project );

    if( !board )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto toolMgr = std::make_unique<TOOL_MANAGER>();
    toolMgr->SetEnvironment( board.get(), nullptr, nullptr, settings, nullptr );

    // 1. Add a track through BOARD_COMMIT.
    PAD* start = nullptr;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 && pad->IsOnCopperLayer() )
            {
                start = pad;
                break;
            }
        }

        if( start )
            break;
    }

    if( !start )
    {
        std::fprintf( stderr, "no pad with a net\n" );
        return 1;
    }

    const PCB_LAYER_ID layer = start->IsOnLayer( F_Cu ) ? F_Cu : B_Cu;
    PCB_TRACK*         track = new PCB_TRACK( board.get() );
    track->SetStart( start->GetPosition() );
    track->SetEnd( start->GetPosition() + VECTOR2I( pcbIUScale.mmToIU( 2 ), 0 ) );
    track->SetWidth( board->GetDesignSettings().GetCurrentTrackWidth() );
    track->SetLayer( layer );
    track->SetNet( start->GetNet() );

    double t = SpikeNowMs();
    {
        BOARD_COMMIT commit( toolMgr.get() );
        commit.Add( track );
        commit.Push( wxS( "S0.b track" ) );
    }
    std::printf( "track pad %s.%s net %s layer %s push_ms %.1f\n",
                 start->GetParentFootprint()->GetReference().ToUTF8().data(),
                 start->GetNumber().ToUTF8().data(), start->GetNetname().ToUTF8().data(),
                 board->GetLayerName( layer ).ToUTF8().data(), SpikeNowMs() - t );

    // DRC's library checks need the footprint libraries loaded, as kicad-cli does.
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( project );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    std::shared_ptr<DRC_ENGINE> drcEngine = board->GetDesignSettings().m_DRCEngine;

    if( !drcEngine->RulesValid() )
        drcEngine->InitEngine( board->GetDesignRulesPath() );

    // 2. Refill every zone (headless ZONE_FILLER_TOOL: teardrops, ZONE_FILLER, generators).
    t = SpikeNowMs();
    toolMgr->RegisterTool( new ZONE_FILLER_TOOL );
    toolMgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, nullptr, true );
    std::printf( "zones %zu fill_ms %.1f\n", board->Zones().size(), SpikeNowMs() - t );

    // 3. DRC through DRC_ENGINE, markers committed to the board, as kicad-cli does.
    DS_PROXY_VIEW_ITEM* drawingSheet = new DS_PROXY_VIEW_ITEM( pcbIUScale, &board->GetPageSettings(), project,
                                                               &board->GetTitleBlock(), &board->GetProperties() );
    drawingSheet->SetSheetName( std::string() );
    drawingSheet->SetSheetPath( std::string() );
    drawingSheet->SetIsFirstPage( true );
    drawingSheet->SetFileName( std::string( board->GetFileName().ToUTF8() ) );
    drcEngine->SetDrawingSheet( drawingSheet );

    t = SpikeNowMs();
    BOARD_COMMIT markers( toolMgr.get() );
    drcEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                PCB_MARKER* marker = new PCB_MARKER( aItem, aPos, aLayer );
                aPathGenerator( marker );
                markers.Add( marker );
            } );

    board->RecordDRCExclusions();
    board->DeleteMARKERs( true, true );
    drcEngine->RunTests( EDA_UNITS::MM, false, false );
    drcEngine->ClearViolationHandler();
    markers.Push( wxS( "DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );
    board->ResolveDRCExclusions( false );
    std::printf( "drc_ms %.1f\n", SpikeNowMs() - t );

    const int severities = RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING;
    auto      markersProvider = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_DRC,
                                                                      MARKER_BASE::MARKER_DRAWING_SHEET );
    auto      ratsnestProvider = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_RATSNEST );
    auto      parityProvider = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_PARITY );
    markersProvider->SetSeverities( severities );
    ratsnestProvider->SetSeverities( severities );
    parityProvider->SetSeverities( severities );

    std::printf( "violations %d unconnected %d\n", markersProvider->GetCount(), ratsnestProvider->GetCount() );

    DRC_REPORT report( board.get(), EDA_UNITS::MM, markersProvider, ratsnestProvider, parityProvider );

    if( !report.WriteJsonReport( reportPath ) )
    {
        std::fprintf( stderr, "report failed\n" );
        return 1;
    }

    // 4. Save.
    t = SpikeNowMs();
    wxString target = path;

    if( !BOARD_LOADER::SaveBoard( target, *board ) )
    {
        std::fprintf( stderr, "save failed\n" );
        return 1;
    }

    std::printf( "save_ms %.1f asserts %d\n", SpikeNowMs() - t, g_spikeAsserts );

    drcEngine->SetDrawingSheet( nullptr );
    delete drawingSheet;
    toolMgr.reset();
    board->ClearProject();
    board.reset();
    return 0;
}
