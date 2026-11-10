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

#include <vector>

#include <wx/string.h>

namespace AGENTIC
{
class AGENT_PROJECT;
class AGENT_SERVICES;
class AGENT_VIEWER;
} // namespace AGENTIC

/**
 * The agent-facing part of one kiface: its tools, document sessions and viewer.
 *
 * Each kiface that takes part in agent mode compiles a provider into itself and returns it
 * from KIFACE::IfaceOrAddress( KIFACE_AGENT_PROVIDER ).  The core reaches every program
 * only through this interface, so no kiface links against another one.
 *
 * The object is owned by the kiface and lives as long as the kiface is loaded.  Objects
 * passed between the host and a provider are used through virtual methods or plain data,
 * never through dynamic_cast, since each module has its own copy of the fork's code.
 */
class AGENT_PROVIDER
{
public:
    virtual ~AGENT_PROVIDER() = default;

    /// Short stable name of the program, e.g. "pcb" or "sch".  Used as the tool group name.
    virtual wxString Name() const = 0;

    /// Human name for the hub button and the viewer title, e.g. "PCB".
    virtual wxString Title() const { return Name(); }

    /**
     * Other programs compiled into the same kiface, each with its own tool group and viewer
     * (the symbol editor in eeschema, the footprint editor in pcbnew).  The host registers them
     * like the kiface's own provider.
     */
    virtual std::vector<AGENT_PROVIDER*> Companions() { return {}; }

    /// Add the program's tools, resources and prompts.  Called once, on the main thread.
    virtual void Register( AGENTIC::AGENT_SERVICES& aServices ) {}

    /**
     * Create the program's viewer for \a aProject (viewer mode only), a top-level window
     * that observes the project's sessions.  nullptr when the program has no viewer.
     */
    virtual AGENTIC::AGENT_VIEWER* CreateViewer( AGENTIC::AGENT_SERVICES& aServices, AGENTIC::AGENT_PROJECT& aProject )
    {
        return nullptr;
    }
};

#endif // AGENTIC_PROVIDER_H
