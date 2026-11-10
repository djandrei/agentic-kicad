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
#include <agentic/pcb/board_doc_session.h>
#include <agentic/protocol/registry.h>

#include <board.h>
#include <board_loader.h>
#include <connectivity/connectivity_data.h>
#include <ki_exception.h>
#include <kiface_base.h>
#include <tool/tool_manager.h>


namespace AGENTIC
{

namespace fs = std::filesystem;


BOARD_DOC_SESSION::BOARD_DOC_SESSION( AGENT_PROJECT& aProject, const std::string& aPath ) :
        DOC_SESSION( aProject, "pcb", aPath )
{
}


BOARD_DOC_SESSION::~BOARD_DOC_SESSION()
{
    unloadBoard();
}


void BOARD_DOC_SESSION::unloadBoard()
{
    m_toolManager.reset();

    if( m_board )
    {
        m_board->ClearProject();
        m_board.reset();
    }
}


void BOARD_DOC_SESSION::load()
{
    const fs::path path = Project().Files().Resolve( Path() );
    unloadBoard();

    if( fs::exists( path ) )
    {
        try
        {
            m_board = BOARD_LOADER::Load( wxString::FromUTF8( path.string() ), PCB_IO_MGR::KICAD_SEXP,
                                          &Project().KicadProject() );
        }
        catch( const IO_ERROR& e )
        {
            throw TOOL_ERROR( "KiCad could not load the board " + Path() + ": " + e.What().ToStdString() );
        }

        if( !m_board )
            throw TOOL_ERROR( "KiCad could not load the board " + Path() + "." );

        m_isNew = false;
    }
    else
    {
        m_board = BOARD_LOADER::CreateEmptyBoard( &Project().KicadProject() );
        m_board->SetFileName( wxString::FromUTF8( path.string() ) );
        m_isNew = true;
    }

    m_board->BuildConnectivity();

    m_toolManager = std::make_unique<TOOL_MANAGER>();
    m_toolManager->SetEnvironment( m_board.get(), nullptr, nullptr, Kiface().KifaceSettings(), nullptr );
}


void BOARD_DOC_SESSION::save( const std::vector<std::string>& aFiles )
{
    wxString target = wxString::FromUTF8( Project().Files().Resolve( Path() ).string() );

    if( !BOARD_LOADER::SaveBoard( target, *m_board ) )
        throw TOOL_ERROR( "Writing " + Path() + " failed." );

    m_isNew = false;
}


BOARD_DOC_SESSION& BoardSession( AGENT_PROJECT& aProject, const std::string& aPath )
{
    std::string path = aPath;

    if( path.empty() )
    {
        for( DOC_SESSION* session : aProject.Sessions() )
        {
            if( session->Program() == "pcb" )
                return static_cast<BOARD_DOC_SESSION&>( *session );
        }

        path = aProject.Name() + ".kicad_pcb";
    }
    else
    {
        path = aProject.Files().Relative( aProject.Files().Resolve( path ) );
    }

    if( DOC_SESSION* existing = aProject.FindSession( "pcb", path ) )
        return static_cast<BOARD_DOC_SESSION&>( *existing );

    if( fs::path( path ).extension() != ".kicad_pcb" )
        throw TOOL_ERROR( "A board is a .kicad_pcb file; got '" + path + "'." );

    auto session = std::make_unique<BOARD_DOC_SESSION>( aProject, path );
    session->Open( false );
    return static_cast<BOARD_DOC_SESSION&>( aProject.AddSession( std::move( session ) ) );
}

} // namespace AGENTIC
