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
#include <agentic/ds/ds_session.h>
#include <agentic/protocol/registry.h>

#include <drawing_sheet/ds_data_model.h>
#include <locale_io.h>


namespace AGENTIC
{

namespace fs = std::filesystem;


SHEET_SESSION::SHEET_SESSION( AGENT_PROJECT& aProject, const std::string& aPath ) :
        DOC_SESSION( aProject, "ds", aPath ),
        m_model( std::make_unique<DS_DATA_MODEL>() )
{
}


SHEET_SESSION::~SHEET_SESSION() = default;


void SHEET_SESSION::SetNew( bool aDefault )
{
    if( aDefault )
        m_model->SetDefaultLayout();
    else
        m_model->SetEmptyLayout();
}


void SHEET_SESSION::load()
{
    const fs::path path = Project().Files().Resolve( Path() );

    if( !fs::exists( path ) )
    {
        // A new sheet: written by its first change (ds_create).
        m_model->SetDefaultLayout();
        return;
    }

    auto     model = std::make_unique<DS_DATA_MODEL>();
    wxString message;

    if( !model->LoadDrawingSheet( wxString::FromUTF8( path.string() ), &message ) )
        throw TOOL_ERROR( "KiCad could not read the drawing sheet " + Path() + ": "
                          + message.ToStdString( wxConvUTF8 ) );

    m_model = std::move( model );
}


void SHEET_SESSION::save( const std::vector<std::string>& )
{
    wxString text;

    {
        LOCALE_IO toggle;
        m_model->SaveInString( &text );
    }

    FILE_STORE::WriteAtomic( Project().Files().Resolve( Path() ), text.ToStdString( wxConvUTF8 ) );
}


SHEET_SESSION& SheetSession( AGENT_PROJECT& aProject, const std::string& aPath )
{
    if( fs::path( aPath ).extension() != ".kicad_wks" )
        throw TOOL_ERROR( "A drawing sheet is a .kicad_wks file." );

    const std::string path = aProject.Files().Relative( aProject.Files().Resolve( aPath ) );

    if( DOC_SESSION* existing = aProject.FindSession( "ds", path ) )
        return static_cast<SHEET_SESSION&>( *existing );

    auto session = std::make_unique<SHEET_SESSION>( aProject, path );
    session->Open( false );
    return static_cast<SHEET_SESSION&>( aProject.AddSession( std::move( session ) ) );
}

} // namespace AGENTIC
