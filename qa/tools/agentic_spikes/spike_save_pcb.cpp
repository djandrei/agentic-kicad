// Agentic-KiCad spike S0.c (board): cost of the per-call save (D5) and of undo (D7).
//
// Usage: spike_save_pcb <board.kicad_pcb> [repetitions]
//
// Works on the given file in place, so pass a copy.  Each repetition does what a mutating
// call and an undo will do:
//   history: copy the current file into a history folder (D7);
//   save:    save the board to <file> (D5).  Upstream's PRETTIFIED_FILE_OUTPUTFORMATTER
//            already writes a sibling temp file, fsyncs it and renames it over the target.
//   reload:  destroy the in-memory board and load it again from disk (undo, D7).

#include "spike_init.h"

#include <board.h>
#include <board_loader.h>
#include <footprint.h>
#include <pcb_track.h>
#include <kiface_base.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>

#include <wx/filefn.h>
#include <wx/filename.h>

#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
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


int main( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::fprintf( stderr, "usage: spike_save_pcb <board.kicad_pcb> [repetitions]\n" );
        return 2;
    }

    if( !SpikeInit( argc, argv ) )
        return 1;

    wxFileName boardFile( wxString::FromUTF8( argv[1] ) );
    boardFile.MakeAbsolute();
    const int reps = argc > 2 ? std::atoi( argv[2] ) : 3;

    wxFileName proFile( boardFile );
    proFile.SetExt( FILEEXT::ProjectFileExtension );

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    PCBNEW_SETTINGS*  settings = new PCBNEW_SETTINGS;
    mgr.RegisterSettings( settings, false );
    mgr.Load();
    Kiface().InitSettings( settings );
    mgr.LoadProject( proFile.GetFullPath() );
    PROJECT* project = &mgr.Prj();

    const wxString path = boardFile.GetFullPath();
    wxFileName     historyDir( boardFile.GetPath(), wxEmptyString );
    historyDir.AppendDir( wxS( ".agentic" ) );
    historyDir.AppendDir( wxS( "history" ) );
    historyDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    double                 t = SpikeNowMs();
    std::unique_ptr<BOARD> board = BOARD_LOADER::Load( path, PCB_IO_MGR::KICAD_SEXP, project );
    const double           firstLoad = SpikeNowMs() - t;

    if( !board )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    size_t tracks = 0;

    for( PCB_TRACK* track : board->Tracks() )
    {
        ( void ) track;
        tracks++;
    }

    std::printf( "file %s\n", path.ToUTF8().data() );
    std::printf( "size_mb %.1f\n", boardFile.GetSize().ToDouble() / 1e6 );
    std::printf( "footprints %zu tracks_and_vias %zu zones %zu nets %u\n", board->Footprints().size(),
                 tracks, board->Zones().size(), board->GetNetCount() );
    std::printf( "first_load_ms %.0f\n", firstLoad );

    std::vector<double> historyMs, saveMs, reloadMs;

    for( int i = 0; i < reps; ++i )
    {
        wxFileName historyFile( historyDir.GetPath(), wxString::Format( wxS( "v%d.kicad_pcb" ), i ) );

        t = SpikeNowMs();

        if( !wxCopyFile( path, historyFile.GetFullPath(), true ) )
        {
            std::fprintf( stderr, "history copy failed\n" );
            return 1;
        }

        historyMs.push_back( SpikeNowMs() - t );

        t = SpikeNowMs();
        wxString target = path;

        if( !BOARD_LOADER::SaveBoard( target, *board ) )
        {
            std::fprintf( stderr, "save failed\n" );
            return 1;
        }

        saveMs.push_back( SpikeNowMs() - t );

        t = SpikeNowMs();
        board->ClearProject();
        board.reset();
        board = BOARD_LOADER::Load( path, PCB_IO_MGR::KICAD_SEXP, project );
        reloadMs.push_back( SpikeNowMs() - t );

        if( !board )
        {
            std::fprintf( stderr, "reload failed\n" );
            return 1;
        }

        std::printf( "rep %d history_ms %.0f save_ms %.0f reload_ms %.0f\n", i, historyMs.back(),
                     saveMs.back(), reloadMs.back() );
    }

    std::printf( "median history_ms %.0f save_ms %.0f reload_ms %.0f\n", median( historyMs ),
                 median( saveMs ), median( reloadMs ) );
    std::printf( "peak_rss_mb %ld\n", peakRssMb() );
    std::printf( "asserts %d\n", g_spikeAsserts );

    board->ClearProject();
    board.reset();
    return 0;
}
