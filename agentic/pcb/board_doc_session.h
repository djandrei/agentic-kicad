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


#ifndef AGENTIC_BOARD_DOC_SESSION_H
#define AGENTIC_BOARD_DOC_SESSION_H

#include <memory>
#include <string>

#include <agentic/core/doc_session.h>

class BOARD;
class TOOL_MANAGER;

namespace AGENTIC
{

class AGENT_PROJECT;

/**
 * An open board (.kicad_pcb) with a frameless TOOL_MANAGER for BOARD_COMMIT, connectivity and
 * ratsnest.  A board that does not exist yet is created empty and written by the first change.
 */
class BOARD_DOC_SESSION : public DOC_SESSION
{
public:
    BOARD_DOC_SESSION( AGENT_PROJECT& aProject, const std::string& aPath );
    ~BOARD_DOC_SESSION() override;

    BOARD&        Board() const { return *m_board; }
    TOOL_MANAGER* ToolManager() const { return m_toolManager.get(); }

    bool IsNew() const { return m_isNew; }

protected:
    void load() override;
    void save( const std::vector<std::string>& aFiles ) override;

private:
    void unloadBoard();

    std::unique_ptr<BOARD>        m_board;
    std::unique_ptr<TOOL_MANAGER> m_toolManager;
    bool                          m_isNew = false;
};


/// The project's board: \a aPath, the open one, or <project>.kicad_pcb (created when missing).
BOARD_DOC_SESSION& BoardSession( AGENT_PROJECT& aProject, const std::string& aPath = std::string() );

} // namespace AGENTIC

#endif // AGENTIC_BOARD_DOC_SESSION_H
