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

#include <agentic/protocol/schema.h>

#include <cmath>


namespace AGENTIC::SCHEMA
{

using JSON = nlohmann::json;


static bool typeMatches( const std::string& aType, const JSON& aValue )
{
    if( aType == "string" )
        return aValue.is_string();

    if( aType == "integer" )
    {
        if( aValue.is_number_integer() )
            return true;

        return aValue.is_number_float() && std::floor( aValue.get<double>() ) == aValue.get<double>();
    }

    if( aType == "number" )
        return aValue.is_number();

    if( aType == "boolean" )
        return aValue.is_boolean();

    if( aType == "object" )
        return aValue.is_object();

    if( aType == "array" )
        return aValue.is_array();

    if( aType == "null" )
        return aValue.is_null();

    return false;
}


std::optional<std::string> Validate( const JSON& aSchema, const JSON& aValue, const std::string& aPath )
{
    if( !aSchema.is_object() )
        return std::nullopt;

    if( aSchema.contains( "type" ) )
    {
        const JSON& type = aSchema["type"];
        bool        ok = false;
        std::string expected;

        if( type.is_string() )
        {
            expected = type.get<std::string>();
            ok = typeMatches( expected, aValue );
        }
        else if( type.is_array() )
        {
            for( const JSON& t : type )
            {
                if( !expected.empty() )
                    expected += "|";

                expected += t.get<std::string>();

                if( typeMatches( t.get<std::string>(), aValue ) )
                    ok = true;
            }
        }

        if( !ok )
            return aPath + ": expected " + expected + ", got " + std::string( aValue.type_name() );
    }

    if( aSchema.contains( "enum" ) )
    {
        bool found = false;

        for( const JSON& allowed : aSchema["enum"] )
        {
            if( allowed == aValue )
            {
                found = true;
                break;
            }
        }

        if( !found )
            return aPath + ": value " + aValue.dump() + " is not one of " + aSchema["enum"].dump();
    }

    if( aValue.is_object() )
    {
        if( aSchema.contains( "required" ) )
        {
            for( const JSON& key : aSchema["required"] )
            {
                if( !aValue.contains( key.get<std::string>() ) )
                    return aPath + ": missing required property '" + key.get<std::string>() + "'";
            }
        }

        const JSON properties = aSchema.value( "properties", JSON::object() );

        for( const auto& [key, propValue] : aValue.items() )
        {
            if( properties.contains( key ) )
            {
                if( std::optional<std::string> err = Validate( properties[key], propValue, aPath + "." + key ) )
                    return err;
            }
            else if( aSchema.contains( "additionalProperties" ) && aSchema["additionalProperties"] == false )
            {
                return aPath + ": unexpected property '" + key + "'";
            }
        }
    }

    if( aValue.is_string() )
    {
        const size_t length = aValue.get<std::string>().size();

        if( aSchema.contains( "minLength" ) && length < aSchema["minLength"].get<size_t>() )
        {
            return aPath + ": expected at least " + std::to_string( aSchema["minLength"].get<size_t>() )
                   + " characters";
        }

        if( aSchema.contains( "maxLength" ) && length > aSchema["maxLength"].get<size_t>() )
        {
            return aPath + ": expected at most " + std::to_string( aSchema["maxLength"].get<size_t>() ) + " characters";
        }
    }

    if( aValue.is_array() )
    {
        if( aSchema.contains( "minItems" ) && aValue.size() < aSchema["minItems"].get<size_t>() )
        {
            return aPath + ": expected at least " + std::to_string( aSchema["minItems"].get<size_t>() ) + " items";
        }

        if( aSchema.contains( "maxItems" ) && aValue.size() > aSchema["maxItems"].get<size_t>() )
        {
            return aPath + ": expected at most " + std::to_string( aSchema["maxItems"].get<size_t>() ) + " items";
        }

        if( aSchema.contains( "items" ) )
        {
            for( size_t i = 0; i < aValue.size(); ++i )
            {
                std::string itemPath = aPath + "[" + std::to_string( i ) + "]";

                if( std::optional<std::string> err = Validate( aSchema["items"], aValue[i], itemPath ) )
                    return err;
            }
        }
    }

    if( aValue.is_number() )
    {
        const double v = aValue.get<double>();

        if( aSchema.contains( "minimum" ) && v < aSchema["minimum"].get<double>() )
            return aPath + ": value " + aValue.dump() + " is below minimum " + aSchema["minimum"].dump();

        if( aSchema.contains( "maximum" ) && v > aSchema["maximum"].get<double>() )
            return aPath + ": value " + aValue.dump() + " is above maximum " + aSchema["maximum"].dump();
    }

    return std::nullopt;
}

} // namespace AGENTIC::SCHEMA
