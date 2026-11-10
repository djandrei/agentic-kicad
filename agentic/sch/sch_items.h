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


#ifndef AGENTIC_SCH_ITEMS_H
#define AGENTIC_SCH_ITEMS_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/vector2d.h>
#include <sch_sheet_path.h>

class SCH_ITEM;
class SCH_PIN;
class SCH_SCREEN;
class SCH_SYMBOL;
class SCHEMATIC;

namespace AGENTIC
{

class SCH_DOC_SESSION;

/// Schematic positions snap to this grid unless a tool is told otherwise (D4).
constexpr double SCH_GRID_MM = 1.27;

/// mm to schematic internal units, snapped to the 1.27 mm grid when \a aSnap.
int SchIu( double aMm, bool aSnap = true );

/// A point from {x, y} (mm, or strings with units).
VECTOR2I SchPoint( const nlohmann::json& aPoint, const std::string& aField, bool aSnap = true );

nlohmann::json SchPointJson( const VECTOR2I& aPoint );

/// What an item is, where, and its main properties, as tools return it.
nlohmann::json SchItemJson( const SCH_ITEM* aItem, const SCH_SHEET_PATH& aPath );

/// An item found by handle with the sheet path it is on.
struct SCH_FOUND
{
    SCH_ITEM*      item = nullptr;
    SCH_SHEET_PATH path;
};

/**
 * Resolve a handle (D4) to items: a KIID, a reference ("R5", every unit of it), a pin
 * ("U1.3").  @throw TOOL_ERROR when nothing matches.
 */
std::vector<SCH_FOUND> SchFind( SCHEMATIC& aSchematic, const std::string& aHandle );

/// The one item a handle names.  @throw TOOL_ERROR when none or several match.
SCH_FOUND SchFindOne( SCHEMATIC& aSchematic, const std::string& aHandle );

/// A sheet path from "/", "/power/" (sheet names), a sheet KIID, or empty (the session's current sheet).
SCH_SHEET_PATH SchSheet( SCH_DOC_SESSION& aSession, const std::string& aSheet );

/// "R1.2" for a pin on a sheet path.
std::string SchPinId( const SCH_PIN* aPin, const SCH_SHEET_PATH& aPath );


/**
 * The nets of a schematic: net name to its pins ("REF.PIN"), and the pins on no net with
 * anything else.  Taken before and after a call to report its connectivity delta (4.3).
 */
struct SCH_NET_SNAPSHOT
{
    std::map<std::string, std::set<std::string>> nets;
    std::set<std::string>                        unconnectedPins; ///< Alone on their net, no no-connect.

    static SCH_NET_SNAPSHOT Take( SCHEMATIC& aSchematic );
};

/// {created, removed, merged, split, unconnectedPins} between two snapshots.
nlohmann::json SchNetDelta( const SCH_NET_SNAPSHOT& aBefore, const SCH_NET_SNAPSHOT& aAfter );

} // namespace AGENTIC

#endif // AGENTIC_SCH_ITEMS_H
