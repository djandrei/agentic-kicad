#
# This program source code file is part of KiCad, a free EDA CAD application.
#
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.


"""
Stage 14: CvPcb, on the stage 4 LED circuit with the fixture resistor footprint library.
"""

import os
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
import test_stage4_schematic as stage4  # noqa: E402
from test_stage6_pcb import fail, ok  # noqa: E402

R0603 = "Resistor_SMD:R_0603_1608Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"


def led_project( client, path ):
    stage4.new_project( client, path )
    stage4.build_led_circuit( client )
    ok( client, "lib_add", { "type": "footprint", "name": "Resistor_SMD", "uri": stage4.LIBS + "/Resistor_SMD.pretty" } )


@pytest.fixture
def client( isolated_env, tmp_path ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    led_project( c, tmp_path / "cv" )
    yield c, tmp_path / "cv"
    c.close()


def test_auto_associate_from_equivalences( client ):
    c, project = client
    ( project / "fixture.equ" ).write_text( "# value footprint\n'330' '" + R0603 + "'\n'LED' '" + R0402 + "'\n"
                                            "'47k' 'Nowhere:Missing'\n" )

    symbols = { s["reference"]: s for s in ok( c, "cvpcb_list", {} )["symbols"] }
    assert symbols["R1"]["footprint"] == "" and symbols["R1"]["filters"] == [ "R_*" ]
    assert symbols["R1"]["pins"] == 2 and symbols["D1"]["value"] == "LED"
    assert { s["reference"] for s in ok( c, "cvpcb_list", { "unassigned": True } )["symbols"] } >= { "R1", "D1" }

    result = ok( c, "cvpcb_auto_associate", { "equivalenceFiles": [ "fixture.equ" ] } )
    assert result["assigned"] == { "R1": R0603, "D1": R0402 } and result["equivalences"] == 3

    # Written into the schematic and saved; one undo step takes both back.
    text = ( project / "cv.kicad_sch" ).read_text()
    assert R0603 in text and R0402 in text
    symbols = { s["reference"]: s for s in ok( c, "cvpcb_list", {} )["symbols"] }
    assert symbols["R1"]["footprint"] == R0603 and symbols["D1"]["footprint"] == R0402

    ok( c, "undo" )
    symbols = { s["reference"]: s for s in ok( c, "cvpcb_list", {} )["symbols"] }
    assert symbols["R1"]["footprint"] == "" and symbols["D1"]["footprint"] == ""

    # Assigned symbols are kept unless overwrite.
    ok( c, "cvpcb_assign", { "assignments": { "R1": R0402 } } )
    again = ok( c, "cvpcb_auto_associate", { "equivalenceFiles": [ "fixture.equ" ] } )["assigned"]
    assert again == { "D1": R0402 }
    assert ok( c, "cvpcb_auto_associate", { "equivalenceFiles": [ "fixture.equ" ], "overwrite": True } )["assigned"]["R1"] == R0603


def test_candidates_and_assign( client ):
    c, project = client

    candidates = ok( c, "cvpcb_candidates", { "item": "R1" } )
    names = [ f["footprint"] for f in candidates["footprints"] ]
    assert R0603 in names and all( n.split( ":" )[1].startswith( "R_" ) for n in names )
    assert all( f["pads"] == 2 for f in candidates["footprints"] )
    assert candidates["total"] == len( names )

    # The LED's filters match nothing in a resistor library; without them its 2 pins do.
    assert ok( c, "cvpcb_candidates", { "item": "D1" } )["total"] == 0
    assert ok( c, "cvpcb_candidates", { "item": "D1", "filters": False } )["total"] == candidates["total"]
    assert ok( c, "cvpcb_candidates", { "item": "R1", "limit": 2 } )["footprints"].__len__() == 2

    assert "No footprint" in fail( c, "cvpcb_assign", { "assignments": { "R1": "Resistor_SMD:Nope" } } )
    assert "No symbol" in fail( c, "cvpcb_assign", { "assignments": { "R9": R0603 } } )

    ok( c, "cvpcb_assign", { "assignments": { "R1": R0603, "D1": R0603 } } )
    assert ( project / "cv.kicad_sch" ).read_text().count( R0603 ) >= 2

    # The board update now finds both footprints.
    update = ok( c, "pcb_update_from_schematic" )
    assert sorted( update["footprintsAdded"] ) == [ "D1", "R1" ]


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_cvpcb_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        led_project( c, tmp_path / "cvview" )
        ok( c, "cvpcb_assign", { "assignments": { "R1": R0603 } } )

        viewer = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }["cvpcb"]
        assert viewer["assignments"]["R1"] == R0603 and viewer["assignments"]["D1"] == ""
        assert viewer["previewPads"] == 2

        ok( c, "undo" )
        viewer = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }["cvpcb"]
        assert viewer["assignments"]["R1"] == ""
    finally:
        stop( proc )
