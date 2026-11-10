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


#ifndef AGENTIC_SCH_DOC_SESSION_H
#define AGENTIC_SCH_DOC_SESSION_H

#include <memory>
#include <string>
#include <vector>

#include <agentic/core/doc_session.h>

#include <kiid.h>

class SCHEMATIC;
class SCH_SCREEN;
class SCH_SHEET_PATH;
class TOOL_MANAGER;

namespace AGENTIC
{

class AGENT_PROJECT;

/**
 * An open schematic: the SCHEMATIC with its whole hierarchy, a frameless TOOL_MANAGER for
 * SCH_COMMIT, and the sheet files as the session's files.  A schematic that does not exist
 * yet is created empty in memory and written by the first mutating call.
 */
class SCH_DOC_SESSION : public DOC_SESSION
{
public:
    SCH_DOC_SESSION( AGENT_PROJECT& aProject, const std::string& aRootPath );
    ~SCH_DOC_SESSION() override;

    SCHEMATIC&    Schematic() const { return *m_schematic; }
    TOOL_MANAGER* ToolManager() const { return m_toolManager.get(); }

    /// Every sheet file (project-relative), root first.
    std::vector<std::string> Files() const override;

    /// The project-relative file of a screen.
    std::string FileOf( const SCH_SCREEN* aScreen ) const;

    /// Mark the file of \a aScreen as changed by the current call.
    void Touch( const SCH_SCREEN* aScreen );

    /// The sheet the agent worked on last (the viewer follows it); the root when none.
    SCH_SHEET_PATH CurrentSheet() const;
    void           SetCurrentSheet( const SCH_SHEET_PATH& aPath );

    /// After a change of the hierarchy: the project file's sheet list follows (marks it).
    void HierarchyChanged();

    /// True while the schematic exists only in memory (no file written yet).
    bool IsNew() const { return m_isNew; }

protected:
    void load() override;
    void save( const std::vector<std::string>& aFiles ) override;

private:
    std::unique_ptr<SCHEMATIC>    m_schematic;
    std::unique_ptr<TOOL_MANAGER> m_toolManager;
    std::vector<KIID>             m_currentSheet; ///< KIID path of the current sheet.
    bool                          m_isNew = false;
};


/**
 * The project's schematic session: \a aPath (project-relative .kicad_sch), or the open one,
 * or <project>.kicad_sch.  Opened (and created empty when missing) on first use.
 */
SCH_DOC_SESSION& SchematicSession( AGENT_PROJECT& aProject, const std::string& aPath = std::string() );

} // namespace AGENTIC

#endif // AGENTIC_SCH_DOC_SESSION_H
