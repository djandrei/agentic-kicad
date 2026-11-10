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
#include <agentic/core/doc_session.h>
#include <agentic/core/file_store.h>
#include <agentic/protocol/registry.h>

#include <lockfile.h>

#include <wx/string.h>


namespace AGENTIC
{

DOC_SESSION::DOC_SESSION( AGENT_PROJECT& aProject, std::string aProgram, std::string aPath ) :
        m_project( aProject ),
        m_program( std::move( aProgram ) ),
        m_path( std::move( aPath ) )
{
}


DOC_SESSION::~DOC_SESSION() = default;


void DOC_SESSION::Open( bool aForce )
{
    const std::filesystem::path absolute = m_project.Files().Resolve( m_path );

    m_lock = std::make_unique<LOCKFILE>( wxString::FromUTF8( absolute.string() ) );

    if( !m_lock->Valid() )
    {
        if( !aForce )
        {
            const std::string who = ( m_lock->GetUsername() + wxS( "@" ) + m_lock->GetHostname() ).ToStdString();
            m_lock.reset();
            throw TOOL_ERROR( "'" + m_path + "' is open in another KiCad (" + who
                                      + "). Close it there, or pass force:true to take it over.",
                              nlohmann::json{ { "locked", true }, { "path", m_path }, { "by", who } } );
        }

        m_lock->OverrideLock();
    }

    load();
    refreshVersions();
}


void DOC_SESSION::MarkModified( const std::string& aFile )
{
    m_modified.insert( aFile.empty() ? m_path : aFile );
    m_project.Touch( *this );
}


void DOC_SESSION::refreshVersions()
{
    m_versions.clear();

    for( const std::string& file : Files() )
        m_versions[file] = m_project.Files().Version( file );
}


std::string DOC_SESSION::Version( const std::string& aFile ) const
{
    auto it = m_versions.find( aFile );
    return it == m_versions.end() ? std::string() : it->second;
}


nlohmann::json DOC_SESSION::Versions() const
{
    nlohmann::json out = nlohmann::json::object();

    for( const auto& [file, version] : m_versions )
        out[file] = version;

    return out;
}


void DOC_SESSION::CheckConflicts() const
{
    for( const std::string& file : m_modified )
    {
        const std::string onDisk = m_project.Files().Version( file );
        auto              known = m_versions.find( file );

        // A file new to the document (a new sheet) is not a conflict when it does not exist yet.
        if( known == m_versions.end() && onDisk.empty() )
            continue;

        if( known == m_versions.end() || known->second != onDisk )
        {
            throw TOOL_ERROR( "'" + file
                                      + "' was changed outside the agent since it was loaded. Nothing was "
                                        "saved and the change was dropped. Call project_open to reload, or repeat the "
                                        "call with force:true to overwrite the other change.",
                              nlohmann::json{ { "conflict", true }, { "path", file }, { "version", onDisk } } );
        }
    }
}


nlohmann::json DOC_SESSION::Save()
{
    nlohmann::json saved = nlohmann::json::array();

    if( m_modified.empty() )
        return saved;

    std::vector<std::string> files( m_modified.begin(), m_modified.end() );

    for( const std::string& file : files )
        m_project.History().Before( file );

    try
    {
        save( files );
    }
    catch( const std::exception& e )
    {
        m_modified.clear();
        Reload();
        throw TOOL_ERROR( std::string( "Saving failed, so the change was undone in memory too: " ) + e.what() );
    }

    m_modified.clear();
    refreshVersions();

    for( const std::string& file : files )
    {
        m_project.History().After( file );
        saved.push_back( { { "path", file }, { "version", Version( file ) } } );
    }

    m_project.Observers().Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnDocumentSaved( *this );
            } );

    return saved;
}


void DOC_SESSION::Reload()
{
    m_project.Observers().Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnDocumentReloading( *this );
            } );

    m_modified.clear();
    load();
    refreshVersions();

    m_project.Observers().Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnDocumentReloaded( *this );
            } );
}


void DOC_SESSION::RecordChange( const nlohmann::json& aChange )
{
    m_changes.push_back( aChange );

    while( m_changes.size() > MAX_CHANGES )
        m_changes.pop_front();

    m_project.Observers().Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnCommitApplied( *this, aChange );
            } );
}

} // namespace AGENTIC
