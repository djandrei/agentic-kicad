// Agentic-KiCad spike S0.e (schematic): a schematic canvas in a plain wxFrame draws a sheet
// owned elsewhere and follows a commit made outside the canvas.
//
// Usage: spike_canvas_sch <root.kicad_sch> <output-dir> [cairo]
//
// Same structure as spike_canvas_pcb: the session owns the SCHEMATIC and a frameless
// TOOL_MANAGER, the canvas observes through a SCHEMATIC_LISTENER, and a commit adds a filled
// rectangle to the displayed sheet.  Writes sch-before.png and sch-after.png.

#include "spike_canvas.h"
#include "spike_init.h"

#include <drawing_sheet/ds_proxy_view_item.h>
#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <gal_display_options_common.h>
#include <pgm_base.h>
#include <sch_commit.h>
#include <sch_draw_panel.h>
#include <sch_screen.h>
#include <sch_shape.h>
#include <sch_view.h>
#include <schematic.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_settings.h>
#include <tool/tool_manager.h>

#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/sizer.h>

#include <cstdio>
#include <memory>


/// The canvas side of the observer: mirrors committed changes on the shown sheet into the view.
class VIEW_SYNC : public SCHEMATIC_LISTENER
{
public:
    VIEW_SYNC( SCH_DRAW_PANEL* aCanvas, SCH_SCREEN* aShown ) : m_canvas( aCanvas ), m_shown( aShown ) {}

    void OnSchItemsAdded( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
    {
        for( SCH_ITEM* item : aItems )
        {
            if( m_shown->CheckIfOnDrawList( item ) )
                m_canvas->GetView()->Add( item );
        }

        changed();
    }

    void OnSchItemsRemoved( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
    {
        for( SCH_ITEM* item : aItems )
            m_canvas->GetView()->Remove( item );

        changed();
    }

    void OnSchItemsChanged( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
    {
        for( SCH_ITEM* item : aItems )
            m_canvas->GetView()->Update( item );

        changed();
    }

    int Calls() const { return m_calls; }

private:
    void changed()
    {
        m_calls++;
        m_canvas->ForceRefresh();
    }

    SCH_DRAW_PANEL* m_canvas;
    SCH_SCREEN*     m_shown;
    int             m_calls = 0;
};


int main( int argc, char** argv )
{
    if( argc < 3 )
    {
        std::fprintf( stderr, "usage: spike_canvas_sch <root.kicad_sch> <output-dir> [cairo]\n" );
        return 2;
    }

    if( !SpikeInitGui( argc, argv ) )
        return 1;

    SETTINGS_MANAGER&  mgr = Pgm().GetSettingsManager();
    EESCHEMA_SETTINGS* settings = new EESCHEMA_SETTINGS;
    mgr.RegisterSettings( new KICAD_SETTINGS, false );
    mgr.RegisterSettings( settings, false );
    mgr.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
    mgr.Load();

    // The schematic painter reads its settings from Kiface().KifaceSettings(); inside the
    // eeschema kiface they are set by the kiface, here the mock kiface needs them.
    Kiface().InitSettings( settings );

    wxFileName rootFile( wxString::FromUTF8( argv[1] ) );
    rootFile.MakeAbsolute();
    const wxString outDir = wxString::FromUTF8( argv[2] );
    const auto     galType = SpikeGalType( argc, argv );

    // The session: owns the document and a frameless tool manager.
    std::unique_ptr<SCHEMATIC> schematic(
            EESCHEMA_HELPERS::LoadSchematic( rootFile.GetFullPath(), true, false ) );

    if( !schematic )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto toolMgr = std::make_unique<TOOL_MANAGER>();
    toolMgr->SetEnvironment( schematic.get(), nullptr, nullptr, settings, nullptr );

    SCH_SCREEN* screen = schematic->RootScreen();

    // The viewer: a plain frame with the schematic canvas, drawing the session's sheet.
    wxFrame* frame = new wxFrame( nullptr, wxID_ANY, wxS( "S0.e schematic" ), wxDefaultPosition,
                                  wxSize( 1200, 900 ) );
    GAL_DISPLAY_OPTIONS_IMPL options;
    SCH_DRAW_PANEL*          canvas =
            new SCH_DRAW_PANEL( frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, options, galType );

    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    sizer->Add( canvas, 1, wxEXPAND );
    frame->SetSizer( sizer );
    frame->Show();
    SpikePump( 300 );

    std::printf( "gal %s (requested %s)\n",
                 canvas->GetBackend() == EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL ? "opengl" : "cairo",
                 galType == EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL ? "opengl" : "cairo" );

    canvas->DisplaySheet( screen );

    const BOX2I bbox = canvas->GetView()->GetDrawingSheet()
                               ? canvas->GetView()->GetDrawingSheet()->ViewBBox()
                               : BOX2I( VECTOR2I( 0, 0 ), screen->GetPageSettings().GetSizeIU(
                                                                  schIUScale.IU_PER_MILS ) );
    canvas->GetView()->SetViewport( BOX2D( bbox.GetOrigin(), bbox.GetSize() ) );
    canvas->StartDrawing();
    SpikePump( 500 );

    // Fit again once the frame has its final size; the first fit used the initial size.
    canvas->GetView()->SetViewport( BOX2D( bbox.GetOrigin(), bbox.GetSize() ) );
    canvas->ForceRefresh();
    SpikePump( 1500 );

    VIEW_SYNC sync( canvas, screen );
    schematic->AddListener( &sync );

    wxImage before = SpikeCapture( canvas, outDir );
    before.SaveFile( outDir + wxS( "/sch-before.png" ), wxBITMAP_TYPE_PNG );

    // A commit made by the session, not by the canvas.
    const VECTOR2I centre = bbox.GetCenter();
    const VECTOR2I half( bbox.GetWidth() / 6, bbox.GetHeight() / 6 );
    SCH_SHAPE*     rect = new SCH_SHAPE( SHAPE_T::RECTANGLE, LAYER_NOTES );
    rect->SetStart( centre - half );
    rect->SetEnd( centre + half );
    rect->SetFillMode( FILL_T::FILLED_SHAPE );
    rect->SetWidth( schIUScale.MilsToIU( 20 ) );

    // SCH_COMMIT::Push finds the schematic to notify through the first item's parents, before
    // it appends the item: a new item must already have its screen as parent.
    rect->SetParent( screen );

    SCH_COMMIT commit( toolMgr.get() );
    commit.Add( rect, screen );
    double t = SpikeNowMs();
    commit.Push( wxS( "S0.e rectangle" ) );
    const double pushMs = SpikeNowMs() - t;

    SpikePump( 1500 );

    wxImage after = SpikeCapture( canvas, outDir );
    after.SaveFile( outDir + wxS( "/sch-after.png" ), wxBITMAP_TYPE_PNG );

    const VECTOR2D a = canvas->GetView()->ToScreen( VECTOR2D( centre - half ) );
    const VECTOR2D b = canvas->GetView()->ToScreen( VECTOR2D( centre + half ) );
    wxRect         region( wxPoint( KiROUND( std::min( a.x, b.x ) ), KiROUND( std::min( a.y, b.y ) ) ),
                           wxPoint( KiROUND( std::max( a.x, b.x ) ), KiROUND( std::max( a.y, b.y ) ) ) );
    region.Deflate( 3 );

    const SPIKE_DIFF diff = SpikeDiff( before, after, region );
    const size_t     colours = SpikeColourCount( before );
    const double     insideFraction = diff.regionArea ? double( diff.inside ) / diff.regionArea : 0;

    std::printf( "canvas %dx%d colours_before %zu\n", before.GetWidth(), before.GetHeight(), colours );
    std::printf( "listener_calls %d push_ms %.1f item_in_view %d on_screen %d\n", sync.Calls(), pushMs,
                 rect->viewPrivData() != nullptr, screen->CheckIfOnDrawList( rect ) );
    std::printf( "region %dx%d changed_inside %ld (%.0f%%) changed_outside %ld\n", region.width,
                 region.height, diff.inside, 100 * insideFraction, diff.outside );

    const bool pass = colours > 8 && sync.Calls() >= 1 && insideFraction > 0.5;
    std::printf( "%s\n", pass ? "PASS" : "FAIL" );

    schematic->RemoveListener( &sync );
    // The view must let go of the document's items before either is destroyed.
    canvas->GetView()->Clear();

    // Delete the viewer before the document: the view holds pointers to the document's items.
    delete frame;
    schematic.reset();
    return pass ? 0 : 1;
}
