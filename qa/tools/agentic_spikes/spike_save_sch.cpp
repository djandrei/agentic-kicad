// Agentic-KiCad spike S0.c (schematic): cost of the per-call save (D5) and of undo (D7).
//
// Usage: spike_save_sch <root.kicad_sch> [repetitions]
//
// Works on the given files in place, so pass a copy.  Each repetition:
//   save:    save every sheet file in place (D5, worst case: a call that touched every
//            sheet; upstream's formatter writes a temp file, fsyncs and renames); the slowest
//            single sheet is
//            reported too (the common case: a call that touched one sheet);
//   reload:  destroy the schematic and load it again with connectivity (undo, D7).

#include "spike_init.h"

#include <api/sch_api_save.h>
#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <pgm_base.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_settings.h>
#include <project.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <schematic.h>

#include <wx/filefn.h>
#include <wx/filename.h>

#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>


static double median( std::vector<double> aValues )
{
    std::sort( aValues.begin(), aValues.end() );
    return aValues[aValues.size() / 2];
}


static long peakRssMb()
{
    struct rusage usage;
    getrusage( RUSAGE_SELF, &usage );
    return usage.ru_maxrss / 1024;
}


/// Save every screen (atomic in upstream's formatter); returns false on the first failure.
static bool saveAll( SCHEMATIC& aSchematic, double& aSlowestSheetMs )
{
    SCH_SCREENS screens( aSchematic.Root() );
    screens.BuildClientSheetPathList();
    aSlowestSheetMs = 0;

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        SCH_SCREEN* screen = screens.GetScreen( i );
        wxFileName  fileName = screen->GetFileName();

        if( !fileName.IsOk() )
            continue;

        const wxString path = fileName.GetFullPath();
        double         t = SpikeNowMs();

        if( !SCH_API_SAVE::SaveSheetToFile( screens.GetSheet( i ), aSchematic, path ) )
        {
            std::fprintf( stderr, "save failed: %s\n", path.ToUTF8().data() );
            return false;
        }

        aSlowestSheetMs = std::max( aSlowestSheetMs, SpikeNowMs() - t );
    }

    return true;
}


int main( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::fprintf( stderr, "usage: spike_save_sch <root.kicad_sch> [repetitions]\n" );
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
    const int      reps = argc > 2 ? std::atoi( argv[2] ) : 3;
    const wxString path = rootFile.GetFullPath();

    double     t = SpikeNowMs();
    SCHEMATIC* schematic = EESCHEMA_HELPERS::LoadSchematic( path, true, false );
    const double firstLoad = SpikeNowMs() - t;

    if( !schematic )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    PROJECT*    project = &schematic->Project();
    SCH_SCREENS screens( schematic->Root() );
    double      totalMb = 0;

    size_t      sheetFiles = 0;

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        wxFileName fn( screens.GetScreen( i )->GetFileName() );

        if( fn.GetFullName().IsEmpty() )
            continue;  // the virtual root screen has no file

        sheetFiles++;
        totalMb += fn.GetSize().ToDouble() / 1e6;
    }

    std::printf( "file %s\n", path.ToUTF8().data() );
    std::printf( "sheet_files %zu sheet_instances %zu total_mb %.1f\n", sheetFiles,
                 schematic->Hierarchy().size(), totalMb );
    std::printf( "first_load_ms %.0f\n", firstLoad );

    std::vector<double> saveMs, sheetMs, reloadMs;

    for( int i = 0; i < reps; ++i )
    {
        double slowestSheet = 0;

        t = SpikeNowMs();

        if( !saveAll( *schematic, slowestSheet ) )
            return 1;

        saveMs.push_back( SpikeNowMs() - t );
        sheetMs.push_back( slowestSheet );

        t = SpikeNowMs();
        delete schematic;
        schematic = EESCHEMA_HELPERS::LoadSchematic( path, true, false, project );
        reloadMs.push_back( SpikeNowMs() - t );

        if( !schematic )
        {
            std::fprintf( stderr, "reload failed\n" );
            return 1;
        }

        std::printf( "rep %d save_all_ms %.0f slowest_sheet_ms %.0f reload_ms %.0f\n", i,
                     saveMs.back(), sheetMs.back(), reloadMs.back() );
    }

    std::printf( "median save_all_ms %.0f slowest_sheet_ms %.0f reload_ms %.0f\n", median( saveMs ),
                 median( sheetMs ), median( reloadMs ) );
    std::printf( "peak_rss_mb %ld\n", peakRssMb() );
    std::printf( "asserts %d\n", g_spikeAsserts );

    delete schematic;
    return 0;
}
