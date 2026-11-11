// Agentic-KiCad spike S0.e support.

#include "spike_canvas.h"
#include "spike_init.h"

#include <class_draw_panel_gal.h>

#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/utils.h>

#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <wx/filefn.h>
#include <cstring>
#include <set>


EDA_DRAW_PANEL_GAL::GAL_TYPE SpikeGalType( int aArgc, char** aArgv )
{
    for( int i = 1; i < aArgc; ++i )
    {
        if( std::strcmp( aArgv[i], "cairo" ) == 0 )
            return EDA_DRAW_PANEL_GAL::GAL_TYPE_CAIRO;
    }

    return EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL;
}


void SpikePump( int aMs )
{
    const double end = SpikeNowMs() + aMs;

    while( SpikeNowMs() < end )
    {
        wxTheApp->Yield( true );
        wxMilliSleep( 10 );
    }
}


wxImage SpikeCapture( EDA_DRAW_PANEL_GAL* aCanvas, const wxString& aTempDir )
{
    // Grab the X screen from outside the process (xwd), then crop to the canvas.  Reading an
    // OpenGL window back through a wxClientDC returned stale pixels after the first capture.
    static int   count = 0;
    const wxString xwdFile = wxString::Format( wxS( "%s/spike-capture-%d-%d.xwd" ), aTempDir, getpid(), count );
    const wxString pngFile = wxString::Format( wxS( "%s/spike-capture-%d-%d.png" ), aTempDir, getpid(), count++ );
    const wxString cmd = wxString::Format( wxS( "xwd -root -silent -out %s && convert %s %s" ), xwdFile,
                                           xwdFile, pngFile );

    if( std::system( cmd.ToUTF8().data() ) != 0 )
    {
        std::fprintf( stderr, "screen capture failed\n" );
        return wxImage( 1, 1 );
    }

    wxImage screen( pngFile, wxBITMAP_TYPE_PNG );
    wxRemoveFile( xwdFile );
    wxRemoveFile( pngFile );

    const wxPoint origin = aCanvas->GetScreenPosition();
    const wxSize  size = aCanvas->GetClientSize();
    return screen.GetSubImage( wxRect( origin, size ) );
}


SPIKE_DIFF SpikeDiff( const wxImage& aBefore, const wxImage& aAfter, const wxRect& aRegion )
{
    SPIKE_DIFF diff;
    wxRect     region = aRegion.Intersect( wxRect( 0, 0, aBefore.GetWidth(), aBefore.GetHeight() ) );

    diff.regionArea = static_cast<long>( region.width ) * region.height;

    for( int y = 0; y < aBefore.GetHeight(); ++y )
    {
        for( int x = 0; x < aBefore.GetWidth(); ++x )
        {
            const int dr = std::abs( aBefore.GetRed( x, y ) - aAfter.GetRed( x, y ) );
            const int dg = std::abs( aBefore.GetGreen( x, y ) - aAfter.GetGreen( x, y ) );
            const int db = std::abs( aBefore.GetBlue( x, y ) - aAfter.GetBlue( x, y ) );

            if( dr + dg + db < 24 )
                continue;

            if( region.Contains( x, y ) )
                diff.inside++;
            else
                diff.outside++;
        }
    }

    return diff;
}


size_t SpikeColourCount( const wxImage& aImage )
{
    std::set<unsigned long> colours;

    for( int y = 0; y < aImage.GetHeight(); y += 2 )
    {
        for( int x = 0; x < aImage.GetWidth(); x += 2 )
        {
            colours.insert( ( static_cast<unsigned long>( aImage.GetRed( x, y ) ) << 16 )
                            | ( aImage.GetGreen( x, y ) << 8 ) | aImage.GetBlue( x, y ) );
        }
    }

    return colours.size();
}
