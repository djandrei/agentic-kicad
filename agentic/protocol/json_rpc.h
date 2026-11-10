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

#ifndef AGENTIC_JSON_RPC_H
#define AGENTIC_JSON_RPC_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

/**
 * Minimal JSON-RPC 2.0 helpers used by the MCP server.  No wx dependency.
 */
namespace AGENTIC::JSON_RPC
{

using JSON = nlohmann::json;

constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

/// MCP: resources/read for an unknown URI.
constexpr int RESOURCE_NOT_FOUND = -32002;

/// MCP: the request was cancelled by the client before it completed.
constexpr int REQUEST_CANCELLED = -32800;

struct ERROR
{
    int         code = 0;
    std::string message;
    JSON        data = nullptr;
};

JSON MakeResult( const JSON& aId, JSON aResult );
JSON MakeError( const JSON& aId, const ERROR& aError );
JSON MakeNotification( const std::string& aMethod, JSON aParams = JSON::object() );

/**
 * Parse one JSON-RPC message (an object, or a batch array).
 *
 * @return the message, or std::nullopt with \a aError filled when the text is not valid
 *         JSON or not a JSON-RPC message.
 */
std::optional<JSON> Parse( const std::string& aText, ERROR& aError );

/// True for a message with "method" and "id".
bool IsRequest( const JSON& aMessage );

/// True for a message with "method" and no "id".
bool IsNotification( const JSON& aMessage );

/**
 * Serialize a message on one line (the stdio transport forbids embedded newlines).
 * Invalid UTF-8 is replaced rather than thrown.
 */
std::string Serialize( const JSON& aMessage );

} // namespace AGENTIC::JSON_RPC

#endif // AGENTIC_JSON_RPC_H
