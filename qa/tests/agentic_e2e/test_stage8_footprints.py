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
Stage 8: the footprint editor.  The acceptance builds an SOIC-8 from its datasheet numbers and
compares it with the fixture library's SOIC-8.
"""

import os
import subprocess
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage6_pcb import fail, ok  # noqa: E402

FIXTURE_LIBS = os.path.abspath( os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "agentic", "libs" ) )

PAD = { "type": "smd", "shape": "roundrect", "size": [ 1.95, 0.6 ] }


def build_soic8( client, library="MyParts", name="SOIC-8_Agent" ):
    lib = { "library": library, "footprint": name }
    ok( client, "fp_create", { "library": library, "name": name, "type": "smd",
                               "description": "SOIC, 8 Pin, 3.9x4.9mm body, 1.27mm pitch" } )
    ok( client, "fp_pads_add", dict( lib, **PAD, array={ "start": { "x": -2.475, "y": -1.905 }, "count": 4,
                                                         "pitch": 1.27, "direction": "down" } ) )
    ok( client, "fp_pads_add", dict( lib, **PAD, array={ "start": { "x": 2.475, "y": 1.905 }, "count": 4,
                                                         "pitch": 1.27, "direction": "up", "firstNumber": 5 } ) )
    ok( client, "fp_graphic_add", dict( lib, kind="rectangle", layer="F.Fab",
                                        start={ "x": -1.95, "y": -2.45 }, end={ "x": 1.95, "y": 2.45 } ) )
    return ok( client, "fp_courtyard_set", lib )


@pytest.fixture
def client( isolated_env, tmp_path ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    ok( c, "project_create", { "path": str( tmp_path / "fplib" ) } )
    ok( c, "lib_create", { "type": "footprint", "name": "MyParts" } )
    ok( c, "lib_add", { "type": "footprint", "name": "Agentic_Packages",
                        "uri": FIXTURE_LIBS + "/Agentic_Packages.pretty" } )
    yield c
    c.close()


def test_soic8_matches_the_library( client, tmp_path, isolated_env ):
    courtyard = build_soic8( client )["courtyard"]

    # Pads span x -3.45..3.45, y -2.205..2.205; the body y -2.45..2.45; plus 0.25 mm.
    assert courtyard == { "x1": -3.7, "y1": -2.7, "x2": 3.7, "y2": 2.7 }

    ours = ok( client, "fp_get", { "library": "MyParts", "footprint": "SOIC-8_Agent" } )
    ok( client, "fp_create", { "library": "MyParts", "name": "SOIC-8_Ref", "copyFrom": "Agentic_Packages:SOIC-8" } )
    ref = ok( client, "fp_get", { "library": "MyParts", "footprint": "SOIC-8_Ref" } )

    assert len( ours["pads"] ) == len( ref["pads"] ) == 8
    theirs = { p["number"]: p for p in ref["pads"] }

    for pad in ours["pads"]:
        other = theirs[pad["number"]]
        assert abs( pad["position"]["x"] - other["position"]["x"] ) < 0.001
        assert abs( pad["position"]["y"] - other["position"]["y"] ) < 0.001
        assert pad["size"] == other["size"] and pad["shape"] == other["shape"] == "roundrect"
        assert set( pad["layers"] ) == set( other["layers"] )

    check = ok( client, "fp_check", { "library": "MyParts", "footprint": "SOIC-8_Agent" } )
    assert check["clean"], check["issues"]

    # The file is upstream's: kicad-cli reads and plots it.
    lib = tmp_path / "fplib" / "MyParts.pretty"
    assert ( lib / "SOIC-8_Agent.kicad_mod" ).exists()
    out = tmp_path / "svg"
    out.mkdir()  # upstream kicad-cli does not create it (and exits 0 anyway)
    run = subprocess.run( [ KICAD_CLI, "fp", "export", "svg", "--footprint", "SOIC-8_Agent", "-o", str( out ), str( lib ) ],
                          env=isolated_env, capture_output=True, text=True, timeout=120 )
    assert run.returncode == 0, run.stdout + run.stderr
    assert ( out / "SOIC-8_Agent.svg" ).exists()


def test_checker_finds_problems( client ):
    ok( client, "fp_create", { "library": "MyParts", "name": "Bad", "type": "through_hole" } )
    lib = { "library": "MyParts", "footprint": "Bad" }
    ok( client, "fp_pads_add", dict( lib, type="smd", shape="rect", size=1,
                                     pads=[ { "number": 1, "x": 0, "y": 0 }, { "number": 2, "x": 0.5, "y": 0 } ] ) )
    issues = { i["type"] for i in ok( client, "fp_check", lib )["issues"] }
    assert "missing_courtyard" in issues
    assert "shorting_items" in issues or "pads_overlap" in issues or any( "short" in i for i in issues )
    assert "footprint_type_mismatch" in issues


def test_edits_undo_and_delete( client, tmp_path ):
    build_soic8( client, name="X" )
    lib = { "library": "MyParts", "footprint": "X" }
    file = tmp_path / "fplib" / "MyParts.pretty" / "X.kicad_mod"

    ok( client, "fp_pad_set", dict( lib, pads=[ "1" ], shape="rect" ) )
    pads = { p["number"]: p for p in ok( client, "fp_get", lib )["pads"] }
    assert pads["1"]["shape"] == "rect" and pads["2"]["shape"] == "roundrect"
    ok( client, "undo" )
    assert { p["number"]: p for p in ok( client, "fp_get", lib )["pads"] }["1"]["shape"] == "roundrect"

    ok( client, "fp_pads_add", dict( lib, type="thru_hole", shape="circle", size=1.6, drill=0.8,
                                     pads=[ { "number": "MP", "x": 0, "y": 0 } ] ) )
    mp = [ p for p in ok( client, "fp_get", lib )["pads"] if p["number"] == "MP" ][0]
    assert mp["drill"] == [ 0.8, 0.8 ] and "B.Cu" in mp["layers"]
    assert "drill" in fail( client, "fp_pads_add", dict( lib, type="thru_hole", shape="circle", size=1,
                                                          pads=[ { "number": "9", "x": 0, "y": 3 } ] ) )

    ok( client, "fp_field_set", dict( lib, keywords="soic so", excludeFromBom=True, fields={ "Vendor": "ACME" } ) )
    got = ok( client, "fp_get", lib )
    assert got["keywords"] == "soic so" and got["excludeFromBom"] and got["fields"]["Vendor"] == "ACME"

    ok( client, "fp_model_set", dict( lib, models=[ { "path": "${KICAD9_3DMODEL_DIR}/Package_SO.3dshapes/SOIC-8.wrl" } ] ) )
    assert ok( client, "fp_get", lib )["models"][0]["scale"] == [ 1, 1, 1 ]

    capture = client.call_tool( "fp_capture", lib )
    assert capture["content"][0]["type"] == "image"
    assert capture["structuredContent"]["area"]["x1"] < -3.4

    svg = ok( client, "fp_export_svg", lib )["file"]
    assert ( tmp_path / "fplib" / svg ).exists()

    ok( client, "fp_delete", lib )
    assert not file.exists()
    assert "X" not in [ f["name"] for f in ok( client, "fp_list", { "library": "MyParts" } )["footprints"] ]
    ok( client, "undo" )
    assert file.exists()
    assert "X" in [ f["name"] for f in ok( client, "fp_list", { "library": "MyParts" } )["footprints"] ]

    assert "outside the project" in fail( client, "fp_pad_set", { "library": "Agentic_Packages", "footprint": "SOIC-8",
                                                                  "pads": [ "1" ], "shape": "rect" } )


def test_placed_on_a_board( client, tmp_path ):
    """A footprint made here is placed from the library on the project's board."""
    build_soic8( client )
    ok( client, "pcb_footprint_place", { "footprint": "MyParts:SOIC-8_Agent", "position": { "x": 50, "y": 50 },
                                         "reference": "U1" } )
    placed = ok( client, "pcb_query", { "reference": "U1" } )["items"][0]
    assert placed["pads"] == 8


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_footprint_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        ok( c, "project_create", { "path": str( tmp_path / "fpview" ) } )
        ok( c, "lib_create", { "type": "footprint", "name": "MyParts" } )
        build_soic8( c )

        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["fp"]["footprint"] == "SOIC-8_Agent"
        assert viewers["fp"]["padRows"] == 8 and viewers["fp"]["padsInView"] == 8

        ok( c, "undo" )
        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["fp"]["padRows"] == 8
    finally:
        stop( proc )
