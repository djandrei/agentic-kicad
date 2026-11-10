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


#ifndef AGENTIC_AGENT_PROJECT_H
#define AGENTIC_AGENT_PROJECT_H

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <agentic/core/doc_session.h>
#include <agentic/core/file_history.h>
#include <agentic/core/file_store.h>
#include <agentic/core/message_log.h>
#include <agentic/core/observer.h>

#include <libraries/library_table.h>

class LIBRARY_MANAGER;
class LIBRARY_MANAGER_ADAPTER;
class LOCKFILE;
class PROJECT;

namespace AGENTIC
{

/**
 * One open KiCad project: a folder with a .kicad_pro.  It holds the sandbox (FILE_STORE),
 * the undo history, the message log, the fork's settings (.agentic/agentic.json) and the
 * open document sessions.  The KiCad PROJECT is loaded through SETTINGS_MANAGER without
 * becoming the active project, so several projects can be open at once; the project file is
 * locked with the same LOCKFILE the KiCad project manager takes.
 */
class AGENT_PROJECT
{
public:
    using ADAPTER_FACTORY = std::function<std::unique_ptr<LIBRARY_MANAGER_ADAPTER>( LIBRARY_MANAGER& )>;
    using ADAPTER_FACTORIES = std::vector<std::pair<LIBRARY_TABLE_TYPE, ADAPTER_FACTORY>>;

    /**
     * Open the project whose .kicad_pro is \a aProFile.  \a aFactories create the programs'
     * library adapters when the project's libraries are first used.
     *
     * @throw TOOL_ERROR when the project is locked by another KiCad and \a aForce is false.
     */
    AGENT_PROJECT( const std::filesystem::path& aProFile, OBSERVERS& aObservers, const ADAPTER_FACTORIES& aFactories,
                   bool aForce );
    ~AGENT_PROJECT();

    AGENT_PROJECT( const AGENT_PROJECT& ) = delete;
    AGENT_PROJECT& operator=( const AGENT_PROJECT& ) = delete;

    const std::string&           Name() const { return m_name; }
    const std::filesystem::path& Root() const { return m_files.Root(); }

    /// The .kicad_pro, project-relative.
    const std::string& ProFile() const { return m_proFile; }

    FILE_STORE&   Files() { return m_files; }
    FILE_HISTORY& History() { return m_history; }
    MESSAGE_LOG&  Log() { return m_log; }
    OBSERVERS&    Observers() { return m_observers; }

    /// The KiCad project (text variables, net classes, library tables).
    PROJECT& KicadProject() const { return *m_project; }

    /**
     * The project's own library manager: the global and project library tables as seen from
     * this project (${KIPRJMOD} is this folder), with the adapters the programs registered.
     * Created on first use; separate per project, so several projects can be open at once.
     */
    LIBRARY_MANAGER& Libraries();

    /// The adapter of one library type, or nullptr when no program registered it.
    LIBRARY_MANAGER_ADAPTER* LibraryAdapter( LIBRARY_TABLE_TYPE aType );

    /// Reread the library tables after they changed (lib_add, undo of a table change).
    void ReloadLibraryTables();

    /// Stop background library loads (another project becomes current, see SESSION_MANAGER).
    void StopLibraryLoads();

    /// True when a library URI resolves inside the project folder (the agent may change it).
    bool IsInsideProject( const wxString& aUri );

    /// The fork's settings from .agentic/agentic.json (e.g. "disabledGroups").
    const nlohmann::json& Settings() const { return m_settings; }
    void                  SetSettings( const nlohmann::json& aSettings );

    // Sessions -----------------------------------------------------------------------------

    DOC_SESSION*              FindSession( const std::string& aProgram, const std::string& aPath ) const;
    DOC_SESSION*              SessionForFile( const std::string& aRelative ) const;
    std::vector<DOC_SESSION*> Sessions() const;

    /// Take ownership of an opened session and tell the observers.
    DOC_SESSION& AddSession( std::unique_ptr<DOC_SESSION> aSession );

    void CloseSession( DOC_SESSION* aSession );

    // Mutating calls (D4, D5, D7) ----------------------------------------------------------

    /**
     * Run one mutating call: \a aChange edits documents (sessions mark what they changed, the
     * project file is marked with TouchProjectFile()).  Afterwards every touched document is
     * checked for conflicts and saved, and the call becomes one undo step.
     *
     * On any failure the touched documents are reloaded from disk, so memory and disk agree,
     * and the error is rethrown.
     *
     * @return the change's result with "saved": [{path, version}] added.
     */
    nlohmann::json Mutate( const std::string& aLabel, bool aForce, const std::function<nlohmann::json()>& aChange );

    /// Called by DOC_SESSION::MarkModified.
    void Touch( DOC_SESSION& aSession );

    /// The current call changed the KiCad project file (text variables, net classes).
    void TouchProjectFile() { m_projectFileTouched = true; }

    /// Record a file written directly by a file tool, inside Mutate (history and observers).
    void BeforeFileWrite( const std::string& aRelative );
    void AfterFileWrite( const std::string& aRelative );

    nlohmann::json Undo( bool aForce );
    nlohmann::json Redo( bool aForce );

    nlohmann::json Info() const;

private:
    void           loadSettings();
    nlohmann::json saveProjectFile();
    void           reloadAfterRestore( const FILE_HISTORY::STEP& aStep );

    OBSERVERS&                                m_observers;
    FILE_STORE                                m_files;
    FILE_HISTORY                              m_history;
    MESSAGE_LOG                               m_log;
    std::string                               m_name;
    std::string                               m_proFile;
    std::string                               m_proVersion;
    PROJECT*                                  m_project = nullptr;
    std::unique_ptr<LOCKFILE>                 m_lock;
    nlohmann::json                            m_settings = nlohmann::json::object();
    const ADAPTER_FACTORIES&                  m_adapterFactories;
    std::unique_ptr<LIBRARY_MANAGER>          m_libraries;
    std::vector<std::unique_ptr<DOC_SESSION>> m_sessions;
    std::set<DOC_SESSION*>                    m_touched;
    bool                                      m_projectFileTouched = false;
    bool                                      m_filesWritten = false;
    int                                       m_mutationDepth = 0;
};

} // namespace AGENTIC

#endif // AGENTIC_AGENT_PROJECT_H
