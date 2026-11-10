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

#include <boost/test/unit_test.hpp>
#include <kiplatform/app.h>
#include <mock_pgm_base.h>
#include <qa_utils/file_utils.h>

#include <wx/init.h>


bool init_unit_test()
{
    KI_TEST::SetMockConfigDir();
    SetPgm( new MOCK_PGM_BASE() );
    KIPLATFORM::APP::Init();
    boost::unit_test::framework::master_test_suite().p_name.value = "Agentic core tests";

    if( !wxInitialize() )
        return false;

    // Projects are opened through the settings manager (stage 2).
    Pgm().InitPgm( true, true );
    return true;
}


int main( int argc, char* argv[] )
{
    KI_TEST::SCOPED_PROCESS_TEMP_DIR scopedTempDir( "kicad_qa_agentic_core" );

    int ret = boost::unit_test::unit_test_main( &init_unit_test, argc, argv );

    wxUninitialize();

    return ret;
}
