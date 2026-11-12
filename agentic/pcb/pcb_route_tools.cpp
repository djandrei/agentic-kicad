/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <agentic/core/core_tools.h>
#include <agentic/core/handles.h>
#include <agentic/core/progress.h>
#include <agentic/core/services.h>
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/pcb_items.h>
#include <agentic/pcb/pcb_route_tools.h>
#include <agentic/pcb/pcb_tools.h>
#include <agentic/pcb/pns_agent_iface.h>

#include <autorouter/ar_autoplacer.h>
#include <base_units.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <cleanup_item.h>
#include <drc/drc_engine.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <reporter.h>
#include <router/pns_item.h>
#include <router/pns_line.h>
#include <router/pns_meander.h>
#include <router/pns_meander_placer_base.h>
#include <router/pns_node.h>
#include <router/pns_placement_algo.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_sizes_settings.h>
#include <specctra_import_export/specctra.h>
#include <tracks_cleaner.h>
#include <view/view_overlay.h>

#include <cmath>
#include <set>


namespace AGENTIC
{

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    double mm( long long aIu )
    {
        return std::round( pcbIUScale.IUTomm( static_cast<double>( aIu ) ) * 1e4 ) / 1e4;
    }


    JSON pcbSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["file"] = { { "type", "string" } };
        aProperties["force"] = { { "type", "boolean" } };
        aProperties["timeoutSeconds"] = { { "type", "number" },
                                          { "minimum", 0 },
                                          { "description", "Give up after this long; the board is left unchanged." } };
        return ProjectToolSchema( aProperties, aRequired );
    }


    BOARD_DOC_SESSION& session( AGENT_SERVICES& aServices, const JSON& aArgs, AGENT_PROJECT** aProject )
    {
        AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
        *aProject = &project;
        return BoardSession( project, aArgs.value( "file", "" ) );
    }


    PNS::PNS_MODE routingMode( const std::string& aMode )
    {
        if( aMode == "shove" )
            return PNS::RM_Shove;

        if( aMode == "ignore" )
            return PNS::RM_MarkObstacles;

        return PNS::RM_Walkaround;
    }


    /// One headless router on a board, set up as the interactive router would be.
    struct ROUTER_RUN
    {
        ROUTER_RUN( BOARD_DOC_SESSION& aSession, PNS::PNS_MODE aMode, PNS::ROUTER_MODE aRouterMode ) :
                iface( aSession.ToolManager() ),
                settings( nullptr, "" )
        {
            // The router's clearances come from the board's DRC engine, which is compiled only
            // when DRC runs; the interactive editor compiles it when the board is loaded.
            BOARD& board = aSession.Board();

            try
            {
                board.GetDesignSettings().m_DRCEngine->InitEngine( board.GetDesignRulesPath() );
            }
            catch( const PARSE_ERROR& e )
            {
                throw TOOL_ERROR( "The custom design rules do not compile: " + utf8( e.What() ) );
            }

            iface.SetBoard( &board );
            router.SetInterface( &iface );
            router.ClearWorld();
            router.SyncWorld();
            settings.SetMode( aMode );
            router.LoadSettings( &settings );
            router.SetMode( aRouterMode );
        }

        PNS::ITEM* itemFor( BOARD_ITEM* aParent ) { return router.GetWorld()->FindItemByParent( aParent ); }

        PNS::ROUTER           router;
        PNS_AGENT_IFACE       iface;
        PNS::ROUTING_SETTINGS settings;
    };


    VECTOR2I lastHead( PNS::ROUTER& aRouter )
    {
        VECTOR2I head;

        if( !aRouter.Placer() )
            return head;

        // Traces() returns a copy: bind it before iterating.
        const PNS::ITEM_SET traces = aRouter.Placer()->Traces();

        for( const PNS::ITEM* item : traces.CItems() )
        {
            if( item->Kind() == PNS::ITEM::LINE_T )
            {
                const PNS::LINE* line = static_cast<const PNS::LINE*>( item );

                if( line->PointCount() > 0 )
                    head = line->CLine().CLastPoint();
            }
        }

        return head;
    }

    /// A route into a pad that already has copper of its net (a second connection to it) can be
    /// laid on top of that copper; a later shove or tuning tears doubled copper into a dangling
    /// stub. Collinear segments of one net and layer that share copper are replaced by pieces that
    /// meet end to end: every point where one of them ends becomes a joint (KiCad joins a track end
    /// to another track's end, not to its middle) and the shared stretch is kept once, at the wider
    /// width. Only pairs with a segment from \a aAdded (or made here) are looked at. Returns the
    /// segments removed; \a aMerged counts the overlaps resolved.
    std::set<KIID> removeDoubledCopper( BOARD& aBoard, BOARD_COMMIT& aCommit, std::set<KIID> aAdded, int& aMerged )
    {
        const double   tolerance = pcbIUScale.mmToIU( 0.001 );
        std::set<KIID> removed;
        aMerged = 0;

        std::set<int> nets;

        for( PCB_TRACK* track : aBoard.Tracks() )
        {
            if( aAdded.count( track->m_Uuid ) )
                nets.insert( track->GetNetCode() );
        }

        // Where aPoint lies along aSegment (from its start), if it lies on its line.
        auto along = [&]( const PCB_TRACK* aSegment, const VECTOR2I& aPoint, double& aAt ) -> bool
        {
            const VECTOR2D direction( aSegment->GetEnd() - aSegment->GetStart() );
            const double   length = direction.EuclideanNorm();

            if( length < 1 )
                return false;

            const VECTOR2D offset( aPoint - aSegment->GetStart() );

            if( std::abs( direction.Cross( offset ) ) / length > tolerance )
                return false;

            aAt = direction.Dot( offset ) / length;
            return true;
        };

        // One overlap per pass: the segments change, so collect them again.
        for( bool changed = true; changed; )
        {
            changed = false;
            std::vector<PCB_TRACK*> segments;

            for( PCB_TRACK* track : aBoard.Tracks() )
            {
                if( track->Type() == PCB_TRACE_T && nets.count( track->GetNetCode() )
                    && !removed.count( track->m_Uuid ) )
                {
                    segments.push_back( track );
                }
            }

            for( size_t i = 0; i < segments.size() && !changed; ++i )
            {
                for( size_t j = 0; j < segments.size() && !changed; ++j )
                {
                    PCB_TRACK* a = segments[i];
                    PCB_TRACK* b = segments[j];

                    if( a == b || a->GetNetCode() != b->GetNetCode() || a->GetLayer() != b->GetLayer()
                        || ( !aAdded.count( a->m_Uuid ) && !aAdded.count( b->m_Uuid ) ) )
                    {
                        continue;
                    }

                    double aStart = 0;
                    double aEnd = 0;

                    if( !along( b, a->GetStart(), aStart ) || !along( b, a->GetEnd(), aEnd ) )
                        continue;

                    const double length = VECTOR2D( b->GetEnd() - b->GetStart() ).EuclideanNorm();
                    const double aLow = std::min( aStart, aEnd );
                    const double aHigh = std::max( aStart, aEnd );

                    if( std::min( aHigh, length ) - std::max( aLow, 0.0 ) <= tolerance )
                        continue;

                    // The joints along b: the four ends, close ones as one.
                    std::vector<std::pair<double, VECTOR2I>> joints = { { 0.0, b->GetStart() },
                                                                        { length, b->GetEnd() },
                                                                        { aStart, a->GetStart() },
                                                                        { aEnd, a->GetEnd() } };
                    std::sort( joints.begin(), joints.end(),
                               []( const auto& l, const auto& r )
                               {
                                   return l.first < r.first;
                               } );

                    std::vector<std::pair<double, VECTOR2I>> points;

                    for( const auto& joint : joints )
                    {
                        if( points.empty() || joint.first - points.back().first > tolerance )
                            points.push_back( joint );
                    }

                    struct PIECE
                    {
                        VECTOR2I start;
                        VECTOR2I end;
                        int      width;
                    };

                    std::vector<PIECE> pieces;

                    for( size_t k = 0; k + 1 < points.size(); ++k )
                    {
                        const double middle = ( points[k].first + points[k + 1].first ) / 2;
                        const bool   inA = middle >= aLow && middle <= aHigh;
                        const bool   inB = middle >= 0 && middle <= length;

                        if( inA || inB )
                        {
                            pieces.push_back( { points[k].second, points[k + 1].second,
                                                std::max( inA ? a->GetWidth() : 0, inB ? b->GetWidth() : 0 ) } );
                        }
                    }

                    // b and a become the first two pieces; more pieces are new tracks.
                    std::vector<PCB_TRACK*> slots = { b, a };

                    for( size_t k = 0; k < pieces.size(); ++k )
                    {
                        PCB_TRACK* track = nullptr;

                        if( k < slots.size() )
                        {
                            track = slots[k];
                            aCommit.Modify( track );
                        }
                        else
                        {
                            track = static_cast<PCB_TRACK*>( b->Duplicate( true, &aCommit ) );
                            aCommit.Add( track );
                            aAdded.insert( track->m_Uuid );
                        }

                        track->SetStart( pieces[k].start );
                        track->SetEnd( pieces[k].end );
                        track->SetWidth( pieces[k].width );
                    }

                    for( size_t k = pieces.size(); k < slots.size(); ++k )
                    {
                        aCommit.Remove( slots[k] );
                        removed.insert( slots[k]->m_Uuid );
                    }

                    aMerged++;
                    changed = true;
                }
            }
        }

        return removed;
    }


    /// The KIIDs of the board's tracks and vias.
    std::set<KIID> copperIds( BOARD& aBoard )
    {
        std::set<KIID> ids;

        for( PCB_TRACK* track : aBoard.Tracks() )
            ids.insert( track->m_Uuid );

        return ids;
    }


    /// The nets that have tracks or vias leading nowhere, found as KiCad's cleanup would (a dry
    /// run; the marks it leaves are cleared).
    std::set<int> netsWithDanglingCopper( BOARD_DOC_SESSION& aSession )
    {
        BOARD&                                     board = aSession.Board();
        BOARD_COMMIT                               probe( aSession.ToolManager() );
        std::vector<std::shared_ptr<CLEANUP_ITEM>> items;
        TRACKS_CLEANER                             cleaner( &board, probe );
        std::set<int>                              nets;

        board.BuildConnectivity();
        cleaner.CleanupBoard( true, &items, false, false, false, true, false, true );

        for( PCB_TRACK* track : board.Tracks() )
        {
            if( track->HasFlag( IS_DELETED ) )
                nets.insert( track->GetNetCode() );

            track->ClearFlags( IS_DELETED );
        }

        return nets;
    }


    /// A shove can break a track of another line that it cannot move cleanly, leaving copper that
    /// leads nowhere. Remove the tracks and vias made by this call (not in \a aBefore) that end
    /// unconnected, back to the first pad, via or junction, with KiCad's track cleaner limited to
    /// them: connectivity does not change. Nets in \a aKeep (copper leading nowhere already before
    /// the call, such as a stub drawn on purpose) are left alone. Returns how many were removed.
    int trimDanglingCopper( BOARD_DOC_SESSION& aSession, const std::set<KIID>& aBefore, const std::set<int>& aKeep )
    {
        BOARD&                                     board = aSession.Board();
        BOARD_COMMIT                               commit( aSession.ToolManager() );
        std::vector<std::shared_ptr<CLEANUP_ITEM>> items;
        TRACKS_CLEANER                             cleaner( &board, commit );

        cleaner.SetFilter(
                [&]( BOARD_CONNECTED_ITEM* aItem )
                {
                    return aBefore.count( aItem->m_Uuid ) > 0 || aKeep.count( aItem->GetNetCode() ) > 0;
                } );

        cleaner.CleanupBoard( false, &items, false, false, false, true, false, true );

        if( !commit.Empty() )
            commit.Push( wxS( "Trim dangling copper" ) );

        int dangling = 0;

        for( const std::shared_ptr<CLEANUP_ITEM>& item : items )
        {
            if( item->GetErrorCode() == CLEANUP_DANGLING_TRACK || item->GetErrorCode() == CLEANUP_DANGLING_VIA )
                dangling++;
        }

        return dangling;
    }


    /// The router starts and ends on the requested layer: a pad that is not on it (an SMD pad on
    /// the other side) would get a track under it that does not connect, so refuse instead.
    void requirePadOnLayer( BOARD& aBoard, const PAD* aPad, const std::string& aName, PCB_LAYER_ID aLayer )
    {
        if( aPad->IsOnLayer( aLayer ) )
            return;

        throw TOOL_ERROR( "Pad " + aName + " is not on " + PcbLayerName( aBoard, aLayer )
                          + ". Route on the pads' layer; to change layers, add the tracks and vias with "
                            "pcb_track_add and pcb_via_add." );
    }

} // namespace


void RegisterPcbRouteTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add(
            { "pcb_route",
              "Route a track with KiCad's interactive router (PNS) from a pad to a pad, through optional "
              "waypoints, on a copper layer the start pad is on (default: its side): walkaround (default) goes "
              "around obstacles, shove pushes other tracks aside. A waypoint with a layer places a via there and "
              "continues on that layer; the end pad must be on the last layer. Width and via size come from the "
              "net class. Copper a shove leaves leading nowhere, on a net that had none, is removed "
              "(danglingRemoved). Returns the tracks and vias made, whether the route reached the end, and the "
              "ratsnest delta.",
              "pcb",
              pcbSchema( { { "from", { { "type", "string" }, { "description", "Start pad, e.g. U1.3." } } },
                           { "to", { { "type", "string" }, { "description", "End pad." } } },
                           { "waypoints",
                             { { "type", "array" },
                               { "description", "Points {x, y} to pass through; {x, y, layer} changes to that copper "
                                                "layer with a via at the point." } } },
                           { "layer", { { "type", "string" } } },
                           { "mode", { { "type", "string" }, { "enum", { "walkaround", "shove" } } } },
                           { "width", { { "type", { "number", "string" } } } } },
                         { "from", "to" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  AGENT_PROJECT*     project = nullptr;
                  BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                  BOARD&             board = s.Board();
                  PAD*               fromPad = PcbPad( board, aArgs["from"] );
                  PAD*               toPad = PcbPad( board, aArgs["to"] );

                  if( fromPad->GetNetCode() <= 0 || fromPad->GetNet() != toPad->GetNet() )
                      throw TOOL_ERROR( "The two pads are not on the same net." );

                  if( aArgs.contains( "timeoutSeconds" ) )
                      aContext.SetTimeout( aArgs["timeoutSeconds"] );

                  const PCB_LAYER_ID layer =
                          PcbLayer( board, aArgs.value( "layer", fromPad->IsOnLayer( F_Cu ) ? "F.Cu" : "B.Cu" ) );

                  // Each waypoint, with the layer the route is on after it (a layer change puts a via there).
                  std::vector<std::pair<VECTOR2I, PCB_LAYER_ID>> waypoints;
                  PCB_LAYER_ID                                   endLayer = layer;

                  for( const JSON& point : aArgs.value( "waypoints", JSON::array() ) )
                  {
                      if( point.is_object() && point.contains( "layer" ) )
                      {
                          const PCB_LAYER_ID next = PcbLayer( board, point["layer"] );

                          if( !IsCopperLayer( next ) )
                              throw TOOL_ERROR( "A waypoint's layer must be a copper layer." );

                          if( next == endLayer )
                              throw TOOL_ERROR( "A waypoint changes to " + PcbLayerName( board, next )
                                                + ", which the route is already on." );

                          endLayer = next;
                      }

                      waypoints.emplace_back( PcbPoint( board, point, "waypoints" ), endLayer );
                  }

                  requirePadOnLayer( board, fromPad, aArgs["from"], layer );
                  requirePadOnLayer( board, toPad, aArgs["to"], endLayer );

                  JSON result = PcbMutate(
                          *project, s,
                          "pcb_route " + aArgs["from"].get<std::string>() + " " + aArgs["to"].get<std::string>(), aArgs,
                          true,
                          [&]( BOARD_COMMIT& ) -> JSON
                          {
                              const std::set<KIID> before = copperIds( board );
                              const std::set<int>  danglingBefore = netsWithDanglingCopper( s );
                              ROUTER_RUN run( s, routingMode( aArgs.value( "mode", "walkaround" ) ),
                                              PNS::PNS_MODE_ROUTE_SINGLE );
                              PNS::ITEM* start = run.itemFor( fromPad );
                              PNS::ITEM* end = run.itemFor( toPad );

                              if( !start || !end )
                                  throw TOOL_ERROR( "The router does not see those pads." );

                              const int pnsLayer = run.iface.GetPNSLayerFromBoardLayer( layer );
                              run.iface.SetStartLayerFromPCBNew( layer );

                              PNS::SIZES_SETTINGS sizes( run.router.Sizes() );
                              run.iface.ImportSizes( sizes, start, nullptr, fromPad->GetPosition() );

                              if( aArgs.contains( "width" ) )
                                  sizes.SetTrackWidth( PcbLength( aArgs["width"], "width" ) );

                              run.router.UpdateSizes( sizes );

                              if( !run.router.StartRouting( fromPad->GetPosition(), start, pnsLayer ) )
                                  throw TOOL_ERROR( "The router cannot start at " + aArgs["from"].get<std::string>()
                                                    + ": " + utf8( run.router.FailureReason() ) );

                              // Each waypoint fixes the route so far; the end pad finishes it. A layer change
                              // is placed as the router tool places a via: the layer pair, via mode, fix, switch.
                              PCB_LAYER_ID current = layer;

                              for( const auto& [waypoint, after] : waypoints )
                              {
                                  if( after != current )
                                  {
                                      PNS::SIZES_SETTINGS viaSizes( run.router.Sizes() );
                                      viaSizes.ClearLayerPairs();
                                      viaSizes.AddLayerPair( run.iface.GetPNSLayerFromBoardLayer( current ),
                                                             run.iface.GetPNSLayerFromBoardLayer( after ) );
                                      run.router.UpdateSizes( viaSizes );

                                      if( !run.router.IsPlacingVia() )
                                          run.router.ToggleViaPlacement();
                                  }

                                  run.router.Move( waypoint, nullptr );
                                  run.router.FixRoute( waypoint, nullptr, false, false );

                                  if( after != current )
                                  {
                                      if( !run.router.SwitchLayer( run.iface.GetPNSLayerFromBoardLayer( after ) ) )
                                      {
                                          run.router.StopRouting();
                                          throw TOOL_ERROR( "The router could not place a via at a waypoint; move "
                                                            "it away from other copper." );
                                      }

                                      current = after;
                                  }

                                  if( aContext.IsCancelled() )
                                  {
                                      run.router.StopRouting();
                                      throw TOOL_ERROR( "Cancelled; nothing was routed.",
                                                        JSON{ { "cancelled", true } } );
                                  }
                              }

                              run.router.Move( toPad->GetPosition(), end );
                              const bool reached = lastHead( run.router ) == toPad->GetPosition();
                              const bool fixed = run.router.FixRoute( toPad->GetPosition(), end, false, false );

                              if( aContext.IsCancelled() )
                              {
                                  run.router.StopRouting();
                                  throw TOOL_ERROR( "Cancelled; nothing was routed.", JSON{ { "cancelled", true } } );
                              }

                              if( run.router.RoutingInProgress() )
                                  run.router.CommitRouting();

                              run.router.StopRouting();

                              if( run.iface.Added() == 0 )
                                  throw TOOL_ERROR(
                                          "The router found no path. Try waypoints, another layer, or shove mode." );

                              s.MarkModified();
                              JSON items = JSON::array();

                              int                  vias = 0;
                              const std::set<KIID> added( run.iface.AddedItems().begin(),
                                                          run.iface.AddedItems().end() );
                              int                  merged = 0;

                              // The merge and the trim each push their own commit, as the router does,
                              // so the trim sees the merged copper on the board.
                              BOARD_COMMIT mergeCommit( s.ToolManager() );
                              removeDoubledCopper( board, mergeCommit, added, merged );

                              if( !mergeCommit.Empty() )
                                  mergeCommit.Push( wxS( "Merge doubled copper" ) );

                              // A route that stopped short ends unconnected by design: keep it as it is.
                              const int trimmed =
                                      ( reached && fixed ) ? trimDanglingCopper( s, before, danglingBefore ) : 0;

                              for( const KIID& kiid : run.iface.AddedItems() )
                              {
                                  BOARD_ITEM* item = board.ResolveItem( kiid, true );

                                  if( !item )
                                      continue;

                                  items.push_back( utf8( kiid.AsString() ) );

                                  if( item->Type() == PCB_VIA_T )
                                      vias++;
                              }

                              return JSON{ { "completed", reached && fixed },
                                           { "tracksAdded", run.iface.Added() },
                                           { "vias", vias },
                                           { "overlapsMerged", merged },
                                           { "danglingRemoved", trimmed },
                                           { "itemsRemoved", run.iface.Removed() },
                                           { "width", mm( sizes.TrackWidth() ) },
                                           { "items", items } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "pcb_diff_pair_route",
                 "Route a differential pair (nets named like USB_P/USB_N or D+/D-) from its two start pads to its two "
                 "end pads with the router's diff pair mode; widths and gap come from the net class.",
                 "pcb",
                 pcbSchema( { { "from", { { "type", "string" }, { "description", "Start pad of either net." } } },
                              { "to", { { "type", "string" }, { "description", "End pad on the same net as from." } } },
                              { "layer", { { "type", "string" } } } },
                            { "from", "to" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();
                     PAD*               fromPad = PcbPad( board, aArgs["from"] );
                     PAD*               toPad = PcbPad( board, aArgs["to"] );
                     const PCB_LAYER_ID layer =
                             PcbLayer( board, aArgs.value( "layer", fromPad->IsOnLayer( F_Cu ) ? "F.Cu" : "B.Cu" ) );

                     requirePadOnLayer( board, fromPad, aArgs["from"], layer );
                     requirePadOnLayer( board, toPad, aArgs["to"], layer );

                     JSON result = PcbMutate(
                             *project, s, "pcb_diff_pair_route", aArgs, true,
                             [&]( BOARD_COMMIT& aCommit ) -> JSON
                             {
                                 ROUTER_RUN run( s, PNS::RM_Walkaround, PNS::PNS_MODE_ROUTE_DIFF_PAIR );
                                 PNS::ITEM* start = run.itemFor( fromPad );
                                 PNS::ITEM* end = run.itemFor( toPad );
                                 run.iface.SetStartLayerFromPCBNew( layer );

                                 PNS::SIZES_SETTINGS sizes( run.router.Sizes() );
                                 run.iface.ImportSizes( sizes, start, nullptr, fromPad->GetPosition() );
                                 run.router.UpdateSizes( sizes );

                                 if( !start
                                     || !run.router.StartRouting( fromPad->GetPosition(), start,
                                                                  run.iface.GetPNSLayerFromBoardLayer( layer ) ) )
                                 {
                                     throw TOOL_ERROR( "The router cannot start a pair here: "
                                                       + utf8( run.router.FailureReason() )
                                                       + " (are the nets named as a pair, e.g. X_P / X_N?)" );
                                 }

                                 run.router.Move( toPad->GetPosition(), end );
                                 const bool fixed = run.router.FixRoute( toPad->GetPosition(), end, true, false );

                                 if( run.router.RoutingInProgress() )
                                     run.router.CommitRouting();

                                 run.router.StopRouting();

                                 if( run.iface.Added() == 0 )
                                     throw TOOL_ERROR( "The router found no path for the pair." );

                                 s.MarkModified();
                                 JSON                 items = JSON::array();
                                 const std::set<KIID> added( run.iface.AddedItems().begin(),
                                                             run.iface.AddedItems().end() );
                                 int                  merged = 0;
                                 const std::set<KIID> doubled = removeDoubledCopper( board, aCommit, added, merged );

                                 for( const KIID& kiid : run.iface.AddedItems() )
                                 {
                                     if( !doubled.count( kiid ) )
                                         items.push_back( utf8( kiid.AsString() ) );
                                 }

                                 return JSON{ { "completed", fixed },
                                              { "tracksAdded", run.iface.Added() },
                                              { "overlapsMerged", merged },
                                              { "items", items } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "pcb_tune_length",
              "Add meanders to a routed track so its net reaches a target length (mm), with KiCad's length "
              "tuner. Give one track of the net (KIID) and the target; tolerance defaults to 0.1 mm. Meanders "
              "come in steps: when the status is too_long or too_short, undo and retry with a smaller spacing "
              "or amplitude (the tuner's settings: minAmplitude 0.2, maxAmplitude 1, spacing 0.6 mm). Returns "
              "the length reached and the tuning status.",
              "pcb",
              pcbSchema( { { "track", { { "type", "string" } } },
                           { "length", { { "type", { "number", "string" } } } },
                           { "tolerance", { { "type", { "number", "string" } } } },
                           { "minAmplitude", { { "type", { "number", "string" } } } },
                           { "maxAmplitude", { { "type", { "number", "string" } } } },
                           { "spacing", { { "type", { "number", "string" } } } } },
                         { "track", "length" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  AGENT_PROJECT*     project = nullptr;
                  BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                  BOARD&             board = s.Board();
                  BOARD_ITEM*        item = PcbFindOne( board, aArgs["track"] );

                  if( item->Type() != PCB_TRACE_T )
                      throw TOOL_ERROR( "Give a track (KIID) of the net to tune." );

                  PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
                  const int  target = PcbLength( aArgs["length"], "length" );
                  const int  tolerance =
                          aArgs.contains( "tolerance" ) ? PcbLength( aArgs["tolerance"], "tolerance" ) : PcbIu( 0.1 );

                  if( aArgs.contains( "timeoutSeconds" ) )
                      aContext.SetTimeout( aArgs["timeoutSeconds"] );

                  JSON result = PcbMutate(
                          *project, s, "pcb_tune_length", aArgs, false,
                          [&]( BOARD_COMMIT& ) -> JSON
                          {
                              ROUTER_RUN run( s, PNS::RM_Walkaround, PNS::PNS_MODE_TUNE_SINGLE );
                              PNS::ITEM* start = run.itemFor( track );

                              if( !start || !start->OfKind( PNS::ITEM::SEGMENT_T ) )
                                  throw TOOL_ERROR( "The router does not see that track." );

                              // Sweep the whole line the track belongs to, end to end, as a person
                              // drags the tuner along it.
                              const PNS::LINE line =
                                      run.router.GetWorld()->AssembleLine( static_cast<PNS::SEGMENT*>( start ) );
                              const VECTOR2I sweepFrom = line.CLine().CPoint( 0 );
                              const VECTOR2I sweepTo = line.CLine().CLastPoint();

                              if( !run.router.StartRouting( sweepFrom, start,
                                                            run.iface.GetPNSLayerFromBoardLayer( track->GetLayer() ) ) )
                              {
                                  throw TOOL_ERROR( "The length tuner cannot start on that track: "
                                                    + utf8( run.router.FailureReason() ) );
                              }

                              auto* placer = dynamic_cast<PNS::MEANDER_PLACER_BASE*>( run.router.Placer() );

                              if( !placer )
                              {
                                  run.router.StopRouting();
                                  throw TOOL_ERROR( "The length tuner did not start." );
                              }

                              PNS::MEANDER_SETTINGS settings = placer->MeanderSettings();

                              // The interactive tuner sets the net class; the delay code reads it.
                              settings.m_netClass = track->GetEffectiveNetClass();
                              MINOPTMAX<int> constraint;
                              constraint.SetMin( target - tolerance );
                              constraint.SetOpt( target );
                              constraint.SetMax( target + tolerance );
                              settings.SetTargetLength( constraint );

                              if( aArgs.contains( "minAmplitude" ) )
                                  settings.m_minAmplitude = PcbLength( aArgs["minAmplitude"], "minAmplitude" );

                              if( aArgs.contains( "maxAmplitude" ) )
                                  settings.m_maxAmplitude = PcbLength( aArgs["maxAmplitude"], "maxAmplitude" );

                              if( aArgs.contains( "spacing" ) )
                                  settings.m_spacing = PcbLength( aArgs["spacing"], "spacing" );

                              if( settings.m_minAmplitude <= 0 || settings.m_maxAmplitude < settings.m_minAmplitude
                                  || settings.m_spacing <= 0 )
                              {
                                  run.router.StopRouting();
                                  throw TOOL_ERROR( "Meander settings need 0 < minAmplitude <= maxAmplitude and a "
                                                    "positive spacing." );
                              }

                              placer->UpdateSettings( settings );

                              // Sweep the tuner along the track, as the mouse would.
                              run.router.Move( sweepTo, nullptr );

                              if( aContext.IsCancelled() )
                              {
                                  run.router.StopRouting();
                                  throw TOOL_ERROR( "Cancelled; the board is unchanged.",
                                                    JSON{ { "cancelled", true } } );
                              }

                              const long long reached = placer->TuningLengthResult();
                              const int       status = placer->TuningStatus();
                              run.router.FixRoute( sweepTo, nullptr, false, false );

                              if( run.router.RoutingInProgress() )
                                  run.router.CommitRouting();

                              run.router.StopRouting();
                              s.MarkModified();

                              JSON items = JSON::array();

                              for( const KIID& kiid : run.iface.AddedItems() )
                                  items.push_back( utf8( kiid.AsString() ) );

                              return JSON{ { "length", mm( reached ) },
                                           { "target", mm( target ) },
                                           { "status", status == PNS::MEANDER_PLACER_BASE::TUNED      ? "tuned"
                                                       : status == PNS::MEANDER_PLACER_BASE::TOO_LONG ? "too_long"
                                                                                                      : "too_short" },
                                           { "items", items } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "pcb_autoplace",
                 "Place footprints automatically inside the board outline with KiCad's autoplacer (by default the "
                 "footprints that are off the board, or the ones listed). Locked footprints stay.",
                 "pcb",
                 pcbSchema( { { "footprints", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                              { "all", { { "type", "boolean" }, { "description", "Every unlocked footprint." } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();

                     if( board.GetBoardEdgesBoundingBox().GetWidth() <= 0 )
                         throw TOOL_ERROR( "The board needs an outline first (pcb_outline_set)." );

                     std::vector<FOOTPRINT*> footprints;

                     if( aArgs.contains( "footprints" ) )
                     {
                         for( const JSON& ref : aArgs["footprints"] )
                             footprints.push_back( PcbFootprint( board, ref ) );
                     }
                     else
                     {
                         // all: every unlocked footprint; default: the unlocked ones off the board
                         // (outside the outline's box, as the autoplacer's own off-board test).
                         const bool  all = aArgs.value( "all", false );
                         const BOX2I boardBox = board.GetBoardEdgesBoundingBox();

                         for( FOOTPRINT* fp : board.Footprints() )
                         {
                             if( !fp->IsLocked() && ( all || !boardBox.Contains( fp->GetPosition() ) ) )
                                 footprints.push_back( fp );
                         }

                         if( footprints.empty() )
                             throw TOOL_ERROR(
                                     "Every footprint is on the board already; list footprints or give all." );
                     }

                     JSON result = PcbMutate(
                             *project, s, "pcb_autoplace", aArgs, true,
                             [&]( BOARD_COMMIT& aCommit ) -> JSON
                             {
                                 AR_AUTOPLACER           placer( &board );
                                 AGENT_PROGRESS_REPORTER progress( aContext, aServices.ProgressSink() );
                                 placer.SetProgressReporter( &progress );

                                 // It always draws its working area; nobody sees this overlay.
                                 placer.SetOverlay( std::make_shared<KIGFX::VIEW_OVERLAY>() );

                                 // The list is exactly what moves; the placer's own off-board pick would add more.
                                 AR_RESULT status = placer.AutoplaceFootprints( footprints, &aCommit, false );

                                 if( status == AR_CANCELLED )
                                     throw TOOL_ERROR( "Cancelled; nothing was moved.", JSON{ { "cancelled", true } } );

                                 if( status == AR_FAILURE )
                                     throw TOOL_ERROR( "The autoplacer failed (is the outline closed?)." );

                                 JSON placed = JSON::array();
                                 JSON kiids = JSON::array();

                                 for( FOOTPRINT* fp : footprints )
                                 {
                                     placed.push_back( { { "reference", utf8( fp->GetReference() ) },
                                                         { "position", PcbPointJson( fp->GetPosition() ) } } );
                                     kiids.push_back( utf8( fp->m_Uuid.AsString() ) );
                                 }

                                 return JSON{ { "placed", placed }, { "items", kiids } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "pcb_import_session",
              "Import the routes of a Specctra session file (.ses) made by an external autorouter (Freerouting): "
              "its tracks and vias replace the board's. The board must have been exported with the same parts.",
              "pcb",
              pcbSchema( { { "session", { { "type", "string" }, { "description", "Project-relative .ses file." } } } },
                         { "session" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT*     project = nullptr;
                  BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                  const std::string  path = project->Files().Resolve( aArgs["session"] ).string();

                  JSON result =
                          PcbMutate( *project, s, "pcb_import_session", aArgs, true,
                                     [&]( BOARD_COMMIT& aCommit ) -> JSON
                                     {
                                         WX_STRING_REPORTER reporter;
                                         int                skipped = 0;

                                         try
                                         {
                                             if( !DSN::ImportSpecctraSession( &s.Board(), wxString::FromUTF8( path ),
                                                                              aCommit, &reporter, &skipped ) )
                                                 throw TOOL_ERROR( "The session could not be imported:\n"
                                                                   + utf8( reporter.GetMessages() ) );
                                         }
                                         catch( const IO_ERROR& e )
                                         {
                                             throw TOOL_ERROR( "The session could not be read: " + utf8( e.What() ) );
                                         }

                                         return JSON{ { "skipped", skipped } };
                                     } );

                  return TOOL_RESULT::Data( result );
              } } );
}

} // namespace AGENTIC
