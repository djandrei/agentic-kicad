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

#include <boost/test/unit_test.hpp>

#include <agentic/protocol/json_rpc.h>
#include <agentic/protocol/schema.h>

using namespace AGENTIC;
using JSON = nlohmann::json;


BOOST_AUTO_TEST_SUITE( AgenticJsonRpc )


BOOST_AUTO_TEST_CASE( ParseRequestAndNotification )
{
    JSON_RPC::ERROR     err;
    std::optional<JSON> request = JSON_RPC::Parse( R"({"jsonrpc":"2.0","id":3,"method":"ping"})", err );
    BOOST_REQUIRE( request );
    BOOST_CHECK( JSON_RPC::IsRequest( *request ) );
    BOOST_CHECK( !JSON_RPC::IsNotification( *request ) );

    std::optional<JSON> note = JSON_RPC::Parse( R"({"jsonrpc":"2.0","method":"notifications/initialized"})", err );
    BOOST_REQUIRE( note );
    BOOST_CHECK( JSON_RPC::IsNotification( *note ) );
}


BOOST_AUTO_TEST_CASE( ParseErrors )
{
    JSON_RPC::ERROR err;

    BOOST_CHECK( !JSON_RPC::Parse( "{not json", err ) );
    BOOST_CHECK_EQUAL( err.code, JSON_RPC::PARSE_ERROR );

    BOOST_CHECK( !JSON_RPC::Parse( "[]", err ) );
    BOOST_CHECK_EQUAL( err.code, JSON_RPC::INVALID_REQUEST );

    BOOST_CHECK( !JSON_RPC::Parse( R"({"jsonrpc":"2.0","id":{"x":1},"method":"m"})", err ) );
    BOOST_CHECK_EQUAL( err.code, JSON_RPC::INVALID_REQUEST );

    BOOST_CHECK( !JSON_RPC::Parse( R"([{"jsonrpc":"2.0","id":1,"method":"m"}, 7])", err ) );
    BOOST_CHECK_EQUAL( err.code, JSON_RPC::INVALID_REQUEST );
}


BOOST_AUTO_TEST_CASE( SerializeIsOneLine )
{
    JSON        message = JSON_RPC::MakeResult( 1, JSON{ { "text", "line one\nline two" } } );
    std::string text = JSON_RPC::Serialize( message );
    BOOST_CHECK( text.find( '\n' ) == std::string::npos );
    BOOST_CHECK_EQUAL( JSON::parse( text )["result"]["text"], "line one\nline two" );
}


BOOST_AUTO_TEST_CASE( SerializeReplacesInvalidUtf8 )
{
    JSON message = JSON_RPC::MakeResult( 1, JSON{ { "text", std::string( "a\xff"
                                                                         "b" ) } } );
    BOOST_CHECK_NO_THROW( JSON_RPC::Serialize( message ) );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( AgenticSchema )


BOOST_AUTO_TEST_CASE( TypesRequiredAndAdditional )
{
    JSON schema = JSON::parse( R"({
        "type": "object",
        "properties": { "path": { "type": "string", "minLength": 1 },
                        "count": { "type": "integer", "minimum": 1, "maximum": 5 },
                        "mode": { "type": "string", "enum": ["a", "b"] },
                        "items": { "type": "array", "items": { "type": "number" }, "maxItems": 2 } },
        "required": ["path"],
        "additionalProperties": false })" );

    BOOST_CHECK( !SCHEMA::Validate( schema, JSON{ { "path", "x" } } ) );
    BOOST_CHECK( !SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "count", 2.0 } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON::object() ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "" } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", 3 } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "count", 2.5 } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "count", 9 } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "mode", "c" } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "extra", 1 } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "items", { 1, "two" } } } ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "items", { 1, 2, 3 } } } ) );

    std::optional<std::string> err = SCHEMA::Validate( schema, JSON{ { "path", "x" }, { "items", { 1, "two" } } } );
    BOOST_REQUIRE( err );
    BOOST_CHECK_EQUAL( *err, "$.items[1]: expected number, got string" );
}


BOOST_AUTO_TEST_CASE( TypeUnion )
{
    JSON schema = JSON{ { "type", JSON::array( { "string", "null" } ) } };
    BOOST_CHECK( !SCHEMA::Validate( schema, JSON( nullptr ) ) );
    BOOST_CHECK( !SCHEMA::Validate( schema, JSON( "s" ) ) );
    BOOST_CHECK( SCHEMA::Validate( schema, JSON( 1 ) ) );
}


BOOST_AUTO_TEST_SUITE_END()
