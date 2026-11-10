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


#ifndef AGENTIC_OBSERVER_H
#define AGENTIC_OBSERVER_H

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AGENTIC
{

class AGENT_PROJECT;
class DOC_SESSION;

/**
 * What the hub and the viewers follow.  Sessions and the host send these on the main
 * thread; observers only read and redraw, they never change a document.
 */
class SESSION_OBSERVER
{
public:
    virtual ~SESSION_OBSERVER() = default;

    virtual void OnProjectOpened( AGENT_PROJECT& aProject ) {}
    virtual void OnProjectClosed( AGENT_PROJECT& aProject ) {}

    /// A project file was added, removed, renamed or written.
    virtual void OnFilesChanged( AGENT_PROJECT& aProject ) {}

    virtual void OnDocumentOpened( DOC_SESSION& aSession ) {}

    /// Sent before a session deletes its document; views must drop their items now.
    virtual void OnDocumentClosing( DOC_SESSION& aSession ) {}

    /// Sent before a document is replaced by a reload (undo, redo, conflict recovery).
    virtual void OnDocumentReloading( DOC_SESSION& aSession ) {}

    /// The document was reloaded; views rebuild from it.
    virtual void OnDocumentReloaded( DOC_SESSION& aSession ) {}

    virtual void OnDocumentSaved( DOC_SESSION& aSession ) {}

    /// A call changed the document; \a aChange describes it (as returned to the agent).
    virtual void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) {}

    /// Check results (ERC, DRC) to show in the viewer.
    virtual void OnResults( DOC_SESSION& aSession, const nlohmann::json& aResults ) {}

    virtual void OnToolStart( const std::string& aTool, const nlohmann::json& aArgs ) {}

    virtual void OnToolEnd( const std::string& aTool, const nlohmann::json& aArgs, bool aOk, double aMillis,
                            const std::string& aSummary )
    {
    }

    virtual void OnPauseChanged( bool aPaused ) {}
};


/// The observers of one host; notification is a loop over a snapshot, so an observer may
/// remove itself while being called.
class OBSERVERS
{
public:
    void Add( SESSION_OBSERVER* aObserver ) { m_list.push_back( aObserver ); }

    void Remove( SESSION_OBSERVER* aObserver ) { std::erase( m_list, aObserver ); }

    template <typename FUNC>
    void Notify( FUNC&& aFunc )
    {
        std::vector<SESSION_OBSERVER*> snapshot = m_list;

        for( SESSION_OBSERVER* observer : snapshot )
        {
            if( std::find( m_list.begin(), m_list.end(), observer ) != m_list.end() )
                aFunc( *observer );
        }
    }

private:
    std::vector<SESSION_OBSERVER*> m_list;
};

} // namespace AGENTIC

#endif // AGENTIC_OBSERVER_H
