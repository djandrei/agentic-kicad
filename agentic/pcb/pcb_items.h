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


#ifndef AGENTIC_PCB_ITEMS_H
#define AGENTIC_PCB_ITEMS_H

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <layer_ids.h>
#include <math/vector2d.h>

class BOARD;
class BOARD_ITEM;
class FOOTPRINT;
class NETINFO_ITEM;
class PAD;

namespace AGENTIC
{

/// mm to board internal units.
int PcbIu( double aMm );

/// A length argument (mm or with units) in board internal units.
int PcbLength( const nlohmann::json& aValue, const std::string& aField );

/// A point from {x, y} (mm, Y down) or a pad handle ("U1.3").
VECTOR2I PcbPoint( BOARD& aBoard, const nlohmann::json& aValue, const std::string& aField );

nlohmann::json PcbPointJson( const VECTOR2I& aPoint );

/// A layer by name ("F.Cu", "B.SilkS", "Edge.Cuts", or a user name).  @throw TOOL_ERROR.
PCB_LAYER_ID PcbLayer( BOARD& aBoard, const std::string& aName );

std::string PcbLayerName( BOARD& aBoard, PCB_LAYER_ID aLayer );

/// A net by name ("GND", "/SDA"), or nullptr for "" (no net).  @throw TOOL_ERROR when unknown.
NETINFO_ITEM* PcbNet( BOARD& aBoard, const std::string& aName );

nlohmann::json PcbItemJson( BOARD& aBoard, const BOARD_ITEM* aItem );

/// Items by handle: KIID, reference (a footprint), pad ("U1.3").  @throw TOOL_ERROR when none.
std::vector<BOARD_ITEM*> PcbFind( BOARD& aBoard, const std::string& aHandle );
BOARD_ITEM*              PcbFindOne( BOARD& aBoard, const std::string& aHandle );

FOOTPRINT* PcbFootprint( BOARD& aBoard, const std::string& aReference );
PAD*       PcbPad( BOARD& aBoard, const std::string& aHandle );

/// The unrouted connections of the board, per net: {net: count}, and the total.
struct PCB_RATSNEST
{
    std::map<std::string, int> unrouted;
    int                        total = 0;

    static PCB_RATSNEST Take( BOARD& aBoard );
};

/// {unroutedBefore, unroutedAfter, nets: {net: {before, after}}} for the nets that changed.
nlohmann::json PcbRatsnestDelta( const PCB_RATSNEST& aBefore, const PCB_RATSNEST& aAfter );

} // namespace AGENTIC

#endif // AGENTIC_PCB_ITEMS_H
