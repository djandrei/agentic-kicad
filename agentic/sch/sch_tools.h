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


#ifndef AGENTIC_SCH_TOOLS_H
#define AGENTIC_SCH_TOOLS_H

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

class SCH_COMMIT;
class SCH_ITEM;
class SCH_SCREEN;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;
class SCH_DOC_SESSION;

/// Queries, placement, editing and wiring (stage 4.1 to 4.3), group "sch".
void RegisterSchEditTools( AGENT_SERVICES& aServices );

/// Annotation, ERC, exports, captures and the title block (stage 4.4), group "sch".
void RegisterSchCheckTools( AGENT_SERVICES& aServices );


/// The edits of one schematic call: items added, changed and removed, and the screens touched.
class SCH_EDIT
{
public:
    SCH_EDIT( SCH_DOC_SESSION& aSession, SCH_COMMIT& aCommit ) :
            m_session( aSession ),
            m_commit( aCommit )
    {
    }

    /// Add a new item to \a aScreen (its parent is set first, see S0.e).
    void Add( SCH_ITEM* aItem, SCH_SCREEN* aScreen );

    /// Record \a aItem before it changes.
    void Modify( SCH_ITEM* aItem, SCH_SCREEN* aScreen );

    void Remove( SCH_ITEM* aItem, SCH_SCREEN* aScreen );

    SCH_COMMIT&      Commit() { return m_commit; }
    SCH_DOC_SESSION& Session() { return m_session; }

    nlohmann::json created = nlohmann::json::array();
    nlohmann::json changed = nlohmann::json::array();
    nlohmann::json removed = nlohmann::json::array();

private:
    SCH_DOC_SESSION& m_session;
    SCH_COMMIT&      m_commit;
};


/**
 * Run one schematic change (D4, D5): \a aEdit fills an SCH_COMMIT, which is pushed; the
 * result gets created/changed/removed items, the connectivity delta when \a aDelta, and the
 * saved files.  The change is recorded for the viewer's Changes dock.
 */
nlohmann::json SchMutate( AGENT_PROJECT& aProject, SCH_DOC_SESSION& aSession, const std::string& aLabel,
                          const nlohmann::json& aArgs, bool aDelta,
                          const std::function<nlohmann::json( SCH_EDIT& )>& aEdit );

} // namespace AGENTIC

#endif // AGENTIC_SCH_TOOLS_H
