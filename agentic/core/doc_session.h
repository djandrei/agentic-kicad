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


#ifndef AGENTIC_DOC_SESSION_H
#define AGENTIC_DOC_SESSION_H

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class LOCKFILE;

namespace AGENTIC
{

class AGENT_PROJECT;

/**
 * One open document of one program: a schematic with its sheets, a board, a symbol library.
 * The provider subclasses it to hold the document itself and its frameless TOOL_MANAGER.
 *
 * The base class does what every document shares:
 *  - the document lock (LOCKFILE, the same one the KiCad editors take);
 *  - the content version of each of its files, as last loaded or saved;
 *  - saving after every mutating call (D5), with the check that nobody else changed the
 *    files, and the file history for undo (D7);
 *  - the change history shown in the viewer's Changes dock.
 *
 * Methods run on the main thread.
 */
class DOC_SESSION
{
public:
    /// Changes kept for the Changes dock.
    static constexpr size_t MAX_CHANGES = 200;

    DOC_SESSION( AGENT_PROJECT& aProject, std::string aProgram, std::string aPath );
    virtual ~DOC_SESSION();

    DOC_SESSION( const DOC_SESSION& ) = delete;
    DOC_SESSION& operator=( const DOC_SESSION& ) = delete;

    AGENT_PROJECT&     Project() const { return m_project; }
    const std::string& Program() const { return m_program; }

    /// The primary file, project-relative.
    const std::string& Path() const { return m_path; }

    /// Every file of the document, project-relative (a schematic's sheet files).
    virtual std::vector<std::string> Files() const { return { m_path }; }

    /**
     * Lock the primary file and load the document.
     *
     * @throw TOOL_ERROR when another KiCad holds the lock and \a aForce is false, or when the
     *        document cannot be loaded.
     */
    void Open( bool aForce );

    /// Mark a file of the document as changed by the current call (default: the primary file).
    void MarkModified( const std::string& aFile = std::string() );

    bool IsModified() const { return !m_modified.empty(); }

    /// Throw a conflict when a modified file changed on disk since it was loaded or saved.
    void CheckConflicts() const;

    /**
     * Save the modified files (D5).  Called by AGENT_PROJECT::Mutate after the change.
     *
     * @return [{path, version}] of the files written.
     * @throw TOOL_ERROR when a save failed; the document is then reloaded from disk.
     */
    nlohmann::json Save();

    /// Replace the in-memory document by the files on disk (undo, redo, failure recovery).
    void Reload();

    /// The content version of \a aFile when it was last loaded or saved.
    std::string Version( const std::string& aFile ) const;

    /// Versions of every file, {path: version}.
    nlohmann::json Versions() const;

    /// Record a change for the Changes dock and tell the observers.
    void RecordChange( const nlohmann::json& aChange );

    const std::deque<nlohmann::json>& Changes() const { return m_changes; }

protected:
    /// Load or reload the document from disk, replacing what is in memory.
    virtual void load() = 0;

    /// Write \a aFiles (project-relative) of the in-memory document.  Throw on failure.
    virtual void save( const std::vector<std::string>& aFiles ) = 0;

    /// Read the versions of every file from disk; called after load and save.
    void refreshVersions();

private:
    AGENT_PROJECT&                     m_project;
    std::string                        m_program;
    std::string                        m_path;
    std::unique_ptr<LOCKFILE>          m_lock;
    std::map<std::string, std::string> m_versions;
    std::set<std::string>              m_modified;
    std::deque<nlohmann::json>         m_changes;
};

} // namespace AGENTIC

#endif // AGENTIC_DOC_SESSION_H
