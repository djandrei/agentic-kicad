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


#ifndef AGENTIC_SYM_LIB_SESSION_H
#define AGENTIC_SYM_LIB_SESSION_H

#include <memory>
#include <string>

#include <agentic/core/doc_session.h>

class LIB_SYMBOL;
class SCH_IO_KICAD_SEXPR_LIB_CACHE;

namespace AGENTIC
{

class AGENT_PROJECT;

/**
 * One open symbol library (.kicad_sym): KiCad's own library cache, edited in memory and
 * written whole after each call (D5).  The file need not exist yet.
 */
class SYMBOL_LIB_SESSION : public DOC_SESSION
{
public:
    SYMBOL_LIB_SESSION( AGENT_PROJECT& aProject, const std::string& aPath );
    ~SYMBOL_LIB_SESSION() override;

    SCH_IO_KICAD_SEXPR_LIB_CACHE& Library() const { return *m_library; }

    /// The symbol named \a aName.  @throw TOOL_ERROR when there is none.
    LIB_SYMBOL& Symbol( const std::string& aName ) const;

    /// Add a new symbol (owned by the library from now on).
    void Add( std::unique_ptr<LIB_SYMBOL> aSymbol );

    /// The symbol the agent worked on last (the viewer shows it).
    const std::string& CurrentSymbol() const { return m_current; }
    void               SetCurrentSymbol( const std::string& aName ) { m_current = aName; }

protected:
    void load() override;
    void save( const std::vector<std::string>& aFiles ) override;

private:
    std::unique_ptr<SCH_IO_KICAD_SEXPR_LIB_CACHE> m_library;
    std::string                                   m_current;
};


/**
 * The session of the symbol library \a aLibrary: a project library nickname (writable libraries
 * only: inside the project) or a project-relative .kicad_sym path.
 */
SYMBOL_LIB_SESSION& SymbolLibSession( AGENT_PROJECT& aProject, const std::string& aLibrary );

} // namespace AGENTIC

#endif // AGENTIC_SYM_LIB_SESSION_H
