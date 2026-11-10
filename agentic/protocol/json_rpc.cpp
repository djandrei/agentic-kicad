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

#include <agentic/protocol/json_rpc.h>


namespace AGENTIC::JSON_RPC
{

JSON MakeResult( const JSON& aId, JSON aResult )
{
    return JSON{ { "jsonrpc", "2.0" }, { "id", aId }, { "result", std::move( aResult ) } };
}


JSON MakeError( const JSON& aId, const ERROR& aError )
{
    JSON err{ { "code", aError.code }, { "message", aError.message } };

    if( !aError.data.is_null() )
        err["data"] = aError.data;

    return JSON{ { "jsonrpc", "2.0" }, { "id", aId }, { "error", err } };
}


JSON MakeNotification( const std::string& aMethod, JSON aParams )
{
    return JSON{ { "jsonrpc", "2.0" }, { "method", aMethod }, { "params", std::move( aParams ) } };
}


static bool isValidId( const JSON& aId )
{
    return aId.is_string() || aId.is_number() || aId.is_null();
}


static bool looksLikeMessage( const JSON& aMessage )
{
    if( !aMessage.is_object() )
        return false;

    if( aMessage.contains( "method" ) )
    {
        if( !aMessage["method"].is_string() )
            return false;

        if( aMessage.contains( "id" ) && !isValidId( aMessage["id"] ) )
            return false;

        return true;
    }

    // A response: a result or an error, with an id
    return ( aMessage.contains( "result" ) || aMessage.contains( "error" ) ) && aMessage.contains( "id" );
}


std::optional<JSON> Parse( const std::string& aText, ERROR& aError )
{
    JSON parsed = JSON::parse( aText, nullptr, false );

    if( parsed.is_discarded() )
    {
        aError = { PARSE_ERROR, "Parse error" };
        return std::nullopt;
    }

    if( parsed.is_array() )
    {
        if( parsed.empty() )
        {
            aError = { INVALID_REQUEST, "Invalid Request: empty batch" };
            return std::nullopt;
        }

        for( const JSON& item : parsed )
        {
            if( !looksLikeMessage( item ) )
            {
                aError = { INVALID_REQUEST, "Invalid Request" };
                return std::nullopt;
            }
        }

        return parsed;
    }

    if( !looksLikeMessage( parsed ) )
    {
        aError = { INVALID_REQUEST, "Invalid Request" };
        return std::nullopt;
    }

    return parsed;
}


bool IsRequest( const JSON& aMessage )
{
    return aMessage.is_object() && aMessage.contains( "method" ) && aMessage.contains( "id" );
}


bool IsNotification( const JSON& aMessage )
{
    return aMessage.is_object() && aMessage.contains( "method" ) && !aMessage.contains( "id" );
}


std::string Serialize( const JSON& aMessage )
{
    // dump() with indent -1 emits no newlines; control characters in strings are escaped
    return aMessage.dump( -1, ' ', false, JSON::error_handler_t::replace );
}

} // namespace AGENTIC::JSON_RPC
