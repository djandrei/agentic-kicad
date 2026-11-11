// Agentic-KiCad spike support: start KiCad the way the upstream QA test modules do.

#include "spike_init.h"

#include <kiplatform/app.h>
#include <mock_pgm_base.h>
#include <pgm_base.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <settings/settings_manager.h>

#include <wx/app.h>
#include <wx/init.h>
#include <wx/image.h>

#include <wx/debug.h>

#include <sys/prctl.h>

#include <chrono>
#include <cstdio>


int g_spikeAsserts = 0;


bool SpikeInit( int aArgc, char** aArgv )
{
    prctl( PR_SET_PTRACER, PR_SET_PTRACER_ANY ); // let a watchdog gdb attach to a hung spike
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    KI_TEST::SetMockConfigDir();
    SetPgm( new MOCK_PGM_BASE() );
    KIPLATFORM::APP::Init();

    wxApp::SetInstance( new wxAppConsole );

    if( !wxInitialize( aArgc, aArgv ) )
    {
        std::fprintf( stderr, "wxInitialize failed\n" );
        return false;
    }

    // Count asserts and print only the first: the default console handler prints a call stack
    // for each one, which would dominate the timings.
    wxSetAssertHandler(
            []( const wxString& aFile, int aLine, const wxString& aFunc, const wxString& aCond,
                const wxString& aMsg )
            {
                if( g_spikeAsserts++ == 0 )
                {
                    std::fprintf( stderr, "first ASSERT %s:%d %s: %s %s\n", aFile.ToUTF8().data(), aLine,
                                  aFunc.ToUTF8().data(), aCond.ToUTF8().data(), aMsg.ToUTF8().data() );
                }
            } );

    Pgm().InitPgm( true, true );
    return true;
}


bool SpikeInitGui( int aArgc, char** aArgv )
{
    prctl( PR_SET_PTRACER, PR_SET_PTRACER_ANY ); // let a watchdog gdb attach to a hung spike
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
    KI_TEST::SetMockConfigDir();
    SetPgm( new MOCK_PGM_BASE() );
    KIPLATFORM::APP::Init();

    wxApp::SetInstance( new wxApp );

    if( !wxEntryStart( aArgc, aArgv ) )
    {
        std::fprintf( stderr, "wxEntryStart failed (no display?)\n" );
        return false;
    }

    // An assert in a GUI program opens a modal dialog and blocks the spike: print and go on.
    wxSetAssertHandler(
            []( const wxString& aFile, int aLine, const wxString& aFunc, const wxString& aCond,
                const wxString& aMsg )
            {
                std::fprintf( stderr, "ASSERT %s:%d %s: %s %s\n", aFile.ToUTF8().data(), aLine,
                              aFunc.ToUTF8().data(), aCond.ToUTF8().data(), aMsg.ToUTF8().data() );
            } );

    wxInitAllImageHandlers();
    Pgm().InitPgm( false, true );
    return true;
}


double SpikeNowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>( steady_clock::now().time_since_epoch() ).count();
}
