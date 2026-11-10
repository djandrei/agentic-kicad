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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numbers>


namespace AGENTIC
{

namespace
{

    std::string trim( const std::string& aText )
    {
        size_t start = aText.find_first_not_of( " \t" );
        size_t end = aText.find_last_not_of( " \t" );

        if( start == std::string::npos )
            return std::string();

        return aText.substr( start, end - start + 1 );
    }


    std::string lower( std::string aText )
    {
        std::transform( aText.begin(), aText.end(), aText.begin(),
                        []( unsigned char c )
                        {
                            return static_cast<char>( std::tolower( c ) );
                        } );
        return aText;
    }


    /// Split "12.5 mm" into 12.5 and "mm"; false when there is no leading number.
    bool splitNumber( const std::string& aText, double& aNumber, std::string& aUnit )
    {
        const std::string text = trim( aText );
        const char*       begin = text.c_str();
        char*             end = nullptr;

        aNumber = std::strtod( begin, &end );

        if( end == begin || !std::isfinite( aNumber ) )
            return false;

        aUnit = lower( trim( std::string( end ) ) );
        return true;
    }

} // namespace


double ParseLengthMm( const nlohmann::json& aValue, const std::string& aField )
{
    if( aValue.is_number() )
        return aValue.get<double>();

    double      number = 0;
    std::string unit;

    if( aValue.is_string() && splitNumber( aValue.get<std::string>(), number, unit ) )
    {
        if( unit.empty() || unit == "mm" )
            return number;

        if( unit == "mil" || unit == "mils" || unit == "th" || unit == "thou" )
            return number * 0.0254;

        if( unit == "in" || unit == "inch" || unit == "\"" )
            return number * 25.4;

        if( unit == "um" || unit == "µm" )
            return number / 1000.0;

        if( unit == "cm" )
            return number * 10.0;
    }

    throw TOOL_ERROR( "'" + aField
                      + "' must be a length: a number in mm, or a string such as "
                        "\"1.27mm\", \"50mil\" or \"0.1in\"; got "
                      + aValue.dump() );
}


double ParseAngleDeg( const nlohmann::json& aValue, const std::string& aField )
{
    if( aValue.is_number() )
        return aValue.get<double>();

    double      number = 0;
    std::string unit;

    if( aValue.is_string() && splitNumber( aValue.get<std::string>(), number, unit ) )
    {
        if( unit.empty() || unit == "deg" || unit == "°" || unit == "degrees" )
            return number;

        if( unit == "rad" )
            return number * 180.0 / std::numbers::pi;
    }

    throw TOOL_ERROR( "'" + aField + "' must be an angle in degrees, e.g. 90 or \"90deg\"; got " + aValue.dump() );
}


double SnapMm( double aMm, double aGridMm )
{
    if( aGridMm <= 0 )
        return aMm;

    return std::round( aMm / aGridMm ) * aGridMm;
}


bool IsUuid( const std::string& aText )
{
    static const int groups[] = { 8, 4, 4, 4, 12 };

    size_t pos = 0;

    for( int g = 0; g < 5; ++g )
    {
        for( int i = 0; i < groups[g]; ++i, ++pos )
        {
            if( pos >= aText.size() || !std::isxdigit( static_cast<unsigned char>( aText[pos] ) ) )
                return false;
        }

        if( g < 4 )
        {
            if( pos >= aText.size() || aText[pos] != '-' )
                return false;

            ++pos;
        }
    }

    return pos == aText.size();
}


HANDLE ParseHandle( const std::string& aText )
{
    const std::string text = trim( aText );

    if( text.empty() )
        throw TOOL_ERROR( "An empty handle names nothing. Use a reference (R5), a pin (U1.3), a net "
                          "(/SDA), a sheet (sheet:/power/), a layer (layer:F.Cu) or an item UUID." );

    const size_t colon = text.find( ':' );

    if( colon != std::string::npos && colon > 0 && text[0] != '/' )
    {
        const std::string prefix = lower( text.substr( 0, colon ) );
        const std::string rest = text.substr( colon + 1 );

        if( prefix == "kiid" )
            return { HANDLE::KIND::KIID, rest, "", "" };

        if( prefix == "ref" )
            return { HANDLE::KIND::REFERENCE, rest, "", "" };

        if( prefix == "net" )
            return { HANDLE::KIND::NET, rest, "", "" };

        if( prefix == "sheet" )
            return { HANDLE::KIND::SHEET, rest, "", "" };

        if( prefix == "layer" )
            return { HANDLE::KIND::LAYER, rest, "", "" };

        if( prefix == "pin" || prefix == "pad" )
        {
            const size_t dot = rest.find( '.' );

            if( dot == std::string::npos || dot == 0 || dot + 1 == rest.size() )
                throw TOOL_ERROR( "A pin handle is REF.PIN, e.g. U1.3; got '" + text + "'" );

            return { HANDLE::KIND::PIN, rest, rest.substr( 0, dot ), rest.substr( dot + 1 ) };
        }

        throw TOOL_ERROR( "Unknown handle prefix '" + prefix + ":' in '" + text
                          + "'. Known: kiid, ref, pin, pad, net, sheet, layer." );
    }

    if( IsUuid( text ) )
        return { HANDLE::KIND::KIID, text, "", "" };

    if( text[0] == '/' )
    {
        if( text.size() > 1 && text.back() == '/' )
            return { HANDLE::KIND::SHEET, text, "", "" };

        return { HANDLE::KIND::NET, text, "", "" };
    }

    const size_t dot = text.find( '.' );

    if( dot != std::string::npos && dot > 0 && dot + 1 < text.size() )
        return { HANDLE::KIND::PIN, text, text.substr( 0, dot ), text.substr( dot + 1 ) };

    return { HANDLE::KIND::REFERENCE, text, "", "" };
}

} // namespace AGENTIC
