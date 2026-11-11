// Agentic-KiCad spike S0.e support: drive a canvas in a plain wxFrame and compare captures.

#ifndef SPIKE_CANVAS_H
#define SPIKE_CANVAS_H

#include <gal/gal_display_options.h>
#include <class_draw_panel_gal.h>

#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/string.h>

class EDA_DRAW_PANEL_GAL;


/// GAL backend from the command line: "opengl" (default) or "cairo".
EDA_DRAW_PANEL_GAL::GAL_TYPE SpikeGalType( int aArgc, char** aArgv );

/// Let the event loop run for about aMs milliseconds so the canvas repaints.
void SpikePump( int aMs );

/// Capture the canvas from the X screen (xwd), using aTempDir for the intermediate files.
wxImage SpikeCapture( EDA_DRAW_PANEL_GAL* aCanvas, const wxString& aTempDir );

/// Pixels that differ between two captures, counted inside aRegion and outside it.
struct SPIKE_DIFF
{
    long inside = 0;
    long outside = 0;
    long regionArea = 0;
};

SPIKE_DIFF SpikeDiff( const wxImage& aBefore, const wxImage& aAfter, const wxRect& aRegion );

/// Count of distinct colours, to tell a drawn canvas from a blank one.
size_t SpikeColourCount( const wxImage& aImage );

#endif
