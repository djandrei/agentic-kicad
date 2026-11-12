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
#include <agentic/core/library_tools.h>
#include <agentic/core/progress.h>
#include <agentic/core/services.h>
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/pcb_items.h>
#include <agentic/pcb/pcb_tools.h>

#include <base_units.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <cleanup_item.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_engine.h>
#include <drc/drc_rule_parser.h>
#include <math/util.h>
#include <project/net_settings.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <netinfo.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <project_pcb.h>
#include <reporter.h>
#include <tracks_cleaner.h>
#include <richio.h>
#include <tool/tool_manager.h>
#include <tools/zone_filler_tool.h>
#include <zone.h>

#include <wx/filename.h>

#include <cmath>
#include <regex>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    wxString wx( const std::string& aText )
    {
        return wxString::FromUTF8( aText );
    }


    double mm( int aIu )
    {
        return std::round( pcbIUScale.IUTomm( aIu ) * 1e4 ) / 1e4;
    }


    const JSON POINT{ { "type", { "object", "array", "string" } },
                      { "description", "{x, y} in mm (Y down), or a pad \"U1.3\"." } };
    const JSON ITEMS_ARG{ { "type", "array" },
                          { "items", { { "type", "string" } } },
                          { "description", "Item handles: KIID, reference (U1) or pad (U1.3)." } };


    JSON pcbSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["file"] = { { "type", "string" },
                                { "description", "Board (project-relative); default the project's board." } };
        aProperties["force"] = { { "type", "boolean" } };
        return ProjectToolSchema( aProperties, aRequired );
    }


    BOARD_DOC_SESSION& session( AGENT_SERVICES& aServices, const JSON& aArgs, AGENT_PROJECT** aProject = nullptr )
    {
        AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );

        if( aProject )
            *aProject = &project;

        return BoardSession( project, aArgs.value( "file", "" ) );
    }


    /// Add a closed outline of straight segments and arcs on Edge.Cuts.
    void addOutline( BOARD& aBoard, BOARD_COMMIT& aCommit, const std::vector<VECTOR2I>& aCorners, int aRadius,
                     int aWidth, JSON& aKiids )
    {
        const size_t n = aCorners.size();

        auto add = [&]( PCB_SHAPE* aShape )
        {
            aShape->SetLayer( Edge_Cuts );
            aShape->SetWidth( aWidth );
            aCommit.Add( aShape );
            aKiids.push_back( utf8( aShape->m_Uuid.AsString() ) );
        };

        for( size_t i = 0; i < n; ++i )
        {
            const VECTOR2I prev = aCorners[( i + n - 1 ) % n];
            const VECTOR2I cur = aCorners[i];
            const VECTOR2I next = aCorners[( i + 1 ) % n];

            VECTOR2D inDir = VECTOR2D( cur - prev ).Resize( 1.0 );
            VECTOR2D outDir = VECTOR2D( next - cur ).Resize( 1.0 );

            const VECTOR2I start = aRadius > 0 ? cur - VECTOR2I( inDir * aRadius ) : cur;
            const VECTOR2I end = aRadius > 0 ? cur + VECTOR2I( outDir * aRadius ) : cur;

            // The straight part leading to this corner, from the previous corner's rounding.
            const VECTOR2I prevEnd = aRadius > 0 ? prev + VECTOR2I( inDir * aRadius ) : prev;
            auto*          line = new PCB_SHAPE( &aBoard, SHAPE_T::SEGMENT );
            line->SetStart( prevEnd );
            line->SetEnd( start );
            add( line );

            if( aRadius > 0 )
            {
                // A quarter arc for a right-angle corner; the centre is offset inward.
                const VECTOR2I center = start + VECTOR2I( outDir * aRadius );
                auto*          arc = new PCB_SHAPE( &aBoard, SHAPE_T::ARC );
                arc->SetCenter( center );
                arc->SetStart( start );
                arc->SetEnd( end );
                add( arc );
            }
        }
    }


    void requirePositiveLength( int aValue, const std::string& aField )
    {
        if( aValue <= 0 )
            throw TOOL_ERROR( "'" + aField + "' must be positive." );
    }

} // namespace


JSON PcbMutate( AGENT_PROJECT& aProject, BOARD_DOC_SESSION& aSession, const std::string& aLabel, const JSON& aArgs,
                bool aRatsnest, const std::function<JSON( BOARD_COMMIT& )>& aEdit )
{
    return aProject.Mutate( aLabel, aArgs.value( "force", false ),
                            [&]() -> JSON
                            {
                                std::optional<PCB_RATSNEST> before;

                                if( aRatsnest )
                                    before = PCB_RATSNEST::Take( aSession.Board() );

                                BOARD_COMMIT commit( aSession.ToolManager() );
                                JSON         result = aEdit( commit );

                                if( !result.is_object() )
                                    result = JSON::object();

                                if( commit.Empty() && !aSession.IsModified() )
                                    throw TOOL_ERROR( "Nothing changed." );

                                commit.Push( wx( aLabel ) );
                                aSession.MarkModified();

                                if( before )
                                    result["ratsnest"] =
                                            PcbRatsnestDelta( *before, PCB_RATSNEST::Take( aSession.Board() ) );

                                JSON change{ { "summary", aLabel }, { "tool", aLabel } };
                                change["items"] = result.value( "items", JSON::array() );
                                aSession.RecordChange( change );
                                return result;
                            } );
}


namespace
{
    /// The net's class from the project's net settings. NETINFO_ITEM keeps the class it had when
    /// the board was loaded; netclasses_set changes the settings, not that copy.
    std::shared_ptr<NETCLASS> netClassOf( BOARD& aBoard, const NETINFO_ITEM* aNet )
    {
        std::shared_ptr<NET_SETTINGS>& settings = aBoard.GetDesignSettings().m_NetSettings;
        return aNet ? settings->GetEffectiveNetClass( aNet->GetNetname() ) : settings->GetDefaultNetclass();
    }


    /// The classes a net matched, as the user named them: the effective class of a net that
    /// matches SE50 is "SE50,Default" (Default fills the fields SE50 leaves unset).
    std::string netClassName( const NETCLASS& aClass )
    {
        std::string name;

        for( const NETCLASS* part : aClass.GetConstituentNetclasses() )
        {
            if( part->IsDefault() && aClass.GetConstituentNetclasses().size() > 1 )
                continue;

            name += ( name.empty() ? "" : "," ) + utf8( part->GetName() );
        }

        return name.empty() ? utf8( aClass.GetName() ) : name;
    }


    std::string cleanupKind( int aCode )
    {
        switch( aCode )
        {
        case CLEANUP_SHORTING_TRACK: return "shorting_track";
        case CLEANUP_SHORTING_VIA: return "shorting_via";
        case CLEANUP_REDUNDANT_VIA: return "redundant_via";
        case CLEANUP_DUPLICATE_TRACK: return "duplicate_track";
        case CLEANUP_MERGE_TRACKS: return "merged_tracks";
        case CLEANUP_DANGLING_TRACK: return "dangling_track";
        case CLEANUP_DANGLING_VIA: return "dangling_via";
        case CLEANUP_ZERO_LENGTH_TRACK: return "zero_length_track";
        case CLEANUP_TRACK_IN_PAD: return "track_in_pad";
        default: return "other";
        }
    }


    /// Run KiCad's track cleaner (Cleanup Tracks & Vias) with the agent's flags. A dry run marks
    /// the items it would delete; the marks are cleared again.
    std::vector<std::shared_ptr<CLEANUP_ITEM>> runCleaner( BOARD& aBoard, BOARD_COMMIT& aCommit, const JSON& aArgs,
                                                           bool aDryRun )
    {
        std::vector<std::shared_ptr<CLEANUP_ITEM>> items;
        TRACKS_CLEANER                             cleaner( &aBoard, aCommit );

        aBoard.BuildConnectivity();
        cleaner.CleanupBoard( aDryRun, &items, aArgs.value( "shorts", true ), aArgs.value( "redundantVias", true ),
                              aArgs.value( "mergeSegments", true ), aArgs.value( "dangling", true ),
                              aArgs.value( "tracksInPads", false ), aArgs.value( "danglingVias", true ) );

        if( aDryRun )
        {
            for( PCB_TRACK* track : aBoard.Tracks() )
                track->ClearFlags( IS_DELETED );
        }

        return items;
    }


    /// The cleaner's findings, with the items as they were before it ran.
    JSON cleanupJson( const std::vector<std::shared_ptr<CLEANUP_ITEM>>& aItems, const std::map<KIID, JSON>& aBefore )
    {
        JSON changes = JSON::array();
        JSON counts = JSON::object();

        for( const std::shared_ptr<CLEANUP_ITEM>& item : aItems )
        {
            const std::string kind = cleanupKind( item->GetErrorCode() );
            JSON              change{ { "kind", kind } };
            auto              it = aBefore.find( item->GetMainItemID() );

            change["item"] = it != aBefore.end() ? it->second : JSON( utf8( item->GetMainItemID().AsString() ) );
            changes.push_back( change );
            counts[kind] = counts.value( kind, 0 ) + 1;
        }

        return JSON{ { "changes", changes }, { "counts", counts } };
    }


    void fillZones( BOARD_DOC_SESSION& aSession, AGENT_SERVICES& aServices, TOOL_CONTEXT& aContext )
    {
        TOOL_MANAGER* tools = aSession.ToolManager();

        if( !tools->FindTool( ZONE_FILLER_TOOL_NAME ) )
            tools->RegisterTool( new ZONE_FILLER_TOOL );

        AGENT_PROGRESS_REPORTER progress( aContext, aServices.ProgressSink() );

        if( !tools->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, &progress, true ) )
            throw TOOL_ERROR( aContext.IsCancelled() ? "Cancelled." : "The zone fill failed." );
    }
} // namespace


void RegisterPcbEditTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    // Setup ------------------------------------------------------------------------------------

    tools.Add( { "pcb_update_from_schematic",
                 "Update the board from the project's schematic, as KiCad's Update PCB from Schematic does: adds "
                 "missing footprints (from the symbols' footprint fields, placed in a row near the origin), updates "
                 "values and nets, removes footprints of deleted symbols. Their tracks and vias stay, as in KiCad: "
                 "danglingCopper counts what now ends unconnected (pcb_cleanup removes it). Returns what changed.",
                 "pcb",
                 pcbSchema( { { "deleteUnused",
                                { { "type", "boolean" },
                                  { "description", "Remove footprints with no symbol; default true." } } },
                              { "replaceFootprints",
                                { { "type", "boolean" },
                                  { "description",
                                    "Swap footprints whose symbol now names another; default true." } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     const std::string  text = aServices.Netlist( *project );

                     NETLIST netlist;

                     try
                     {
                         KICAD_NETLIST_READER reader( new STRING_LINE_READER( text, wxS( "schematic netlist" ) ),
                                                      &netlist );
                         reader.LoadNetlist();
                     }
                     catch( const IO_ERROR& e )
                     {
                         throw TOOL_ERROR( "The schematic netlist could not be read: " + utf8( e.What() ) );
                     }

                     // The updater loads footprints through KiCad's process-wide adapter.
                     PROJECT_PCB::FootprintLibAdapter( &project->KicadProject() );
                     project->ActivateGlobalLibraryTables( LIBRARY_TABLE_TYPE::FOOTPRINT );

                     JSON result = PcbMutate(
                             *project, s, "pcb_update_from_schematic", aArgs, true,
                             [&]( BOARD_COMMIT& ) -> JSON
                             {
                                 // Footprints are told apart by UUID: a renumbered one keeps it (so it is
                                 // not new), and so does one whose package is swapped.
                                 std::map<KIID, wxString> before;

                                 for( FOOTPRINT* fp : s.Board().Footprints() )
                                     before[fp->m_Uuid] = fp->GetReference();

                                 WX_STRING_REPORTER    reporter;
                                 BOARD_NETLIST_UPDATER updater( s.ToolManager(), &s.Board() );
                                 updater.SetReporter( &reporter );
                                 updater.SetIsDryRun( false );
                                 updater.SetDeleteUnusedFootprints( aArgs.value( "deleteUnused", true ) );
                                 updater.SetReplaceFootprints( aArgs.value( "replaceFootprints", true ) );
                                 updater.SetLookupByTimestamp( true );

                                 if( !updater.UpdateNetlist( netlist ) )
                                     throw TOOL_ERROR( "The update failed:\n" + utf8( reporter.GetMessages() ) );

                                 // New footprints land at the origin: line them up so they can be seen.
                                 JSON added = JSON::array();
                                 int  x = PcbIu( 10 );

                                 std::set<KIID> after;

                                 for( FOOTPRINT* fp : s.Board().Footprints() )
                                 {
                                     after.insert( fp->m_Uuid );

                                     if( before.count( fp->m_Uuid ) )
                                         continue;

                                     BOX2I box = fp->GetBoundingBox( false );
                                     fp->SetPosition( VECTOR2I( x + box.GetWidth() / 2, PcbIu( 10 ) ) );
                                     x += box.GetWidth() + PcbIu( 2 );
                                     added.push_back( utf8( fp->GetReference() ) );
                                 }

                                 JSON removed = JSON::array();

                                 for( const auto& [uuid, reference] : before )
                                 {
                                     if( !after.count( uuid ) )
                                         removed.push_back( utf8( reference ) );
                                 }

                                 s.Board().BuildConnectivity();
                                 s.MarkModified();

                                 // Removed footprints leave their tracks and vias behind, as in KiCad.
                                 int          dangling = 0;
                                 JSON         danglingArgs{ { "shorts", false },
                                                            { "redundantVias", false },
                                                            { "mergeSegments", false },
                                                            { "dangling", true },
                                                            { "danglingVias", true } };
                                 BOARD_COMMIT probe( s.ToolManager() );

                                 for( const auto& item : runCleaner( s.Board(), probe, danglingArgs, true ) )
                                 {
                                     if( item->GetErrorCode() == CLEANUP_DANGLING_TRACK
                                         || item->GetErrorCode() == CLEANUP_DANGLING_VIA )
                                     {
                                         dangling++;
                                     }
                                 }

                                 wxString          log = reporter.GetMessages();
                                 JSON              lines = JSON::array();
                                 wxStringTokenizer tokens( log, wxS( "\n" ) );

                                 while( tokens.HasMoreTokens() )
                                     lines.push_back( utf8( tokens.GetNextToken() ) );

                                 JSON out{ { "footprintsAdded", added },
                                           { "footprintsRemoved", removed },
                                           { "report", lines },
                                           { "footprints", s.Board().Footprints().size() },
                                           { "danglingCopper", dangling } };

                                 if( dangling > 0 )
                                     out["note"] = std::to_string( dangling )
                                                   + " tracks or vias now end unconnected (copper of removed "
                                                     "footprints); pcb_cleanup removes them.";

                                 return out;
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_query",
                 "List board items, filtered by type (footprint, track, arc, via, pad, zone, graphic, text, net), "
                 "reference, net, layer or area {x1,y1,x2,y2}; paged with offset and limit. Tracks and arcs give "
                 "their length (arcs also mid); a net gives trackLength, its tracks' and arcs' total, and its vias.",
                 "pcb",
                 pcbSchema( { { "type", { { "type", "string" } } },
                              { "reference", { { "type", "string" } } },
                              { "net", { { "type", "string" } } },
                              { "layer", { { "type", "string" } } },
                              { "area", { { "type", "object" } } },
                              { "offset", { { "type", "integer" }, { "minimum", 0 } } },
                              { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     BOARD_DOC_SESSION& s = session( aServices, aArgs );
                     BOARD&             board = s.Board();
                     const std::string  type = aArgs.value( "type", "" );
                     const size_t       offset = aArgs.value( "offset", size_t( 0 ) );
                     const size_t       limit = aArgs.value( "limit", size_t( 100 ) );
                     JSON               items = JSON::array();
                     size_t             total = 0;

                     if( type == "net" )
                     {
                         for( NETINFO_ITEM* net : board.GetNetInfo() )
                         {
                             if( net->GetNetCode() <= 0 )
                                 continue;

                             if( total++ >= offset && items.size() < limit )
                             {
                                 // The copper's own length (tracks and arcs), as the board shows it; vias
                                 // are counted, not measured.
                                 double length = 0;
                                 int    vias = 0;

                                 for( PCB_TRACK* track : board.Tracks() )
                                 {
                                     if( track->GetNetCode() != net->GetNetCode() )
                                         continue;

                                     if( track->Type() == PCB_VIA_T )
                                         vias++;
                                     else
                                         length += track->GetLength();
                                 }

                                 items.push_back( { { "net", utf8( net->GetNetname() ) },
                                                    { "netclass", netClassName( *netClassOf( board, net ) ) },
                                                    { "pads", board.GetConnectivity()
                                                                      ->GetNetItems( net->GetNetCode(), { PCB_PAD_T } )
                                                                      .size() },
                                                    { "trackLength", mm( KiROUND( length ) ) },
                                                    { "vias", vias } } );
                             }
                         }

                         return TOOL_RESULT::Data( JSON{ { "items", items }, { "total", total } } );
                     }

                     std::optional<BOX2I> area;

                     if( aArgs.contains( "area" ) )
                     {
                         const JSON& a = aArgs["area"];
                         VECTOR2I    p1( PcbLength( a.at( "x1" ), "x1" ), PcbLength( a.at( "y1" ), "y1" ) );
                         VECTOR2I    p2( PcbLength( a.at( "x2" ), "x2" ), PcbLength( a.at( "y2" ), "y2" ) );
                         area = BOX2I( p1, p2 - p1 );
                         area->Normalize();
                     }

                     std::optional<PCB_LAYER_ID> layer;

                     if( aArgs.contains( "layer" ) )
                         layer = PcbLayer( board, aArgs["layer"] );

                     NETINFO_ITEM* net = aArgs.contains( "net" ) ? PcbNet( board, aArgs["net"] ) : nullptr;

                     auto consider = [&]( BOARD_ITEM* aItem )
                     {
                         JSON json = PcbItemJson( board, aItem );

                         if( !type.empty() && json["type"] != type )
                             return;

                         if( aArgs.contains( "reference" )
                             && json.value( "reference", "" ) != aArgs["reference"].get<std::string>() )
                             return;

                         if( net )
                         {
                             BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( aItem );

                             if( !connected || connected->GetNet() != net )
                                 return;
                         }

                         if( layer && !aItem->IsOnLayer( *layer ) )
                             return;

                         if( area && !area->Intersects( aItem->GetBoundingBox() ) )
                             return;

                         if( total++ >= offset && items.size() < limit )
                             items.push_back( json );
                     };

                     for( FOOTPRINT* fp : board.Footprints() )
                     {
                         consider( fp );

                         if( type == "pad" )
                         {
                             for( PAD* pad : fp->Pads() )
                                 consider( pad );
                         }
                     }

                     for( PCB_TRACK* track : board.Tracks() )
                         consider( track );

                     for( ZONE* zone : board.Zones() )
                         consider( zone );

                     for( BOARD_ITEM* item : board.Drawings() )
                         consider( item );

                     JSON out{ { "items", items }, { "total", total } };

                     if( offset + items.size() < total )
                         out["next"] = offset + items.size();

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "pcb_outline_set",
                 "Set the board outline on Edge.Cuts, replacing the old one: a rectangle {x, y, width, height} or "
                 "a polygon of points, with optional corner radius (mm).",
                 "pcb",
                 pcbSchema( { { "rectangle", { { "type", "object" } } },
                              { "polygon", { { "type", "array" } } },
                              { "cornerRadius", { { "type", { "number", "string" } } } },
                              { "lineWidth", { { "type", { "number", "string" } } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*        project = nullptr;
                     BOARD_DOC_SESSION&    s = session( aServices, aArgs, &project );
                     BOARD&                board = s.Board();
                     std::vector<VECTOR2I> corners;

                     if( aArgs.contains( "rectangle" ) )
                     {
                         const JSON& r = aArgs["rectangle"];
                         const int   x = PcbLength( r.value( "x", JSON( 0 ) ), "x" );
                         const int   y = PcbLength( r.value( "y", JSON( 0 ) ), "y" );
                         const int   w = PcbLength( r.at( "width" ), "width" );
                         const int   h = PcbLength( r.at( "height" ), "height" );
                         requirePositiveLength( w, "width" );
                         requirePositiveLength( h, "height" );
                         corners = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
                     }
                     else if( aArgs.contains( "polygon" ) )
                     {
                         for( const JSON& point : aArgs["polygon"] )
                             corners.push_back( PcbPoint( board, point, "polygon" ) );

                         if( corners.size() < 3 )
                             throw TOOL_ERROR( "A polygon outline needs at least 3 points." );
                     }
                     else
                     {
                         throw TOOL_ERROR( "Give rectangle or polygon." );
                     }

                     const int radius =
                             aArgs.contains( "cornerRadius" ) ? PcbLength( aArgs["cornerRadius"], "cornerRadius" ) : 0;
                     const int width = aArgs.contains( "lineWidth" ) ? PcbLength( aArgs["lineWidth"], "lineWidth" )
                                                                     : PcbIu( 0.1 );

                     JSON result = PcbMutate( *project, s, "pcb_outline_set", aArgs, false,
                                              [&]( BOARD_COMMIT& aCommit ) -> JSON
                                              {
                                                  int removed = 0;

                                                  for( BOARD_ITEM* item : board.Drawings() )
                                                  {
                                                      if( item->GetLayer() == Edge_Cuts && item->Type() == PCB_SHAPE_T )
                                                      {
                                                          aCommit.Remove( item );
                                                          removed++;
                                                      }
                                                  }

                                                  JSON kiids = JSON::array();
                                                  addOutline( board, aCommit, corners, radius, width, kiids );
                                                  return JSON{ { "removed", removed }, { "items", kiids } };
                                              } );

                     BOX2I box = board.GetBoardEdgesBoundingBox();
                     result["bbox"] = { { "x1", mm( box.GetLeft() ) },
                                        { "y1", mm( box.GetTop() ) },
                                        { "x2", mm( box.GetRight() ) },
                                        { "y2", mm( box.GetBottom() ) } };
                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_stackup_get", "The board's copper layer count, thickness and stackup layers.", "pcb",
                 pcbSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     BOARD_DOC_SESSION&     s = session( aServices, aArgs );
                     BOARD_DESIGN_SETTINGS& bds = s.Board().GetDesignSettings();
                     JSON                   layers = JSON::array();

                     for( BOARD_STACKUP_ITEM* item : bds.GetStackupDescriptor().GetList() )
                     {
                         // Board layers by their board name; dielectrics are numbered from the top.
                         const std::string name =
                                 item->GetBrdLayerId() != UNDEFINED_LAYER
                                         ? PcbLayerName( s.Board(), item->GetBrdLayerId() )
                                         : "Dielectric " + std::to_string( item->GetDielectricLayerId() );

                         layers.push_back( { { "name", name },
                                             { "type", utf8( item->GetTypeName() ) },
                                             { "thickness", mm( item->GetThickness() ) },
                                             { "material", utf8( item->GetMaterial() ) } } );
                     }

                     return TOOL_RESULT::Data( JSON{ { "copperLayers", s.Board().GetCopperLayerCount() },
                                                     { "thickness", mm( bds.GetBoardThickness() ) },
                                                     { "layers", layers } } );
                 } } );

    tools.Add( { "pcb_stackup_set", "Set the copper layer count (2, 4, 6, ...) and board thickness (mm).", "pcb",
                 pcbSchema( { { "copperLayers", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 32 } } },
                              { "thickness", { { "type", { "number", "string" } } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );

                     JSON result = project->Mutate(
                             "pcb_stackup_set", aArgs.value( "force", false ),
                             [&]() -> JSON
                             {
                                 BOARD&                 board = s.Board();
                                 BOARD_DESIGN_SETTINGS& bds = board.GetDesignSettings();

                                 if( aArgs.contains( "copperLayers" ) )
                                 {
                                     const int count = aArgs["copperLayers"];

                                     if( count != 1 && count % 2 )
                                         throw TOOL_ERROR( "Copper layer count must be 1 or even." );

                                     board.SetCopperLayerCount( count );
                                     board.SetEnabledLayers( board.GetEnabledLayers() | LSET::AllCuMask( count ) );
                                 }

                                 if( aArgs.contains( "thickness" ) )
                                     bds.SetBoardThickness( PcbLength( aArgs["thickness"], "thickness" ) );

                                 bds.GetStackupDescriptor().BuildDefaultStackupList( &bds,
                                                                                     board.GetCopperLayerCount() );
                                 s.MarkModified();
                                 s.RecordChange( { { "summary", "pcb_stackup_set" }, { "tool", "pcb_stackup_set" } } );
                                 return JSON{ { "copperLayers", board.GetCopperLayerCount() },
                                              { "thickness", mm( bds.GetBoardThickness() ) } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_rules_get",
                 "The board's design rule minimums (mm) and its custom rules (.kicad_dru text). Net class sizes "
                 "are in netclasses_get.",
                 "pcb", pcbSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*         project = nullptr;
                     BOARD_DOC_SESSION&     s = session( aServices, aArgs, &project );
                     BOARD_DESIGN_SETTINGS& bds = s.Board().GetDesignSettings();
                     const fs::path         rules = fs::path( utf8( s.Board().GetDesignRulesPath() ) );

                     return TOOL_RESULT::Data(
                             JSON{ { "minClearance", mm( bds.m_MinClearance ) },
                                   { "minTrackWidth", mm( bds.m_TrackMinWidth ) },
                                   { "minViaDiameter", mm( bds.m_ViasMinSize ) },
                                   { "minViaAnnularWidth", mm( bds.m_ViasMinAnnularWidth ) },
                                   { "minThroughDrill", mm( bds.m_MinThroughDrill ) },
                                   { "holeClearance", mm( bds.m_HoleClearance ) },
                                   { "holeToHole", mm( bds.m_HoleToHoleMin ) },
                                   { "copperEdgeClearance", mm( bds.m_CopperEdgeClearance ) },
                                   { "silkClearance", mm( bds.m_SilkClearance ) },
                                   { "customRules", fs::exists( rules ) ? FILE_STORE::ReadFile( rules ) : "" } } );
                 } } );

    tools.Add( { "pcb_rules_set",
                 "Set design rule minimums (mm or with units; keys as pcb_rules_get returns them) and/or replace the "
                 "custom rules text. The text is checked by KiCad's rules parser first and refused if it has errors "
                 "(see docs_rules_syntax).",
                 "pcb",
                 pcbSchema( { { "minClearance", { { "type", { "number", "string" } } } },
                              { "minTrackWidth", { { "type", { "number", "string" } } } },
                              { "minViaDiameter", { { "type", { "number", "string" } } } },
                              { "minViaAnnularWidth", { { "type", { "number", "string" } } } },
                              { "minThroughDrill", { { "type", { "number", "string" } } } },
                              { "holeClearance", { { "type", { "number", "string" } } } },
                              { "holeToHole", { { "type", { "number", "string" } } } },
                              { "copperEdgeClearance", { { "type", { "number", "string" } } } },
                              { "silkClearance", { { "type", { "number", "string" } } } },
                              { "customRules", { { "type", "string" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );

                     if( aArgs.contains( "customRules" ) )
                     {
                         std::vector<std::shared_ptr<DRC_RULE>> parsed;
                         WX_STRING_REPORTER                     reporter;

                         try
                         {
                             DRC_RULES_PARSER parser( wx( aArgs["customRules"] ), wxS( "customRules" ) );
                             parser.Parse( parsed, &reporter );
                         }
                         catch( const PARSE_ERROR& e )
                         {
                             throw TOOL_ERROR( "The custom rules do not parse: " + utf8( e.What() ) );
                         }

                         if( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) )
                         {
                             // The parser links each message to its place for the GUI: <a href='3:1'>text</a>.
                             static const std::regex link( "<a href='([0-9]+):([0-9]+)'>(.*?)</a>" );
                             const std::string       plain = std::regex_replace( utf8( reporter.GetMessages() ), link,
                                                                                 "line $1, column $2: $3" );

                             throw TOOL_ERROR( "The custom rules have errors:\n" + plain );
                         }
                     }

                     JSON result = project->Mutate(
                             "pcb_rules_set", aArgs.value( "force", false ),
                             [&]() -> JSON
                             {
                                 BOARD_DESIGN_SETTINGS&             bds = s.Board().GetDesignSettings();
                                 const std::pair<const char*, int*> fields[] = {
                                     { "minClearance", &bds.m_MinClearance },
                                     { "minTrackWidth", &bds.m_TrackMinWidth },
                                     { "minViaDiameter", &bds.m_ViasMinSize },
                                     { "minViaAnnularWidth", &bds.m_ViasMinAnnularWidth },
                                     { "minThroughDrill", &bds.m_MinThroughDrill },
                                     { "holeClearance", &bds.m_HoleClearance },
                                     { "holeToHole", &bds.m_HoleToHoleMin },
                                     { "copperEdgeClearance", &bds.m_CopperEdgeClearance },
                                     { "silkClearance", &bds.m_SilkClearance },
                                 };

                                 bool settings = false;

                                 for( const auto& [name, field] : fields )
                                 {
                                     if( aArgs.contains( name ) )
                                     {
                                         *field = PcbLength( aArgs[name], name );
                                         settings = true;
                                     }
                                 }

                                 // The board's design settings live in the project file.
                                 if( settings )
                                     project->TouchProjectFile();

                                 if( aArgs.contains( "customRules" ) )
                                 {
                                     const fs::path    rules = fs::path( utf8( s.Board().GetDesignRulesPath() ) );
                                     const std::string rel = project->Files().Relative( rules );
                                     project->BeforeFileWrite( rel );
                                     FILE_STORE::WriteAtomic( rules, aArgs["customRules"].get<std::string>() );
                                     project->AfterFileWrite( rel );
                                 }

                                 // DRC compiles the rules again on its next run.
                                 bds.m_DRCEngine->InitEngine( s.Board().GetDesignRulesPath() );
                                 return JSON{ { "updated", true } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    // Placement --------------------------------------------------------------------------------

    tools.Add( { "pcb_footprint_place",
                 "Place a library footprint (Library:Footprint) at a position (mm), with rotation (degrees) and side "
                 "(front or back). Give reference and value. Footprints from the schematic come with "
                 "pcb_update_from_schematic; this is for extra parts (mounting holes, logos, test points).",
                 "pcb",
                 pcbSchema( { { "footprint", { { "type", "string" } } },
                              { "position", POINT },
                              { "rotation", { { "type", { "number", "string" } } } },
                              { "side", { { "type", "string" }, { "enum", { "front", "back" } } } },
                              { "reference", { { "type", "string" } } },
                              { "value", { { "type", "string" } } } },
                            { "footprint", "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     LIB_ID             id;

                     if( id.Parse( wx( aArgs["footprint"] ) ) >= 0 || !id.IsValid() )
                         throw TOOL_ERROR( "A footprint id is Library:Footprint." );

                     JSON  errors = JSON::array();
                     auto& adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER&>(
                             LoadLibraries( *project, LIBRARY_TABLE_TYPE::FOOTPRINT, aContext, errors ) );
                     FOOTPRINT* footprint = adapter.LoadFootprint( id, false );

                     if( !footprint )
                         throw TOOL_ERROR( "No footprint '" + aArgs["footprint"].get<std::string>()
                                           + "'; use footprint_search." );

                     JSON result = PcbMutate(
                             *project, s, "pcb_footprint_place " + aArgs["footprint"].get<std::string>(), aArgs, true,
                             [&]( BOARD_COMMIT& aCommit ) -> JSON
                             {
                                 footprint->SetParent( &s.Board() );
                                 footprint->SetFPID( id );
                                 footprint->SetPosition( PcbPoint( s.Board(), aArgs["position"], "position" ) );

                                 if( aArgs.contains( "rotation" ) )
                                     footprint->SetOrientation(
                                             EDA_ANGLE( ParseAngleDeg( aArgs["rotation"], "rotation" ), DEGREES_T ) );

                                 if( aArgs.value( "side", "front" ) == "back" )
                                     footprint->Flip( footprint->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

                                 if( aArgs.contains( "reference" ) )
                                     footprint->SetReference( wx( aArgs["reference"] ) );

                                 if( aArgs.contains( "value" ) )
                                     footprint->SetValue( wx( aArgs["value"] ) );

                                 aCommit.Add( footprint );
                                 return JSON{ { "created", PcbItemJson( s.Board(), footprint ) },
                                              { "items", { utf8( footprint->m_Uuid.AsString() ) } } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    auto transform = [&aServices]( const std::string& aName, const std::string& aDescription, JSON aExtra,
                                   std::function<void( BOARD_ITEM*, const JSON&, BOARD& )> aApply )
    {
        JSON properties = aExtra;
        properties["items"] = ITEMS_ARG;

        aServices.Tools().Add(
                { aName, aDescription, "pcb", pcbSchema( properties, { "items" } ),
                  [&aServices, aName, aApply]( const JSON& aArgs, TOOL_CONTEXT& )
                  {
                      AGENT_PROJECT*           project = nullptr;
                      BOARD_DOC_SESSION&       s = session( aServices, aArgs, &project );
                      std::vector<BOARD_ITEM*> items;

                      for( const JSON& handle : aArgs["items"] )
                      {
                          BOARD_ITEM* item = PcbFindOne( s.Board(), handle );

                          if( item->Type() == PCB_PAD_T )
                              throw TOOL_ERROR( "Pads move with their footprint; name the footprint." );

                          items.push_back( item );
                      }

                      JSON result =
                              PcbMutate( *project, s, aName, aArgs, true,
                                         [&]( BOARD_COMMIT& aCommit ) -> JSON
                                         {
                                             JSON changed = JSON::array();
                                             JSON kiids = JSON::array();

                                             for( BOARD_ITEM* item : items )
                                             {
                                                 JSON before = PcbItemJson( s.Board(), item );
                                                 aCommit.Modify( item );
                                                 aApply( item, aArgs, s.Board() );
                                                 changed.push_back( { { "before", before },
                                                                      { "after", PcbItemJson( s.Board(), item ) } } );
                                                 kiids.push_back( utf8( item->m_Uuid.AsString() ) );
                                             }

                                             return JSON{ { "changed", changed }, { "items", kiids } };
                                         } );

                      return TOOL_RESULT::Data( result );
                  } } );
    };

    transform( "pcb_move", "Move items to a position \"to\" (footprints: their origin) or by \"delta\" {x, y} in mm.",
               { { "to", POINT }, { "delta", POINT } },
               []( BOARD_ITEM* aItem, const JSON& aArgs, BOARD& aBoard )
               {
                   if( aArgs.contains( "to" ) )
                       aItem->SetPosition( PcbPoint( aBoard, aArgs["to"], "to" ) );
                   else if( aArgs.contains( "delta" ) )
                       aItem->Move( PcbPoint( aBoard, aArgs["delta"], "delta" ) );
                   else
                       throw TOOL_ERROR( "Give to or delta." );
               } );

    transform(
            "pcb_rotate",
            "Rotate items by an angle (degrees, counterclockwise) about their position, or set "
            "a footprint's absolute rotation with \"to\".",
            { { "angle", { { "type", { "number", "string" } } } }, { "to", { { "type", { "number", "string" } } } } },
            []( BOARD_ITEM* aItem, const JSON& aArgs, BOARD& )
            {
                if( aArgs.contains( "to" ) && aItem->Type() == PCB_FOOTPRINT_T )
                {
                    static_cast<FOOTPRINT*>( aItem )->SetOrientation(
                            EDA_ANGLE( ParseAngleDeg( aArgs["to"], "to" ), DEGREES_T ) );
                    return;
                }

                aItem->Rotate( aItem->GetPosition(),
                               EDA_ANGLE( aArgs.contains( "angle" ) ? ParseAngleDeg( aArgs["angle"], "angle" ) : 90.0,
                                          DEGREES_T ) );
            } );

    transform( "pcb_flip", "Flip footprints (or other items) to the other side of the board.", JSON::object(),
               []( BOARD_ITEM* aItem, const JSON&, BOARD& )
               {
                   aItem->Flip( aItem->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );
               } );

    tools.Add( { "pcb_delete", "Delete board items (footprints, tracks, vias, zones, graphics, text).", "pcb",
                 pcbSchema( { { "items", ITEMS_ARG } }, { "items" } ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*           project = nullptr;
                     BOARD_DOC_SESSION&       s = session( aServices, aArgs, &project );
                     std::vector<BOARD_ITEM*> items;

                     for( const JSON& handle : aArgs["items"] )
                     {
                         BOARD_ITEM* item = PcbFindOne( s.Board(), handle );

                         if( item->Type() == PCB_PAD_T )
                             throw TOOL_ERROR( "Pads are deleted with their footprint." );

                         items.push_back( item );
                     }

                     JSON result = PcbMutate( *project, s, "pcb_delete", aArgs, true,
                                              [&]( BOARD_COMMIT& aCommit ) -> JSON
                                              {
                                                  JSON removed = JSON::array();

                                                  for( BOARD_ITEM* item : items )
                                                  {
                                                      removed.push_back( PcbItemJson( s.Board(), item ) );
                                                      aCommit.Remove( item );
                                                  }

                                                  return JSON{ { "removed", removed } };
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    // Copper -----------------------------------------------------------------------------------

    tools.Add( { "pcb_track_add",
                 "Draw a track through points (mm, or pads \"U1.3\") on a copper layer. width defaults to the net "
                 "class track width; the net comes from the first pad unless given. Returns the ratsnest delta. "
                 "Route each connection pad to pad; add vias with pcb_via_add to change layer.",
                 "pcb",
                 pcbSchema( { { "points", { { "type", "array" }, { "minItems", 2 } } },
                              { "layer", { { "type", "string" } } },
                              { "width", { { "type", { "number", "string" } } } },
                              { "net", { { "type", "string" } } } },
                            { "points" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*        project = nullptr;
                     BOARD_DOC_SESSION&    s = session( aServices, aArgs, &project );
                     BOARD&                board = s.Board();
                     NETINFO_ITEM*         net = aArgs.contains( "net" ) ? PcbNet( board, aArgs["net"] ) : nullptr;
                     std::vector<VECTOR2I> points;

                     for( const JSON& point : aArgs["points"] )
                     {
                         if( point.is_string() && !net )
                             net = PcbPad( board, point )->GetNet();

                         points.push_back( PcbPoint( board, point, "points" ) );
                     }

                     const PCB_LAYER_ID layer = PcbLayer( board, aArgs.value( "layer", "F.Cu" ) );

                     if( !IsCopperLayer( layer ) )
                         throw TOOL_ERROR( "Tracks go on copper layers (F.Cu, B.Cu, In1.Cu, ...)." );

                     int width = aArgs.contains( "width" ) ? PcbLength( aArgs["width"], "width" ) : 0;

                     if( width <= 0 )
                         width = net ? netClassOf( board, net )->GetTrackWidth()
                                     : board.GetDesignSettings().GetCurrentTrackWidth();

                     if( width <= 0 )
                         width = PcbIu( 0.25 );

                     JSON result = PcbMutate( *project, s, "pcb_track_add", aArgs, true,
                                              [&]( BOARD_COMMIT& aCommit ) -> JSON
                                              {
                                                  JSON kiids = JSON::array();

                                                  for( size_t i = 1; i < points.size(); ++i )
                                                  {
                                                      auto* track = new PCB_TRACK( &board );
                                                      track->SetStart( points[i - 1] );
                                                      track->SetEnd( points[i] );
                                                      track->SetWidth( width );
                                                      track->SetLayer( layer );

                                                      if( net )
                                                          track->SetNet( net );

                                                      aCommit.Add( track );
                                                      kiids.push_back( utf8( track->m_Uuid.AsString() ) );
                                                  }

                                                  return JSON{ { "items", kiids },
                                                               { "net", net ? utf8( net->GetNetname() ) : "" },
                                                               { "width", mm( width ) } };
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_via_add", "Add a through via at a position on a net. Sizes default to the net class.", "pcb",
                 pcbSchema( { { "position", POINT },
                              { "net", { { "type", "string" } } },
                              { "diameter", { { "type", { "number", "string" } } } },
                              { "drill", { { "type", { "number", "string" } } } } },
                            { "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();
                     NETINFO_ITEM*      net = aArgs.contains( "net" ) ? PcbNet( board, aArgs["net"] ) : nullptr;
                     const std::shared_ptr<NETCLASS> netclass = netClassOf( board, net );
                     const int diameter = aArgs.contains( "diameter" ) ? PcbLength( aArgs["diameter"], "diameter" )
                                                                       : netclass->GetViaDiameter();
                     const int drill =
                             aArgs.contains( "drill" ) ? PcbLength( aArgs["drill"], "drill" ) : netclass->GetViaDrill();

                     if( drill >= diameter )
                         throw TOOL_ERROR( "The drill must be smaller than the via diameter." );

                     JSON result = PcbMutate( *project, s, "pcb_via_add", aArgs, true,
                                              [&]( BOARD_COMMIT& aCommit ) -> JSON
                                              {
                                                  auto* via = new PCB_VIA( &board );
                                                  via->SetPosition( PcbPoint( board, aArgs["position"], "position" ) );
                                                  via->SetViaType( VIATYPE::THROUGH );
                                                  via->SetLayerPair( F_Cu, B_Cu );
                                                  via->SetWidth( PADSTACK::ALL_LAYERS, diameter );
                                                  via->SetDrill( drill );

                                                  if( net )
                                                      via->SetNet( net );

                                                  aCommit.Add( via );
                                                  return JSON{ { "created", PcbItemJson( board, via ) },
                                                               { "items", { utf8( via->m_Uuid.AsString() ) } } };
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_zone_add",
                 "Add a copper zone (pour) on a net: a polygon of points or the board outline (outline:true), on "
                 "layers, with clearance, minimum width, priority and pad connection (thermal or solid). Fill it with "
                 "pcb_zones_fill.",
                 "pcb",
                 pcbSchema( { { "net", { { "type", "string" } } },
                              { "layers", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                              { "polygon", { { "type", "array" } } },
                              { "outline", { { "type", "boolean" } } },
                              { "clearance", { { "type", { "number", "string" } } } },
                              { "minWidth", { { "type", { "number", "string" } } } },
                              { "priority", { { "type", "integer" }, { "minimum", 0 } } },
                              { "connection", { { "type", "string" }, { "enum", { "thermal", "solid", "none" } } } } },
                            { "net" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();
                     NETINFO_ITEM*      net = PcbNet( board, aArgs["net"] );
                     LSET               layers;

                     for( const JSON& name : aArgs.value( "layers", JSON::array( { "F.Cu" } ) ) )
                         layers.set( PcbLayer( board, name ) );

                     std::vector<VECTOR2I> corners;

                     if( aArgs.value( "outline", false ) )
                     {
                         BOX2I box = board.GetBoardEdgesBoundingBox();

                         if( box.GetWidth() <= 0 )
                             throw TOOL_ERROR( "The board has no outline yet; set it with pcb_outline_set." );

                         corners = { box.GetOrigin(), VECTOR2I( box.GetRight(), box.GetTop() ), box.GetEnd(),
                                     VECTOR2I( box.GetLeft(), box.GetBottom() ) };
                     }
                     else
                     {
                         for( const JSON& point : aArgs.value( "polygon", JSON::array() ) )
                             corners.push_back( PcbPoint( board, point, "polygon" ) );
                     }

                     if( corners.size() < 3 )
                         throw TOOL_ERROR( "A zone needs a polygon of at least 3 points, or outline:true." );

                     JSON result =
                             PcbMutate( *project, s, "pcb_zone_add " + aArgs["net"].get<std::string>(), aArgs, false,
                                        [&]( BOARD_COMMIT& aCommit ) -> JSON
                                        {
                                            auto* zone = new ZONE( &board );
                                            zone->SetLayerSet( layers );
                                            zone->SetNet( net );
                                            zone->Outline()->NewOutline();

                                            for( const VECTOR2I& corner : corners )
                                                zone->Outline()->Append( corner.x, corner.y );

                                            if( aArgs.contains( "clearance" ) )
                                                zone->SetLocalClearance( PcbLength( aArgs["clearance"], "clearance" ) );

                                            if( aArgs.contains( "minWidth" ) )
                                                zone->SetMinThickness( PcbLength( aArgs["minWidth"], "minWidth" ) );

                                            zone->SetAssignedPriority( aArgs.value( "priority", 0 ) );

                                            const std::string connection = aArgs.value( "connection", "thermal" );
                                            zone->SetPadConnection( connection == "solid"  ? ZONE_CONNECTION::FULL
                                                                    : connection == "none" ? ZONE_CONNECTION::NONE
                                                                                           : ZONE_CONNECTION::THERMAL );
                                            aCommit.Add( zone );
                                            return JSON{ { "created", PcbItemJson( board, zone ) },
                                                         { "items", { utf8( zone->m_Uuid.AsString() ) } } };
                                        } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_zones_fill",
                 "Fill every copper zone (as KiCad's Fill All Zones does: teardrops, fills, then generators). Long "
                 "on big boards; reports progress and can be cancelled.",
                 "pcb", pcbSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );

                     JSON result = project->Mutate(
                             "pcb_zones_fill", aArgs.value( "force", false ),
                             [&]() -> JSON
                             {
                                 PCB_RATSNEST  before = PCB_RATSNEST::Take( s.Board() );
                                 TOOL_MANAGER* tools = s.ToolManager();

                                 if( !tools->FindTool( ZONE_FILLER_TOOL_NAME ) )
                                     tools->RegisterTool( new ZONE_FILLER_TOOL );

                                 AGENT_PROGRESS_REPORTER progress( aContext, aServices.ProgressSink() );

                                 if( !tools->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, &progress, true ) )
                                     throw TOOL_ERROR( aContext.IsCancelled() ? "Cancelled."
                                                                              : "The zone fill failed." );

                                 s.MarkModified();
                                 JSON zones = JSON::array();

                                 for( ZONE* zone : s.Board().Zones() )
                                     zones.push_back( PcbItemJson( s.Board(), zone ) );

                                 s.RecordChange( { { "summary", "pcb_zones_fill" }, { "tool", "pcb_zones_fill" } } );
                                 return JSON{ { "zones", zones },
                                              { "ratsnest",
                                                PcbRatsnestDelta( before, PCB_RATSNEST::Take( s.Board() ) ) } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "pcb_cleanup",
                 "Clean up tracks and vias as KiCad's Cleanup Tracks & Vias does: remove tracks and vias that lead "
                 "nowhere (dangling, e.g. left by removed footprints), redundant vias, tracks shorting two nets, and "
                 "merge collinear segments; tracksInPads also removes tracks inside a pad. Each flag defaults to the "
                 "dialog's default. Zones are filled first (refillZones, default true) so copper ending in a plane "
                 "counts as connected. dryRun lists what would change, with the zones as they are. Locked items "
                 "stay.",
                 "pcb",
                 pcbSchema( { { "dangling", { { "type", "boolean" } } },
                              { "danglingVias", { { "type", "boolean" } } },
                              { "redundantVias", { { "type", "boolean" } } },
                              { "shorts", { { "type", "boolean" } } },
                              { "mergeSegments", { { "type", "boolean" } } },
                              { "tracksInPads", { { "type", "boolean" } } },
                              { "refillZones", { { "type", "boolean" } } },
                              { "dryRun", { { "type", "boolean" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();

                     auto snapshot = [&]()
                     {
                         std::map<KIID, JSON> items;

                         for( PCB_TRACK* track : board.Tracks() )
                             items[track->m_Uuid] = PcbItemJson( board, track );

                         return items;
                     };

                     const bool refill = aArgs.value( "refillZones", true );

                     if( aArgs.value( "dryRun", false ) || !refill )
                     {
                         BOARD_COMMIT probe( s.ToolManager() );
                         JSON         found = cleanupJson( runCleaner( board, probe, aArgs, true ), snapshot() );
                         found["dryRun"] = true;

                         // Nothing to do, or only asked what would change: the board is untouched.
                         if( aArgs.value( "dryRun", false ) || found["changes"].empty() )
                             return TOOL_RESULT::Data( found );
                     }

                     JSON result = PcbMutate( *project, s, "pcb_cleanup", aArgs, true,
                                              [&]( BOARD_COMMIT& aCommit ) -> JSON
                                              {
                                                  if( refill )
                                                  {
                                                      fillZones( s, aServices, aContext );
                                                      s.MarkModified();
                                                  }

                                                  const std::map<KIID, JSON> before = snapshot();
                                                  JSON out = cleanupJson( runCleaner( board, aCommit, aArgs, false ),
                                                                          before );

                                                  if( refill && !out["changes"].empty() )
                                                      fillZones( s, aServices, aContext );

                                                  out["dryRun"] = false;
                                                  return out;
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "pcb_graphic_add",
              "Draw a graphic on a layer (silkscreen, fab, user, Edge.Cuts...): line {start, end}, rectangle "
              "{start, end}, circle {center, radius}, polygon {points}.",
              "pcb",
              pcbSchema(
                      { { "kind", { { "type", "string" }, { "enum", { "line", "rectangle", "circle", "polygon" } } } },
                        { "layer", { { "type", "string" } } },
                        { "start", POINT },
                        { "end", POINT },
                        { "center", POINT },
                        { "radius", { { "type", { "number", "string" } } } },
                        { "points", { { "type", "array" } } },
                        { "width", { { "type", { "number", "string" } } } },
                        { "filled", { { "type", "boolean" } } } },
                      { "kind", "layer" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT*     project = nullptr;
                  BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                  BOARD&             board = s.Board();
                  const std::string  kind = aArgs["kind"];

                  JSON result = PcbMutate(
                          *project, s, "pcb_graphic_add", aArgs, false,
                          [&]( BOARD_COMMIT& aCommit ) -> JSON
                          {
                              auto* shape = new PCB_SHAPE( &board, kind == "line"        ? SHAPE_T::SEGMENT
                                                                   : kind == "rectangle" ? SHAPE_T::RECTANGLE
                                                                   : kind == "circle"    ? SHAPE_T::CIRCLE
                                                                                         : SHAPE_T::POLY );
                              shape->SetLayer( PcbLayer( board, aArgs["layer"] ) );
                              shape->SetWidth( aArgs.contains( "width" ) ? PcbLength( aArgs["width"], "width" )
                                                                         : PcbIu( 0.15 ) );

                              if( kind == "circle" )
                              {
                                  VECTOR2I center = PcbPoint( board, aArgs.at( "center" ), "center" );
                                  shape->SetStart( center );
                                  shape->SetEnd( center + VECTOR2I( PcbLength( aArgs.at( "radius" ), "radius" ), 0 ) );
                              }
                              else if( kind == "polygon" )
                              {
                                  std::vector<VECTOR2I> points;

                                  for( const JSON& p : aArgs.at( "points" ) )
                                      points.push_back( PcbPoint( board, p, "points" ) );

                                  shape->SetPolyPoints( points );
                              }
                              else
                              {
                                  shape->SetStart( PcbPoint( board, aArgs.at( "start" ), "start" ) );
                                  shape->SetEnd( PcbPoint( board, aArgs.at( "end" ), "end" ) );
                              }

                              if( aArgs.value( "filled", false ) )
                                  shape->SetFilled( true );

                              aCommit.Add( shape );
                              return JSON{ { "created", PcbItemJson( board, shape ) },
                                           { "items", { utf8( shape->m_Uuid.AsString() ) } } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "pcb_text_add", "Add text on a layer (default F.SilkS), with size and rotation.", "pcb",
                 pcbSchema( { { "text", { { "type", "string" } } },
                              { "position", POINT },
                              { "layer", { { "type", "string" } } },
                              { "size", { { "type", { "number", "string" } } } },
                              { "rotation", { { "type", { "number", "string" } } } } },
                            { "text", "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();

                     JSON result =
                             PcbMutate( *project, s, "pcb_text_add", aArgs, false,
                                        [&]( BOARD_COMMIT& aCommit ) -> JSON
                                        {
                                            auto* text = new PCB_TEXT( &board );
                                            text->SetText( wx( aArgs["text"] ) );
                                            text->SetPosition( PcbPoint( board, aArgs["position"], "position" ) );
                                            text->SetLayer( PcbLayer( board, aArgs.value( "layer", "F.SilkS" ) ) );

                                            if( aArgs.contains( "size" ) )
                                            {
                                                int size = PcbLength( aArgs["size"], "size" );
                                                text->SetTextSize( VECTOR2I( size, size ) );
                                                text->SetTextThickness( std::max( 1, size / 6 ) );
                                            }

                                            if( aArgs.contains( "rotation" ) )
                                                text->SetTextAngle( EDA_ANGLE(
                                                        ParseAngleDeg( aArgs["rotation"], "rotation" ), DEGREES_T ) );

                                            if( IsBackLayer( text->GetLayer() ) )
                                                text->SetMirrored( true );

                                            aCommit.Add( text );
                                            return JSON{ { "created", PcbItemJson( board, text ) },
                                                         { "items", { utf8( text->m_Uuid.AsString() ) } } };
                                        } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
