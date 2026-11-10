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


#ifndef AGENTIC_HANDLES_H
#define AGENTIC_HANDLES_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

/**
 * Parsers for the values agents type (D4): lengths, angles and item handles.  No wx and no
 * KiCad dependency, so every provider uses the same rules.
 */
namespace AGENTIC
{

/**
 * A length in millimetres from a JSON number (millimetres) or a string with a unit:
 * "1.27", "1.27mm", "50mil", "50 mils", "0.1in", "0.1\"", "2.54 mm", "100um".
 *
 * @throw TOOL_ERROR naming \a aField when the value is not a length.
 */
double ParseLengthMm( const nlohmann::json& aValue, const std::string& aField );

/// An angle in degrees from a number (degrees) or a string: "90", "90deg", "-45°", "1.5708rad".
double ParseAngleDeg( const nlohmann::json& aValue, const std::string& aField );

/// Round \a aMm to the nearest multiple of \a aGridMm (D4: schematic positions snap to 1.27 mm).
double SnapMm( double aMm, double aGridMm );


/**
 * What a handle string names.  The providers resolve it against their document; this only
 * classifies the text so every tool reads handles the same way.
 */
struct HANDLE
{
    enum class KIND
    {
        KIID,      ///< "0f9c6c1e-...", an item's UUID.
        REFERENCE, ///< "R5", "U1".
        PIN,       ///< "U1.3", "U1.A12" (reference and pin or pad number).
        NET,       ///< "net:/SDA", "/SDA", "net:GND".
        SHEET,     ///< "sheet:/power/", a sheet path (ends with '/').
        LAYER      ///< "layer:F.Cu".
    };

    KIND        kind;
    std::string text;      ///< The handle without its prefix.
    std::string reference; ///< PIN: the reference part.
    std::string pin;       ///< PIN: the pin or pad number.
};

/**
 * Classify a handle.  Prefixes ("kiid:", "ref:", "pin:", "net:", "sheet:", "layer:") force
 * a kind; without one, a UUID is a KIID, text starting with '/' is a net (or a sheet when it
 * ends with '/'), "REF.PIN" is a pin and anything else is a reference.
 *
 * @throw TOOL_ERROR when \a aText is empty or has an unknown prefix.
 */
HANDLE ParseHandle( const std::string& aText );

/// True for the 8-4-4-4-12 hexadecimal form of a UUID.
bool IsUuid( const std::string& aText );

} // namespace AGENTIC

#endif // AGENTIC_HANDLES_H
