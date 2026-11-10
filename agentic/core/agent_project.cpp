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


#include <agentic/core/agent_project.h>
#include <agentic/protocol/registry.h>

#include <libraries/library_manager.h>
#include <lockfile.h>
#include <pgm_base.h>
#include <project.h>
#include <project/project_file.h>
#include <settings/settings_manager.h>

#include <algorithm>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    const char* SETTINGS_FILE = ".agentic/agentic.json";

} // namespace


AGENT_PROJECT::AGENT_PROJECT( const fs::path& aProFile, OBSERVERS& aObservers, const ADAPTER_FACTORIES& aFactories,
                              bool aForce ) :
        m_observers( aObservers ),
        m_files( aProFile.parent_path() ),
        m_history( m_files.Root() ),
        m_name( aProFile.stem().string() ),
        m_proFile( aProFile.filename().string() ),
        m_adapterFactories( aFactories )
{
    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    const wxString    fullPath = wxString::FromUTF8( ( m_files.Root() / m_proFile ).string() );

    // Load passively: an active load would unload the other open projects ("no MDI yet").
    // The lock is taken here instead, after the load, which only inspects it.
    if( !mgr.LoadProject( fullPath, false ) )
    {
        // A missing or unreadable .kicad_pro loads defaults; that is how a project is created.
        if( fs::exists( m_files.Root() / m_proFile ) )
        {
            if( PROJECT* broken = mgr.GetProject( fullPath ) )
                mgr.UnloadProject( broken, false );

            throw TOOL_ERROR( "Cannot read the project file " + m_proFile + "." );
        }
    }

    m_project = mgr.GetProject( fullPath );

    if( !m_project )
        throw TOOL_ERROR( "KiCad could not open the project " + m_proFile + "." );

    m_lock = std::make_unique<LOCKFILE>( fullPath );

    if( !m_lock->Valid() )
    {
        if( !aForce )
        {
            const std::string who = ( m_lock->GetUsername() + wxS( "@" ) + m_lock->GetHostname() ).ToStdString();
            m_lock.reset();
            mgr.UnloadProject( m_project, false );
            throw TOOL_ERROR( "The project " + m_name + " is open in another KiCad (" + who
                                      + "). Close it there, or pass force:true to take it over.",
                              nlohmann::json{ { "locked", true }, { "by", who } } );
        }

        m_lock->OverrideLock();
        m_project->SetReadOnly( false );
    }

    m_proVersion = m_files.Version( m_proFile );
    loadSettings();
}


AGENT_PROJECT::~AGENT_PROJECT()
{
    while( !m_sessions.empty() )
        CloseSession( m_sessions.back().get() );

    if( m_libraries )
    {
        m_libraries->AbortAsyncLoads( true );
        m_libraries.reset();
    }

    m_lock.reset();

    if( m_project )
        Pgm().GetSettingsManager().UnloadProject( m_project, false );
}


LIBRARY_MANAGER& AGENT_PROJECT::Libraries()
{
    if( !m_libraries )
    {
        m_libraries = std::make_unique<LIBRARY_MANAGER>( m_project );

        for( const auto& [type, factory] : m_adapterFactories )
            m_libraries->RegisterAdapter( type, factory( *m_libraries ) );

        m_libraries->LoadGlobalTables();
        m_libraries->LoadProjectTables( wxString::FromUTF8( Root().string() ) );
    }

    return *m_libraries;
}


LIBRARY_MANAGER_ADAPTER* AGENT_PROJECT::LibraryAdapter( LIBRARY_TABLE_TYPE aType )
{
    std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = Libraries().Adapter( aType );
    return adapter ? *adapter : nullptr;
}


void AGENT_PROJECT::ReloadLibraryTables()
{
    if( !m_libraries )
        return;

    m_libraries->AbortAsyncLoads( true );
    m_libraries->LoadProjectTables( wxString::FromUTF8( Root().string() ) );
}


void AGENT_PROJECT::StopLibraryLoads()
{
    if( m_libraries )
        m_libraries->AbortAsyncLoads( true );
}


void AGENT_PROJECT::ActivateGlobalLibraryTables( LIBRARY_TABLE_TYPE aType )
{
    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
    manager.AbortAsyncLoads();
    manager.LoadProjectTables( wxString::FromUTF8( Root().string() ), { aType } );

    if( std::optional<LIBRARY_MANAGER_ADAPTER*> adapter = manager.Adapter( aType ) )
    {
        ( *adapter )->AsyncLoad();
        ( *adapter )->BlockUntilLoaded();
    }
}


bool AGENT_PROJECT::IsInsideProject( const wxString& aUri )
{
    const wxString  expanded = LIBRARY_MANAGER::ExpandURI( aUri, *m_project );
    std::error_code ec;
    const fs::path  path = fs::weakly_canonical( fs::path( expanded.ToStdString( wxConvUTF8 ) ), ec );

    if( ec )
        return false;

    auto [rootEnd, pathIt] = std::mismatch( Root().begin(), Root().end(), path.begin(), path.end() );
    return rootEnd == Root().end();
}


void AGENT_PROJECT::loadSettings()
{
    const fs::path path = m_files.Root() / SETTINGS_FILE;

    if( !fs::exists( path ) )
        return;

    try
    {
        m_settings = nlohmann::json::parse( FILE_STORE::ReadFile( path ) );

        if( !m_settings.is_object() )
            m_settings = nlohmann::json::object();
    }
    catch( const std::exception& )
    {
        m_settings = nlohmann::json::object();
    }
}


void AGENT_PROJECT::SetSettings( const nlohmann::json& aSettings )
{
    m_settings = aSettings;
    FILE_STORE::WriteAtomic( m_files.Root() / SETTINGS_FILE, m_settings.dump( 2 ) + "\n" );
}


DOC_SESSION* AGENT_PROJECT::FindSession( const std::string& aProgram, const std::string& aPath ) const
{
    for( const auto& session : m_sessions )
    {
        if( session->Program() == aProgram && session->Path() == aPath )
            return session.get();
    }

    return nullptr;
}


DOC_SESSION* AGENT_PROJECT::SessionForFile( const std::string& aRelative ) const
{
    for( const auto& session : m_sessions )
    {
        const std::vector<std::string> files = session->Files();

        if( std::find( files.begin(), files.end(), aRelative ) != files.end() )
            return session.get();
    }

    return nullptr;
}


std::vector<DOC_SESSION*> AGENT_PROJECT::Sessions() const
{
    std::vector<DOC_SESSION*> out;

    for( const auto& session : m_sessions )
        out.push_back( session.get() );

    return out;
}


DOC_SESSION& AGENT_PROJECT::AddSession( std::unique_ptr<DOC_SESSION> aSession )
{
    DOC_SESSION& session = *aSession;
    m_sessions.push_back( std::move( aSession ) );

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnDocumentOpened( session );
            } );

    return session;
}


void AGENT_PROJECT::CloseSession( DOC_SESSION* aSession )
{
    auto it = std::find_if( m_sessions.begin(), m_sessions.end(),
                            [&]( const std::unique_ptr<DOC_SESSION>& aEntry )
                            {
                                return aEntry.get() == aSession;
                            } );

    if( it == m_sessions.end() )
        return;

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnDocumentClosing( *aSession );
            } );

    m_touched.erase( aSession );
    m_sessions.erase( it );
}


void AGENT_PROJECT::Touch( DOC_SESSION& aSession )
{
    m_touched.insert( &aSession );
}


void AGENT_PROJECT::BeforeFileWrite( const std::string& aRelative )
{
    m_history.Before( aRelative );
}


void AGENT_PROJECT::AfterFileWrite( const std::string& aRelative )
{
    m_history.After( aRelative );
    m_filesWritten = true;
}


nlohmann::json AGENT_PROJECT::saveProjectFile()
{
    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();

    m_history.Before( m_proFile );

    if( !mgr.SaveProject( m_project->GetProjectFullName(), m_project ) )
        throw TOOL_ERROR( "Saving the project file " + m_proFile + " failed." );

    m_history.After( m_proFile );
    m_proVersion = m_files.Version( m_proFile );
    return nlohmann::json{ { "path", m_proFile }, { "version", m_proVersion } };
}


nlohmann::json AGENT_PROJECT::Mutate( const std::string& aLabel, bool aForce,
                                      const std::function<nlohmann::json()>& aChange )
{
    // A mutating tool called from inside another is part of the outer call.
    if( m_mutationDepth > 0 )
        return aChange();

    m_mutationDepth++;
    m_history.Begin( aLabel );
    m_touched.clear();
    m_projectFileTouched = false;
    m_filesWritten = false;

    nlohmann::json result;

    auto restoreAll = [&]()
    {
        std::set<DOC_SESSION*> touched;
        touched.swap( m_touched );

        for( DOC_SESSION* session : touched )
            session->Reload();

        if( m_projectFileTouched )
            m_project->GetProjectFile().LoadFromFile( wxString::FromUTF8( Root().string() ) );

        m_projectFileTouched = false;
    };

    try
    {
        result = aChange();

        if( !aForce )
        {
            for( DOC_SESSION* session : m_touched )
                session->CheckConflicts();

            if( m_projectFileTouched && m_files.Version( m_proFile ) != m_proVersion )
            {
                throw TOOL_ERROR( "The project file was changed outside the agent. Nothing was saved. Call "
                                  "project_open to reload, or repeat with force:true.",
                                  nlohmann::json{ { "conflict", true }, { "path", m_proFile } } );
            }
        }

        nlohmann::json saved = nlohmann::json::array();

        for( DOC_SESSION* session : m_touched )
        {
            for( const nlohmann::json& file : session->Save() )
                saved.push_back( file );
        }

        if( m_projectFileTouched )
            saved.push_back( saveProjectFile() );

        if( !result.is_object() )
            result = nlohmann::json{ { "result", result } };

        if( !saved.empty() )
            result["saved"] = saved;
    }
    catch( ... )
    {
        m_history.Abandon();
        m_mutationDepth--;
        restoreAll();
        throw;
    }

    const bool changed = !m_touched.empty() || m_projectFileTouched || m_filesWritten;

    m_touched.clear();
    m_projectFileTouched = false;
    m_history.End();
    m_mutationDepth--;

    if( changed )
    {
        m_observers.Notify(
                [&]( SESSION_OBSERVER& aObserver )
                {
                    aObserver.OnFilesChanged( *this );
                } );
    }

    return result;
}


void AGENT_PROJECT::reloadAfterRestore( const FILE_HISTORY::STEP& aStep )
{
    std::set<DOC_SESSION*> sessions;

    for( const FILE_HISTORY::CHANGE& change : aStep.changes )
    {
        if( change.path == "sym-lib-table" || change.path == "fp-lib-table" || change.path == "design-block-lib-table" )
        {
            ReloadLibraryTables();
        }
        else if( change.path == m_proFile )
        {
            m_project->GetProjectFile().LoadFromFile( wxString::FromUTF8( Root().string() ) );
            m_proVersion = m_files.Version( m_proFile );
        }
        else if( DOC_SESSION* session = SessionForFile( change.path ) )
        {
            sessions.insert( session );
        }
    }

    for( DOC_SESSION* session : sessions )
        session->Reload();

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnFilesChanged( *this );
            } );
}


nlohmann::json AGENT_PROJECT::Undo( bool aForce )
{
    FILE_HISTORY::STEP step = m_history.Undo( aForce );
    reloadAfterRestore( step );
    nlohmann::json out = step.ToJson();
    out["canUndo"] = m_history.CanUndo();
    out["canRedo"] = m_history.CanRedo();
    return nlohmann::json{ { "undone", out } };
}


nlohmann::json AGENT_PROJECT::Redo( bool aForce )
{
    FILE_HISTORY::STEP step = m_history.Redo( aForce );
    reloadAfterRestore( step );
    nlohmann::json out = step.ToJson();
    out["canUndo"] = m_history.CanUndo();
    out["canRedo"] = m_history.CanRedo();
    return nlohmann::json{ { "redone", out } };
}


nlohmann::json AGENT_PROJECT::Info() const
{
    nlohmann::json sessions = nlohmann::json::array();

    for( const auto& session : m_sessions )
    {
        sessions.push_back( { { "program", session->Program() },
                              { "path", session->Path() },
                              { "versions", session->Versions() } } );
    }

    return nlohmann::json{ { "name", m_name },
                           { "root", Root().string() },
                           { "projectFile", m_proFile },
                           { "readOnly", m_project->IsReadOnly() },
                           { "documents", sessions },
                           { "canUndo", m_history.CanUndo() },
                           { "canRedo", m_history.CanRedo() },
                           { "settings", m_settings } };
}

} // namespace AGENTIC
