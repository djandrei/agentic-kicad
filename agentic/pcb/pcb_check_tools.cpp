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
#include <agentic/core/file_store.h>
#include <agentic/core/handles.h>
#include <agentic/core/progress.h>
#include <agentic/core/services.h>
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/pcb_items.h>
#include <agentic/pcb/pcb_tools.h>

#include <base_units.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <connectivity/connectivity_algo.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_items.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_report.h>
#include <exporters/export_d356.h>
#include <exporters/gendrill_excellon_writer.h>
#include <exporters/gerber_jobfile_writer.h>
#include <exporters/place_file_exporter.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_marker.h>
#include <pcb_plot_params.h>
#include <pcb_track.h>
#include <pcb_plotter.h>
#include <pcbplot.h>
#include <ratsnest/ratsnest_data.h>
#include <tool/tool_manager.h>
#include <tools/zone_filler_tool.h>
#include <plotters/plotter.h>
#include <project_pcb.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <title_block.h>
#include <wildcards_and_files_ext.h>
#include <zone.h>

#include <wx/image.h>
#include <wx/mstream.h>

#include <cmath>
#include <cstdio>


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


    double mm( double aIu )
    {
        return std::round( pcbIUScale.IUTomm( aIu ) * 1e4 ) / 1e4;
    }


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


    std::string base64( const std::string& aData )
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string        out;
        int                val = 0, bits = -6;

        for( unsigned char c : aData )
        {
            val = ( val << 8 ) + c;
            bits += 8;

            while( bits >= 0 )
            {
                out.push_back( table[( val >> bits ) & 0x3F] );
                bits -= 6;
            }
        }

        if( bits > -6 )
            out.push_back( table[( ( val << 8 ) >> ( bits + 8 ) ) & 0x3F] );

        while( out.size() % 4 )
            out.push_back( '=' );

        return out;
    }


    /// An output folder inside the project (default output/).
    fs::path outputDir( AGENT_PROJECT& aProject, const JSON& aArgs )
    {
        const fs::path    dir = aProject.Files().Resolve( aArgs.value( "output", "output" ) );
        const std::string rel = aProject.Files().Relative( dir );

        if( FILE_STORE::IsReserved( rel ) || FILE_STORE::IsDesignFile( rel ) )
            throw TOOL_ERROR( "'" + rel + "' cannot be an output folder." );

        fs::create_directories( dir );
        return dir;
    }


    LSEQ layerList( BOARD& aBoard, const JSON& aArgs, const LSEQ& aDefault )
    {
        if( !aArgs.contains( "layers" ) )
            return aDefault;

        LSEQ layers;

        for( const JSON& name : aArgs["layers"] )
            layers.push_back( PcbLayer( aBoard, name ) );

        return layers;
    }


    PCB_PLOT_PARAMS plotParams( PLOT_FORMAT aFormat )
    {
        PCB_PLOT_PARAMS opts;
        opts.SetFormat( aFormat );
        opts.SetPlotFrameRef( false );
        opts.SetUseAuxOrigin( false );
        opts.SetAutoScale( false );
        opts.SetScale( 1.0 );
        opts.SetMirror( false );
        opts.SetColorSettings( ::GetColorSettings( wxEmptyString ) );
        return opts;
    }


    /// Run DRC like kicad-cli pcb drc (spike S0.b); the board's markers are replaced.
    JSON runDrc( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject, BOARD_DOC_SESSION& aSession, bool aRefill,
                 TOOL_CONTEXT& aContext, bool aAll )
    {
        BOARD& board = aSession.Board();

        // DRC's library checks read KiCad's process-wide footprint adapter.
        PROJECT_PCB::FootprintLibAdapter( &aProject.KicadProject() );
        aProject.ActivateGlobalLibraryTables( LIBRARY_TABLE_TYPE::FOOTPRINT );

        std::shared_ptr<DRC_ENGINE> engine = board.GetDesignSettings().m_DRCEngine;

        try
        {
            engine->InitEngine( board.GetDesignRulesPath() );
        }
        catch( const PARSE_ERROR& e )
        {
            throw TOOL_ERROR( "The custom design rules do not compile: " + utf8( e.What() ) );
        }

        AGENT_PROGRESS_REPORTER progress( aContext, aServices.ProgressSink() );

        if( aRefill )
        {
            TOOL_MANAGER* tools = aSession.ToolManager();

            if( !tools->FindTool( ZONE_FILLER_TOOL_NAME ) )
                tools->RegisterTool( new ZONE_FILLER_TOOL );

            tools->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, &progress, true );
        }

        auto sheet =
                std::make_unique<DS_PROXY_VIEW_ITEM>( pcbIUScale, &board.GetPageSettings(), &aProject.KicadProject(),
                                                      &board.GetTitleBlock(), &board.GetProperties() );
        sheet->SetSheetName( std::string() );
        sheet->SetSheetPath( std::string() );
        sheet->SetIsFirstPage( true );
        sheet->SetFileName( std::string( board.GetFileName().ToUTF8() ) );
        engine->SetDrawingSheet( sheet.get() );
        engine->SetProgressReporter( &progress );

        BOARD_COMMIT markers( aSession.ToolManager() );
        engine->SetViolationHandler(
                [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                     const std::function<void( PCB_MARKER* )>& aPathGenerator )
                {
                    PCB_MARKER* marker = new PCB_MARKER( aItem, aPos, aLayer );
                    aPathGenerator( marker );
                    markers.Add( marker );
                } );

        board.RecordDRCExclusions();
        board.DeleteMARKERs( true, true );
        engine->RunTests( EDA_UNITS::MM, false, false );
        engine->ClearViolationHandler();
        engine->SetDrawingSheet( nullptr );
        engine->SetProgressReporter( nullptr );
        markers.Push( wxS( "DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );
        board.ResolveDRCExclusions( false );

        const int severities = aAll ? RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING | RPT_SEVERITY_EXCLUSION
                                    : RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING;
        auto      violations = std::make_shared<DRC_ITEMS_PROVIDER>( &board, MARKER_BASE::MARKER_DRC,
                                                                     MARKER_BASE::MARKER_DRAWING_SHEET );
        auto      unconnected = std::make_shared<DRC_ITEMS_PROVIDER>( &board, MARKER_BASE::MARKER_RATSNEST );
        auto      parity = std::make_shared<DRC_ITEMS_PROVIDER>( &board, MARKER_BASE::MARKER_PARITY );
        violations->SetSeverities( severities );
        unconnected->SetSeverities( severities );
        parity->SetSeverities( severities );

        // The JSON KiCad writes, so the records equal kicad-cli's.
        const fs::path file = aProject.Root() / ".agentic" / "drc.json";
        fs::create_directories( file.parent_path() );
        DRC_REPORT report( &board, EDA_UNITS::MM, violations, unconnected, parity );

        if( !report.WriteJsonReport( wx( file.string() ) ) )
            throw TOOL_ERROR( "Writing the DRC report failed." );

        JSON parsed = JSON::parse( FILE_STORE::ReadFile( file ) );
        int  errors = 0;

        for( const JSON& v : parsed.value( "violations", JSON::array() ) )
            errors += v.value( "severity", "" ) == "error";

        JSON out{ { "errors", errors },
                  { "violations", parsed.value( "violations", JSON::array() ) },
                  { "unconnected", parsed.value( "unconnected_items", JSON::array() ) } };
        out["violationCount"] = out["violations"].size();
        out["unconnectedCount"] = out["unconnected"].size();
        return out;
    }

} // namespace


TOOL_RESULT PcbCapturePng( AGENT_PROJECT& aProject, BOARD& aBoard, const BOX2I& aArea, const LSEQ& aLayers, int aWidth )
{
    const double pxPerMm = aWidth / pcbIUScale.IUTomm( aArea.GetWidth() );
    const int    dpi = std::clamp( static_cast<int>( std::ceil( pxPerMm * 25.4 ) ), 20, 1200 );

    PCB_PLOT_PARAMS opts = plotParams( PLOT_FORMAT::PNG );
    opts.SetPngDPI( dpi );
    opts.SetPngAntialias( true );
    opts.SetBlackAndWhite( false );
    opts.SetBackgroundColor( opts.ColorSettings()->GetColor( LAYER_PCB_BACKGROUND ) );

    const fs::path file = aProject.Root() / ".agentic" / "captures" / "board.png";
    fs::create_directories( file.parent_path() );

    std::unique_ptr<PLOTTER> plotter( StartPlotBoard( &aBoard, &opts, aLayers.front(), wxS( "capture" ),
                                                      wx( file.string() ), wxEmptyString, wxEmptyString ) );

    if( !plotter )
        throw TOOL_ERROR( "Plotting the board failed." );

    PlotBoardLayers( &aBoard, plotter.get(), aLayers, opts );
    plotter->EndPlot();
    plotter.reset();

    wxImage image;

    if( !image.LoadFile( wx( file.string() ), wxBITMAP_TYPE_PNG ) )
        throw TOOL_ERROR( "Reading the plotted picture failed." );

    const double scale = dpi / 25.4;
    wxRect       crop( KiROUND( pcbIUScale.IUTomm( aArea.GetLeft() ) * scale ),
                       KiROUND( pcbIUScale.IUTomm( aArea.GetTop() ) * scale ),
                       KiROUND( pcbIUScale.IUTomm( aArea.GetWidth() ) * scale ),
                       KiROUND( pcbIUScale.IUTomm( aArea.GetHeight() ) * scale ) );
    crop.Intersect( wxRect( 0, 0, image.GetWidth(), image.GetHeight() ) );

    if( crop.IsEmpty() )
        throw TOOL_ERROR( "The area is outside the page." );

    image = image.GetSubImage( crop );

    if( image.GetWidth() > aWidth )
        image.Rescale( aWidth, KiROUND( image.GetHeight() * double( aWidth ) / image.GetWidth() ),
                       wxIMAGE_QUALITY_HIGH );

    wxMemoryOutputStream stream;
    image.SaveFile( stream, wxBITMAP_TYPE_PNG );
    std::string data( stream.GetSize(), '\0' );
    stream.CopyTo( data.data(), data.size() );

    TOOL_RESULT result;
    result.content.push_back( ImageContent( base64( data ), "image/png" ) );
    JSON info{ { "width", image.GetWidth() },
               { "height", image.GetHeight() },
               { "pixelsPerMm", std::round( image.GetWidth() / pcbIUScale.IUTomm( aArea.GetWidth() ) * 1000 ) / 1000 },
               { "area",
                 { { "x1", mm( aArea.GetLeft() ) },
                   { "y1", mm( aArea.GetTop() ) },
                   { "x2", mm( aArea.GetRight() ) },
                   { "y2", mm( aArea.GetBottom() ) } } } };
    result.content.push_back( TextContent( info.dump() ) );
    result.structuredContent = info;
    return result;
}


void RegisterPcbCheckTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "pcb_drc",
                 "Run the design rule check (as kicad-cli pcb drc does) and return the violations and unconnected "
                 "items with their positions and item KIIDs. refillZones (default true) fills zones first and saves "
                 "the fills. Markers show in the viewer.",
                 "pcb",
                 pcbSchema( { { "refillZones", { { "type", "boolean" } } },
                              { "severity", { { "type", "string" }, { "enum", { "default", "all" } } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     const bool         refill = aArgs.value( "refillZones", true );
                     JSON               out;

                     if( refill )
                     {
                         // Fills change the board, so they are saved like any change (D5).
                         out = project->Mutate( "pcb_drc", aArgs.value( "force", false ),
                                                [&]() -> JSON
                                                {
                                                    JSON result = runDrc( aServices, *project, s, true, aContext,
                                                                          aArgs.value( "severity", "" ) == "all" );
                                                    s.MarkModified();
                                                    return result;
                                                } );
                     }
                     else
                     {
                         out = runDrc( aServices, *project, s, false, aContext,
                                       aArgs.value( "severity", "" ) == "all" );
                     }

                     aServices.Observers().Notify(
                             [&]( SESSION_OBSERVER& aObserver )
                             {
                                 aObserver.OnResults( s, JSON{ { "checker", "drc" }, { "report", out } } );
                             } );

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "pcb_unrouted", "Connections still to route: per net, the pads to join and the airline length.", "pcb",
                 pcbSchema( WithPageArgs( { { "net", { { "type", "string" } } } } ) ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     BOARD_DOC_SESSION&                 s = session( aServices, aArgs );
                     BOARD&                             board = s.Board();
                     std::shared_ptr<CONNECTIVITY_DATA> connectivity = board.GetConnectivity();
                     connectivity->RecalculateRatsnest();

                     JSON nets = JSON::array();
                     int  total = 0;

                     for( NETINFO_ITEM* net : board.GetNetInfo() )
                     {
                         if( net->GetNetCode() <= 0 )
                             continue;

                         if( aArgs.contains( "net" ) && utf8( net->GetNetname() ) != aArgs["net"].get<std::string>() )
                             continue;

                         RN_NET* rn = connectivity->GetRatsnestForNet( net->GetNetCode() );

                         if( !rn || rn->GetEdges().empty() )
                             continue;

                         JSON edges = JSON::array();

                         for( const CN_EDGE& edge : rn->GetEdges() )
                         {
                             auto describe = [&]( const std::shared_ptr<const CN_ANCHOR>& aAnchor ) -> JSON
                             {
                                 JSON                  end{ { "position", PcbPointJson( aAnchor->Pos() ) } };
                                 BOARD_CONNECTED_ITEM* parent = aAnchor->Parent();

                                 if( parent && parent->Type() == PCB_PAD_T )
                                 {
                                     PAD* pad = static_cast<PAD*>( parent );
                                     end["pad"] = utf8( pad->GetParentFootprint()->GetReference() + wxS( "." )
                                                        + pad->GetNumber() );
                                 }

                                 return end;
                             };

                             edges.push_back(
                                     { { "from", describe( edge.GetSourceNode() ) },
                                       { "to", describe( edge.GetTargetNode() ) },
                                       { "length", mm( ( edge.GetTargetNode()->Pos() - edge.GetSourceNode()->Pos() )
                                                               .EuclideanNorm() ) } } );
                         }

                         total += static_cast<int>( edges.size() );
                         nets.push_back( { { "net", utf8( net->GetNetname() ) }, { "connections", edges } } );
                     }

                     return TOOL_RESULT::Data( JSON{
                             { "unrouted", total }, { "nets", Page( nets, aArgs ) }, { "netsTotal", nets.size() } } );
                 } } );

    tools.Add( { "pcb_stats",
                 "Board statistics: size, layers, footprints (front, back, SMD, through-hole), pads, vias, tracks and "
                 "their length, zones, nets and unrouted connections.",
                 "pcb", pcbSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     BOARD_DOC_SESSION& s = session( aServices, aArgs );
                     BOARD&             board = s.Board();
                     int                front = 0, back = 0, smd = 0, tht = 0, pads = 0, vias = 0, tracks = 0;
                     double             length = 0;

                     for( FOOTPRINT* fp : board.Footprints() )
                     {
                         ( fp->IsFlipped() ? back : front )++;
                         smd += ( fp->GetAttributes() & FP_SMD ) != 0;
                         tht += ( fp->GetAttributes() & FP_THROUGH_HOLE ) != 0;
                         pads += static_cast<int>( fp->Pads().size() );
                     }

                     for( PCB_TRACK* track : board.Tracks() )
                     {
                         if( track->Type() == PCB_VIA_T )
                         {
                             vias++;
                         }
                         else
                         {
                             tracks++;
                             length += track->GetLength();
                         }
                     }

                     BOX2I edges = board.GetBoardEdgesBoundingBox();

                     return TOOL_RESULT::Data(
                             JSON{ { "width", mm( edges.GetWidth() ) },
                                   { "height", mm( edges.GetHeight() ) },
                                   { "copperLayers", board.GetCopperLayerCount() },
                                   { "footprints",
                                     { { "front", front }, { "back", back }, { "smd", smd }, { "throughHole", tht } } },
                                   { "pads", pads },
                                   { "vias", vias },
                                   { "tracks", tracks },
                                   { "trackLength", mm( length ) },
                                   { "zones", board.Zones().size() },
                                   { "nets", board.GetNetCount() > 0 ? board.GetNetCount() - 1 : 0 },
                                   { "unrouted", PCB_RATSNEST::Take( board ).total } } );
                 } } );

    tools.Add( { "pcb_capture",
                 "Look at the board: a PNG of layers (default copper, silkscreen and Edge.Cuts) for the board area, "
                 "an area {x1,y1,x2,y2} in mm, or around an item (focus: handle). Returns the scale and the area so "
                 "pixels map back to mm.",
                 "pcb",
                 pcbSchema( { { "layers", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                              { "area", { { "type", "object" } } },
                              { "focus", { { "type", "string" } } },
                              { "width", { { "type", "integer" }, { "minimum", 200 }, { "maximum", 4096 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );
                     BOARD&             board = s.Board();
                     const int          width = aArgs.value( "width", 1024 );

                     BOX2I area;

                     if( aArgs.contains( "area" ) )
                     {
                         const JSON& a = aArgs["area"];
                         VECTOR2I    p1( PcbLength( a.at( "x1" ), "x1" ), PcbLength( a.at( "y1" ), "y1" ) );
                         VECTOR2I    p2( PcbLength( a.at( "x2" ), "x2" ), PcbLength( a.at( "y2" ), "y2" ) );
                         area = BOX2I( p1, p2 - p1 );
                         area.Normalize();
                     }
                     else if( aArgs.contains( "focus" ) )
                     {
                         area = PcbFindOne( board, aArgs["focus"] )->GetBoundingBox();
                         area.Inflate( PcbIu( 5 ) );
                     }
                     else
                     {
                         area = board.GetBoardEdgesBoundingBox();

                         if( area.GetWidth() <= 0 )
                             area = board.GetBoundingBox();

                         area.Inflate( PcbIu( 2 ) );
                     }

                     if( area.GetWidth() <= 0 || area.GetHeight() <= 0 )
                         throw TOOL_ERROR( "Nothing to capture: the board is empty and no area was given." );

                     LSEQ layers = layerList( board, aArgs,
                                              { B_Cu, In1_Cu, In2_Cu, F_Cu, B_SilkS, F_SilkS, F_Mask, Edge_Cuts } );
                     LSEQ enabled;

                     for( PCB_LAYER_ID layer : layers )
                     {
                         if( board.IsLayerEnabled( layer ) )
                             enabled.push_back( layer );
                     }

                     return PcbCapturePng( *project, board, area, enabled, width );
                 } } );

    tools.Add(
            { "pcb_export",
              "Export fabrication and documentation files into a project folder (default output/): gerbers (with "
              "a job file), drill (Excellon, with map), position (CSV), ipcd356, pdf, svg or dxf (layers). Zones "
              "should be filled first (pcb_zones_fill or pcb_drc).",
              "pcb",
              pcbSchema( { { "format",
                             { { "type", "string" },
                               { "enum", { "gerbers", "drill", "position", "ipcd356", "pdf", "svg", "dxf" } } } },
                           { "output", { { "type", "string" } } },
                           { "layers", { { "type", "array" }, { "items", { { "type", "string" } } } } } },
                         { "format" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT*        project = nullptr;
                  BOARD_DOC_SESSION&    s = session( aServices, aArgs, &project );
                  BOARD&                board = s.Board();
                  const std::string     format = aArgs["format"];
                  const fs::path        dir = outputDir( *project, aArgs );
                  const wxString        outDir = wx( dir.string() ) + wxFileName::GetPathSeparator();
                  REPORTER&             reporter = aServices.Reporter();
                  std::vector<wxString> files;

                  if( format == "gerbers" )
                  {
                      LSEQ defaults;

                      const LSET wanted = LSET::AllCuMask()
                                          | LSET( { F_SilkS, B_SilkS, F_Mask, B_Mask, F_Paste, B_Paste, Edge_Cuts } );
                      const LSET plotted = board.GetEnabledLayers() & wanted;

                      for( PCB_LAYER_ID layer : plotted.Seq() )
                          defaults.push_back( layer );

                      LSEQ                  layers = layerList( board, aArgs, defaults );
                      GERBER_JOBFILE_WRITER jobfile( &board );

                      for( PCB_LAYER_ID layer : layers )
                      {
                          PCB_PLOT_PARAMS opts = plotParams( PLOT_FORMAT::GERBER );
                          opts.SetUseGerberX2format( true );
                          opts.SetIncludeGerberNetlistInfo( true );
                          wxFileName     fn( board.GetFileName() );
                          const wxString suffix = board.GetLayerName( layer );
                          PCB_PLOTTER::BuildPlotFileName( &fn, outDir, suffix, FILEEXT::GerberFileExtension );
                          wxString name = fn.GetFullName();
                          jobfile.AddGbrFile( layer, name );

                          std::unique_ptr<PLOTTER> plotter(
                                  StartPlotBoard( &board, &opts, layer, board.GetLayerName( layer ), fn.GetFullPath(),
                                                  wxEmptyString, wxEmptyString ) );

                          if( !plotter )
                              throw TOOL_ERROR( "Cannot write " + utf8( fn.GetFullName() ) + "." );

                          PlotBoardLayers( &board, plotter.get(), { layer }, opts );
                          plotter->EndPlot();
                          files.push_back( fn.GetFullPath() );
                      }

                      wxFileName job( board.GetFileName() );
                      PCB_PLOTTER::BuildPlotFileName( &job, outDir, wxS( "job" ), FILEEXT::GerberJobFileExtension );
                      jobfile.CreateJobFile( job.GetFullPath() );
                      files.push_back( job.GetFullPath() );
                  }
                  else if( format == "drill" )
                  {
                      EXCELLON_WRITER writer( &board );
                      writer.SetFormat( true, EXCELLON_WRITER::DECIMAL_FORMAT, 3, 3 );
                      writer.SetOptions( false, false, VECTOR2I( 0, 0 ), false );
                      writer.SetMapFileFormat( PLOT_FORMAT::PDF );

                      if( !writer.CreateDrillandMapFilesSet( outDir, true, true, &reporter ) )
                          throw TOOL_ERROR( "Writing the drill files failed." );

                      for( const wxString& file : writer.GetCreatedFiles() )
                          files.push_back( file );
                  }
                  else if( format == "position" )
                  {
                      PLACE_FILE_EXPORTER exporter( &board, true, false, false, true, false, true, true, true, false,
                                                    false );
                      const std::string   data = exporter.GenPositionData();
                      const fs::path      file = dir / ( project->Name() + "-pos.csv" );
                      FILE_STORE::WriteAtomic( file, data );
                      files.push_back( wx( file.string() ) );
                  }
                  else if( format == "ipcd356" )
                  {
                      const fs::path file = dir / ( project->Name() + ".d356" );
                      IPC356D_WRITER writer( &board );

                      if( !writer.Write( wx( file.string() ) ) )
                          throw TOOL_ERROR( "Writing the IPC-D-356 netlist failed." );

                      files.push_back( wx( file.string() ) );
                  }
                  else
                  {
                      const PLOT_FORMAT plotFormat = format == "pdf"   ? PLOT_FORMAT::PDF
                                                     : format == "svg" ? PLOT_FORMAT::SVG
                                                                       : PLOT_FORMAT::DXF;
                      LSEQ              layers = layerList( board, aArgs, { F_Cu, B_Cu, F_SilkS, B_SilkS, Edge_Cuts } );

                      for( PCB_LAYER_ID layer : layers )
                      {
                          PCB_PLOT_PARAMS opts = plotParams( plotFormat );
                          opts.SetPlotFrameRef( format == "pdf" );
                          wxFileName     fn( board.GetFileName() );
                          const wxString suffix = board.GetLayerName( layer );
                          PCB_PLOTTER::BuildPlotFileName( &fn, outDir, suffix, GetDefaultPlotExtension( plotFormat ) );

                          std::unique_ptr<PLOTTER> plotter(
                                  StartPlotBoard( &board, &opts, layer, board.GetLayerName( layer ), fn.GetFullPath(),
                                                  wxEmptyString, wxEmptyString ) );

                          if( !plotter )
                              throw TOOL_ERROR( "Cannot write " + utf8( fn.GetFullName() ) + "." );

                          PlotBoardLayers( &board, plotter.get(), { layer, Edge_Cuts }, opts );
                          plotter->EndPlot();
                          files.push_back( fn.GetFullPath() );
                      }
                  }

                  JSON list = JSON::array();

                  for( const wxString& file : files )
                      list.push_back( project->Files().Relative( fs::path( utf8( file ) ) ) );

                  aServices.Observers().Notify(
                          [&]( SESSION_OBSERVER& aObserver )
                          {
                              aObserver.OnFilesChanged( *project );
                          } );

                  return TOOL_RESULT::Data( JSON{ { "files", list } } );
              } } );

    tools.Add( { "pcb_title_block_get", "The board's title block: title, date, revision, company, comments.", "pcb",
                 pcbSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     BOARD_DOC_SESSION& s = session( aServices, aArgs );
                     const TITLE_BLOCK& tb = s.Board().GetTitleBlock();
                     JSON               comments = JSON::array();

                     for( int i = 0; i < 9; ++i )
                         comments.push_back( utf8( tb.GetComment( i ) ) );

                     return TOOL_RESULT::Data( JSON{ { "title", utf8( tb.GetTitle() ) },
                                                     { "date", utf8( tb.GetDate() ) },
                                                     { "revision", utf8( tb.GetRevision() ) },
                                                     { "company", utf8( tb.GetCompany() ) },
                                                     { "comments", comments } } );
                 } } );

    tools.Add( { "pcb_title_block_set", "Set the board's title block fields: title, date, revision, company, comments.",
                 "pcb",
                 pcbSchema( { { "title", { { "type", "string" } } },
                              { "date", { { "type", "string" } } },
                              { "revision", { { "type", "string" } } },
                              { "company", { { "type", "string" } } },
                              { "comments", { { "type", "array" }, { "maxItems", 9 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*     project = nullptr;
                     BOARD_DOC_SESSION& s = session( aServices, aArgs, &project );

                     JSON result =
                             project->Mutate( "pcb_title_block_set", aArgs.value( "force", false ),
                                              [&]() -> JSON
                                              {
                                                  TITLE_BLOCK tb = s.Board().GetTitleBlock();

                                                  if( aArgs.contains( "title" ) )
                                                      tb.SetTitle( wx( aArgs["title"] ) );

                                                  if( aArgs.contains( "date" ) )
                                                      tb.SetDate( wx( aArgs["date"] ) );

                                                  if( aArgs.contains( "revision" ) )
                                                      tb.SetRevision( wx( aArgs["revision"] ) );

                                                  if( aArgs.contains( "company" ) )
                                                      tb.SetCompany( wx( aArgs["company"] ) );

                                                  int n = 0;

                                                  for( const JSON& comment : aArgs.value( "comments", JSON::array() ) )
                                                      tb.SetComment( n++, wx( comment.get<std::string>() ) );

                                                  s.Board().SetTitleBlock( tb );
                                                  s.MarkModified();
                                                  s.RecordChange( { { "summary", "pcb_title_block_set" } } );
                                                  return JSON::object();
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
