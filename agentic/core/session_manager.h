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


#ifndef AGENTIC_SESSION_MANAGER_H
#define AGENTIC_SESSION_MANAGER_H

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <agentic/core/agent_project.h>

namespace AGENTIC
{

/**
 * The open projects of the process.  The current project is the one last used; tools take
 * an optional "project" argument (name or folder) and otherwise act on the current one.
 */
class SESSION_MANAGER
{
public:
    /// Creates a program's library adapter (symbols, footprints) for a project's libraries.
    using ADAPTER_FACTORY = AGENT_PROJECT::ADAPTER_FACTORY;

    explicit SESSION_MANAGER( OBSERVERS& aObservers ) :
            m_observers( aObservers )
    {
    }
    ~SESSION_MANAGER();

    /**
     * Open the project in \a aPath (a folder holding one .kicad_pro, or the .kicad_pro
     * itself) and make it current.  An open project is returned as is.
     */
    AGENT_PROJECT& Open( const std::filesystem::path& aPath, bool aForce );

    /// Create a folder with a new .kicad_pro named \a aName (default: the folder name).
    AGENT_PROJECT& Create( const std::filesystem::path& aFolder, const std::string& aName );

    void Close( AGENT_PROJECT& aProject );

    /// The project named by aArgs["project"], or the current one; it becomes current.
    AGENT_PROJECT& Resolve( const nlohmann::json& aArgs );

    AGENT_PROJECT* Current() const { return m_current; }

    void SetCurrent( AGENT_PROJECT& aProject ) { activate( &aProject ); }

    std::vector<AGENT_PROJECT*> Projects() const;

    /// Find an open project by name or folder; nullptr if not open.
    AGENT_PROJECT* Find( const std::string& aNameOrPath ) const;

    /// Called by providers: every project's library manager gets this adapter.
    void AddLibraryAdapter( LIBRARY_TABLE_TYPE aType, ADAPTER_FACTORY aFactory );

private:
    /**
     * Make \a aProject current.  KiCad expands ${KIPRJMOD} from the environment, set for its
     * one active project; the agent's projects are loaded passively, so the variable follows
     * the current project, and the other projects' background library loads are stopped.
     */
    void activate( AGENT_PROJECT* aProject );

    OBSERVERS&                                  m_observers;
    AGENT_PROJECT::ADAPTER_FACTORIES            m_adapterFactories;
    std::vector<std::unique_ptr<AGENT_PROJECT>> m_projects;
    AGENT_PROJECT*                              m_current = nullptr;
};

} // namespace AGENTIC

#endif // AGENTIC_SESSION_MANAGER_H
