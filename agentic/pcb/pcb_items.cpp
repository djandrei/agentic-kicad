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
#include <agentic/pcb/pcb_items.h>
#include <agentic/protocol/registry.h>

#include <base_units.h>
#include <board.h>
#include <connectivity/connectivity_data.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <ratsnest/ratsnest_data.h>
#include <zone.h>

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
        return std::round( pcbIUScale.IUTomm( aIu ) * 1e4 ) / 1e4;
    }

} // namespace


int PcbIu( double aMm )
{
    return pcbIUScale.mmToIU( aMm );
}


int PcbLength( const nlohmann::json& aValue, const std::string& aField )
{
    return PcbIu( ParseLengthMm( aValue, aField ) );
}


VECTOR2I PcbPoint( BOARD& aBoard, const nlohmann::json& aValue, const std::string& aField )
{
    if( aValue.is_string() )
        return PcbPad( aBoard, aValue.get<std::string>() )->GetPosition();

    if( aValue.is_array() && aValue.size() == 2 )
        return VECTOR2I( PcbLength( aValue[0], aField + ".x" ), PcbLength( aValue[1], aField + ".y" ) );

    if( !aValue.is_object() || !aValue.contains( "x" ) || !aValue.contains( "y" ) )
        throw TOOL_ERROR( "'" + aField + "' must be a point {x, y} in mm, or a pad (\"U1.3\")." );

    return VECTOR2I( PcbLength( aValue["x"], aField + ".x" ), PcbLength( aValue["y"], aField + ".y" ) );
}


nlohmann::json PcbPointJson( const VECTOR2I& aPoint )
{
    return { { "x", mm( aPoint.x ) }, { "y", mm( aPoint.y ) } };
}


PCB_LAYER_ID PcbLayer( BOARD& aBoard, const std::string& aName )
{
    std::string name = aName;

    if( name.rfind( "layer:", 0 ) == 0 )
        name = name.substr( 6 );

    PCB_LAYER_ID layer = aBoard.GetLayerID( wxString::FromUTF8( name ) );

    if( layer == UNDEFINED_LAYER )
    {
        // Canonical names work whatever the board calls its layers.
        for( PCB_LAYER_ID candidate : LSET::AllLayersMask().Seq() )
        {
            if( utf8( LSET::Name( candidate ) ) == name )
                layer = candidate;
        }
    }

    if( layer == UNDEFINED_LAYER || !aBoard.IsLayerEnabled( layer ) )
        throw TOOL_ERROR( "No layer '" + aName + "' on this board (e.g. F.Cu, B.Cu, F.SilkS, Edge.Cuts)." );

    return layer;
}


std::string PcbLayerName( BOARD& aBoard, PCB_LAYER_ID aLayer )
{
    return utf8( aBoard.GetLayerName( aLayer ) );
}


NETINFO_ITEM* PcbNet( BOARD& aBoard, const std::string& aName )
{
    if( aName.empty() )
        return nullptr;

    std::string name = aName.rfind( "net:", 0 ) == 0 ? aName.substr( 4 ) : aName;

    if( NETINFO_ITEM* net = aBoard.FindNet( wxString::FromUTF8( name ) ) )
        return net;

    throw TOOL_ERROR( "No net '" + aName + "' on the board. pcb_query type net lists them." );
}


FOOTPRINT* PcbFootprint( BOARD& aBoard, const std::string& aReference )
{
    FOOTPRINT* footprint = aBoard.FindFootprintByReference( wxString::FromUTF8( aReference ) );

    if( !footprint )
        throw TOOL_ERROR( "No footprint '" + aReference + "' on the board." );

    return footprint;
}


PAD* PcbPad( BOARD& aBoard, const std::string& aHandle )
{
    HANDLE handle = ParseHandle( aHandle );

    if( handle.kind != HANDLE::KIND::PIN )
        throw TOOL_ERROR( "'" + aHandle + "' is not a pad; give REF.PAD, e.g. U1.3." );

    FOOTPRINT* footprint = PcbFootprint( aBoard, handle.reference );
    PAD*       pad = footprint->FindPadByNumber( wxString::FromUTF8( handle.pin ) );

    if( !pad )
        throw TOOL_ERROR( "Footprint " + handle.reference + " has no pad " + handle.pin + "." );

    return pad;
}


nlohmann::json PcbItemJson( BOARD& aBoard, const BOARD_ITEM* aItem )
{
    nlohmann::json out{ { "kiid", utf8( aItem->m_Uuid.AsString() ) },
                        { "type", utf8( aItem->GetClass() ) },
                        { "layer", PcbLayerName( aBoard, aItem->GetLayer() ) } };

    if( const BOARD_CONNECTED_ITEM* connected = dynamic_cast<const BOARD_CONNECTED_ITEM*>( aItem ) )
        out["net"] = utf8( connected->GetNetname() );

    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
    {
        const FOOTPRINT* fp = static_cast<const FOOTPRINT*>( aItem );
        out["type"] = "footprint";
        out["reference"] = utf8( fp->GetReference() );
        out["value"] = utf8( fp->GetValue() );
        out["footprint"] = utf8( fp->GetFPID().Format() );
        out["position"] = PcbPointJson( fp->GetPosition() );
        out["rotation"] = fp->GetOrientation().AsDegrees();
        out["side"] = fp->IsFlipped() ? "back" : "front";
        out["pads"] = fp->Pads().size();
        out["locked"] = fp->IsLocked();
        break;
    }

    case PCB_TRACE_T:
    case PCB_ARC_T:
    {
        const PCB_TRACK* track = static_cast<const PCB_TRACK*>( aItem );
        out["type"] = aItem->Type() == PCB_ARC_T ? "arc" : "track";
        out["start"] = PcbPointJson( track->GetStart() );
        out["end"] = PcbPointJson( track->GetEnd() );
        out["width"] = mm( track->GetWidth() );
        break;
    }

    case PCB_VIA_T:
    {
        const PCB_VIA* via = static_cast<const PCB_VIA*>( aItem );
        out["type"] = "via";
        out["position"] = PcbPointJson( via->GetPosition() );
        out["diameter"] = mm( via->GetWidth( PADSTACK::ALL_LAYERS ) );
        out["drill"] = mm( via->GetDrillValue() );
        break;
    }

    case PCB_PAD_T:
    {
        const PAD* pad = static_cast<const PAD*>( aItem );
        out["type"] = "pad";
        out["pad"] = utf8( pad->GetParentFootprint()->GetReference() + wxS( "." ) + pad->GetNumber() );
        out["position"] = PcbPointJson( pad->GetPosition() );
        break;
    }

    case PCB_ZONE_T:
    {
        const ZONE* zone = static_cast<const ZONE*>( aItem );
        out["type"] = "zone";
        out["filled"] = zone->IsFilled();
        out["priority"] = zone->GetAssignedPriority();
        nlohmann::json layers = nlohmann::json::array();

        for( PCB_LAYER_ID layer : zone->GetLayerSet().Seq() )
            layers.push_back( PcbLayerName( aBoard, layer ) );

        out["layers"] = layers;
        BOX2I box = zone->GetBoundingBox();
        out["bbox"] = { { "x1", mm( box.GetLeft() ) },
                        { "y1", mm( box.GetTop() ) },
                        { "x2", mm( box.GetRight() ) },
                        { "y2", mm( box.GetBottom() ) } };
        break;
    }

    case PCB_SHAPE_T:
    {
        const PCB_SHAPE* shape = static_cast<const PCB_SHAPE*>( aItem );
        out["type"] = "graphic";
        out["shape"] = utf8( shape->ShowShape() );
        out["start"] = PcbPointJson( shape->GetStart() );
        out["end"] = PcbPointJson( shape->GetEnd() );
        break;
    }

    case PCB_TEXT_T:
    {
        const PCB_TEXT* text = static_cast<const PCB_TEXT*>( aItem );
        out["type"] = "text";
        out["text"] = utf8( text->GetText() );
        out["position"] = PcbPointJson( text->GetPosition() );
        break;
    }

    default: out["position"] = PcbPointJson( aItem->GetPosition() ); break;
    }

    return out;
}


std::vector<BOARD_ITEM*> PcbFind( BOARD& aBoard, const std::string& aHandle )
{
    HANDLE                   handle = ParseHandle( aHandle );
    std::vector<BOARD_ITEM*> found;

    switch( handle.kind )
    {
    case HANDLE::KIND::KIID:
        if( BOARD_ITEM* item = aBoard.ResolveItem( KIID( wxString::FromUTF8( handle.text ) ), true ) )
            found.push_back( item );

        break;

    case HANDLE::KIND::REFERENCE: found.push_back( PcbFootprint( aBoard, handle.text ) ); break;

    case HANDLE::KIND::PIN: found.push_back( PcbPad( aBoard, aHandle ) ); break;

    default: break;
    }

    if( found.empty() )
        throw TOOL_ERROR( "Nothing on the board matches '" + aHandle
                          + "'. Use pcb_query (handles: KIID, reference "
                            "U1, pad U1.3)." );

    return found;
}


BOARD_ITEM* PcbFindOne( BOARD& aBoard, const std::string& aHandle )
{
    return PcbFind( aBoard, aHandle ).front();
}


PCB_RATSNEST PCB_RATSNEST::Take( BOARD& aBoard )
{
    PCB_RATSNEST                       ratsnest;
    std::shared_ptr<CONNECTIVITY_DATA> connectivity = aBoard.GetConnectivity();
    connectivity->RecalculateRatsnest();

    for( NETINFO_ITEM* net : aBoard.GetNetInfo() )
    {
        if( net->GetNetCode() <= 0 )
            continue;

        RN_NET* rn = connectivity->GetRatsnestForNet( net->GetNetCode() );

        if( !rn )
            continue;

        const int edges = static_cast<int>( rn->GetEdges().size() );

        if( edges > 0 )
            ratsnest.unrouted[utf8( net->GetNetname() )] = edges;

        ratsnest.total += edges;
    }

    return ratsnest;
}


nlohmann::json PcbRatsnestDelta( const PCB_RATSNEST& aBefore, const PCB_RATSNEST& aAfter )
{
    nlohmann::json        nets = nlohmann::json::object();
    std::set<std::string> names;

    for( const auto& [name, count] : aBefore.unrouted )
        names.insert( name );

    for( const auto& [name, count] : aAfter.unrouted )
        names.insert( name );

    for( const std::string& name : names )
    {
        auto before = aBefore.unrouted.find( name );
        auto after = aAfter.unrouted.find( name );
        int  b = before == aBefore.unrouted.end() ? 0 : before->second;
        int  a = after == aAfter.unrouted.end() ? 0 : after->second;

        if( a != b )
            nets[name] = { { "before", b }, { "after", a } };
    }

    return nlohmann::json{ { "unroutedBefore", aBefore.total }, { "unroutedAfter", aAfter.total }, { "nets", nets } };
}

} // namespace AGENTIC
