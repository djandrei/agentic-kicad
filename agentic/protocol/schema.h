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

#ifndef AGENTIC_SCHEMA_H
#define AGENTIC_SCHEMA_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace AGENTIC::SCHEMA
{

/**
 * Validate tool arguments against a JSON schema.
 *
 * Supports the subset the tool definitions use: type (string or list), required, properties,
 * additionalProperties: false, enum, items, minItems, maxItems, minimum, maximum, minLength,
 * maxLength.  A full validator (thirdparty/json_schema_validator) was not needed for this
 * subset and would have to be fed a draft-07 meta schema for every tool.
 *
 * @return an error description, or std::nullopt when \a aValue conforms to \a aSchema.
 */
std::optional<std::string> Validate( const nlohmann::json& aSchema, const nlohmann::json& aValue,
                                     const std::string& aPath = "$" );

} // namespace AGENTIC::SCHEMA

#endif // AGENTIC_SCHEMA_H
