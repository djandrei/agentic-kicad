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
#include <agentic/sch/sch_library_tools.h>

#include <lib_symbol.h>
#include <libraries/symbol_library_adapter.h>
#include <sch_field.h>
#include <sch_pin.h>

#include <algorithm>


namespace AGENTIC
{

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    std::string filtersText( const LIB_SYMBOL& aSymbol )
    {
        std::string out;

        for( const wxString& filter : aSymbol.GetFPFilters() )
            out += ( out.empty() ? "" : " " ) + utf8( filter );

        return out;
    }


    JSON symbolSummary( const wxString& aLibrary, const LIB_SYMBOL& aSymbol )
    {
        JSON out{ { "id", utf8( aLibrary + wxS( ":" ) + aSymbol.GetName() ) },
                  { "description", utf8( aSymbol.GetDescription() ) },
                  { "keywords", utf8( aSymbol.GetKeyWords() ) },
                  { "pins", aSymbol.GetPinCount() },
                  { "units", aSymbol.GetUnitCount() } };

        if( !aSymbol.GetFootprint().IsEmpty() )
            out["footprint"] = utf8( aSymbol.GetFootprint() );

        if( aSymbol.IsPower() )
            out["power"] = true;

        return out;
    }

} // namespace


void RegisterSymbolLibraryTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY&   tools = aServices.Tools();
    SESSION_MANAGER& sessions = aServices.Sessions();

    tools.Add( { "symbol_search",
                 "Find symbols in the project's symbol libraries by free text (name, keywords, description; \"8 "
                 "pin\" matches the pin count), best match first. Narrow with library, pins, power. The id "
                 "(Library:Symbol) is what sch tools take.",
                 "sch",
                 ProjectToolSchema( { { "query", { { "type", "string" } } },
                                      { "library", { { "type", "string" } } },
                                      { "pins", { { "type", "integer" }, { "minimum", 0 } } },
                                      { "power", { { "type", "boolean" } } },
                                      { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } } } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     JSON           errors = JSON::array();
                     auto&          adapter = static_cast<SYMBOL_LIBRARY_ADAPTER&>(
                             LoadLibraries( project, LIBRARY_TABLE_TYPE::SYMBOL, aContext, errors ) );

                     const std::string                    query = aArgs.value( "query", "" );
                     const size_t                         limit = aArgs.value( "limit", size_t( 20 ) );
                     std::vector<std::pair<double, JSON>> hits;

                     for( const wxString& library : adapter.GetLibraryNames() )
                     {
                         if( aArgs.contains( "library" ) && utf8( library ) != aArgs["library"].get<std::string>() )
                             continue;

                         for( LIB_SYMBOL* symbol : adapter.GetSymbols( library ) )
                         {
                             if( aArgs.contains( "pins" ) && symbol->GetPinCount() != aArgs["pins"].get<int>() )
                                 continue;

                             if( aArgs.contains( "power" ) && symbol->IsPower() != aArgs["power"].get<bool>() )
                                 continue;

                             double score = 1.0;

                             if( !query.empty() )
                             {
                                 score = SearchScore( query, utf8( symbol->GetName() ), utf8( symbol->GetKeyWords() ),
                                                      utf8( symbol->GetDescription() ) + " " + filtersText( *symbol ),
                                                      symbol->GetPinCount() );
                             }

                             if( score > 0 )
                                 hits.emplace_back( score, symbolSummary( library, *symbol ) );
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

    tools.Add( { "symbol_info",
                 "Everything about one library symbol: pins (number, name, type, unit), units, fields, footprint "
                 "filters and SPICE model fields.",
                 "sch",
                 ProjectToolSchema( { { "id", { { "type", "string" }, { "description", "Library:Symbol" } } } },
                                    { "id" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string id = aArgs["id"].get<std::string>();
                     const size_t      colon = id.find( ':' );

                     if( colon == std::string::npos || colon == 0 || colon + 1 == id.size() )
                         throw TOOL_ERROR( "A symbol id is Library:Symbol; got '" + id + "'." );

                     JSON  errors = JSON::array();
                     auto& adapter = static_cast<SYMBOL_LIBRARY_ADAPTER&>(
                             LoadLibraries( project, LIBRARY_TABLE_TYPE::SYMBOL, aContext, errors ) );

                     LIB_SYMBOL* symbol = adapter.LoadSymbol( wxString::FromUTF8( id.substr( 0, colon ) ),
                                                              wxString::FromUTF8( id.substr( colon + 1 ) ) );

                     if( !symbol )
                         throw TOOL_ERROR( "No symbol '" + id + "'. Use symbol_search to find one." );

                     JSON out = symbolSummary( wxString::FromUTF8( id.substr( 0, colon ) ), *symbol );
                     JSON pins = JSON::array();

                     for( SCH_PIN* pin : symbol->GetGraphicalPins( 0, 0 ) )
                     {
                         pins.push_back( { { "number", utf8( pin->GetNumber() ) },
                                           { "name", utf8( pin->GetName() ) },
                                           { "type", utf8( pin->GetCanonicalElectricalTypeName() ) },
                                           { "unit", pin->GetUnit() },
                                           { "bodyStyle", pin->GetBodyStyle() } } );
                     }

                     std::sort( pins.begin(), pins.end(),
                                []( const JSON& a, const JSON& b )
                                {
                                    return std::make_pair( a["unit"].get<int>(), a["number"].get<std::string>() )
                                           < std::make_pair( b["unit"].get<int>(), b["number"].get<std::string>() );
                                } );

                     JSON                    fields = JSON::object();
                     JSON                    sim = JSON::object();
                     std::vector<SCH_FIELD*> fieldList;
                     symbol->GetFields( fieldList );

                     for( SCH_FIELD* field : fieldList )
                     {
                         const std::string name = utf8( field->GetName() );

                         if( name.rfind( "Sim.", 0 ) == 0 )
                             sim[name.substr( 4 )] = utf8( field->GetText() );
                         else
                             fields[name] = utf8( field->GetText() );
                     }

                     JSON units = JSON::array();

                     for( int unit = 1; unit <= symbol->GetUnitCount(); ++unit )
                         units.push_back(
                                 { { "unit", unit }, { "name", utf8( symbol->GetUnitDisplayName( unit, true ) ) } } );

                     JSON filters = JSON::array();

                     for( const wxString& filter : symbol->GetFPFilters() )
                         filters.push_back( utf8( filter ) );

                     out["pinList"] = pins;
                     out["unitList"] = units;
                     out["fields"] = fields;
                     out["footprintFilters"] = filters;

                     if( !sim.empty() )
                         out["spice"] = sim;

                     return TOOL_RESULT::Data( out );
                 } } );
}

} // namespace AGENTIC
