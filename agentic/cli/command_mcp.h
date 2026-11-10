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

#ifndef AGENTIC_COMMAND_MCP_H
#define AGENTIC_COMMAND_MCP_H

#include "command.h"

namespace CLI
{

/**
 * kicad-cli mcp: run the Agentic-KiCad MCP server headless, over stdio or loopback HTTP.
 */
class MCP_COMMAND : public COMMAND
{
public:
    MCP_COMMAND();

protected:
    int doPerform( KIWAY& aKiway ) override;
};

} // namespace CLI

#endif // AGENTIC_COMMAND_MCP_H
