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

#ifndef AGENTIC_PROVIDER_H
#define AGENTIC_PROVIDER_H

#include <wx/string.h>

/**
 * The agent-facing part of one kiface: its tools, document sessions and viewer.
 *
 * Each kiface that takes part in agent mode compiles a provider into itself and returns it
 * from KIFACE::IfaceOrAddress( KIFACE_AGENT_PROVIDER ).  The core reaches every program
 * only through this interface, so no kiface links against another one.
 *
 * The object is owned by the kiface and lives as long as the kiface is loaded.
 */
class AGENT_PROVIDER
{
public:
    virtual ~AGENT_PROVIDER() = default;

    /// Short stable name of the program, e.g. "pcb" or "sch".  Used as the tool group name.
    virtual wxString Name() const = 0;
};

#endif // AGENTIC_PROVIDER_H
