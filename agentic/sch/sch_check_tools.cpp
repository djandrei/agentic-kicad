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
#include <agentic/core/library_tools.h>
#include <agentic/core/progress.h>
#include <agentic/core/services.h>
#include <agentic/sch/sch_doc_session.h>
#include <agentic/sch/sch_items.h>
#include <agentic/sch/sch_tools.h>

#include <base_units.h>
#include <drawing_sheet/ds_data_model.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <erc/erc.h>
#include <erc/erc_report.h>
#include <erc/erc_settings.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <netlist_exporters/netlist_exporter_cadstar.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <netlist_exporters/netlist_exporter_orcadpcb2.h>
#include <netlist_exporters/netlist_exporter_spice.h>
#include <netlist_exporters/netlist_exporter_xml.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <sch_commit.h>
#include <sch_plotter.h>
#include <sch_reference_list.h>
#include <sch_render_settings.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <title_block.h>

#include <wx/filename.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <cmath>
#include <map>


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


    SCH_DOC_SESSION& session( AGENT_SERVICES& aServices, const JSON& aArgs, AGENT_PROJECT** aProject = nullptr )
    {
        AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );

        if( aProject )
            *aProject = &project;

        return SchematicSession( project, aArgs.value( "file", "" ) );
    }


    JSON schSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["file"] = { { "type", "string" },
                                { "description",
                                  "Root schematic (project-relative); default the project's schematic." } };
        aProperties["force"] = { { "type", "boolean" } };
        return ProjectToolSchema( aProperties, aRequired );
    }


    /// The global library manager sees this project's tables, for engine code that uses it
    /// (ERC's library checks call PROJECT_SCH::SymbolLibAdapter).
    void activateGlobalSymbolLibraries( AGENT_PROJECT& aProject )
    {
        // The adapter must exist before the tables load into it.
        PROJECT_SCH::SymbolLibAdapter( &aProject.KicadProject() );
        aProject.ActivateGlobalLibraryTables( LIBRARY_TABLE_TYPE::SYMBOL );
    }


    std::unique_ptr<DS_PROXY_VIEW_ITEM> drawingSheetProxy( SCHEMATIC& aSch )
    {
        auto sheet = std::make_unique<DS_PROXY_VIEW_ITEM>( schIUScale, &aSch.RootScreen()->GetPageSettings(),
                                                           &aSch.Project(), &aSch.RootScreen()->GetTitleBlock(),
                                                           aSch.GetProperties() );
        sheet->SetPageNumber( utf8( aSch.RootScreen()->GetPageNumber() ) );
        sheet->SetSheetCount( aSch.RootScreen()->GetPageCount() );
        sheet->SetFileName( utf8( aSch.RootScreen()->GetFileName() ) );
        sheet->SetColorLayer( LAYER_SCHEMATIC_DRAWINGSHEET );
        sheet->SetPageBorderColorLayer( LAYER_SCHEMATIC_PAGE_LIMITS );
        sheet->SetIsFirstPage( aSch.RootScreen()->GetVirtualPageNumber() == 1 );
        sheet->SetSheetName( "" );
        sheet->SetSheetPath( "" );
        return sheet;
    }


    void initRenderSettings( SCH_RENDER_SETTINGS& aSettings, SCHEMATIC& aSch, bool aDrawingSheet )
    {
        aSettings.LoadColors( ::GetColorSettings( wxEmptyString ) );
        aSettings.m_ShowHiddenPins = false;
        aSettings.m_ShowHiddenFields = false;
        aSettings.m_ShowPinAltIcons = false;
        aSettings.SetDefaultPenWidth( aSch.Settings().m_DefaultLineWidth );
        aSettings.m_LabelSizeRatio = aSch.Settings().m_LabelSizeRatio;
        aSettings.m_TextOffsetRatio = aSch.Settings().m_TextOffsetRatio;
        aSettings.m_PinSymbolSize = aSch.Settings().m_PinSymbolSize;
        aSettings.m_ShowDNPMarkers = aSch.Settings().m_ShowDNPMarkers;
        aSettings.SetDashLengthRatio( aSch.Settings().m_DashedLineDashRatio );
        aSettings.SetGapLengthRatio( aSch.Settings().m_DashedLineGapRatio );

        if( aDrawingSheet )
        {
            wxString msg;
            DS_DATA_MODEL::GetTheInstance().LoadFromName(
                    aSch.Settings().m_SchDrawingSheetFileName,
                    wx( aSch.Project().GetProjectPath().ToStdString( wxConvUTF8 ) ), &aSch.Project(),
                    { aSch.GetEmbeddedFiles() }, &msg );
        }
    }


    /// A writable output path inside the project (default under "output/").
    fs::path outputPath( AGENT_PROJECT& aProject, const std::string& aOutput, const std::string& aDefault )
    {
        const std::string rel = aOutput.empty() ? aDefault : aOutput;
        const fs::path    path = aProject.Files().Resolve( rel );
        const std::string relative = aProject.Files().Relative( path );

        if( FILE_STORE::IsDesignFile( relative ) || FILE_STORE::IsReserved( relative ) )
            throw TOOL_ERROR( "'" + relative + "' cannot be an output file." );

        fs::create_directories( path.has_extension() ? path.parent_path() : path );
        return path;
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


    /// Plot one sheet to PNG in a scratch folder; returns the file.
    fs::path plotSheetPng( AGENT_PROJECT& aProject, SCH_DOC_SESSION& aSession, const SCH_SHEET_PATH& aPath, int aDpi,
                           REPORTER& aReporter )
    {
        SCHEMATIC& sch = aSession.Schematic();
        fs::path   dir = aProject.Root() / ".agentic" / "captures";
        fs::create_directories( dir );

        for( const fs::directory_entry& old : fs::directory_iterator( dir ) )
            fs::remove( old.path() );

        SCH_RENDER_SETTINGS renderSettings;
        initRenderSettings( renderSettings, sch, true );

        SCH_PLOT_OPTS options;
        options.m_plotAll = false;
        options.m_plotDrawingSheet = true;
        options.m_plotPages = { aPath.GetPageNumber() };
        options.m_sheetPath = aPath;
        options.m_blackAndWhite = false;
        options.m_useBackgroundColor = true;
        options.m_pageSizeSelect = PageFormatReq::PAGE_SIZE_AUTO;
        options.m_outputDirectory = wx( dir.string() );
        options.m_pngDPI = aDpi;
        options.m_pngAntialias = true;

        SCH_PLOTTER plotter( &sch );
        plotter.Plot( PLOT_FORMAT::PNG, options, &renderSettings, &aReporter );

        if( plotter.GetOutputFilePaths().empty() )
            throw TOOL_ERROR( "Plotting the sheet failed." );

        return fs::path( utf8( plotter.GetOutputFilePaths().front() ) );
    }

} // namespace


std::string SchNetlist( AGENT_PROJECT& aProject )
{
    SCH_DOC_SESSION& s = SchematicSession( aProject );
    const fs::path   file = aProject.Root() / ".agentic" / "netlist.net";
    fs::create_directories( file.parent_path() );

    NETLIST_EXPORTER_KICAD exporter( &s.Schematic(), nullptr );

    if( !exporter.WriteNetlist( wx( file.string() ), 0, NULL_REPORTER::GetInstance() ) )
        throw TOOL_ERROR( "Writing the schematic netlist failed." );

    std::string text = FILE_STORE::ReadFile( file );
    fs::remove( file );
    return text;
}


void RegisterSchCheckTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "sch_annotate",
                 "Give references to unannotated symbols (R?, U?), or renumber all with reset. scope all or sheet; "
                 "order x (left to right) or y (top to bottom); startNumber.",
                 "sch",
                 schSchema( { { "scope", { { "type", "string" }, { "enum", { "all", "sheet" } } } },
                              { "order", { { "type", "string" }, { "enum", { "x", "y" } } } },
                              { "startNumber", { { "type", "integer" }, { "minimum", 0 } } },
                              { "reset", { { "type", "boolean" } } },
                              { "sheet", { { "type", "string" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCHEMATIC&       sch = s.Schematic();
                     const bool       sheetOnly = aArgs.value( "scope", "all" ) == "sheet";
                     SCH_SHEET_PATH   current = SchSheet( s, aArgs.value( "sheet", "" ) );

                     JSON result = SchMutate(
                             *project, s, "sch_annotate", aArgs, false,
                             [&]( SCH_EDIT& aEdit ) -> JSON
                             {
                                 SCH_REFERENCE_LIST                 references;
                                 SCH_MULTI_UNIT_REFERENCE_MAP       locked;
                                 SCH_SHEET_LIST                     sheets = sch.Hierarchy();
                                 std::map<std::string, std::string> before;

                                 for( const SCH_SHEET_PATH& path : sheets )
                                 {
                                     for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                                     {
                                         SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
                                         before[utf8( symbol->m_Uuid.AsString() ) + utf8( path.Path().AsString() )] =
                                                 utf8( symbol->GetRef( &path, true ) );
                                     }
                                 }

                                 if( sheetOnly )
                                 {
                                     current.GetMultiUnitSymbols( locked, SYMBOL_FILTER_NON_POWER );
                                     current.GetSymbols( references, SYMBOL_FILTER_NON_POWER );
                                 }
                                 else
                                 {
                                     sheets.GetMultiUnitSymbols( locked, SYMBOL_FILTER_NON_POWER );
                                     sheets.GetSymbols( references, SYMBOL_FILTER_NON_POWER );
                                 }

                                 AnnotateReferences(
                                         references, std::move( locked ), sch,
                                         sheetOnly ? ANNOTATE_CURRENT_SHEET : ANNOTATE_ALL,
                                         aArgs.value( "order", "x" ) == "y" ? SORT_BY_Y_POSITION : SORT_BY_X_POSITION,
                                         INCREMENTAL_BY_REF, aArgs.value( "startNumber", 0 ),
                                         aArgs.value( "reset", false ),
                                         [&]( SCH_REFERENCE& aRef )
                                         {
                                             aEdit.Modify( aRef.GetSymbol(), aRef.GetSheetPath().LastScreen() );
                                         } );

                                 JSON renamed = JSON::array();

                                 for( const SCH_SHEET_PATH& path : sheets )
                                 {
                                     for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                                     {
                                         SCH_SYMBOL*       symbol = static_cast<SCH_SYMBOL*>( item );
                                         const std::string key =
                                                 utf8( symbol->m_Uuid.AsString() ) + utf8( path.Path().AsString() );
                                         const std::string now = utf8( symbol->GetRef( &path, true ) );

                                         if( before[key] != now )
                                             renamed.push_back( { { "from", before[key] },
                                                                  { "to", now },
                                                                  { "kiid", utf8( symbol->m_Uuid.AsString() ) } } );
                                     }
                                 }

                                 if( renamed.empty() )
                                     throw TOOL_ERROR( "Every symbol is already annotated (use reset to renumber)." );

                                 return JSON{ { "renamed", renamed } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "sch_erc",
                 "Run the electrical rules check (as kicad-cli sch erc does) and return the violations: type, "
                 "severity, description, items (with KIIDs) and positions. Errors first. Markers show in the viewer.",
                 "sch",
                 schSchema( { { "severity",
                                { { "type", "string" },
                                  { "enum", { "error", "warning", "all" } },
                                  { "description", "Default: errors and warnings." } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCHEMATIC&       sch = s.Schematic();

                     activateGlobalSymbolLibraries( *project );

                     // ERC markers live on the screens: clear the old ones first (not saved; the
                     // file only keeps exclusions, in the project file).
                     SCH_SCREENS screens( sch.Root() );
                     screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, false );

                     AGENT_PROGRESS_REPORTER progress( aContext, aServices.ProgressSink() );
                     ERC_TESTER              tester( &sch );
                     auto                    sheet = drawingSheetProxy( sch );
                     tester.RunTests( sheet.get(), nullptr, nullptr, &sch.Project(), &progress );

                     auto              markers = std::make_shared<SHEETLIST_ERC_ITEMS_PROVIDER>( &sch );
                     const std::string severity = aArgs.value( "severity", "" );
                     markers->SetSeverities( severity == "error" ? RPT_SEVERITY_ERROR
                                             : severity == "all" ? RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING
                                                                           | RPT_SEVERITY_EXCLUSION | RPT_SEVERITY_INFO
                                                                 : RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );

                     // The JSON report KiCad writes, so the records equal kicad-cli's.
                     const fs::path reportFile = project->Root() / ".agentic" / "erc.json";
                     fs::create_directories( reportFile.parent_path() );
                     ERC_REPORT report( &sch, EDA_UNITS::MM, markers );

                     if( !report.WriteJsonReport( wx( reportFile.string() ) ) )
                         throw TOOL_ERROR( "Writing the ERC report failed." );

                     JSON parsed = JSON::parse( FILE_STORE::ReadFile( reportFile ) );
                     JSON violations = JSON::array();
                     int  errors = 0, warnings = 0;

                     for( const JSON& sheetJson : parsed.value( "sheets", JSON::array() ) )
                     {
                         for( JSON v : sheetJson.value( "violations", JSON::array() ) )
                         {
                             v["sheet"] = sheetJson.value( "path", "" );
                             ( v.value( "severity", "" ) == "error" ? errors : warnings )++;
                             violations.push_back( v );
                         }
                     }

                     std::stable_sort( violations.begin(), violations.end(),
                                       []( const JSON& a, const JSON& b )
                                       {
                                           return a.value( "severity", "" ) == "error"
                                                  && b.value( "severity", "" ) != "error";
                                       } );

                     JSON out{ { "errors", errors }, { "warnings", warnings }, { "violations", violations } };

                     aServices.Observers().Notify(
                             [&]( SESSION_OBSERVER& aObserver )
                             {
                                 aObserver.OnResults( s, JSON{ { "checker", "erc" }, { "report", out } } );
                             } );

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add(
            { "sch_export",
              "Export the schematic: pdf, svg (one per sheet), png, dxf, a netlist (netlist with netlistFormat "
              "kicadsexpr, kicadxml, orcadpcb2, cadstar, spice) or a BOM (bom: CSV grouped by value and "
              "footprint). Output goes inside the project, default output/.",
              "sch",
              schSchema(
                      { { "format",
                          { { "type", "string" }, { "enum", { "pdf", "svg", "png", "dxf", "netlist", "bom" } } } },
                        { "output", { { "type", "string" }, { "description", "Project-relative file or folder." } } },
                        { "netlistFormat", { { "type", "string" } } },
                        { "blackAndWhite", { { "type", "boolean" } } },
                        { "drawingSheet", { { "type", "boolean" } } } },
                      { "format" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT*    project = nullptr;
                  SCH_DOC_SESSION&  s = session( aServices, aArgs, &project );
                  SCHEMATIC&        sch = s.Schematic();
                  const std::string format = aArgs["format"];
                  const std::string name = project->Name();
                  JSON              files = JSON::array();
                  REPORTER&         reporter = aServices.Reporter();

                  if( format == "netlist" || format == "bom" )
                  {
                      const std::string netFormat = aArgs.value( "netlistFormat", "kicadsexpr" );
                      const std::string ext = format == "bom"            ? ".csv"
                                              : netFormat == "kicadxml"  ? ".xml"
                                              : netFormat == "spice"     ? ".cir"
                                              : netFormat == "orcadpcb2" ? ".net"
                                              : netFormat == "cadstar"   ? ".frp"
                                                                         : ".net";
                      fs::path out = outputPath( *project, aArgs.value( "output", "" ), "output/" + name + ext );

                      if( !out.has_extension() )
                          out /= name + ext;

                      if( format == "bom" )
                      {
                          std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::string>> groups;

                          for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
                          {
                              for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                              {
                                  SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                                  if( symbol->IsPower() || symbol->GetExcludedFromBOM( &path )
                                      || symbol->GetDNP( &path ) )
                                      continue;

                                  groups[{ utf8( symbol->GetValue( &path, FOR_GUI ) ),
                                           utf8( symbol->GetFootprintFieldText( &path, FOR_GUI ) ),
                                           utf8( symbol->GetLibId().Format() ) }]
                                          .push_back( utf8( symbol->GetRef( &path, false ) ) );
                              }
                          }

                          std::string csv = "\"Reference\",\"Qty\",\"Value\",\"Footprint\",\"Symbol\"\n";

                          for( auto& [key, refs] : groups )
                          {
                              std::sort( refs.begin(), refs.end() );
                              refs.erase( std::unique( refs.begin(), refs.end() ), refs.end() );
                              std::string joined;

                              for( const std::string& ref : refs )
                                  joined += ( joined.empty() ? "" : "," ) + ref;

                              csv += "\"" + joined + "\"," + std::to_string( refs.size() ) + ",\"" + std::get<0>( key )
                                     + "\",\"" + std::get<1>( key ) + "\",\"" + std::get<2>( key ) + "\"\n";
                          }

                          FILE_STORE::WriteAtomic( out, csv );
                      }
                      else
                      {
                          std::unique_ptr<NETLIST_EXPORTER_BASE> exporter;
                          unsigned                               options = 0;

                          if( netFormat == "kicadsexpr" )
                              exporter = std::make_unique<NETLIST_EXPORTER_KICAD>( &sch, nullptr );
                          else if( netFormat == "kicadxml" )
                              exporter = std::make_unique<NETLIST_EXPORTER_XML>( &sch, nullptr );
                          else if( netFormat == "orcadpcb2" )
                              exporter = std::make_unique<NETLIST_EXPORTER_ORCADPCB2>( &sch, nullptr );
                          else if( netFormat == "cadstar" )
                              exporter = std::make_unique<NETLIST_EXPORTER_CADSTAR>( &sch, nullptr );
                          else if( netFormat == "spice" )
                          {
                              exporter = std::make_unique<NETLIST_EXPORTER_SPICE>( &sch, nullptr );
                              options = NETLIST_EXPORTER_SPICE::OPTION_SIM_COMMAND;
                          }
                          else
                              throw TOOL_ERROR( "netlistFormat is kicadsexpr, kicadxml, orcadpcb2, cadstar or spice." );

                          if( !exporter->WriteNetlist( wx( out.string() ), options, reporter ) )
                              throw TOOL_ERROR( "Writing the netlist failed." );
                      }

                      files.push_back( project->Files().Relative( out ) );
                  }
                  else
                  {
                      const fs::path      dir = outputPath( *project, aArgs.value( "output", "" ), "output" );
                      SCH_RENDER_SETTINGS renderSettings;
                      initRenderSettings( renderSettings, sch, aArgs.value( "drawingSheet", true ) );

                      SCH_PLOT_OPTS options;
                      options.m_plotAll = true;
                      options.m_plotDrawingSheet = aArgs.value( "drawingSheet", true );
                      options.m_blackAndWhite = aArgs.value( "blackAndWhite", false );
                      options.m_useBackgroundColor = format == "png";
                      options.m_pageSizeSelect = PageFormatReq::PAGE_SIZE_AUTO;
                      options.m_pngDPI = 150;
                      options.m_pngAntialias = true;
                      options.m_DXF_File_Unit = DXF_UNITS::MM;

                      if( dir.has_extension() && format == "pdf" )
                          options.m_outputFile = wx( dir.string() );
                      else
                          options.m_outputDirectory = wx( dir.string() );

                      const PLOT_FORMAT plotFormat = format == "pdf"   ? PLOT_FORMAT::PDF
                                                     : format == "svg" ? PLOT_FORMAT::SVG
                                                     : format == "png" ? PLOT_FORMAT::PNG
                                                                       : PLOT_FORMAT::DXF;
                      SCH_PLOTTER       plotter( &sch );
                      plotter.Plot( plotFormat, options, &renderSettings, &reporter );

                      for( const wxString& path : plotter.GetOutputFilePaths() )
                          files.push_back( project->Files().Relative( fs::path( utf8( path ) ) ) );

                      if( files.empty() )
                          throw TOOL_ERROR( "Nothing was plotted." );
                  }

                  aServices.Observers().Notify(
                          [&]( SESSION_OBSERVER& aObserver )
                          {
                              aObserver.OnFilesChanged( *project );
                          } );

                  return TOOL_RESULT::Data( JSON{ { "files", files } } );
              } } );

    tools.Add( { "sch_capture",
                 "Look at a sheet: a PNG picture of the sheet, or of an area {x1,y1,x2,y2} in mm. Returns the image "
                 "and the scale (pixels per mm) and the area shown, so positions in the image map back to mm.",
                 "sch",
                 schSchema( { { "sheet", { { "type", "string" } } },
                              { "area", { { "type", "object" } } },
                              { "width",
                                { { "type", "integer" },
                                  { "minimum", 200 },
                                  { "maximum", 4000 },
                                  { "description", "Image width in pixels, default 1600." } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCH_SHEET_PATH   path = SchSheet( s, aArgs.value( "sheet", "" ) );
                     const int        width = aArgs.value( "width", 1600 );
                     const PAGE_INFO& page = path.LastScreen()->GetPageSettings();
                     const double     pageWmm = page.GetWidthMils() * 0.0254;
                     const double     pageHmm = page.GetHeightMils() * 0.0254;

                     double x1 = 0, y1 = 0, x2 = pageWmm, y2 = pageHmm;

                     if( aArgs.contains( "area" ) )
                     {
                         const JSON& a = aArgs["area"];
                         x1 = std::max( 0.0, ParseLengthMm( a.at( "x1" ), "area.x1" ) );
                         y1 = std::max( 0.0, ParseLengthMm( a.at( "y1" ), "area.y1" ) );
                         x2 = std::min( pageWmm, ParseLengthMm( a.at( "x2" ), "area.x2" ) );
                         y2 = std::min( pageHmm, ParseLengthMm( a.at( "y2" ), "area.y2" ) );

                         if( x2 <= x1 || y2 <= y1 )
                             throw TOOL_ERROR( "The area is empty or outside the page." );
                     }

                     // Resolution so that the area fills the requested width.
                     const double pxPerMm = width / ( x2 - x1 );
                     const int    dpi = std::clamp( static_cast<int>( std::ceil( pxPerMm * 25.4 ) ), 40, 1200 );

                     // The plotter's own progress lines are not the agent's business.
                     const fs::path png = plotSheetPng( *project, s, path, dpi, NULL_REPORTER::GetInstance() );
                     wxImage        image;

                     if( !image.LoadFile( wx( png.string() ), wxBITMAP_TYPE_PNG ) )
                         throw TOOL_ERROR( "Reading the plotted picture failed." );

                     const double scale = image.GetWidth() / pageWmm;
                     wxRect       crop( KiROUND( x1 * scale ), KiROUND( y1 * scale ), KiROUND( ( x2 - x1 ) * scale ),
                                        KiROUND( ( y2 - y1 ) * scale ) );
                     crop.Intersect( wxRect( 0, 0, image.GetWidth(), image.GetHeight() ) );
                     image = image.GetSubImage( crop );

                     if( image.GetWidth() > width )
                         image.Rescale( width, KiROUND( image.GetHeight() * double( width ) / image.GetWidth() ),
                                        wxIMAGE_QUALITY_HIGH );

                     wxMemoryOutputStream stream;
                     image.SaveFile( stream, wxBITMAP_TYPE_PNG );
                     std::string data( stream.GetSize(), '\0' );
                     stream.CopyTo( data.data(), data.size() );

                     TOOL_RESULT result;
                     result.content.push_back( ImageContent( base64( data ), "image/png" ) );
                     JSON info{ { "sheet", utf8( path.PathHumanReadable( false ) ) },
                                { "width", image.GetWidth() },
                                { "height", image.GetHeight() },
                                { "pixelsPerMm", std::round( image.GetWidth() / ( x2 - x1 ) * 1000 ) / 1000 },
                                { "area", { { "x1", x1 }, { "y1", y1 }, { "x2", x2 }, { "y2", y2 } } } };
                     result.content.push_back( TextContent( info.dump() ) );
                     result.structuredContent = info;
                     s.SetCurrentSheet( path );
                     return result;
                 } } );

    tools.Add( { "title_block_get", "The schematic's title block: title, date, revision, company, comments 1 to 9.",
                 "sch", schSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     SCH_DOC_SESSION&   s = session( aServices, aArgs );
                     const TITLE_BLOCK& tb = s.Schematic().RootScreen()->GetTitleBlock();
                     JSON               comments = JSON::array();

                     for( int i = 0; i < 9; ++i )
                         comments.push_back( utf8( tb.GetComment( i ) ) );

                     return TOOL_RESULT::Data( JSON{ { "title", utf8( tb.GetTitle() ) },
                                                     { "date", utf8( tb.GetDate() ) },
                                                     { "revision", utf8( tb.GetRevision() ) },
                                                     { "company", utf8( tb.GetCompany() ) },
                                                     { "comments", comments } } );
                 } } );

    tools.Add( { "title_block_set",
                 "Set title block fields of the schematic (every sheet shows the root's): title, date, revision, "
                 "company, comments (array, index 0 is comment 1). Text variables such as ${REV} work.",
                 "sch",
                 schSchema( { { "title", { { "type", "string" } } },
                              { "date", { { "type", "string" } } },
                              { "revision", { { "type", "string" } } },
                              { "company", { { "type", "string" } } },
                              { "comments", { { "type", "array" }, { "maxItems", 9 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );

                     JSON result = project->Mutate(
                             "title_block_set", aArgs.value( "force", false ),
                             [&]() -> JSON
                             {
                                 SCH_SCREENS screens( s.Schematic().Root() );

                                 for( size_t i = 0; i < screens.GetCount(); ++i )
                                 {
                                     TITLE_BLOCK& tb = screens.GetScreen( i )->GetTitleBlock();

                                     if( aArgs.contains( "title" ) && i == 0 )
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

                                     s.Touch( screens.GetScreen( i ) );
                                 }

                                 s.RecordChange( { { "summary", "title_block_set" }, { "tool", "title_block_set" } } );
                                 return JSON::object();
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "sch_drawing_sheet_set",
                 "Use a drawing sheet for the schematic: a project-relative .kicad_wks (made with ds_create), or \"\" "
                 "for KiCad's default. It is a project setting (saved in the .kicad_pro).",
                 "sch", schSchema( { { "drawingSheet", { { "type", "string" } } } }, { "drawingSheet" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*    project = nullptr;
                     SCH_DOC_SESSION&  s = session( aServices, aArgs, &project );
                     const std::string sheet = aArgs["drawingSheet"];

                     if( !sheet.empty() )
                     {
                         if( fs::path( sheet ).extension() != ".kicad_wks" )
                             throw TOOL_ERROR( "A drawing sheet is a .kicad_wks file." );

                         if( !fs::exists( project->Files().Resolve( sheet ) ) )
                             throw TOOL_ERROR( "No drawing sheet '" + sheet + "' in the project." );
                     }

                     JSON result = project->Mutate( "sch_drawing_sheet_set", aArgs.value( "force", false ),
                                                    [&]() -> JSON
                                                    {
                                                        s.Schematic().Settings().m_SchDrawingSheetFileName =
                                                                wx( sheet );
                                                        project->TouchProjectFile();
                                                        s.RecordChange( { { "summary", "sch_drawing_sheet_set" } } );
                                                        return JSON{ { "drawingSheet", sheet } };
                                                    } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
