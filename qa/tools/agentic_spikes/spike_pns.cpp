// Agentic-KiCad spike S0.d: route a track between two pads with the PNS router and no frame.
//
// Usage: spike_pns <board.kicad_pcb> [net-name]
//
// Works on the file in place, so pass a copy.  Picks a routed net with exactly two pads (or the
// named one), deletes its tracks and vias through BOARD_COMMIT, then routes pad to pad with
// PNS::ROUTER in walkaround mode.  The router writes to the board through HEADLESS_PNS_IFACE, a
// PNS_KICAD_IFACE_BASE subclass that turns committed PNS items into board items in one
// BOARD_COMMIT (upstream's PNS_KICAD_IFACE needs a PCB_TOOL_BASE and a VIEW).  Saves the board.
// The caller checks DRC with kicad-cli against the original board.

#include "spike_init.h"

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <board_loader.h>
#include <connectivity/connectivity_data.h>
#include <ratsnest/ratsnest_data.h>
#include <footprint.h>
#include <kiface_base.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project.h>
#include <router/pns_arc.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_line_placer.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_segment.h>
#include <router/pns_sizes_settings.h>
#include <router/pns_via.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <wildcards_and_files_ext.h>

#include <wx/filename.h>

#include <cstdio>
#include <map>
#include <memory>


/// Writes what the router commits into a BOARD_COMMIT; no view, no tool.
class HEADLESS_PNS_IFACE : public PNS_KICAD_IFACE_BASE
{
public:
    explicit HEADLESS_PNS_IFACE( TOOL_MANAGER* aToolMgr ) : m_toolMgr( aToolMgr ) {}

    void AddItem( PNS::ITEM* aItem ) override
    {
        BOARD_CONNECTED_ITEM* item = createBoardItem( aItem );

        if( !item )
            return;

        aItem->SetParent( item );
        commit().Add( item );
        m_added++;
    }

    void RemoveItem( PNS::ITEM* aItem ) override
    {
        // A pad the router "moves" is not supported here; tracks and vias only.
        if( aItem->Parent() && !aItem->OfKind( PNS::ITEM::SOLID_T ) )
        {
            commit().Remove( aItem->Parent() );
            m_removed++;
        }
    }

    void UpdateItem( PNS::ITEM* aItem ) override
    {
        // Not reached by a plain route; the editor's version moves the board item in place.
        RemoveItem( aItem );
        AddItem( aItem );
    }

    void Commit() override
    {
        if( m_commit )
            m_commit->Push( wxS( "Route" ) );

        m_commit.reset();
    }

    int GetNetCode( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetCode() : -1;
    }

    wxString GetNetName( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetname() : wxString();
    }

    int Added() const { return m_added; }
    int Removed() const { return m_removed; }

private:
    BOARD_COMMIT& commit()
    {
        if( !m_commit )
            m_commit = std::make_unique<BOARD_COMMIT>( m_toolMgr );

        return *m_commit;
    }

    BOARD_CONNECTED_ITEM* createBoardItem( PNS::ITEM* aItem )
    {
        NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( aItem->Net() );

        if( !net )
            net = NETINFO_LIST::OrphanedItem();

        switch( aItem->Kind() )
        {
        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( aItem );
            PCB_TRACK*    track = new PCB_TRACK( m_board );
            track->SetStart( seg->Seg().A );
            track->SetEnd( seg->Seg().B );
            track->SetWidth( seg->Width() );
            track->SetLayer( GetBoardLayerFromPNSLayer( seg->Layers().Start() ) );
            track->SetNet( net );
            return track;
        }

        case PNS::ITEM::ARC_T:
        {
            PNS::ARC* arc = static_cast<PNS::ARC*>( aItem );
            PCB_ARC*  pcbArc = new PCB_ARC( m_board, static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) ) );
            pcbArc->SetWidth( arc->Width() );
            pcbArc->SetLayer( GetBoardLayerFromPNSLayer( arc->Layers().Start() ) );
            pcbArc->SetNet( net );
            return pcbArc;
        }

        case PNS::ITEM::VIA_T:
        {
            PNS::VIA* via = static_cast<PNS::VIA*>( aItem );
            PCB_VIA*  pcbVia = new PCB_VIA( m_board );
            pcbVia->SetPosition( via->Pos() );
            pcbVia->SetWidth( PADSTACK::ALL_LAYERS, via->Diameter( 0 ) );
            pcbVia->SetDrill( via->Drill() );
            pcbVia->SetNet( net );
            pcbVia->SetViaType( via->ViaType() );
            pcbVia->SetLayerPair( GetBoardLayerFromPNSLayer( via->Layers().Start() ),
                                  GetBoardLayerFromPNSLayer( via->Layers().End() ) );
            return pcbVia;
        }

        default: return nullptr;
        }
    }

    TOOL_MANAGER*                 m_toolMgr;
    std::unique_ptr<BOARD_COMMIT> m_commit;
    int                           m_added = 0;
    int                           m_removed = 0;
};


int main( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::fprintf( stderr, "usage: spike_pns <board.kicad_pcb> [net-name]\n" );
        return 2;
    }

    if( !SpikeInit( argc, argv ) )
        return 1;

    wxFileName boardFile( wxString::FromUTF8( argv[1] ) );
    boardFile.MakeAbsolute();
    const wxString wantedNet = argc > 2 ? wxString::FromUTF8( argv[2] ) : wxString();

    wxFileName proFile( boardFile );
    proFile.SetExt( FILEEXT::ProjectFileExtension );

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    PCBNEW_SETTINGS*  settings = new PCBNEW_SETTINGS;
    mgr.RegisterSettings( settings, false );
    mgr.Load();
    Kiface().InitSettings( settings );
    mgr.LoadProject( proFile.GetFullPath() );

    const wxString         path = boardFile.GetFullPath();
    std::unique_ptr<BOARD> board = BOARD_LOADER::Load( path, PCB_IO_MGR::KICAD_SEXP, &mgr.Prj() );

    if( !board )
    {
        std::fprintf( stderr, "load failed\n" );
        return 1;
    }

    auto toolMgr = std::make_unique<TOOL_MANAGER>();
    toolMgr->SetEnvironment( board.get(), nullptr, nullptr, settings, nullptr );

    // Pick a routed two-pad net on the top side (or the requested one).
    std::map<int, std::vector<PAD*>>        padsByNet;
    std::map<int, std::vector<PCB_TRACK*>> tracksByNet;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 )
                padsByNet[pad->GetNetCode()].push_back( pad );
        }
    }

    for( PCB_TRACK* track : board->Tracks() )
        tracksByNet[track->GetNetCode()].push_back( track );

    int netCode = -1;

    for( const auto& [code, pads] : padsByNet )
    {
        const wxString name = board->FindNet( code )->GetNetname();

        if( !wantedNet.IsEmpty() )
        {
            if( name == wantedNet )
                netCode = code;

            continue;
        }

        if( pads.size() != 2 || tracksByNet[code].empty() || !pads[0]->IsOnLayer( F_Cu )
            || !pads[1]->IsOnLayer( F_Cu ) )
        {
            continue;
        }

        const double distance = ( pads[0]->GetPosition() - pads[1]->GetPosition() ).EuclideanNorm();

        if( distance > pcbIUScale.mmToIU( 5 ) && distance < pcbIUScale.mmToIU( 40 ) )
        {
            netCode = code;
            break;
        }
    }

    if( netCode < 0 || padsByNet[netCode].size() != 2 )
    {
        std::fprintf( stderr, "no suitable two-pad net\n" );
        return 1;
    }

    PAD* padA = padsByNet[netCode][0];
    PAD* padB = padsByNet[netCode][1];

    std::printf( "net %s pads %s.%s -> %s.%s distance_mm %.1f tracks_removed %zu\n",
                 board->FindNet( netCode )->GetNetname().ToUTF8().data(),
                 padA->GetParentFootprint()->GetReference().ToUTF8().data(), padA->GetNumber().ToUTF8().data(),
                 padB->GetParentFootprint()->GetReference().ToUTF8().data(), padB->GetNumber().ToUTF8().data(),
                 pcbIUScale.IUTomm( ( padA->GetPosition() - padB->GetPosition() ).EuclideanNorm() ),
                 tracksByNet[netCode].size() );

    {
        BOARD_COMMIT commit( toolMgr.get() );

        for( PCB_TRACK* track : tracksByNet[netCode] )
            commit.Remove( track );

        commit.Push( wxS( "S0.d unroute" ) );
    }

    board->GetConnectivity()->RecalculateRatsnest();
    std::printf( "unconnected_after_unroute %u\n", board->GetConnectivity()->GetUnconnectedCount( false ) );

    // Route pad to pad.
    double                t = SpikeNowMs();
    PNS::ROUTER           router;
    HEADLESS_PNS_IFACE    iface( toolMgr.get() );
    PNS::ROUTING_SETTINGS routingSettings( nullptr, "" );

    iface.SetBoard( board.get() );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    routingSettings.SetMode( PNS::RM_Walkaround );
    router.LoadSettings( &routingSettings );
    router.SetMode( PNS::PNS_MODE_ROUTE_SINGLE );

    const int  pnsFront = iface.GetPNSLayerFromBoardLayer( F_Cu );
    PNS::ITEM* startItem = router.GetWorld()->FindItemByParent( padA );
    PNS::ITEM* endItem = router.GetWorld()->FindItemByParent( padB );

    if( !startItem || !endItem )
    {
        std::fprintf( stderr, "pads not in the router world\n" );
        return 1;
    }

    PNS::SIZES_SETTINGS sizes( router.Sizes() );
    iface.SetStartLayerFromPCBNew( F_Cu );
    iface.ImportSizes( sizes, startItem, nullptr, padA->GetPosition() );
    router.UpdateSizes( sizes );

    bool ok = router.StartRouting( padA->GetPosition(), startItem, pnsFront );

    if( !ok )
    {
        std::fprintf( stderr, "StartRouting failed: %s\n", router.FailureReason().ToUTF8().data() );
        return 1;
    }

    router.Move( padB->GetPosition(), endItem );

    VECTOR2I head;
    const PNS::ITEM_SET traces = router.Placer()->Traces();

    for( const PNS::ITEM* item : traces.CItems() )
    {
        if( item->Kind() == PNS::ITEM::LINE_T )
            head = static_cast<const PNS::LINE*>( item )->CLine().CLastPoint();
    }

    const bool reached = head == padB->GetPosition();
    const bool fixed = router.FixRoute( padB->GetPosition(), endItem, false, false );

    if( router.RoutingInProgress() )
        router.CommitRouting();

    router.StopRouting();
    const double routeMs = SpikeNowMs() - t;

    board->GetConnectivity()->RecalculateRatsnest();
    const unsigned netUnconnected = board->GetConnectivity()->GetRatsnestForNet( netCode )
                                            ? board->GetConnectivity()->GetRatsnestForNet( netCode )->GetEdges().size()
                                            : 0;

    std::printf( "route_ms %.1f width_mm %.3f reached %d fixed %d board_items_added %d removed %d\n", routeMs,
                 pcbIUScale.IUTomm( sizes.TrackWidth() ), reached, fixed, iface.Added(), iface.Removed() );
    std::printf( "net_unconnected_edges %u unconnected_total %u\n", netUnconnected,
                 board->GetConnectivity()->GetUnconnectedCount( false ) );

    wxString target = path;

    if( !BOARD_LOADER::SaveBoard( target, *board ) )
    {
        std::fprintf( stderr, "save failed\n" );
        return 1;
    }

    std::printf( "asserts %d\n", g_spikeAsserts );

    const bool pass = reached && fixed && iface.Added() > 0 && netUnconnected == 0;
    std::printf( "%s\n", pass ? "PASS (in process; check DRC with kicad-cli)" : "FAIL" );

    toolMgr.reset();
    board->ClearProject();
    board.reset();
    return pass ? 0 : 1;
}
