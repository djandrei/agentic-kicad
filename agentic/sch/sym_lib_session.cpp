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
#include <agentic/sch/sym_lib_session.h>

#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <ki_exception.h>

#include <wx/filename.h>


namespace AGENTIC
{

namespace fs = std::filesystem;


SYMBOL_LIB_SESSION::SYMBOL_LIB_SESSION( AGENT_PROJECT& aProject, const std::string& aPath ) :
        DOC_SESSION( aProject, "sym", aPath )
{
}


SYMBOL_LIB_SESSION::~SYMBOL_LIB_SESSION() = default;


void SYMBOL_LIB_SESSION::load()
{
    const fs::path path = Project().Files().Resolve( Path() );
    auto           library = std::make_unique<SCH_IO_KICAD_SEXPR_LIB_CACHE>( wxString::FromUTF8( path.string() ) );

    if( fs::exists( path ) )
    {
        try
        {
            library->Load();
        }
        catch( const IO_ERROR& e )
        {
            throw TOOL_ERROR( "KiCad could not read the symbol library " + Path() + ": " + e.What().ToStdString() );
        }

        if( library->HasParseError() )
            throw TOOL_ERROR( "The symbol library " + Path()
                              + " has a syntax error; it is not opened, so saving "
                                "cannot lose the symbols after the error." );
    }

    m_library = std::move( library );
}


void SYMBOL_LIB_SESSION::save( const std::vector<std::string>& aFiles )
{
    try
    {
        m_library->SetModified();
        m_library->Save();
    }
    catch( const IO_ERROR& e )
    {
        throw TOOL_ERROR( "Writing " + Path() + " failed: " + e.What().ToStdString() );
    }
}


LIB_SYMBOL& SYMBOL_LIB_SESSION::Symbol( const std::string& aName ) const
{
    LIB_SYMBOL* symbol = m_library->GetSymbol( wxString::FromUTF8( aName ) );

    if( !symbol )
        throw TOOL_ERROR( "No symbol '" + aName + "' in " + Path() + "." );

    return *symbol;
}


void SYMBOL_LIB_SESSION::Add( std::unique_ptr<LIB_SYMBOL> aSymbol )
{
    m_library->AddSymbol( std::move( aSymbol ) );
}


SYMBOL_LIB_SESSION& SymbolLibSession( AGENT_PROJECT& aProject, const std::string& aLibrary )
{
    std::string path = aLibrary;

    if( fs::path( aLibrary ).extension() != ".kicad_sym" )
    {
        // A nickname: the project table row's file, which must be inside the project.
        std::optional<LIBRARY_TABLE_ROW*> row =
                aProject.Libraries().GetRow( LIBRARY_TABLE_TYPE::SYMBOL, wxString::FromUTF8( aLibrary ) );

        if( !row )
            throw TOOL_ERROR( "No symbol library '" + aLibrary + "'. Make one with lib_create, or see lib_tables." );

        if( !aProject.IsInsideProject( ( *row )->URI() ) )
            throw TOOL_ERROR( "'" + aLibrary
                              + "' is outside the project folder, so the agent cannot change it. Copy "
                                "the symbol into a project library." );

        const wxString full = LIBRARY_MANAGER::ExpandURI( ( *row )->URI(), aProject.KicadProject() );
        path = aProject.Files().Relative( fs::path( full.ToStdString( wxConvUTF8 ) ) );
    }
    else
    {
        path = aProject.Files().Relative( aProject.Files().Resolve( aLibrary ) );
    }

    if( DOC_SESSION* existing = aProject.FindSession( "sym", path ) )
        return static_cast<SYMBOL_LIB_SESSION&>( *existing );

    auto session = std::make_unique<SYMBOL_LIB_SESSION>( aProject, path );
    session->Open( false );
    return static_cast<SYMBOL_LIB_SESSION&>( aProject.AddSession( std::move( session ) ) );
}

} // namespace AGENTIC
