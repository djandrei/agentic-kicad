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


#include <agentic/core/handles.h>
#include <agentic/protocol/registry.h>
#include <agentic/sch/sch_doc_session.h>
#include <agentic/sch/sch_items.h>

#include <base_units.h>
#include <connectivity/conn_facade.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <schematic.h>

#include <cmath>


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
        return std::round( schIUScale.IUTomm( aIu ) * 1e4 ) / 1e4;
    }


    const char* typeName( const SCH_ITEM* aItem )
    {
        switch( aItem->Type() )
        {
        case SCH_SYMBOL_T: return "symbol";
        case SCH_LINE_T:
            return aItem->GetLayer() == LAYER_WIRE ? "wire" : aItem->GetLayer() == LAYER_BUS ? "bus" : "line";
        case SCH_LABEL_T: return "label";
        case SCH_GLOBAL_LABEL_T: return "global_label";
        case SCH_HIER_LABEL_T: return "hierarchical_label";
        case SCH_DIRECTIVE_LABEL_T: return "directive_label";
        case SCH_JUNCTION_T: return "junction";
        case SCH_NO_CONNECT_T: return "no_connect";
        case SCH_TEXT_T: return "text";
        case SCH_TEXTBOX_T: return "textbox";
        case SCH_SHEET_T: return "sheet";
        case SCH_SHEET_PIN_T: return "sheet_pin";
        case SCH_BUS_WIRE_ENTRY_T: return "bus_entry";
        case SCH_SHAPE_T: return "shape";
        case SCH_PIN_T: return "pin";
        default: return "item";
        }
    }

} // namespace


int SchIu( double aMm, bool aSnap )
{
    return schIUScale.mmToIU( aSnap ? SnapMm( aMm, SCH_GRID_MM ) : aMm );
}


VECTOR2I SchPoint( const nlohmann::json& aPoint, const std::string& aField, bool aSnap )
{
    if( aPoint.is_array() && aPoint.size() == 2 )
        return VECTOR2I( SchIu( ParseLengthMm( aPoint[0], aField + ".x" ), aSnap ),
                         SchIu( ParseLengthMm( aPoint[1], aField + ".y" ), aSnap ) );

    if( !aPoint.is_object() || !aPoint.contains( "x" ) || !aPoint.contains( "y" ) )
        throw TOOL_ERROR( "'" + aField + "' must be a point {x, y} in mm (or [x, y])." );

    return VECTOR2I( SchIu( ParseLengthMm( aPoint["x"], aField + ".x" ), aSnap ),
                     SchIu( ParseLengthMm( aPoint["y"], aField + ".y" ), aSnap ) );
}


nlohmann::json SchPointJson( const VECTOR2I& aPoint )
{
    return { { "x", mm( aPoint.x ) }, { "y", mm( aPoint.y ) } };
}


std::string SchPinId( const SCH_PIN* aPin, const SCH_SHEET_PATH& aPath )
{
    const SCH_SYMBOL* symbol = dynamic_cast<const SCH_SYMBOL*>( aPin->GetParentSymbol() );

    if( !symbol )
        return utf8( aPin->GetNumber() );

    return utf8( symbol->GetRef( &aPath, false ) + wxS( "." ) + aPin->GetNumber() );
}


nlohmann::json SchItemJson( const SCH_ITEM* aItem, const SCH_SHEET_PATH& aPath )
{
    nlohmann::json out{ { "kiid", utf8( aItem->m_Uuid.AsString() ) },
                        { "type", typeName( aItem ) },
                        { "sheet", utf8( aPath.PathHumanReadable() ) } };

    switch( aItem->Type() )
    {
    case SCH_SYMBOL_T:
    {
        const SCH_SYMBOL* symbol = static_cast<const SCH_SYMBOL*>( aItem );
        out["reference"] = utf8( symbol->GetRef( &aPath, false ) );
        out["value"] = utf8( symbol->GetValue( &aPath, FOR_GUI ) );
        out["libId"] = utf8( symbol->GetLibId().Format() );
        out["footprint"] = utf8( symbol->GetFootprintFieldText( &aPath, FOR_GUI ) );
        out["position"] = SchPointJson( symbol->GetPosition() );
        out["unit"] = symbol->GetUnit();

        // The unit's own name or letter (U1A, U1Logic), so a reference names all of its units.
        if( symbol->GetUnitCount() > 1 )
            out["unitName"] = utf8( symbol->SubReference( symbol->GetUnit(), false ) );
        out["rotation"] = static_cast<int>( symbol->GetOrientationProp() ) == SYMBOL_ANGLE_90    ? 90
                          : static_cast<int>( symbol->GetOrientationProp() ) == SYMBOL_ANGLE_180 ? 180
                          : static_cast<int>( symbol->GetOrientationProp() ) == SYMBOL_ANGLE_270 ? 270
                                                                                                 : 0;
        out["mirrorX"] = symbol->GetMirrorX();
        out["mirrorY"] = symbol->GetMirrorY();

        if( symbol->IsPower() )
            out["power"] = true;

        if( symbol->GetDNP( &aPath ) )
            out["dnp"] = true;

        break;
    }

    case SCH_LINE_T:
    {
        const SCH_LINE* line = static_cast<const SCH_LINE*>( aItem );
        out["start"] = SchPointJson( line->GetStartPoint() );
        out["end"] = SchPointJson( line->GetEndPoint() );
        break;
    }

    case SCH_LABEL_T:
    case SCH_GLOBAL_LABEL_T:
    case SCH_HIER_LABEL_T:
    case SCH_DIRECTIVE_LABEL_T:
    case SCH_TEXT_T:
    {
        const SCH_TEXT* text = static_cast<const SCH_TEXT*>( aItem );
        out["text"] = utf8( text->GetText() );
        out["position"] = SchPointJson( text->GetPosition() );
        break;
    }

    case SCH_SHEET_T:
    {
        const SCH_SHEET* sheet = static_cast<const SCH_SHEET*>( aItem );
        out["name"] = utf8( sheet->GetName() );
        out["file"] = utf8( sheet->GetFileName() );
        out["position"] = SchPointJson( sheet->GetPosition() );
        out["size"] = SchPointJson( sheet->GetSize() );

        nlohmann::json pins = nlohmann::json::array();

        for( const SCH_SHEET_PIN* pin : sheet->GetPins() )
            pins.push_back(
                    { { "name", utf8( pin->GetText() ) }, { "position", SchPointJson( pin->GetPosition() ) } } );

        out["pins"] = pins;
        break;
    }

    default: out["position"] = SchPointJson( aItem->GetPosition() ); break;
    }

    return out;
}


std::vector<SCH_FOUND> SchFind( SCHEMATIC& aSchematic, const std::string& aHandle )
{
    const HANDLE           handle = ParseHandle( aHandle );
    std::vector<SCH_FOUND> found;

    for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        for( SCH_ITEM* item : screen->Items() )
        {
            switch( handle.kind )
            {
            case HANDLE::KIND::KIID:
                if( utf8( item->m_Uuid.AsString() ) == handle.text )
                    found.push_back( { item, path } );

                // Sheet pins are children of their sheet.
                if( item->Type() == SCH_SHEET_T )
                {
                    for( SCH_SHEET_PIN* pin : static_cast<SCH_SHEET*>( item )->GetPins() )
                    {
                        if( utf8( pin->m_Uuid.AsString() ) == handle.text )
                            found.push_back( { pin, path } );
                    }
                }

                break;

            case HANDLE::KIND::REFERENCE:
            case HANDLE::KIND::PIN:
                if( item->Type() == SCH_SYMBOL_T )
                {
                    SCH_SYMBOL*       symbol = static_cast<SCH_SYMBOL*>( item );
                    const std::string ref = utf8( symbol->GetRef( &path, false ) );
                    const std::string wanted = handle.kind == HANDLE::KIND::PIN ? handle.reference : handle.text;

                    if( ref != wanted && utf8( symbol->GetRef( &path, true ) ) != wanted )
                        break;

                    if( handle.kind == HANDLE::KIND::REFERENCE )
                    {
                        found.push_back( { item, path } );
                        break;
                    }

                    for( SCH_PIN* pin : symbol->GetPins( &path ) )
                    {
                        if( utf8( pin->GetNumber() ) == handle.pin )
                            found.push_back( { pin, path } );
                    }
                }

                break;

            default: break;
            }
        }

        // A KIID is unique; the other handles may name items on several sheets.
        if( handle.kind == HANDLE::KIND::KIID && !found.empty() )
            break;
    }

    if( found.empty() )
    {
        throw TOOL_ERROR( "Nothing in the schematic matches '" + aHandle
                          + "'. Use sch_query to list items (handles: KIID, reference R5, pin U1.3)." );
    }

    return found;
}


SCH_FOUND SchFindOne( SCHEMATIC& aSchematic, const std::string& aHandle )
{
    std::vector<SCH_FOUND> found = SchFind( aSchematic, aHandle );

    if( found.size() > 1 )
    {
        throw TOOL_ERROR( "'" + aHandle + "' names " + std::to_string( found.size() )
                          + " items (units or sheet instances); use the KIID of the one you mean." );
    }

    return found.front();
}


SCH_SHEET_PATH SchSheet( SCH_DOC_SESSION& aSession, const std::string& aSheet )
{
    SCHEMATIC& schematic = aSession.Schematic();

    if( aSheet.empty() )
        return aSession.CurrentSheet();

    for( const SCH_SHEET_PATH& path : schematic.Hierarchy() )
    {
        // The documented form ("/", "/power/") and the one with the root file name ("board/power/").
        for( const std::string& human : { utf8( path.PathHumanReadable() ), utf8( path.PathHumanReadable( false ) ) } )
        {
            if( human == aSheet || human + "/" == aSheet )
                return path;
        }

        if( utf8( path.Last()->m_Uuid.AsString() ) == aSheet )
            return path;
    }

    throw TOOL_ERROR( "No sheet '" + aSheet + "'. sch_hierarchy lists the sheets (\"/\" is the root)." );
}


SCH_NET_SNAPSHOT SCH_NET_SNAPSHOT::Take( SCHEMATIC& aSchematic )
{
    SCH_NET_SNAPSHOT      snapshot;
    SCH_SHEET_LIST        hierarchy = aSchematic.Hierarchy();
    std::set<std::string> noConnected;

    // Bring the published connectivity up to date first: rows of items changed since the
    // last update (a save rewrites instance data) are stale and read as unconnected.
    aSchematic.Connectivity().Update( aSchematic );

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_NO_CONNECT_T ) )
        {
            for( SCH_ITEM* symbolItem : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            {
                for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( symbolItem )->GetPins( &path ) )
                {
                    if( pin->GetPosition() == item->GetPosition() )
                        noConnected.insert( SchPinId( pin, path ) );
                }
            }
        }
    }

    std::set<std::string> netsWithOthers; // nets holding more than their pins (wires, labels, power)

    for( const SCH_CONNECTIVITY::NET_GROUP& net : aSchematic.Connectivity().GetNetMap() )
    {
        const std::string      name = utf8( net.name );
        std::set<std::string>& pins = snapshot.nets[name];

        for( const SCH_CONNECTIVITY::NET_VIEW& instance : net.instances )
        {
            std::optional<SCH_SHEET_PATH> path = hierarchy.GetSheetPathByKIIDPath( instance.Instance() );

            for( SCH_ITEM* item : instance.Items() )
            {
                if( item->Type() != SCH_PIN_T )
                {
                    if( item->Type() != SCH_NO_CONNECT_T )
                        netsWithOthers.insert( name );

                    continue;
                }

                if( path )
                {
                    const SCH_PIN*    pin = static_cast<const SCH_PIN*>( item );
                    const SCH_SYMBOL* symbol = dynamic_cast<const SCH_SYMBOL*>( pin->GetParentSymbol() );

                    // Power symbols' own pins name a net; they are not parts to connect.
                    if( symbol && symbol->IsPower() )
                    {
                        netsWithOthers.insert( name );
                        continue;
                    }

                    pins.insert( SchPinId( pin, *path ) );
                }
            }
        }
    }

    for( auto it = snapshot.nets.begin(); it != snapshot.nets.end(); )
    {
        if( it->second.size() == 1 && !netsWithOthers.count( it->first ) && !noConnected.count( *it->second.begin() ) )
            snapshot.unconnectedPins.insert( *it->second.begin() );

        if( it->second.empty() )
            it = snapshot.nets.erase( it );
        else
            ++it;
    }

    // Pins of symbols the engine did not put on any net (no connection at all).
    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            for( SCH_PIN* pin : symbol->GetPins( &path ) )
            {
                const std::string id = SchPinId( pin, path );
                bool              onNet = false;

                for( const auto& [name, pins] : snapshot.nets )
                    onNet |= pins.count( id ) > 0;

                if( !onNet && !noConnected.count( id ) )
                    snapshot.unconnectedPins.insert( id );
            }
        }
    }

    return snapshot;
}


nlohmann::json SchNetDelta( const SCH_NET_SNAPSHOT& aBefore, const SCH_NET_SNAPSHOT& aAfter )
{
    auto netOf = []( const SCH_NET_SNAPSHOT& aSnapshot, const std::string& aPin ) -> std::string
    {
        for( const auto& [name, pins] : aSnapshot.nets )
        {
            if( pins.count( aPin ) )
                return name;
        }

        return std::string();
    };

    nlohmann::json created = nlohmann::json::array();
    nlohmann::json removed = nlohmann::json::array();
    nlohmann::json merged = nlohmann::json::array();
    nlohmann::json split = nlohmann::json::array();

    // A net counts only once it has two pins: a lone pin is not a connection.
    for( const auto& [name, pins] : aAfter.nets )
    {
        if( pins.size() < 2 )
            continue;

        std::set<std::string> sources;

        for( const std::string& pin : pins )
        {
            const std::string before = netOf( aBefore, pin );

            if( !before.empty() && aBefore.nets.at( before ).size() > 1 )
                sources.insert( before );
        }

        if( sources.size() > 1 )
            merged.push_back( { { "net", name }, { "from", sources }, { "pins", pins } } );
        else if( !aBefore.nets.count( name ) || aBefore.nets.at( name ).size() < 2 )
            created.push_back( { { "net", name }, { "pins", pins } } );
    }

    for( const auto& [name, pins] : aBefore.nets )
    {
        if( pins.size() < 2 )
            continue;

        std::set<std::string> targets;

        for( const std::string& pin : pins )
        {
            const std::string after = netOf( aAfter, pin );

            if( !after.empty() && aAfter.nets.at( after ).size() > 1 )
                targets.insert( after );
        }

        if( targets.size() > 1 )
            split.push_back( { { "net", name }, { "into", targets } } );
        else if( targets.empty() )
            removed.push_back( name );
    }

    nlohmann::json delta = nlohmann::json::object();

    if( !created.empty() )
        delta["created"] = created;

    if( !removed.empty() )
        delta["removed"] = removed;

    if( !merged.empty() )
        delta["merged"] = merged;

    if( !split.empty() )
        delta["split"] = split;

    std::vector<std::string> unconnected( aAfter.unconnectedPins.begin(), aAfter.unconnectedPins.end() );

    if( unconnected.size() > 50 )
        unconnected.resize( 50 );

    delta["unconnectedPins"] = unconnected;
    delta["unconnectedCount"] = aAfter.unconnectedPins.size();
    return delta;
}

} // namespace AGENTIC
