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


#ifndef AGENTIC_PNS_AGENT_IFACE_H
#define AGENTIC_PNS_AGENT_IFACE_H

#include <memory>

#include <router/pns_kicad_iface.h>

class BOARD_COMMIT;
class BOARD_CONNECTED_ITEM;
class TOOL_MANAGER;

namespace AGENTIC
{

/**
 * The PNS router's board interface with no tool and no view (spike S0.d): what the router
 * commits becomes board items in one BOARD_COMMIT, pushed by Commit().  Upstream's
 * PNS_KICAD_IFACE needs a PCB_TOOL_BASE and a VIEW, so it cannot run headless.
 */
class PNS_AGENT_IFACE : public PNS_KICAD_IFACE_BASE
{
public:
    explicit PNS_AGENT_IFACE( TOOL_MANAGER* aToolManager );
    ~PNS_AGENT_IFACE() override;

    void AddItem( PNS::ITEM* aItem ) override;
    void RemoveItem( PNS::ITEM* aItem ) override;
    void UpdateItem( PNS::ITEM* aItem ) override;
    void Commit() override;

    int      GetNetCode( PNS::NET_HANDLE aNet ) const override;
    wxString GetNetName( PNS::NET_HANDLE aNet ) const override;

    int Added() const { return m_added; }
    int Removed() const { return m_removed; }

    /// KIIDs of the board items the router added, for the viewer's highlight.
    const std::vector<KIID>& AddedItems() const { return m_addedItems; }

private:
    BOARD_COMMIT&         commit();
    BOARD_CONNECTED_ITEM* createBoardItem( PNS::ITEM* aItem );

    TOOL_MANAGER*                 m_toolManager;
    std::unique_ptr<BOARD_COMMIT> m_commit;
    int                           m_added = 0;
    int                           m_removed = 0;
    std::vector<KIID>             m_addedItems;
};

} // namespace AGENTIC

#endif // AGENTIC_PNS_AGENT_IFACE_H
