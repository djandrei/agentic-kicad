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
#include <agentic/core/library_tools.h>
#include <agentic/core/services.h>
#include <agentic/pcb/pcb_library_tools.h>

#include <base_units.h>
#include <env_vars.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <pad.h>
#include <hotkeys_basic.h>
#include <pgm_base.h>
#include <project.h>

#include <wx/dir.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <filesystem>


namespace AGENTIC
{

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    double mm( int aIu )
    {
        return std::round( pcbIUScale.IUTomm( aIu ) * 1e4 ) / 1e4;
    }


    JSON footprintSummary( const wxString& aLibrary, const FOOTPRINT& aFootprint )
    {
        JSON out{ { "id", utf8( aLibrary + wxS( ":" ) + aFootprint.GetFPID().GetLibItemName() ) },
                  { "description", utf8( aFootprint.GetLibDescription() ) },
                  { "keywords", utf8( aFootprint.GetKeywords() ) },
                  { "pads", aFootprint.GetUniquePadNumbers().size() },
                  { "smd", ( aFootprint.GetAttributes() & FP_SMD ) != 0 },
                  { "throughHole", ( aFootprint.GetAttributes() & FP_THROUGH_HOLE ) != 0 } };
        return out;
    }


    /// The custom DRC rules help, compiled into pcbnew from the .md files it ships (each
    /// generated header is an expression, as panel_setup_rules.cpp uses them).
    std::string rulesHelp()
    {
        std::string text;
        wxString    part;

        part =
#include "dialogs/panel_setup_rules_help_1clauses.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_2constraints.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_3items.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_4severity_names.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_5examples.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_6notes.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_7properties.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_8expression_functions.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_9more_examples.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        part =
#include "dialogs/panel_setup_rules_help_10documentation.h"
                text += part.ToStdString( wxConvUTF8 ) + "\n";

        return text;
    }

} // namespace


void RegisterFootprintLibraryTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY&   tools = aServices.Tools();
    SESSION_MANAGER& sessions = aServices.Sessions();

    tools.Add( { "footprint_search",
                 "Find footprints in the project's footprint libraries by free text (name, keywords, description; "
                 "\"8 pin\" matches the pad count), best match first. Narrow with library, pads, smd. The id "
                 "(Library:Footprint) is what pcb tools and symbol footprint fields take.",
                 "pcb",
                 ProjectToolSchema( { { "query", { { "type", "string" } } },
                                      { "library", { { "type", "string" } } },
                                      { "pads", { { "type", "integer" }, { "minimum", 0 } } },
                                      { "smd", { { "type", "boolean" } } },
                                      { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } } } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     JSON           errors = JSON::array();
                     auto&          adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER&>(
                             LoadLibraries( project, LIBRARY_TABLE_TYPE::FOOTPRINT, aContext, errors ) );

                     const std::string                    query = aArgs.value( "query", "" );
                     const size_t                         limit = aArgs.value( "limit", size_t( 20 ) );
                     std::vector<std::pair<double, JSON>> hits;

                     for( const wxString& library : adapter.GetLibraryNames() )
                     {
                         if( aArgs.contains( "library" ) && utf8( library ) != aArgs["library"].get<std::string>() )
                             continue;

                         for( FOOTPRINT* footprint : adapter.GetFootprints( library, true ) )
                         {
                             const int pads = static_cast<int>( footprint->GetUniquePadNumbers().size() );

                             if( aArgs.contains( "pads" ) && pads != aArgs["pads"].get<int>() )
                                 continue;

                             if( aArgs.contains( "smd" )
                                 && ( ( footprint->GetAttributes() & FP_SMD ) != 0 ) != aArgs["smd"].get<bool>() )
                                 continue;

                             double score = 1.0;

                             if( !query.empty() )
                             {
                                 score = SearchScore( query, utf8( footprint->GetFPID().GetLibItemName() ),
                                                      utf8( footprint->GetKeywords() ),
                                                      utf8( footprint->GetLibDescription() ), pads );
                             }

                             if( score > 0 )
                                 hits.emplace_back( score, footprintSummary( library, *footprint ) );
                         }
                     }

                     std::stable_sort( hits.begin(), hits.end(),
                                       []( const auto& a, const auto& b )
                                       {
                                           if( a.first != b.first )
                                               return a.first > b.first;

                                           return a.second["id"] < b.second["id"];
                                       } );

                     JSON results = JSON::array();

                     for( size_t i = 0; i < hits.size() && i < limit; ++i )
                     {
                         JSON hit = hits[i].second;
                         hit["score"] = std::round( hits[i].first * 100 ) / 100;
                         results.push_back( hit );
                     }

                     JSON out{ { "results", results }, { "total", hits.size() } };

                     if( !errors.empty() )
                         out["libraryErrors"] = errors;

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "footprint_info",
                 "Everything about one library footprint: pads (number, position, size, shape, type, layers), the "
                 "courtyard bounds and the 3D models. Positions in mm relative to the footprint origin, Y down.",
                 "pcb",
                 ProjectToolSchema( { { "id", { { "type", "string" }, { "description", "Library:Footprint" } } } },
                                    { "id" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string id = aArgs["id"].get<std::string>();
                     const size_t      colon = id.find( ':' );

                     if( colon == std::string::npos || colon == 0 || colon + 1 == id.size() )
                         throw TOOL_ERROR( "A footprint id is Library:Footprint; got '" + id + "'." );

                     JSON  errors = JSON::array();
                     auto& adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER&>(
                             LoadLibraries( project, LIBRARY_TABLE_TYPE::FOOTPRINT, aContext, errors ) );

                     std::unique_ptr<FOOTPRINT> footprint(
                             adapter.LoadFootprint( wxString::FromUTF8( id.substr( 0, colon ) ),
                                                    wxString::FromUTF8( id.substr( colon + 1 ) ), false ) );

                     if( !footprint )
                         throw TOOL_ERROR( "No footprint '" + id + "'. Use footprint_search to find one." );

                     JSON out = footprintSummary( wxString::FromUTF8( id.substr( 0, colon ) ), *footprint );
                     JSON pads = JSON::array();

                     for( PAD* pad : footprint->Pads() )
                     {
                         const VECTOR2I pos = pad->GetFPRelativePosition();
                         const VECTOR2I size = pad->GetSize( PADSTACK::ALL_LAYERS );
                         JSON           layers = JSON::array();

                         for( PCB_LAYER_ID layer : pad->GetLayerSet().Seq() )
                             layers.push_back( utf8( LSET::Name( layer ) ) );

                         JSON entry{ { "number", utf8( pad->GetNumber() ) },
                                     { "x", mm( pos.x ) },
                                     { "y", mm( pos.y ) },
                                     { "width", mm( size.x ) },
                                     { "height", mm( size.y ) },
                                     { "shape", utf8( pad->ShowPadShape( PADSTACK::ALL_LAYERS ) ) },
                                     { "type", utf8( pad->ShowPadAttr() ) },
                                     { "layers", layers } };

                         if( pad->GetDrillSize().x > 0 )
                             entry["drill"] = mm( pad->GetDrillSize().x );

                         pads.push_back( entry );
                     }

                     footprint->BuildCourtyardCaches();
                     JSON courtyard = JSON::object();

                     for( PCB_LAYER_ID layer : { F_CrtYd, B_CrtYd } )
                     {
                         const SHAPE_POLY_SET& poly = footprint->GetCourtyard( layer );

                         if( poly.OutlineCount() == 0 )
                             continue;

                         BOX2I box = poly.BBox();
                         courtyard[utf8( LSET::Name( layer ) )] = { { "x1", mm( box.GetLeft() ) },
                                                                    { "y1", mm( box.GetTop() ) },
                                                                    { "x2", mm( box.GetRight() ) },
                                                                    { "y2", mm( box.GetBottom() ) } };
                     }

                     JSON models = JSON::array();

                     for( const FP_3DMODEL& model : footprint->Models() )
                         models.push_back( { { "file", utf8( model.m_Filename ) }, { "shown", model.m_Show } } );

                     out["padList"] = pads;
                     out["courtyard"] = courtyard;
                     out["models"] = models;
                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "model3d_search",
                 "Find 3D model files (.step, .stp, .wrl) by words in their file name, in the 3D model folder "
                 "(${KICADn_3DMODEL_DIR}) and the project folder. Paths come back in the form footprints store.",
                 "pcb",
                 ProjectToolSchema( { { "query", { { "type", "string" } } },
                                      { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } } } },
                                    { "query" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string query = aArgs["query"].get<std::string>();
                     const size_t      limit = aArgs.value( "limit", size_t( 20 ) );

                     std::vector<std::pair<wxString, wxString>> roots; // (variable form, folder)
                     const ENV_VAR_MAP&                         vars = Pgm().GetLocalEnvVariables();
                     std::optional<wxString> modelDir = ENV_VAR::GetVersionedEnvVarValue( vars, wxT( "3DMODEL_DIR" ) );

                     if( modelDir && wxDirExists( *modelDir ) )
                         roots.emplace_back( wxS( "${" ) + ENV_VAR::GetVersionedEnvVarName( wxS( "3DMODEL_DIR" ) )
                                                     + wxS( "}" ),
                                             *modelDir );

                     roots.emplace_back( wxS( "${KIPRJMOD}" ), wxString::FromUTF8( project.Root().string() ) );

                     std::vector<std::pair<double, JSON>> hits;
                     int                                  scanned = 0;

                     for( const auto& [variable, folder] : roots )
                     {
                         std::error_code ec;

                         for( auto it = std::filesystem::recursive_directory_iterator( folder.ToStdString( wxConvUTF8 ),
                                                                                       ec );
                              !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
                         {
                             if( ++scanned % 2000 == 0 && aContext.IsCancelled() )
                                 throw TOOL_ERROR( "Cancelled." );

                             std::string ext = it->path().extension().string();
                             std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

                             if( !it->is_regular_file() || ( ext != ".step" && ext != ".stp" && ext != ".wrl" ) )
                                 continue;

                             const std::string stem = it->path().stem().string();
                             const double      score =
                                     SearchScore( query, stem, it->path().parent_path().filename().string(), "", -1 );

                             if( score <= 0 )
                                 continue;

                             const std::string rel = std::filesystem::path( it->path() )
                                                             .lexically_relative( folder.ToStdString( wxConvUTF8 ) )
                                                             .generic_string();
                             hits.emplace_back( score, JSON{ { "file", utf8( variable ) + "/" + rel } } );
                         }
                     }

                     std::stable_sort( hits.begin(), hits.end(),
                                       []( const auto& a, const auto& b )
                                       {
                                           return a.first > b.first;
                                       } );

                     JSON results = JSON::array();

                     for( size_t i = 0; i < hits.size() && i < limit; ++i )
                         results.push_back( hits[i].second );

                     return TOOL_RESULT::Data( JSON{ { "results", results }, { "total", hits.size() } } );
                 } } );

    tools.Add( { "docs_rules_syntax",
                 "The syntax of KiCad's custom design rules (.kicad_dru): clauses, constraints, item types, "
                 "severities, properties, expression functions and examples, from the help KiCad ships.",
                 "pcb",
                 JSON{ { "type", "object" }, { "properties", JSON::object() }, { "additionalProperties", false } },
                 []( const JSON&, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Text( rulesHelp() );
                 } } );

    aServices.Resources().Add( { "kicad://docs/rules-syntax", "Custom design rules syntax",
                                 "The syntax of KiCad's custom DRC rules (.kicad_dru files).", "text/markdown", []()
                                 {
                                     return RESOURCE_CONTENT{ "kicad://docs/rules-syntax", "text/markdown", rulesHelp(),
                                                              "" };
                                 } } );
}

} // namespace AGENTIC
