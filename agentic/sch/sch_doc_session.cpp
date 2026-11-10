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
#include <agentic/sch/sch_doc_session.h>

#include <api/sch_api_save.h>
#include <eeschema_helpers.h>
#include <project.h>
#include <project/project_file.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <tool/tool_manager.h>
#include <kiface_base.h>

#include <wx/filename.h>


namespace AGENTIC
{

namespace fs = std::filesystem;


SCH_DOC_SESSION::SCH_DOC_SESSION( AGENT_PROJECT& aProject, const std::string& aRootPath ) :
        DOC_SESSION( aProject, "sch", aRootPath )
{
}


SCH_DOC_SESSION::~SCH_DOC_SESSION()
{
    m_toolManager.reset();

    if( m_schematic )
        m_schematic->SetProject( nullptr );

    m_schematic.reset();
}


void SCH_DOC_SESSION::load()
{
    const fs::path absolute = Project().Files().Resolve( Path() );
    const wxString file = wxString::FromUTF8( absolute.string() );

    m_toolManager.reset();

    if( m_schematic )
        m_schematic->SetProject( nullptr );

    m_schematic.reset();

    if( fs::exists( absolute ) )
    {
        SCHEMATIC* loaded = EESCHEMA_HELPERS::LoadSchematic( file, false, false, &Project().KicadProject(), true );

        if( !loaded )
            throw TOOL_ERROR( "KiCad could not load the schematic " + Path() + "." );

        m_schematic.reset( loaded );
        m_isNew = false;
    }
    else
    {
        m_schematic = std::make_unique<SCHEMATIC>( &Project().KicadProject() );
        m_schematic->CreateDefaultScreens();
        m_schematic->RootScreen()->SetFileName( file );
        m_isNew = true;
    }

    m_toolManager = std::make_unique<TOOL_MANAGER>();
    m_toolManager->SetEnvironment( m_schematic.get(), nullptr, nullptr, Kiface().KifaceSettings(), nullptr );
}


std::vector<std::string> SCH_DOC_SESSION::Files() const
{
    std::vector<std::string> files;

    if( !m_schematic )
        return { Path() };

    SCH_SCREENS screens( m_schematic->Root() );

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        std::string file = FileOf( screens.GetScreen( i ) );

        if( !file.empty() && std::find( files.begin(), files.end(), file ) == files.end() )
            files.push_back( file );
    }

    // Root first.
    auto root = std::find( files.begin(), files.end(), Path() );

    if( root != files.end() )
        std::rotate( files.begin(), root, root + 1 );
    else
        files.insert( files.begin(), Path() );

    return files;
}


std::string SCH_DOC_SESSION::FileOf( const SCH_SCREEN* aScreen ) const
{
    if( !aScreen || aScreen->GetFileName().IsEmpty() )
        return std::string();

    wxFileName fn( aScreen->GetFileName() );

    if( !fn.IsAbsolute() )
        fn.MakeAbsolute( wxString::FromUTF8( Project().Root().string() ) );

    return Project().Files().Relative( fs::path( fn.GetFullPath().ToStdString( wxConvUTF8 ) ).lexically_normal() );
}


void SCH_DOC_SESSION::Touch( const SCH_SCREEN* aScreen )
{
    const std::string file = FileOf( aScreen );
    MarkModified( file.empty() ? Path() : file );
}


SCH_SHEET_PATH SCH_DOC_SESSION::CurrentSheet() const
{
    SCH_SHEET_LIST hierarchy = m_schematic->Hierarchy();

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        KIID_PATH kiids = path.Path();

        if( std::vector<KIID>( kiids.begin(), kiids.end() ) == m_currentSheet )
            return path;
    }

    return hierarchy.empty() ? SCH_SHEET_PATH() : hierarchy.front();
}


void SCH_DOC_SESSION::SetCurrentSheet( const SCH_SHEET_PATH& aPath )
{
    KIID_PATH kiids = aPath.Path();
    m_currentSheet.assign( kiids.begin(), kiids.end() );
}


void SCH_DOC_SESSION::HierarchyChanged()
{
    m_schematic->RefreshHierarchy();

    // The project file lists the sheets (SCH_API_SAVE::UpdateProjectFile does this and saves
    // the active project; here the project is passive and Mutate() saves it).
    PROJECT_FILE&                projectFile = Project().KicadProject().GetProjectFile();
    std::vector<FILE_INFO_PAIR>& sheets = projectFile.GetSheets();
    sheets.clear();

    for( SCH_SHEET_PATH& sheetPath : m_schematic->Hierarchy() )
    {
        SCH_SHEET* sheet = sheetPath.Last();

        if( sheet && !sheet->IsVirtualRootSheet() )
            sheets.emplace_back( std::make_pair( sheet->m_Uuid, sheet->GetName() ) );
    }

    Project().TouchProjectFile();
}


void SCH_DOC_SESSION::save( const std::vector<std::string>& aFiles )
{
    SCH_SCREENS screens( m_schematic->Root() );
    screens.BuildClientSheetPathList();

    for( const std::string& file : aFiles )
    {
        SCH_SHEET* sheet = nullptr;

        for( size_t i = 0; i < screens.GetCount() && !sheet; i++ )
        {
            if( FileOf( screens.GetScreen( i ) ) == file )
                sheet = screens.GetSheet( i );
        }

        if( !sheet )
            throw TOOL_ERROR( "No sheet of the schematic is stored in " + file + "." );

        const wxString path = wxString::FromUTF8( Project().Files().Resolve( file ).string() );

        if( !SCH_API_SAVE::SaveSheetToFile( sheet, *m_schematic, path ) )
            throw TOOL_ERROR( "Writing " + file + " failed." );
    }

    m_isNew = false;
}


SCH_DOC_SESSION& SchematicSession( AGENT_PROJECT& aProject, const std::string& aPath )
{
    std::string path = aPath;

    if( path.empty() )
    {
        for( DOC_SESSION* session : aProject.Sessions() )
        {
            if( session->Program() == "sch" )
                return static_cast<SCH_DOC_SESSION&>( *session );
        }

        path = aProject.Name() + ".kicad_sch";
    }
    else
    {
        path = aProject.Files().Relative( aProject.Files().Resolve( path ) );
    }

    if( DOC_SESSION* existing = aProject.FindSession( "sch", path ) )
        return static_cast<SCH_DOC_SESSION&>( *existing );

    // A sheet file of an open schematic opens that schematic.
    if( DOC_SESSION* owner = aProject.SessionForFile( path ); owner && owner->Program() == "sch" )
        return static_cast<SCH_DOC_SESSION&>( *owner );

    if( fs::path( path ).extension() != ".kicad_sch" )
        throw TOOL_ERROR( "A schematic is a .kicad_sch file; got '" + path + "'." );

    auto session = std::make_unique<SCH_DOC_SESSION>( aProject, path );
    session->Open( false );
    return static_cast<SCH_DOC_SESSION&>( aProject.AddSession( std::move( session ) ) );
}

} // namespace AGENTIC
