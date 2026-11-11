// Agentic-KiCad spike S0.e (board): a PCB canvas in a plain wxFrame draws a board owned
// elsewhere and follows a commit made outside the canvas.
//
// Usage: spike_canvas_pcb <board.kicad_pcb> <output-dir> [cairo]
//
// The "session" owns the board and a frameless TOOL_MANAGER, as HEADLESS_PCB_CONTEXT does.
// The canvas is created in a plain wxFrame and only observes: a BOARD_LISTENER adds, removes
// and updates view items when the session commits.  The program captures the canvas before
// and after a commit that adds a filled rectangle, and checks that the pixels inside the
// rectangle changed.  Writes before.png and after.png to <output-dir>.

#include "spike_canvas.h"
#include "spike_init.h"

#include <board.h>
#include <board_commit.h>
#include <board_loader.h>
#include <gal_display_options_common.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_shape.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/cvpcb_settings.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <view/view.h>
#include <wildcards_and_files_ext.h>

#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/sizer.h>

#include <cstdio>
#include <memory>


/// The canvas side of the observer: mirrors committed changes into the canvas's view.
class VIEW_SYNC : public BOARD_LISTENER
{
public:
    explicit VIEW_SYNC( PCB_DRAW_PANEL_GAL* aCanvas ) : m_canvas( aCanvas ) {}

    void OnBoardCompositeUpdate( BOARD& aBoard, std::vector<BOARD_ITEM*>& aAdded,
                                 std::vector<BOARD_ITEM*>& aRemoved,
                                 std::vector<BOARD_ITEM*>& aChanged ) override
    {
        KIGFX::VIEW* view = m_canvas->GetView();

        for( BOARD_ITEM* item : aRemoved )
            view->Remove( item );

        for( BOARD_ITEM* item : aAdded )
            view->Add( item );

        for( BOARD_ITEM* item : aChanged )
            view->Update( item );

        m_calls++;
        m_canvas->ForceRefresh();
    }

    int Calls() const { return m_calls; }

private:
    PCB_DRAW_PANEL_GAL* m_canvas;
    int                 m_calls = 0;
};


int main( int argc, char** argv )
{
    if( argc < 3 )
    {
        std::fprintf( stderr, "usage: spike_canvas_pcb <board.kicad_pcb> <output-dir> [cairo]\n" );
        return 2;
    }

    if( !SpikeInitGui( argc, argv ) )
        return 1;

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    PCBNEW_SETTINGS*  settings = new PCBNEW_SETTINGS;
    mgr.RegisterSettings( settings, false );
    // A canvas whose parent is not a KiCad frame paints as a footprint preview, which reads
    // the CvPcb display settings (pcb_painter.cpp).
    mgr.RegisterSettings( new CVPCB_SETTINGS, false );
    mgr.Load();

    wxFileName boardFile( wxString::FromUTF8( argv[1] ) );
    boardFile.MakeAbsolute();
    wxFileName proFile( boardFile );
    proFile.SetExt( FILEEXT::ProjectFileExtension );
    mgr.LoadProject( proFile.GetFullPath() );

    const wxString outDir = wxString::FromUTF8( argv[2] );
    const auto     galType = SpikeGalType( argc, argv );

    // The session: owns the document and a frameless tool manager.
    std::unique_ptr<BOARD> board =
            BOARD_LOADER::Load( boardFile.GetFullPath(), PCB_IO_MGR::KICAD_SEXP, &mgr.Prj() );

    if( !board )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto toolMgr = std::make_unique<TOOL_MANAGER>();
    toolMgr->SetEnvironment( board.get(), nullptr, nullptr, settings, nullptr );

    // The viewer: a plain frame with the PCB canvas, drawing the session's board.
    wxFrame* frame = new wxFrame( nullptr, wxID_ANY, wxS( "S0.e board" ), wxDefaultPosition,
                                  wxSize( 1200, 900 ) );
    GAL_DISPLAY_OPTIONS_IMPL options;
    PCB_DRAW_PANEL_GAL*      canvas =
            new PCB_DRAW_PANEL_GAL( frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, options, galType );

    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    sizer->Add( canvas, 1, wxEXPAND );
    frame->SetSizer( sizer );
    frame->Show();
    SpikePump( 300 );

    std::printf( "gal %s (requested %s)\n",
                 canvas->GetBackend() == EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL ? "opengl" : "cairo",
                 galType == EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL ? "opengl" : "cairo" );

    canvas->DisplayBoard( board.get() );
    canvas->SyncLayersVisibility( board.get() );
    canvas->UpdateColors();

    const BOX2I bbox = board->ComputeBoundingBox( true );
    canvas->GetView()->SetViewport( BOX2D( bbox.GetOrigin(), bbox.GetSize() ) );
    canvas->StartDrawing();
    SpikePump( 500 );

    // Fit again once the frame has its final size; the first fit used the initial size.
    canvas->GetView()->SetViewport( BOX2D( bbox.GetOrigin(), bbox.GetSize() ) );
    canvas->ForceRefresh();
    SpikePump( 1500 );

    VIEW_SYNC sync( canvas );
    board->AddListener( &sync );

    wxImage before = SpikeCapture( canvas, outDir );
    before.SaveFile( outDir + wxS( "/pcb-before.png" ), wxBITMAP_TYPE_PNG );

    // A commit made by the session, not by the canvas.
    const VECTOR2I centre = bbox.GetCenter();
    const VECTOR2I half( bbox.GetWidth() / 6, bbox.GetHeight() / 6 );
    PCB_SHAPE*     rect = new PCB_SHAPE( board.get(), SHAPE_T::RECTANGLE );
    rect->SetStart( centre - half );
    rect->SetEnd( centre + half );
    rect->SetLayer( Dwgs_User );
    rect->SetFilled( true );
    rect->SetWidth( pcbIUScale.mmToIU( 0.5 ) );

    BOARD_COMMIT commit( toolMgr.get(), true, false );
    commit.Add( rect );
    double t = SpikeNowMs();
    commit.Push( wxS( "S0.e rectangle" ) );
    const double pushMs = SpikeNowMs() - t;

    SpikePump( 1500 );

    wxImage after = SpikeCapture( canvas, outDir );
    after.SaveFile( outDir + wxS( "/pcb-after.png" ), wxBITMAP_TYPE_PNG );

    const VECTOR2D a = canvas->GetView()->ToScreen( VECTOR2D( centre - half ) );
    const VECTOR2D b = canvas->GetView()->ToScreen( VECTOR2D( centre + half ) );
    wxRect         region( wxPoint( KiROUND( std::min( a.x, b.x ) ), KiROUND( std::min( a.y, b.y ) ) ),
                           wxPoint( KiROUND( std::max( a.x, b.x ) ), KiROUND( std::max( a.y, b.y ) ) ) );
    region.Deflate( 3 );

    const SPIKE_DIFF diff = SpikeDiff( before, after, region );
    const size_t     colours = SpikeColourCount( before );
    const double     insideFraction = diff.regionArea ? double( diff.inside ) / diff.regionArea : 0;

    std::printf( "canvas %dx%d colours_before %zu\n", before.GetWidth(), before.GetHeight(), colours );
    std::printf( "listener_calls %d push_ms %.1f item_in_view %d\n", sync.Calls(), pushMs,
                 rect->viewPrivData() != nullptr );
    std::printf( "region %dx%d changed_inside %ld (%.0f%%) changed_outside %ld\n", region.width,
                 region.height, diff.inside, 100 * insideFraction, diff.outside );

    // Control: force a full redraw, to tell "not propagated" from "not painted".
    canvas->GetView()->MarkDirty();
    canvas->ForceRefresh();
    SpikePump( 1500 );
    wxImage full = SpikeCapture( canvas, outDir );
    full.SaveFile( outDir + wxS( "/pcb-after-full-redraw.png" ), wxBITMAP_TYPE_PNG );
    const SPIKE_DIFF fullDiff = SpikeDiff( before, full, region );
    std::printf( "after_full_redraw changed_inside %ld changed_outside %ld\n", fullDiff.inside,
                 fullDiff.outside );

    // Control 2: a different viewport (zoomed in on the rectangle); any repaint changes pixels.
    canvas->GetView()->SetViewport( BOX2D( VECTOR2D( centre - half * 2 ), VECTOR2D( half * 4 ) ) );
    canvas->ForceRefresh();
    SpikePump( 1500 );
    wxImage refit = SpikeCapture( canvas, outDir );
    refit.SaveFile( outDir + wxS( "/pcb-after-refit.png" ), wxBITMAP_TYPE_PNG );
    const SPIKE_DIFF refitDiff = SpikeDiff( before, refit, region );
    std::printf( "after_refit changed_inside %ld changed_outside %ld\n", refitDiff.inside,
                 refitDiff.outside );

    {
        const BOX2I vb = rect->ViewBBox();
        std::printf( "rect bbox %d,%d %dx%d visible %d layers", vb.GetX(), vb.GetY(), vb.GetWidth(),
                     vb.GetHeight(), canvas->GetView()->IsVisible( rect ) );

        for( int l : rect->ViewGetLayers() )
            std::printf( " %d(vis %d)", l, canvas->GetView()->IsLayerVisible( l ) );

        std::printf( "\nboard_has_rect %d dwgs_user_visible_on_board %d\n",
                     board->ResolveItem( rect->m_Uuid ) == rect, board->IsLayerVisible( Dwgs_User ) );
    }

    std::printf( "layer_enabled %d layer_visible_in_view %d view_dirty_after %d\n",
                 board->IsLayerEnabled( rect->GetLayer() ),
                 canvas->GetView()->IsLayerVisible( rect->GetLayer() ), canvas->GetView()->IsDirty() );

    const bool pass = colours > 8 && sync.Calls() == 1 && insideFraction > 0.5;
    std::printf( "%s\n", pass ? "PASS" : "FAIL" );

    board->RemoveListener( &sync );
    // The view must let go of the document's items before either is destroyed.
    canvas->GetView()->Clear();

    // Delete the viewer before the document: the view holds pointers to the document's items.
    delete frame;
    board->ClearProject();
    board.reset();
    return pass ? 0 : 1;
}
