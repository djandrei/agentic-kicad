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


#include <agentic/pcb/pns_agent_iface.h>

#include <board.h>
#include <board_commit.h>
#include <netinfo.h>
#include <pcb_track.h>
#include <router/pns_arc.h>
#include <router/pns_segment.h>
#include <router/pns_via.h>


namespace AGENTIC
{

PNS_AGENT_IFACE::PNS_AGENT_IFACE( TOOL_MANAGER* aToolManager ) :
        m_toolManager( aToolManager )
{
}


PNS_AGENT_IFACE::~PNS_AGENT_IFACE() = default;


BOARD_COMMIT& PNS_AGENT_IFACE::commit()
{
    if( !m_commit )
        m_commit = std::make_unique<BOARD_COMMIT>( m_toolManager );

    return *m_commit;
}


BOARD_CONNECTED_ITEM* PNS_AGENT_IFACE::createBoardItem( PNS::ITEM* aItem )
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
        pcbVia->SetViaType( via->ViaType() ); // must be before SetLayerPair()
        pcbVia->SetLayerPair( GetBoardLayerFromPNSLayer( via->Layers().Start() ),
                              GetBoardLayerFromPNSLayer( via->Layers().End() ) );
        return pcbVia;
    }

    default: return nullptr;
    }
}


void PNS_AGENT_IFACE::AddItem( PNS::ITEM* aItem )
{
    BOARD_CONNECTED_ITEM* item = createBoardItem( aItem );

    if( !item )
        return;

    aItem->SetParent( item );
    commit().Add( item );
    m_addedItems.push_back( item->m_Uuid );
    m_added++;
}


void PNS_AGENT_IFACE::RemoveItem( PNS::ITEM* aItem )
{
    // Pads the router would move are not moved headless; tracks and vias only.
    if( aItem->Parent() && !aItem->OfKind( PNS::ITEM::SOLID_T ) )
    {
        commit().Remove( aItem->Parent() );
        m_removed++;
    }
}


void PNS_AGENT_IFACE::UpdateItem( PNS::ITEM* aItem )
{
    BOARD_ITEM* parent = aItem->Parent();

    if( !parent )
        return;

    commit().Modify( parent );

    switch( aItem->Kind() )
    {
    case PNS::ITEM::SEGMENT_T:
    {
        PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( aItem );
        PCB_TRACK*    track = static_cast<PCB_TRACK*>( parent );
        track->SetStart( seg->Seg().A );
        track->SetEnd( seg->Seg().B );
        track->SetWidth( seg->Width() );
        break;
    }

    case PNS::ITEM::VIA_T:
    {
        PNS::VIA* via = static_cast<PNS::VIA*>( aItem );
        static_cast<PCB_VIA*>( parent )->SetPosition( via->Pos() );
        break;
    }

    default: break;
    }
}


void PNS_AGENT_IFACE::Commit()
{
    if( m_commit )
        m_commit->Push( wxS( "Route" ) );

    m_commit.reset();
}


int PNS_AGENT_IFACE::GetNetCode( PNS::NET_HANDLE aNet ) const
{
    return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetCode() : -1;
}


wxString PNS_AGENT_IFACE::GetNetName( PNS::NET_HANDLE aNet ) const
{
    return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetname() : wxString();
}

} // namespace AGENTIC
