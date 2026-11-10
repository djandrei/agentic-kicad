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
#include <agentic/core/file_store.h>
#include <agentic/pcb/footprint_lib_session.h>
#include <agentic/protocol/registry.h>

#include <board.h>
#include <footprint.h>
#include <ki_exception.h>
#include <libraries/library_manager.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <locale_io.h>

#include <wx/filename.h>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }

} // namespace


FOOTPRINT_LIB_SESSION::FOOTPRINT_LIB_SESSION( AGENT_PROJECT& aProject, const std::string& aPath ) :
        DOC_SESSION( aProject, "fp", aPath )
{
}


FOOTPRINT_LIB_SESSION::~FOOTPRINT_LIB_SESSION()
{
    // Footprints point at the holder; drop them first.
    m_footprints.clear();
}


std::vector<std::string> FOOTPRINT_LIB_SESSION::Files() const
{
    return std::vector<std::string>( m_known.begin(), m_known.end() );
}


std::string FOOTPRINT_LIB_SESSION::LibraryName() const
{
    return fs::path( Path() ).stem().string();
}


std::vector<std::string> FOOTPRINT_LIB_SESSION::Names() const
{
    std::vector<std::string> names;

    for( const auto& [name, footprint] : m_footprints )
        names.push_back( name );

    return names;
}


bool FOOTPRINT_LIB_SESSION::Has( const std::string& aName ) const
{
    return m_footprints.count( aName ) > 0;
}


FOOTPRINT& FOOTPRINT_LIB_SESSION::Footprint( const std::string& aName ) const
{
    auto it = m_footprints.find( aName );

    if( it == m_footprints.end() )
        throw TOOL_ERROR( "No footprint '" + aName + "' in " + Path() + ". fp_list lists them." );

    return *it->second;
}


std::string FOOTPRINT_LIB_SESSION::FileOf( const std::string& aName ) const
{
    return ( fs::path( Path() ) / ( aName + ".kicad_mod" ) ).generic_string();
}


void FOOTPRINT_LIB_SESSION::Add( std::unique_ptr<FOOTPRINT> aFootprint )
{
    const std::string name = utf8( aFootprint->GetFPID().GetLibItemName().wx_str() );
    aFootprint->SetParent( m_holder.get() );
    m_footprints[name] = std::move( aFootprint );
    m_known.insert( FileOf( name ) );
    MarkModified( FileOf( name ) );
}


void FOOTPRINT_LIB_SESSION::Remove( const std::string& aName )
{
    Footprint( aName );
    m_footprints.erase( aName );
    MarkModified( FileOf( aName ) );

    if( m_current == aName )
        m_current.clear();
}


void FOOTPRINT_LIB_SESSION::Touch( const std::string& aName )
{
    Footprint( aName );
    MarkModified( FileOf( aName ) );
}


void FOOTPRINT_LIB_SESSION::load()
{
    const fs::path dir = Project().Files().Resolve( Path() );

    if( !fs::is_directory( dir ) )
        throw TOOL_ERROR( "The footprint library " + Path() + " does not exist. Make one with lib_create." );

    auto                                              holder = std::make_unique<BOARD>();
    std::map<std::string, std::unique_ptr<FOOTPRINT>> footprints;
    PCB_IO_KICAD_SEXPR                                io;

    holder->SetBoardUse( BOARD_USE::FPHOLDER );

    for( const fs::directory_entry& entry : fs::directory_iterator( dir ) )
    {
        if( entry.path().extension() != ".kicad_mod" )
            continue;

        wxString                   name;
        std::unique_ptr<FOOTPRINT> footprint;

        try
        {
            footprint = io.ImportFootprint( wxString::FromUTF8( entry.path().string() ), name );
        }
        catch( const IO_ERROR& e )
        {
            throw TOOL_ERROR( "KiCad could not read " + entry.path().filename().string() + ": " + utf8( e.What() )
                              + ". The library is not opened, so saving cannot lose it." );
        }

        if( !footprint )
            throw TOOL_ERROR( "KiCad could not read " + entry.path().filename().string() + "." );

        // The file name is the footprint's name in a .pretty library.
        const std::string fileName = entry.path().stem().string();
        footprint->SetFPID( LIB_ID( wxString::FromUTF8( LibraryName() ), wxString::FromUTF8( fileName ) ) );
        footprint->SetParent( holder.get() );
        m_known.insert( FileOf( fileName ) );
        footprints[fileName] = std::move( footprint );
    }

    m_footprints.clear();
    m_footprints = std::move( footprints );
    m_holder = std::move( holder );

    if( !m_current.empty() && !Has( m_current ) )
        m_current.clear();
}


void FOOTPRINT_LIB_SESSION::save( const std::vector<std::string>& aFiles )
{
    for( const std::string& file : aFiles )
    {
        const fs::path    path = Project().Files().Resolve( file );
        const std::string name = path.stem().string();
        auto              it = m_footprints.find( name );

        if( it == m_footprints.end() )
        {
            std::error_code ec;
            fs::remove( path, ec );

            if( ec )
                throw TOOL_ERROR( "Deleting " + file + " failed: " + ec.message() );

            continue;
        }

        // Formatting changes the copy's embedded fonts; format a copy (pcb_io_kicad_sexpr.h).
        std::unique_ptr<FOOTPRINT> copy( static_cast<FOOTPRINT*>( it->second->Clone() ) );
        copy->SetParent( nullptr );

        PCB_IO_KICAD_SEXPR io;
        std::string        text;

        try
        {
            LOCALE_IO toggle;
            io.FormatFootprintLibFile( copy.get() );
            text = io.GetStringOutput( true );
        }
        catch( const IO_ERROR& e )
        {
            throw TOOL_ERROR( "Writing " + file + " failed: " + utf8( e.What() ) );
        }

        FILE_STORE::WriteAtomic( path, text );
    }
}


FOOTPRINT_LIB_SESSION& FootprintLibSession( AGENT_PROJECT& aProject, const std::string& aLibrary )
{
    std::string path;

    if( fs::path( aLibrary ).extension() != ".pretty" )
    {
        std::optional<LIBRARY_TABLE_ROW*> row =
                aProject.Libraries().GetRow( LIBRARY_TABLE_TYPE::FOOTPRINT, wxString::FromUTF8( aLibrary ) );

        if( !row )
            throw TOOL_ERROR( "No footprint library '" + aLibrary + "'. Make one with lib_create, or see lib_tables." );

        if( !aProject.IsInsideProject( ( *row )->URI() ) )
            throw TOOL_ERROR( "'" + aLibrary
                              + "' is outside the project folder, so the agent cannot change it. Copy the "
                                "footprint into a project library (fp_create with copyFrom)." );

        const wxString full = LIBRARY_MANAGER::ExpandURI( ( *row )->URI(), aProject.KicadProject() );
        path = aProject.Files().Relative( fs::path( utf8( full ) ) );
    }
    else
    {
        path = aProject.Files().Relative( aProject.Files().Resolve( aLibrary ) );
    }

    if( DOC_SESSION* existing = aProject.FindSession( "fp", path ) )
        return static_cast<FOOTPRINT_LIB_SESSION&>( *existing );

    auto session = std::make_unique<FOOTPRINT_LIB_SESSION>( aProject, path );
    session->Open( false );
    return static_cast<FOOTPRINT_LIB_SESSION&>( aProject.AddSession( std::move( session ) ) );
}

} // namespace AGENTIC
