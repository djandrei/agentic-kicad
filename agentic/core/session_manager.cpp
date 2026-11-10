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


#include <agentic/core/session_manager.h>
#include <agentic/protocol/registry.h>

#include <pgm_base.h>
#include <project.h>
#include <settings/settings_manager.h>

#include <wx/utils.h>

#include <algorithm>


namespace AGENTIC
{

namespace fs = std::filesystem;


SESSION_MANAGER::~SESSION_MANAGER()
{
    while( !m_projects.empty() )
        Close( *m_projects.back() );
}


AGENT_PROJECT& SESSION_MANAGER::Open( const fs::path& aPath, bool aForce )
{
    std::error_code ec;
    fs::path        path = fs::weakly_canonical( aPath, ec );

    if( ec || path.empty() )
        throw TOOL_ERROR( "Cannot resolve the path " + aPath.string() + "." );

    fs::path proFile;

    if( fs::is_directory( path ) )
    {
        std::vector<fs::path> found;

        for( const fs::directory_entry& entry : fs::directory_iterator( path, ec ) )
        {
            if( entry.is_regular_file() && entry.path().extension() == ".kicad_pro" )
                found.push_back( entry.path() );
        }

        if( found.empty() )
            throw TOOL_ERROR( "No .kicad_pro in " + path.string() + ". Use project_create to make a project there." );

        if( found.size() > 1 )
            throw TOOL_ERROR( "Several .kicad_pro files in " + path.string() + "; give the one to open." );

        proFile = found.front();
    }
    else if( path.extension() == ".kicad_pro" && fs::exists( path ) )
    {
        proFile = path;
    }
    else
    {
        throw TOOL_ERROR( path.string() + " is neither a project folder nor a .kicad_pro file." );
    }

    for( const auto& project : m_projects )
    {
        if( project->Root() == proFile.parent_path() )
        {
            activate( project.get() );
            return *project;
        }
    }

    auto           project = std::make_unique<AGENT_PROJECT>( proFile, m_observers, m_adapterFactories, aForce );
    AGENT_PROJECT* opened = project.get();
    m_projects.push_back( std::move( project ) );
    activate( opened );

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnProjectOpened( *m_current );
            } );

    return *m_current;
}


AGENT_PROJECT& SESSION_MANAGER::Create( const fs::path& aFolder, const std::string& aName )
{
    std::error_code ec;
    fs::path        folder = fs::weakly_canonical( aFolder, ec );

    if( ec || folder.empty() || !folder.is_absolute() )
        throw TOOL_ERROR( "project_create needs an absolute folder path; got " + aFolder.string() + "." );

    const std::string name = aName.empty() ? folder.filename().string() : aName;

    if( name.empty() || name.find_first_of( "/\\:*?\"<>|" ) != std::string::npos )
        throw TOOL_ERROR( "'" + name + "' is not a valid project name." );

    fs::create_directories( folder, ec );

    if( ec )
        throw TOOL_ERROR( "Cannot create " + folder.string() + ": " + ec.message() );

    for( const fs::directory_entry& entry : fs::directory_iterator( folder, ec ) )
    {
        if( entry.path().extension() == ".kicad_pro" )
            throw TOOL_ERROR( folder.string() + " already holds a project (" + entry.path().filename().string()
                              + "); use project_open." );
    }

    const fs::path    proFile = folder / ( name + ".kicad_pro" );
    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    const wxString    fullPath = wxString::FromUTF8( proFile.string() );

    // A load of a missing project gives defaults; saving it writes the new .kicad_pro.
    mgr.LoadProject( fullPath, false );
    PROJECT* kicadProject = mgr.GetProject( fullPath );

    if( !kicadProject || !mgr.SaveProject( fullPath, kicadProject ) )
    {
        if( kicadProject )
            mgr.UnloadProject( kicadProject, false );

        throw TOOL_ERROR( "Cannot write " + proFile.string() + "." );
    }

    mgr.UnloadProject( kicadProject, false );
    return Open( proFile, false );
}


void SESSION_MANAGER::Close( AGENT_PROJECT& aProject )
{
    auto it = std::find_if( m_projects.begin(), m_projects.end(),
                            [&]( const std::unique_ptr<AGENT_PROJECT>& aEntry )
                            {
                                return aEntry.get() == &aProject;
                            } );

    if( it == m_projects.end() )
        return;

    m_observers.Notify(
            [&]( SESSION_OBSERVER& aObserver )
            {
                aObserver.OnProjectClosed( aProject );
            } );

    std::unique_ptr<AGENT_PROJECT> closing = std::move( *it );
    m_projects.erase( it );

    if( m_current == closing.get() )
    {
        m_current = nullptr;
        activate( m_projects.empty() ? nullptr : m_projects.back().get() );
    }
}


AGENT_PROJECT* SESSION_MANAGER::Find( const std::string& aNameOrPath ) const
{
    std::error_code ec;
    const fs::path  asPath = fs::weakly_canonical( fs::path( aNameOrPath ), ec );
    AGENT_PROJECT*  byName = nullptr;
    int             nameMatches = 0;

    for( const auto& project : m_projects )
    {
        if( !ec && ( project->Root() == asPath || project->Root() / project->ProFile() == asPath ) )
            return project.get();

        if( project->Name() == aNameOrPath )
        {
            byName = project.get();
            nameMatches++;
        }
    }

    if( nameMatches > 1 )
        throw TOOL_ERROR( "Several open projects are named '" + aNameOrPath + "'; give the folder instead." );

    return byName;
}


AGENT_PROJECT& SESSION_MANAGER::Resolve( const nlohmann::json& aArgs )
{
    if( aArgs.is_object() && aArgs.contains( "project" ) && aArgs["project"].is_string() )
    {
        const std::string wanted = aArgs["project"].get<std::string>();

        if( AGENT_PROJECT* project = Find( wanted ) )
        {
            activate( project );
            return *project;
        }

        throw TOOL_ERROR( "No open project '" + wanted + "'. Open it with project_open, or see project_list." );
    }

    if( !m_current )
        throw TOOL_ERROR( "No project is open. Call project_open with a project folder, or project_create." );

    return *m_current;
}


void SESSION_MANAGER::AddLibraryAdapter( LIBRARY_TABLE_TYPE aType, ADAPTER_FACTORY aFactory )
{
    m_adapterFactories.emplace_back( aType, std::move( aFactory ) );
}


void SESSION_MANAGER::activate( AGENT_PROJECT* aProject )
{
    if( aProject == m_current && aProject )
        return;

    for( const auto& project : m_projects )
    {
        if( project.get() != aProject )
            project->StopLibraryLoads();
    }

    m_current = aProject;

    if( aProject )
        wxSetEnv( PROJECT_VAR_NAME, wxString::FromUTF8( aProject->Root().string() ) );
    else
        wxUnsetEnv( PROJECT_VAR_NAME );
}


std::vector<AGENT_PROJECT*> SESSION_MANAGER::Projects() const
{
    std::vector<AGENT_PROJECT*> out;

    for( const auto& project : m_projects )
        out.push_back( project.get() );

    return out;
}

} // namespace AGENTIC
