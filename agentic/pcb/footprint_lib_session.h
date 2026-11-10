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


#ifndef AGENTIC_FOOTPRINT_LIB_SESSION_H
#define AGENTIC_FOOTPRINT_LIB_SESSION_H

#include <map>
#include <memory>
#include <set>
#include <string>

#include <agentic/core/doc_session.h>

class BOARD;
class FOOTPRINT;

namespace AGENTIC
{

class AGENT_PROJECT;

/**
 * One footprint library (a .pretty folder): every footprint in memory, one .kicad_mod file
 * each.  A change marks the footprint's file; saving writes or removes only those files.
 * The footprints are parented to a footprint-holder board, as in the footprint editor.
 */
class FOOTPRINT_LIB_SESSION : public DOC_SESSION
{
public:
    FOOTPRINT_LIB_SESSION( AGENT_PROJECT& aProject, const std::string& aPath );
    ~FOOTPRINT_LIB_SESSION() override;

    /// Every footprint file seen in this session, including deleted ones (undo restores them).
    std::vector<std::string> Files() const override;

    /// The library's nickname-free name, e.g. "MyParts" for MyParts.pretty.
    std::string LibraryName() const;

    std::vector<std::string> Names() const;
    bool                     Has( const std::string& aName ) const;

    /// @throw TOOL_ERROR when there is no footprint \a aName.
    FOOTPRINT& Footprint( const std::string& aName ) const;

    /// The project-relative file of footprint \a aName.
    std::string FileOf( const std::string& aName ) const;

    /// Add a footprint (owned by the session from now on) and mark its file.
    void Add( std::unique_ptr<FOOTPRINT> aFootprint );

    /// Remove footprint \a aName and mark its file (saving deletes it).
    void Remove( const std::string& aName );

    /// Mark footprint \a aName changed by the current call.
    void Touch( const std::string& aName );

    BOARD& Holder() const { return *m_holder; }

    /// The footprint the agent worked on last (the viewer shows it).
    const std::string& Current() const { return m_current; }
    void               SetCurrent( const std::string& aName ) { m_current = aName; }

protected:
    void load() override;
    void save( const std::vector<std::string>& aFiles ) override;

private:
    std::unique_ptr<BOARD>                            m_holder;
    std::map<std::string, std::unique_ptr<FOOTPRINT>> m_footprints;
    std::set<std::string>                             m_known;
    std::string                                       m_current;
};


/// The session of footprint library \a aLibrary: a project table nickname or a .pretty path.
FOOTPRINT_LIB_SESSION& FootprintLibSession( AGENT_PROJECT& aProject, const std::string& aLibrary );

} // namespace AGENTIC

#endif // AGENTIC_FOOTPRINT_LIB_SESSION_H
