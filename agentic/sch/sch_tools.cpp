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
#include <agentic/core/services.h>
#include <agentic/sch/sch_doc_session.h>
#include <agentic/sch/sch_items.h>
#include <agentic/sch/sch_tools.h>

#include <base_units.h>
#include <connectivity/conn_facade.h>
#include <lib_symbol.h>
#include <libraries/symbol_library_adapter.h>
#include <sch_commit.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <schematic.h>
#include <schematic_settings.h>

#include <wx/filename.h>

#include <algorithm>
#include <cmath>


namespace AGENTIC
{

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


    const JSON POINT{ { "description", "{x, y} in mm (or strings with units), Y down." },
                      { "type", { "object", "array" } } };

    const JSON FILE_ARG{ { "type", "string" },
                         { "description", "Root schematic (project-relative); default the project's schematic." } };

    const JSON SHEET_ARG{ { "type", "string" },
                          { "description", "Sheet path such as \"/\" or \"/power/\"; default the sheet used last." } };

    const JSON ITEMS_ARG{ { "type", "array" },
                          { "items", { { "type", "string" } } },
                          { "description", "Item handles: KIID, reference (R5) or pin (U1.3)." } };


    JSON schSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["file"] = FILE_ARG;
        aProperties["force"] = { { "type", "boolean" } };
        return ProjectToolSchema( aProperties, aRequired );
    }


    SCH_DOC_SESSION& session( AGENT_SERVICES& aServices, const JSON& aArgs, AGENT_PROJECT** aProject = nullptr )
    {
        AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );

        if( aProject )
            *aProject = &project;

        return SchematicSession( project, aArgs.value( "file", "" ) );
    }


    /// Rotation in degrees (0, 90, 180, 270) to the symbol orientation.
    int orientation( double aDegrees )
    {
        int quarter = static_cast<int>( std::lround( aDegrees / 90.0 ) ) % 4;

        if( quarter < 0 )
            quarter += 4;

        if( std::fabs( aDegrees - quarter * 90.0 ) > 1e-6 && std::fabs( aDegrees - ( quarter - 4 ) * 90.0 ) > 1e-6 )
            throw TOOL_ERROR( "Schematic rotations are multiples of 90 degrees." );

        static const int angles[] = { SYM_ORIENT_0, SYM_ORIENT_90, SYM_ORIENT_180, SYM_ORIENT_270 };
        return angles[quarter];
    }


    SPIN_STYLE spin( double aDegrees )
    {
        int                     quarter = ( ( static_cast<int>( std::lround( aDegrees / 90.0 ) ) % 4 ) + 4 ) % 4;
        static const SPIN_STYLE spins[] = { SPIN_STYLE::RIGHT, SPIN_STYLE::UP, SPIN_STYLE::LEFT, SPIN_STYLE::BOTTOM };
        return spins[quarter];
    }


    /// The pin a handle ("U1.3") names, on one sheet instance.
    SCH_FOUND pinOf( SCHEMATIC& aSchematic, const std::string& aHandle )
    {
        SCH_FOUND found = SchFindOne( aSchematic, aHandle );

        if( found.item->Type() != SCH_PIN_T )
            throw TOOL_ERROR( "'" + aHandle + "' is not a pin; give REF.PIN, e.g. U1.3." );

        return found;
    }


    /// A point argument that may name a pin instead: "U1.3" or {x, y}.
    VECTOR2I pointOrPin( SCHEMATIC& aSchematic, const JSON& aValue, const std::string& aField, SCH_SHEET_PATH* aPath )
    {
        if( aValue.is_string() )
        {
            SCH_FOUND pin = pinOf( aSchematic, aValue.get<std::string>() );

            if( aPath )
                *aPath = pin.path;

            return pin.item->GetPosition();
        }

        return SchPoint( aValue, aField );
    }


    /// Unit vector a pin points away from its symbol, in schematic coordinates.
    VECTOR2I outward( const SCH_PIN* aPin )
    {
        const SCH_SYMBOL* symbol = dynamic_cast<const SCH_SYMBOL*>( aPin->GetParentSymbol() );
        PIN_ORIENTATION   orient = symbol ? aPin->PinDrawOrient( symbol->GetTransform() ) : aPin->GetOrientation();

        // The orientation names where the pin's body side extends from its connection point.
        switch( orient )
        {
        case PIN_ORIENTATION::PIN_RIGHT: return VECTOR2I( -1, 0 );
        case PIN_ORIENTATION::PIN_LEFT: return VECTOR2I( 1, 0 );
        case PIN_ORIENTATION::PIN_UP: return VECTOR2I( 0, 1 );
        case PIN_ORIENTATION::PIN_DOWN: return VECTOR2I( 0, -1 );
        default: return VECTOR2I( 0, 0 );
        }
    }


    bool segmentHitsBox( const VECTOR2I& aA, const VECTOR2I& aB, const BOX2I& aBox )
    {
        // Manhattan segments only.
        if( aA.x == aB.x )
        {
            const int y1 = std::min( aA.y, aB.y ), y2 = std::max( aA.y, aB.y );
            return aA.x > aBox.GetLeft() && aA.x < aBox.GetRight() && y2 > aBox.GetTop() && y1 < aBox.GetBottom();
        }

        const int x1 = std::min( aA.x, aB.x ), x2 = std::max( aA.x, aB.x );
        return aA.y > aBox.GetTop() && aA.y < aBox.GetBottom() && x2 > aBox.GetLeft() && x1 < aBox.GetRight();
    }


    /// A Manhattan path from \a aFrom to \a aTo that avoids the symbol bodies, or empty.
    std::vector<VECTOR2I> route( const VECTOR2I& aFrom, const VECTOR2I& aFromDir, const VECTOR2I& aTo,
                                 const VECTOR2I& aToDir, const std::vector<BOX2I>& aBodies )
    {
        const int                          step = schIUScale.mmToIU( 2.54 );
        std::vector<std::vector<VECTOR2I>> candidates;

        auto add = [&]( std::vector<VECTOR2I> aPoints )
        {
            // Drop repeated and collinear points.
            std::vector<VECTOR2I> clean;

            for( const VECTOR2I& p : aPoints )
            {
                if( !clean.empty() && clean.back() == p )
                    continue;

                if( clean.size() >= 2 )
                {
                    const VECTOR2I& a = clean[clean.size() - 2];
                    const VECTOR2I& b = clean.back();

                    if( ( a.x == b.x && b.x == p.x ) || ( a.y == b.y && b.y == p.y ) )
                    {
                        clean.back() = p;
                        continue;
                    }
                }

                clean.push_back( p );
            }

            candidates.push_back( clean );
        };

        add( { aFrom, VECTOR2I( aTo.x, aFrom.y ), aTo } );
        add( { aFrom, VECTOR2I( aFrom.x, aTo.y ), aTo } );

        for( int lead = 1; lead <= 4; ++lead )
        {
            const VECTOR2I a = aFrom + aFromDir * ( step * lead );
            const VECTOR2I b = aTo + aToDir * ( step * lead );
            add( { aFrom, a, VECTOR2I( b.x, a.y ), b, aTo } );
            add( { aFrom, a, VECTOR2I( a.x, b.y ), b, aTo } );
            add( { aFrom, a, VECTOR2I( ( a.x + b.x ) / 2, a.y ), VECTOR2I( ( a.x + b.x ) / 2, b.y ), b, aTo } );
            add( { aFrom, a, VECTOR2I( a.x, ( a.y + b.y ) / 2 ), VECTOR2I( b.x, ( a.y + b.y ) / 2 ), b, aTo } );
        }

        for( const std::vector<VECTOR2I>& path : candidates )
        {
            bool clear = true;

            for( size_t i = 1; i < path.size() && clear; ++i )
            {
                for( const BOX2I& body : aBodies )
                {
                    if( segmentHitsBox( path[i - 1], path[i], body ) )
                    {
                        clear = false;
                        break;
                    }
                }
            }

            if( clear )
                return path;
        }

        return {};
    }


    LIB_SYMBOL* loadLibSymbol( AGENT_PROJECT& aProject, const std::string& aLibId, TOOL_CONTEXT& aContext )
    {
        LIB_ID id;

        if( id.Parse( wx( aLibId ) ) >= 0 || !id.IsValid() )
            throw TOOL_ERROR( "A symbol id is Library:Symbol; got '" + aLibId + "'." );

        JSON  errors = JSON::array();
        auto& adapter = static_cast<SYMBOL_LIBRARY_ADAPTER&>(
                LoadLibraries( aProject, LIBRARY_TABLE_TYPE::SYMBOL, aContext, errors ) );
        LIB_SYMBOL* symbol = adapter.LoadSymbol( id );

        if( !symbol )
            throw TOOL_ERROR( "No symbol '" + aLibId + "' in the project's libraries; use symbol_search." );

        return symbol;
    }


    SCH_SYMBOL* placeSymbol( SCH_EDIT& aEdit, const LIB_SYMBOL& aLibSymbol, const std::string& aLibId,
                             const SCH_SHEET_PATH& aPath, const VECTOR2I& aPosition, int aUnit, int aBodyStyle,
                             const JSON& aArgs )
    {
        SCHEMATIC& schematic = aEdit.Session().Schematic();
        LIB_ID     id;
        id.Parse( wx( aLibId ) );

        if( aUnit < 1 || ( aLibSymbol.GetUnitCount() > 0 && aUnit > aLibSymbol.GetUnitCount() ) )
            throw TOOL_ERROR( "Unit " + std::to_string( aUnit ) + " is out of range for " + aLibId + " ("
                              + std::to_string( aLibSymbol.GetUnitCount() ) + " units)." );

        auto symbol = std::make_unique<SCH_SYMBOL>( aLibSymbol, id, &aPath, aUnit, aBodyStyle, aPosition );

        int orient = orientation( aArgs.contains( "rotation" ) ? ParseAngleDeg( aArgs["rotation"], "rotation" ) : 0 );

        if( aArgs.value( "mirror", "" ) == "x" )
            orient |= SYM_MIRROR_X;
        else if( aArgs.value( "mirror", "" ) == "y" )
            orient |= SYM_MIRROR_Y;

        symbol->SetOrientation( orient );

        SCH_SHEET_LIST hierarchy = schematic.Hierarchy();

        if( aArgs.contains( "reference" ) )
        {
            symbol->SetRef( &aPath, wx( aArgs["reference"].get<std::string>() ) );
        }
        else
        {
            SCH_REFERENCE_LIST existing;
            hierarchy.GetSymbols( existing, aLibSymbol.IsPower() ? SYMBOL_FILTER_POWER : SYMBOL_FILTER_NON_POWER );
            existing.SortByReferenceOnly();
            AnnotateNewSymbol( symbol.get(), { aPath }, existing, schematic.Settings(), &hierarchy );
        }

        if( aArgs.contains( "value" ) )
            symbol->SetValueFieldText( wx( aArgs["value"].get<std::string>() ) );

        if( aArgs.contains( "footprint" ) )
            symbol->SetFootprintFieldText( wx( aArgs["footprint"].get<std::string>() ) );

        symbol->AutoplaceFields( nullptr, AUTOPLACE_AUTO );

        SCH_SYMBOL* placed = symbol.release();
        aEdit.Add( placed, aPath.LastScreen() );
        return placed;
    }


    SCH_TEXT* makeLabel( const std::string& aKind, const VECTOR2I& aPosition, const wxString& aText )
    {
        if( aKind == "local" )
            return new SCH_LABEL( aPosition, aText );

        if( aKind == "global" )
            return new SCH_GLOBALLABEL( aPosition, aText );

        if( aKind == "hierarchical" )
            return new SCH_HIERLABEL( aPosition, aText );

        throw TOOL_ERROR( "Label kind is local, global, hierarchical or power; got '" + aKind + "'." );
    }

} // namespace


// SCH_EDIT ---------------------------------------------------------------------------------

void SCH_EDIT::Add( SCH_ITEM* aItem, SCH_SCREEN* aScreen )
{
    aItem->SetParent( aScreen );
    m_commit.Add( aItem, aScreen );
    m_session.Touch( aScreen );
    created.push_back( SchItemJson( aItem, m_session.CurrentSheet() ) );
}


void SCH_EDIT::Modify( SCH_ITEM* aItem, SCH_SCREEN* aScreen )
{
    m_commit.Modify( aItem, aScreen );
    m_session.Touch( aScreen );
}


void SCH_EDIT::Remove( SCH_ITEM* aItem, SCH_SCREEN* aScreen )
{
    removed.push_back( SchItemJson( aItem, m_session.CurrentSheet() ) );
    m_commit.Remove( aItem, aScreen );
    m_session.Touch( aScreen );
}


JSON SchMutate( AGENT_PROJECT& aProject, SCH_DOC_SESSION& aSession, const std::string& aLabel, const JSON& aArgs,
                bool aDelta, const std::function<JSON( SCH_EDIT& )>& aEdit )
{
    return aProject.Mutate(
            aLabel, aArgs.value( "force", false ),
            [&]() -> JSON
            {
                std::optional<SCH_NET_SNAPSHOT> before;

                if( aDelta )
                    before = SCH_NET_SNAPSHOT::Take( aSession.Schematic() );

                SCH_COMMIT commit( aSession.ToolManager() );
                SCH_EDIT   edit( aSession, commit );
                JSON       result = aEdit( edit );

                if( !result.is_object() )
                    result = JSON::object();

                if( commit.Empty() && edit.created.empty() && edit.removed.empty() && edit.changed.empty()
                    && !aSession.IsModified() )
                {
                    throw TOOL_ERROR( "Nothing changed." );
                }

                commit.Push( wx( aLabel ) );

                // Items changed: their state after the push.
                for( JSON& item : edit.changed )
                {
                    if( item.contains( "kiid" ) )
                    {
                        try
                        {
                            SCH_FOUND found = SchFindOne( aSession.Schematic(), item["kiid"].get<std::string>() );
                            item["after"] = SchItemJson( found.item, found.path );
                        }
                        catch( const TOOL_ERROR& )
                        {
                        }
                    }
                }

                if( !edit.created.empty() )
                    result["created"] = edit.created;

                if( !edit.changed.empty() )
                    result["changed"] = edit.changed;

                if( !edit.removed.empty() )
                    result["removed"] = edit.removed;

                if( before )
                    result["connectivity"] = SchNetDelta( *before, SCH_NET_SNAPSHOT::Take( aSession.Schematic() ) );

                JSON change{ { "summary", aLabel + ": " + std::to_string( edit.created.size() ) + " added, "
                                                  + std::to_string( edit.changed.size() ) + " changed, "
                                                  + std::to_string( edit.removed.size() ) + " removed" },
                             { "tool", aLabel } };

                JSON kiids = JSON::array();

                for( const JSON* list : { &edit.created, &edit.changed } )
                {
                    for( const JSON& item : *list )
                    {
                        if( item.contains( "kiid" ) )
                            kiids.push_back( item["kiid"] );
                    }
                }

                change["items"] = kiids;
                change["sheet"] = utf8( aSession.CurrentSheet().PathHumanReadable( false ) );
                aSession.RecordChange( change );
                return result;
            } );
}


void RegisterSchEditTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    // Queries ----------------------------------------------------------------------------------

    tools.Add( { "sch_hierarchy",
                 "The schematic's sheets: path, name, file, page number and how many symbols each holds. Opens the "
                 "project's schematic (creating it empty in memory when missing).",
                 "sch", schSchema( JSON::object() ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     SCH_DOC_SESSION& s = session( aServices, aArgs );
                     JSON             sheets = JSON::array();

                     for( const SCH_SHEET_PATH& path : s.Schematic().Hierarchy() )
                     {
                         SCH_SCREEN* screen = path.LastScreen();
                         size_t      symbols = 0;

                         for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
                         {
                             (void) item;
                             symbols++;
                         }

                         sheets.push_back( { { "path", utf8( path.PathHumanReadable( false ) ) },
                                             { "name", utf8( path.Last()->GetName() ) },
                                             { "kiid", utf8( path.Last()->m_Uuid.AsString() ) },
                                             { "file", s.FileOf( screen ) },
                                             { "page", utf8( path.GetPageNumber() ) },
                                             { "symbols", symbols } } );
                     }

                     return TOOL_RESULT::Data(
                             JSON{ { "root", s.Path() }, { "new", s.IsNew() }, { "sheets", sheets } } );
                 } } );

    tools.Add( { "sch_query",
                 "List schematic items, filtered by type (symbol, wire, bus, label, global_label, hierarchical_label, "
                 "junction, no_connect, text, sheet), reference, net, sheet or area {x1,y1,x2,y2}; paged with "
                 "offset and limit.",
                 "sch",
                 schSchema( { { "type", { { "type", "string" } } },
                              { "reference", { { "type", "string" } } },
                              { "net", { { "type", "string" } } },
                              { "sheet", SHEET_ARG },
                              { "area", { { "type", "object" } } },
                              { "offset", { { "type", "integer" }, { "minimum", 0 } } },
                              { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     SCH_DOC_SESSION&  s = session( aServices, aArgs );
                     SCHEMATIC&        schematic = s.Schematic();
                     const std::string type = aArgs.value( "type", "" );
                     const size_t      offset = aArgs.value( "offset", size_t( 0 ) );
                     const size_t      limit = aArgs.value( "limit", size_t( 100 ) );

                     std::optional<BOX2I> area;

                     if( aArgs.contains( "area" ) )
                     {
                         const JSON& a = aArgs["area"];
                         VECTOR2I p1 = SchPoint( JSON{ { "x", a.at( "x1" ) }, { "y", a.at( "y1" ) } }, "area", false );
                         VECTOR2I p2 = SchPoint( JSON{ { "x", a.at( "x2" ) }, { "y", a.at( "y2" ) } }, "area", false );
                         area = BOX2I( p1, p2 - p1 );
                         area->Normalize();
                     }

                     std::optional<SCH_SHEET_PATH> onlySheet;

                     if( aArgs.contains( "sheet" ) )
                         onlySheet = SchSheet( s, aArgs["sheet"].get<std::string>() );

                     JSON   items = JSON::array();
                     size_t total = 0;

                     for( const SCH_SHEET_PATH& path : schematic.Hierarchy() )
                     {
                         if( onlySheet && !( path == *onlySheet ) )
                             continue;

                         for( SCH_ITEM* item : path.LastScreen()->Items() )
                         {
                             if( item->Type() == SCH_MARKER_T )
                                 continue;

                             JSON json = SchItemJson( item, path );

                             if( !type.empty() && json["type"] != type )
                                 continue;

                             if( aArgs.contains( "reference" )
                                 && json.value( "reference", "" ) != aArgs["reference"].get<std::string>() )
                                 continue;

                             if( area && !area->Intersects( item->GetBoundingBox() ) )
                                 continue;

                             if( aArgs.contains( "net" ) )
                             {
                                 const wxString net = wx( aArgs["net"].get<std::string>() );
                                 bool           onNet = false;

                                 if( item->Type() == SCH_SYMBOL_T )
                                 {
                                     for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( item )->GetPins( &path ) )
                                     {
                                         auto conn = schematic.Connectivity().Connection( pin->m_Uuid, path.Path() );
                                         onNet |= conn && conn->Name() == net;
                                     }
                                 }
                                 else
                                 {
                                     auto conn = schematic.Connectivity().Connection( item->m_Uuid, path.Path() );
                                     onNet = conn && conn->Name() == net;
                                 }

                                 if( !onNet )
                                     continue;
                             }

                             if( total++ >= offset && items.size() < limit )
                                 items.push_back( json );
                         }
                     }

                     JSON out{ { "items", items }, { "total", total } };

                     if( offset + items.size() < total )
                         out["next"] = offset + items.size();

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "sch_nets",
                 "The schematic's nets and the pins on each (REF.PIN), plus the pins on no net with anything else. "
                 "Give net for one net.",
                 "sch", schSchema( WithPageArgs( { { "net", { { "type", "string" } } } } ) ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     SCH_DOC_SESSION& s = session( aServices, aArgs );
                     SCH_NET_SNAPSHOT snapshot = SCH_NET_SNAPSHOT::Take( s.Schematic() );
                     JSON             nets = JSON::array();

                     for( const auto& [name, pins] : snapshot.nets )
                     {
                         if( aArgs.contains( "net" ) && name != aArgs["net"].get<std::string>() )
                             continue;

                         nets.push_back( { { "net", name }, { "pins", pins } } );
                     }

                     if( aArgs.contains( "net" ) && nets.empty() )
                         throw TOOL_ERROR( "No net '" + aArgs["net"].get<std::string>()
                                           + "'. Names are as sch_nets lists them, e.g. /SDA." );

                     return TOOL_RESULT::Data( JSON{ { "nets", Page( nets, aArgs ) },
                                                     { "total", nets.size() },
                                                     { "unconnectedPins", snapshot.unconnectedPins } } );
                 } } );

    // Placement and editing --------------------------------------------------------------------

    tools.Add( { "sch_symbol_place",
                 "Place a library symbol (Library:Symbol from symbol_search) at a position (mm, snapped to 1.27 mm). "
                 "It is annotated with the next free reference unless reference is given. Optional rotation "
                 "(0/90/180/270), mirror (x or y), unit, value, footprint.",
                 "sch",
                 schSchema( { { "libId", { { "type", "string" } } },
                              { "position", POINT },
                              { "rotation", { { "type", { "number", "string" } } } },
                              { "mirror", { { "type", "string" }, { "enum", { "x", "y", "" } } } },
                              { "unit", { { "type", "integer" }, { "minimum", 1 } } },
                              { "bodyStyle", { { "type", "integer" }, { "minimum", 1 } } },
                              { "reference", { { "type", "string" } } },
                              { "value", { { "type", "string" } } },
                              { "footprint", { { "type", "string" } } },
                              { "sheet", SHEET_ARG } },
                            { "libId", "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT*    project = nullptr;
                     SCH_DOC_SESSION&  s = session( aServices, aArgs, &project );
                     const std::string libId = aArgs["libId"].get<std::string>();
                     LIB_SYMBOL*       libSymbol = loadLibSymbol( *project, libId, aContext );
                     SCH_SHEET_PATH    path = SchSheet( s, aArgs.value( "sheet", "" ) );
                     s.SetCurrentSheet( path );

                     JSON result = SchMutate(
                             *project, s, "sch_symbol_place " + libId, aArgs, true,
                             [&]( SCH_EDIT& aEdit ) -> JSON
                             {
                                 SCH_SYMBOL* placed = placeSymbol(
                                         aEdit, *libSymbol, libId, path, SchPoint( aArgs["position"], "position" ),
                                         aArgs.value( "unit", 1 ), aArgs.value( "bodyStyle", 0 ), aArgs );

                                 JSON pins = JSON::array();

                                 for( SCH_PIN* pin : placed->GetPins( &path ) )
                                 {
                                     pins.push_back( { { "pin", SchPinId( pin, path ) },
                                                       { "name", utf8( pin->GetShownName() ) },
                                                       { "position", SchPointJson( pin->GetPosition() ) } } );
                                 }

                                 return JSON{ { "reference", utf8( placed->GetRef( &path, false ) ) },
                                              { "pins", pins } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "sch_symbol_set",
                 "Change a placed symbol: value, footprint, reference, fields ({name: text}, null removes a user "
                 "field), unit, dnp, excludeFromBom, excludeFromBoard, excludeFromSim.",
                 "sch",
                 schSchema( { { "item", { { "type", "string" } } },
                              { "value", { { "type", "string" } } },
                              { "footprint", { { "type", "string" } } },
                              { "reference", { { "type", "string" } } },
                              { "fields", { { "type", "object" } } },
                              { "unit", { { "type", "integer" }, { "minimum", 1 } } },
                              { "dnp", { { "type", "boolean" } } },
                              { "excludeFromBom", { { "type", "boolean" } } },
                              { "excludeFromBoard", { { "type", "boolean" } } },
                              { "excludeFromSim", { { "type", "boolean" } } } },
                            { "item" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCH_FOUND        found = SchFindOne( s.Schematic(), aArgs["item"].get<std::string>() );

                     if( found.item->Type() != SCH_SYMBOL_T )
                         throw TOOL_ERROR( "'" + aArgs["item"].get<std::string>() + "' is not a symbol." );

                     SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( found.item );
                     s.SetCurrentSheet( found.path );

                     JSON result = SchMutate(
                             *project, s, "sch_symbol_set " + aArgs["item"].get<std::string>(), aArgs,
                             aArgs.contains( "unit" ),
                             [&]( SCH_EDIT& aEdit ) -> JSON
                             {
                                 JSON before = SchItemJson( symbol, found.path );
                                 aEdit.Modify( symbol, found.path.LastScreen() );
                                 const SCH_SHEET_PATH* path = &found.path;

                                 if( aArgs.contains( "value" ) )
                                     symbol->SetValueFieldText( wx( aArgs["value"] ) );

                                 if( aArgs.contains( "footprint" ) )
                                     symbol->SetFootprintFieldText( wx( aArgs["footprint"] ) );

                                 if( aArgs.contains( "reference" ) )
                                     symbol->SetRef( path, wx( aArgs["reference"] ) );

                                 if( aArgs.contains( "unit" ) )
                                 {
                                     const int unit = aArgs["unit"];

                                     if( unit > symbol->GetUnitCount() )
                                         throw TOOL_ERROR( "The symbol has " + std::to_string( symbol->GetUnitCount() )
                                                           + " units." );

                                     symbol->SetUnitSelection( path, unit );
                                     symbol->SetUnit( unit );
                                 }

                                 if( aArgs.contains( "dnp" ) )
                                     symbol->SetDNP( aArgs["dnp"], path );

                                 if( aArgs.contains( "excludeFromBom" ) )
                                     symbol->SetExcludedFromBOM( aArgs["excludeFromBom"], path );

                                 if( aArgs.contains( "excludeFromBoard" ) )
                                     symbol->SetExcludedFromBoard( aArgs["excludeFromBoard"], path );

                                 if( aArgs.contains( "excludeFromSim" ) )
                                     symbol->SetExcludedFromSim( aArgs["excludeFromSim"], path );

                                 const JSON fields = aArgs.value( "fields", JSON::object() );

                                 for( const auto& [name, value] : fields.items() )
                                 {
                                     SCH_FIELD* field = symbol->GetField( wx( name ) );

                                     if( value.is_null() )
                                     {
                                         if( field && !field->IsMandatory() )
                                             symbol->RemoveField( field );

                                         continue;
                                     }

                                     if( !field )
                                     {
                                         SCH_FIELD newField( symbol, FIELD_T::USER, wx( name ) );
                                         newField.SetPosition( symbol->GetPosition() );
                                         newField.SetVisible( false );
                                         field = symbol->AddField( newField );
                                     }

                                     field->SetText( wx( value.get<std::string>() ) );
                                 }

                                 aEdit.changed.push_back(
                                         { { "kiid", utf8( symbol->m_Uuid.AsString() ) }, { "before", before } } );
                                 return JSON::object();
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    // aCenter: the grid point nearest the items' centre; aAnchor: the first item's position before.
    using TRANSFORM = std::function<void( SCH_ITEM*, const JSON&, const VECTOR2I& aCenter, const VECTOR2I& aAnchor )>;

    auto transformTool =
            [&aServices]( const std::string& aName, const std::string& aDescription, JSON aExtra, TRANSFORM aApply )
    {
        JSON properties = aExtra;
        properties["items"] = ITEMS_ARG;

        aServices.Tools().Add( { aName, aDescription, "sch", schSchema( properties, { "items" } ),
                                 [&aServices, aName, aApply]( const JSON& aArgs, TOOL_CONTEXT& )
                                 {
                                     AGENT_PROJECT*         project = nullptr;
                                     SCH_DOC_SESSION&       s = session( aServices, aArgs, &project );
                                     std::vector<SCH_FOUND> items;

                                     for( const JSON& handle : aArgs["items"] )
                                     {
                                         SCH_FOUND found = SchFindOne( s.Schematic(), handle.get<std::string>() );

                                         if( found.item->Type() == SCH_PIN_T )
                                             throw TOOL_ERROR( "Pins move with their symbol; name the symbol." );

                                         items.push_back( found );
                                     }

                                     if( items.empty() )
                                         throw TOOL_ERROR( "No items given." );

                                     BOX2I bbox = items.front().item->GetBoundingBox();

                                     for( const SCH_FOUND& found : items )
                                         bbox.Merge( found.item->GetBoundingBox() );

                                     // Rotate about the grid point nearest the centre, so pins stay on the grid.
                                     const VECTOR2I center( SchIu( schIUScale.IUTomm( bbox.GetCenter().x ) ),
                                                            SchIu( schIUScale.IUTomm( bbox.GetCenter().y ) ) );
                                     s.SetCurrentSheet( items.front().path );

                                     // The first item's position before any change: sch_move's "to" refers to it.
                                     const VECTOR2I anchor = items.front().item->GetPosition();

                                     JSON result = SchMutate(
                                             *project, s, aName, aArgs, true,
                                             [&]( SCH_EDIT& aEdit ) -> JSON
                                             {
                                                 for( const SCH_FOUND& found : items )
                                                 {
                                                     JSON before = SchItemJson( found.item, found.path );
                                                     aEdit.Modify( found.item, found.path.LastScreen() );
                                                     aApply( found.item, aArgs, center, anchor );
                                                     aEdit.changed.push_back(
                                                             { { "kiid", utf8( found.item->m_Uuid.AsString() ) },
                                                               { "before", before } } );
                                                 }

                                                 return JSON::object();
                                             } );

                                     return TOOL_RESULT::Data( result );
                                 } } );
    };

    transformTool( "sch_move",
                   "Move items by delta {x, y} (mm, snapped), or the first item to position \"to\" with the others "
                   "following. Wires attached to moved pins are not dragged; reconnect them.",
                   { { "delta", POINT }, { "to", POINT } },
                   []( SCH_ITEM* aItem, const JSON& aArgs, const VECTOR2I&, const VECTOR2I& aAnchor )
                   {
                       VECTOR2I offset;

                       if( aArgs.contains( "to" ) )
                           offset = SchPoint( aArgs["to"], "to" ) - aAnchor;
                       else if( aArgs.contains( "delta" ) )
                           offset = SchPoint( aArgs["delta"], "delta" );
                       else
                           throw TOOL_ERROR( "Give delta or to." );

                       aItem->Move( offset );
                   } );

    transformTool( "sch_rotate",
                   "Rotate items by a multiple of 90 degrees (counterclockwise by default) about their centre.",
                   { { "angle", { { "type", { "number", "string" } } } } },
                   []( SCH_ITEM* aItem, const JSON& aArgs, const VECTOR2I& aCenter, const VECTOR2I& )
                   {
                       double angle = aArgs.contains( "angle" ) ? ParseAngleDeg( aArgs["angle"], "angle" ) : 90.0;
                       int    turns = static_cast<int>( std::lround( angle / 90.0 ) );

                       if( std::fabs( angle - turns * 90.0 ) > 1e-6 )
                           throw TOOL_ERROR( "Schematic rotations are multiples of 90 degrees." );

                       for( int i = 0; i < std::abs( turns ) % 4; ++i )
                           aItem->Rotate( aCenter, turns > 0 );
                   } );

    transformTool( "sch_mirror",
                   "Mirror items about their centre: axis \"y\" flips left-right, \"x\" flips top-bottom.",
                   { { "axis", { { "type", "string" }, { "enum", { "x", "y" } } } } },
                   []( SCH_ITEM* aItem, const JSON& aArgs, const VECTOR2I& aCenter, const VECTOR2I& )
                   {
                       if( aArgs.value( "axis", "y" ) == "y" )
                           aItem->MirrorHorizontally( aCenter.x );
                       else
                           aItem->MirrorVertically( aCenter.y );
                   } );

    tools.Add( { "sch_delete", "Delete schematic items (symbols, wires, labels, junctions, sheets, text).", "sch",
                 schSchema( { { "items", ITEMS_ARG } }, { "items" } ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*         project = nullptr;
                     SCH_DOC_SESSION&       s = session( aServices, aArgs, &project );
                     std::vector<SCH_FOUND> items;
                     bool                   sheets = false;

                     for( const JSON& handle : aArgs["items"] )
                     {
                         for( SCH_FOUND found : SchFind( s.Schematic(), handle.get<std::string>() ) )
                         {
                             if( found.item->Type() == SCH_PIN_T )
                                 throw TOOL_ERROR( "Pins are deleted with their symbol; name the symbol." );

                             sheets |= found.item->Type() == SCH_SHEET_T;
                             items.push_back( found );
                         }
                     }

                     JSON result = SchMutate( *project, s, "sch_delete", aArgs, true,
                                              [&]( SCH_EDIT& aEdit ) -> JSON
                                              {
                                                  for( const SCH_FOUND& found : items )
                                                      aEdit.Remove( found.item, found.path.LastScreen() );

                                                  return JSON::object();
                                              } );

                     if( sheets )
                     {
                         s.HierarchyChanged();
                     }

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "sch_text_add", "Add a text note (not connected to anything) at a position.", "sch",
                 schSchema( { { "text", { { "type", "string" } } },
                              { "position", POINT },
                              { "size",
                                { { "type", { "number", "string" } },
                                  { "description", "Text height, default 1.27 mm." } } },
                              { "rotation", { { "type", { "number", "string" } } } },
                              { "sheet", SHEET_ARG } },
                            { "text", "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCH_SHEET_PATH   path = SchSheet( s, aArgs.value( "sheet", "" ) );
                     s.SetCurrentSheet( path );

                     JSON result =
                             SchMutate( *project, s, "sch_text_add", aArgs, false,
                                        [&]( SCH_EDIT& aEdit ) -> JSON
                                        {
                                            auto* text = new SCH_TEXT( SchPoint( aArgs["position"], "position", false ),
                                                                       wx( aArgs["text"] ) );

                                            if( aArgs.contains( "size" ) )
                                            {
                                                int size = SchIu( ParseLengthMm( aArgs["size"], "size" ), false );
                                                text->SetTextSize( VECTOR2I( size, size ) );
                                            }

                                            if( aArgs.contains( "rotation" ) )
                                                text->SetTextAngle( EDA_ANGLE(
                                                        ParseAngleDeg( aArgs["rotation"], "rotation" ), DEGREES_T ) );

                                            aEdit.Add( text, path.LastScreen() );
                                            return JSON::object();
                                        } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "sch_sheet_add",
              "Add a hierarchical sub-sheet: a sheet symbol on the parent sheet with a new sheet file (or an "
              "existing one, which then becomes another instance). Each pin {name, side: left|right|top|bottom, "
              "shape: input|output|bidirectional|passive} gets a hierarchical label inside a new sub-sheet.",
              "sch",
              schSchema( { { "name", { { "type", "string" } } },
                           { "sheetFile",
                             { { "type", "string" },
                               { "description", "Project-relative .kicad_sch for the sub-sheet." } } },
                           { "position", POINT },
                           { "size", POINT },
                           { "pins", { { "type", "array" }, { "items", { { "type", "object" } } } } },
                           { "sheet", SHEET_ARG } },
                         { "name", "sheetFile", "position" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT*    project = nullptr;
                  SCH_DOC_SESSION&  s = session( aServices, aArgs, &project );
                  SCHEMATIC&        schematic = s.Schematic();
                  SCH_SHEET_PATH    parent = SchSheet( s, aArgs.value( "sheet", "" ) );
                  const std::string file = project->Files().Relative( project->Files().Resolve( aArgs["sheetFile"] ) );

                  if( std::filesystem::path( file ).extension() != ".kicad_sch" )
                      throw TOOL_ERROR( "sheetFile must be a .kicad_sch file." );

                  if( file == s.Path() )
                      throw TOOL_ERROR( "A sub-sheet cannot use the root file." );

                  s.SetCurrentSheet( parent );

                  JSON result = SchMutate(
                          *project, s, "sch_sheet_add " + aArgs["name"].get<std::string>(), aArgs, true,
                          [&]( SCH_EDIT& aEdit ) -> JSON
                          {
                              const VECTOR2I pos = SchPoint( aArgs["position"], "position" );
                              VECTOR2I       size = aArgs.contains( "size" ) ? SchPoint( aArgs["size"], "size" )
                                                                             : VECTOR2I( SchIu( 25.4 ), SchIu( 20.32 ) );
                              auto*          sheet = new SCH_SHEET( parent.Last(), pos, size );
                              sheet->SetName( wx( aArgs["name"] ) );
                              sheet->SetFileName( wx( file ) );

                              // Reuse a screen already in the hierarchy (another instance), else a new one.
                              SCH_SCREEN* screen = nullptr;
                              SCH_SCREENS screens( schematic.Root() );

                              for( size_t i = 0; i < screens.GetCount() && !screen; i++ )
                              {
                                  if( s.FileOf( screens.GetScreen( i ) ) == file )
                                      screen = screens.GetScreen( i );
                              }

                              const bool newScreen = !screen;

                              if( newScreen )
                              {
                                  if( std::filesystem::exists( project->Files().Resolve( file ) ) )
                                      throw TOOL_ERROR( "'" + file
                                                        + "' exists but is not in this schematic; adding an "
                                                          "existing file is not supported yet." );

                                  screen = new SCH_SCREEN( &schematic );
                                  screen->SetFileName( wx( project->Files().Resolve( file ).string() ) );
                              }

                              sheet->SetScreen( screen );

                              int pinIndex = 0;

                              for( const JSON& pinSpec : aArgs.value( "pins", JSON::array() ) )
                              {
                                  const std::string side = pinSpec.value( "side", "left" );
                                  const std::string shape = pinSpec.value( "shape", "bidirectional" );
                                  const wxString    name = wx( pinSpec.at( "name" ).get<std::string>() );
                                  const int         offset = SchIu( 2.54 * ( pinIndex + 1 ) );
                                  VECTOR2I          at = pos;
                                  SHEET_SIDE        edge = SHEET_SIDE::LEFT;

                                  if( side == "right" )
                                  {
                                      at = pos + VECTOR2I( size.x, offset );
                                      edge = SHEET_SIDE::RIGHT;
                                  }
                                  else if( side == "top" )
                                  {
                                      at = pos + VECTOR2I( offset, 0 );
                                      edge = SHEET_SIDE::TOP;
                                  }
                                  else if( side == "bottom" )
                                  {
                                      at = pos + VECTOR2I( offset, size.y );
                                      edge = SHEET_SIDE::BOTTOM;
                                  }
                                  else
                                  {
                                      at = pos + VECTOR2I( 0, offset );
                                  }

                                  LABEL_FLAG_SHAPE flag = shape == "input"     ? L_INPUT
                                                          : shape == "output"  ? L_OUTPUT
                                                          : shape == "passive" ? L_UNSPECIFIED
                                                                               : L_BIDI;

                                  auto* pin = new SCH_SHEET_PIN( sheet, at, name );
                                  pin->SetSide( edge );
                                  pin->SetShape( flag );
                                  sheet->AddPin( pin );

                                  if( newScreen )
                                  {
                                      auto* label = new SCH_HIERLABEL(
                                              VECTOR2I( SchIu( 25.4 ), SchIu( 25.4 + 5.08 * pinIndex ) ), name );
                                      label->SetShape( flag );
                                      label->SetParent( screen );
                                      screen->Append( label );
                                  }

                                  pinIndex++;
                              }

                              aEdit.Add( sheet, parent.LastScreen() );

                              SCH_SHEET_PATH child = parent;
                              child.push_back( sheet );
                              child.SetPageNumber( wxString::Format( wxS( "%zu" ), schematic.Hierarchy().size() + 1 ) );

                              if( newScreen )
                                  s.MarkModified( file );

                              return JSON{ { "sheetPath", utf8( child.PathHumanReadable( false ) ) },
                                           { "file", file } };
                          } );

                  s.HierarchyChanged();
                  return TOOL_RESULT::Data( result );
              } } );

    // Wiring -----------------------------------------------------------------------------------

    auto lineTool = [&aServices]( const std::string& aName, int aLayer, const std::string& aDescription )
    {
        aServices.Tools().Add(
                { aName, aDescription, "sch",
                  schSchema( { { "points",
                                 { { "type", "array" },
                                   { "minItems", 2 },
                                   { "description",
                                     "Points {x,y} or pin handles (\"U1.3\"); each pair is one segment." } } },
                               { "sheet", SHEET_ARG } },
                             { "points" } ),
                  [&aServices, aName, aLayer]( const JSON& aArgs, TOOL_CONTEXT& )
                  {
                      AGENT_PROJECT*        project = nullptr;
                      SCH_DOC_SESSION&      s = session( aServices, aArgs, &project );
                      SCH_SHEET_PATH        path = SchSheet( s, aArgs.value( "sheet", "" ) );
                      std::vector<VECTOR2I> points;

                      for( size_t i = 0; i < aArgs["points"].size(); ++i )
                      {
                          SCH_SHEET_PATH pinPath = path;
                          points.push_back( pointOrPin( s.Schematic(), aArgs["points"][i],
                                                        "points[" + std::to_string( i ) + "]", &pinPath ) );
                          path = pinPath;
                      }

                      s.SetCurrentSheet( path );

                      JSON result = SchMutate( *project, s, aName, aArgs, true,
                                               [&]( SCH_EDIT& aEdit ) -> JSON
                                               {
                                                   for( size_t i = 1; i < points.size(); ++i )
                                                   {
                                                       if( points[i] == points[i - 1] )
                                                           continue;

                                                       auto* line = new SCH_LINE( points[i - 1], aLayer );
                                                       line->SetEndPoint( points[i] );
                                                       aEdit.Add( line, path.LastScreen() );
                                                   }

                                                   return JSON::object();
                                               } );

                      return TOOL_RESULT::Data( result );
                  } } );
    };

    lineTool( "sch_wire_add", LAYER_WIRE,
              "Draw a wire through points (mm, snapped) or pins. A wire ending on a pin or another wire connects; "
              "KiCad adds junctions where three or more meet. Returns the connectivity delta." );
    lineTool( "sch_bus_add", LAYER_BUS, "Draw a bus through points (mm, snapped)." );

    tools.Add( { "sch_connect",
                 "Connect two pins (REF.PIN) with a Manhattan wire path that leaves each pin outward and avoids symbol "
                 "bodies. Returns the wires made and the connectivity delta.",
                 "sch",
                 schSchema( { { "from", { { "type", "string" } } }, { "to", { { "type", "string" } } } },
                            { "from", "to" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCH_FOUND        from = pinOf( s.Schematic(), aArgs["from"] );
                     SCH_FOUND        to = pinOf( s.Schematic(), aArgs["to"] );

                     if( !( from.path == to.path ) )
                         throw TOOL_ERROR(
                                 "The pins are on different sheets; connect them with hierarchical or global labels." );

                     std::vector<BOX2I> bodies;

                     for( SCH_ITEM* item : from.path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                     {
                         SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                         if( !symbol->IsPower() )
                         {
                             BOX2I body = symbol->GetBodyBoundingBox();
                             body.Inflate( -schIUScale.mmToIU( 0.1 ) );
                             bodies.push_back( body );
                         }
                     }

                     const SCH_PIN*        a = static_cast<const SCH_PIN*>( from.item );
                     const SCH_PIN*        b = static_cast<const SCH_PIN*>( to.item );
                     std::vector<VECTOR2I> path =
                             route( a->GetPosition(), outward( a ), b->GetPosition(), outward( b ), bodies );
                     JSON warning;

                     if( path.empty() )
                     {
                         path = { a->GetPosition(), VECTOR2I( b->GetPosition().x, a->GetPosition().y ),
                                  b->GetPosition() };
                         warning = "No path clear of every symbol body was found; the wire crosses a body. Move parts "
                                   "apart or route with sch_wire_add.";
                     }

                     s.SetCurrentSheet( from.path );

                     JSON result = SchMutate( *project, s,
                                              "sch_connect " + aArgs["from"].get<std::string>() + " "
                                                      + aArgs["to"].get<std::string>(),
                                              aArgs, true,
                                              [&]( SCH_EDIT& aEdit ) -> JSON
                                              {
                                                  for( size_t i = 1; i < path.size(); ++i )
                                                  {
                                                      auto* line = new SCH_LINE( path[i - 1], LAYER_WIRE );
                                                      line->SetEndPoint( path[i] );
                                                      aEdit.Add( line, from.path.LastScreen() );
                                                  }

                                                  JSON points = JSON::array();

                                                  for( const VECTOR2I& p : path )
                                                      points.push_back( SchPointJson( p ) );

                                                  JSON out{ { "path", points } };

                                                  if( !warning.is_null() )
                                                      out["warning"] = warning;

                                                  return out;
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "sch_label_add",
              "Add a net label: kind local (this sheet), global (all sheets), hierarchical (to the parent sheet "
              "pin) or power (places the power symbol named text, e.g. GND or +3V3, from a library with power "
              "symbols). at is a position or a pin (\"R1.1\"): on a pin the label sits on it and connects.",
              "sch",
              schSchema( { { "text", { { "type", "string" } } },
                           { "kind",
                             { { "type", "string" }, { "enum", { "local", "global", "hierarchical", "power" } } } },
                           { "at", { { "type", { "object", "array", "string" } } } },
                           { "rotation", { { "type", { "number", "string" } } } },
                           { "shape",
                             { { "type", "string" },
                               { "enum", { "input", "output", "bidirectional", "tristate", "passive" } } } },
                           { "libId",
                             { { "type", "string" }, { "description", "Power symbol id; default found by name." } } },
                           { "sheet", SHEET_ARG } },
                         { "text", "at" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  AGENT_PROJECT*    project = nullptr;
                  SCH_DOC_SESSION&  s = session( aServices, aArgs, &project );
                  SCH_SHEET_PATH    path = SchSheet( s, aArgs.value( "sheet", "" ) );
                  const VECTOR2I    at = pointOrPin( s.Schematic(), aArgs["at"], "at", &path );
                  const std::string kind = aArgs.value( "kind", "local" );
                  const std::string text = aArgs["text"];
                  s.SetCurrentSheet( path );

                  LIB_SYMBOL* power = nullptr;
                  std::string powerId = aArgs.value( "libId", "" );

                  if( kind == "power" )
                  {
                      if( powerId.empty() )
                      {
                          JSON  errors = JSON::array();
                          auto& adapter = static_cast<SYMBOL_LIBRARY_ADAPTER&>(
                                  LoadLibraries( *project, LIBRARY_TABLE_TYPE::SYMBOL, aContext, errors ) );

                          for( const wxString& library : adapter.GetLibraryNames() )
                          {
                              for( LIB_SYMBOL* candidate :
                                   adapter.GetSymbols( library, SYMBOL_LIBRARY_ADAPTER::SYMBOL_TYPE::POWER_ONLY ) )
                              {
                                  if( utf8( candidate->GetName() ) == text && powerId.empty() )
                                      powerId = utf8( library + wxS( ":" ) + candidate->GetName() );
                              }
                          }

                          if( powerId.empty() )
                              throw TOOL_ERROR( "No power symbol named '" + text
                                                + "' in the project's libraries. Add a "
                                                  "library with power symbols (lib_add) or give libId." );
                      }

                      power = loadLibSymbol( *project, powerId, aContext );
                  }

                  JSON result = SchMutate(
                          *project, s, "sch_label_add " + text, aArgs, true,
                          [&]( SCH_EDIT& aEdit ) -> JSON
                          {
                              if( power )
                              {
                                  JSON args = aArgs;
                                  args.erase( "reference" );
                                  SCH_SYMBOL* placed = placeSymbol( aEdit, *power, powerId, path, at, 1, 0, args );
                                  return JSON{ { "reference", utf8( placed->GetRef( &path, false ) ) } };
                              }

                              SCH_TEXT* label = makeLabel( kind, at, wx( text ) );

                              if( aArgs.contains( "rotation" ) )
                                  static_cast<SCH_LABEL_BASE*>( label )->SetSpinStyle(
                                          spin( ParseAngleDeg( aArgs["rotation"], "rotation" ) ) );

                              if( aArgs.contains( "shape" ) && kind != "local" )
                              {
                                  const std::string shape = aArgs["shape"];
                                  static_cast<SCH_LABEL_BASE*>( label )->SetShape( shape == "input"      ? L_INPUT
                                                                                   : shape == "output"   ? L_OUTPUT
                                                                                   : shape == "tristate" ? L_TRISTATE
                                                                                   : shape == "passive"  ? L_UNSPECIFIED
                                                                                                         : L_BIDI );
                              }

                              aEdit.Add( label, path.LastScreen() );
                              return JSON::object();
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "sch_no_connect_add", "Mark pins as intentionally unconnected (a no-connect flag on each).", "sch",
                 schSchema( { { "at",
                                { { "type", "array" },
                                  { "items", { { "type", { "object", "array", "string" } } } },
                                  { "description", "Pins (\"U1.5\") or positions." } } } },
                            { "at" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*                                   project = nullptr;
                     SCH_DOC_SESSION&                                 s = session( aServices, aArgs, &project );
                     std::vector<std::pair<VECTOR2I, SCH_SHEET_PATH>> places;

                     for( const JSON& value : aArgs["at"] )
                     {
                         SCH_SHEET_PATH path = s.CurrentSheet();
                         VECTOR2I       pos = pointOrPin( s.Schematic(), value, "at", &path );
                         places.emplace_back( pos, path );
                     }

                     if( !places.empty() )
                         s.SetCurrentSheet( places.front().second );

                     JSON result = SchMutate( *project, s, "sch_no_connect_add", aArgs, true,
                                              [&]( SCH_EDIT& aEdit ) -> JSON
                                              {
                                                  for( const auto& [pos, path] : places )
                                                      aEdit.Add( new SCH_NO_CONNECT( pos ), path.LastScreen() );

                                                  return JSON::object();
                                              } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "sch_junction_add",
                 "Add a junction dot where wires cross and should connect (KiCad adds them itself where three or "
                 "more wire ends meet).",
                 "sch", schSchema( { { "position", POINT }, { "sheet", SHEET_ARG } }, { "position" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT*   project = nullptr;
                     SCH_DOC_SESSION& s = session( aServices, aArgs, &project );
                     SCH_SHEET_PATH   path = SchSheet( s, aArgs.value( "sheet", "" ) );
                     s.SetCurrentSheet( path );

                     JSON result =
                             SchMutate( *project, s, "sch_junction_add", aArgs, true,
                                        [&]( SCH_EDIT& aEdit ) -> JSON
                                        {
                                            aEdit.Add( new SCH_JUNCTION( SchPoint( aArgs["position"], "position" ) ),
                                                       path.LastScreen() );
                                            return JSON::object();
                                        } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
