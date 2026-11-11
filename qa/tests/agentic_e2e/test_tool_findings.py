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
Regressions found by driving every tool through one design (the full tool test).
"""

import os
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage4_schematic import new_project  # noqa: E402
from test_stage6_pcb import fail, led_board, ok  # noqa: E402
from test_stage10_simulator import rc_lowpass  # noqa: E402


@pytest.fixture
def client( isolated_env ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    yield c
    c.close()


def two_unit_part( client ):
    """A project library with a two-unit symbol (unit 1 logic, unit 2 power)."""
    ok( client, "lib_create", { "type": "symbol", "name": "Parts" } )
    ok( client, "sym_create", { "library": "Parts", "name": "DUO", "reference": "U", "units": 2 } )
    ok( client, "sym_pins_set", { "library": "Parts", "symbol": "DUO", "pins": [
        { "number": "1", "name": "IN", "type": "input", "side": "left", "unit": 1 },
        { "number": "2", "name": "OUT", "type": "output", "side": "right", "unit": 1 },
        { "number": "3", "name": "VDD", "type": "power_in", "side": "top", "unit": 2 },
        { "number": "4", "name": "GND", "type": "power_in", "side": "bottom", "unit": 2 } ] } )


def position( item ):
    return ( round( item["position"]["x"], 3 ), round( item["position"]["y"], 3 ) )


# F9 ---------------------------------------------------------------------------------------------

@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_close_project_with_open_viewer( isolated_env, tmp_path ):
    """Closing a project whose viewers are open must not crash the server (viewer retired late)."""
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], isolated_env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        led_board( c, tmp_path / "first" )
        ok( c, "view3d_camera_set", { "view": "front" } )
        ok( c, "calc_list" )
        # A board change leaves the 3D viewer's reload timer pending while the project closes.
        ok( c, "pcb_move", { "items": [ "R1" ], "to": { "x": 108, "y": 112 } } )
        new_project( c, tmp_path / "second" )

        ok( c, "project_close", { "project": str( tmp_path / "first" ) } )
        ok( c, "status" )
        assert proc.poll() is None, "the server died"
        assert [ p["name"] for p in ok( c, "project_list" )["projects"] ] == [ "second" ]
    finally:
        stop( proc )


# F6 ---------------------------------------------------------------------------------------------

def test_sch_move_uses_each_calls_target( client, tmp_path ):
    """Every sch_move goes where its own arguments say, not by an earlier call's offset."""
    new_project( client, tmp_path / "move" )
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 50.8, "y": 50.8 } } )
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 76.2, "y": 50.8 } } )

    for item, to in [ ( "R1", ( 101.6, 76.2 ) ), ( "R2", ( 127.0, 101.6 ) ), ( "R1", ( 25.4, 25.4 ) ),
                      ( "R2", ( 152.4, 50.8 ) ) ]:
        moved = ok( client, "sch_move", { "items": [ item ], "to": { "x": to[0], "y": to[1] } } )
        assert position( moved["changed"][0]["after"] ) == to

    moved = ok( client, "sch_move", { "items": [ "R1" ], "delta": { "x": 2.54, "y": 0 } } )
    assert position( moved["changed"][0]["after"] ) == ( 27.94, 25.4 )


# F11 --------------------------------------------------------------------------------------------

def test_route_refuses_a_layer_the_pads_are_not_on( client, tmp_path ):
    """Front SMD pads cannot be routed on B.Cu: the router would leave the tracks unconnected."""
    led_board( client, tmp_path / "route" )
    message = fail( client, "pcb_route", { "from": "R1.2", "to": "D1.2", "layer": "B.Cu" } )
    assert "not on B.Cu" in message and "pcb_via_add" in message
    assert ok( client, "pcb_unrouted" )["unrouted"] == 1

    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )
    assert routed["completed"] and ok( client, "pcb_unrouted" )["unrouted"] == 0


# F8 ---------------------------------------------------------------------------------------------

def test_sim_model_needs_a_type_and_sim_run_reports_a_bad_circuit( client, tmp_path ):
    """A device with several model types needs one; a circuit ngspice cannot load is an error."""
    rc_lowpass( client, tmp_path / "sim" )

    message = fail( client, "sim_model_set", { "item": "R1", "device": "NPN", "params": "bf=200" } )
    assert "GUMMELPOON" in message
    assert "device" in fail( client, "sim_model_set", { "item": "R1", "device": "NOPE" } )
    ok( client, "sim_model_set", { "item": "R1", "device": "R", "params": "r=1k" } )

    # Fields written around the check give ngspice a line it cannot load.
    ok( client, "sch_symbol_set", { "item": "C1", "fields": { "Sim.Device": "NPN", "Sim.Params": "bf=200" } } )
    ok( client, "sim_analysis_set", { "type": "op" } )
    assert "load" in fail( client, "sim_run" )


# F10 --------------------------------------------------------------------------------------------

def test_autoplace_default_places_only_footprints_off_the_board( client, tmp_path ):
    """By default only footprints outside the outline move; placed ones stay."""
    led_board( client, tmp_path / "place" )
    before = { r: position( ok( client, "pcb_query", { "reference": r } )["items"][0] ) for r in ( "R1", "D1" ) }
    ok( client, "pcb_footprint_place", { "footprint": "Resistor_SMD:R_0603_1608Metric", "reference": "R9",
                                         "position": { "x": 50, "y": 50 } } )

    placed = ok( client, "pcb_autoplace" )["placed"]
    assert [ p["reference"] for p in placed ] == [ "R9" ]

    for ref, at in before.items():
        assert position( ok( client, "pcb_query", { "reference": ref } )["items"][0] ) == at


# F2 ---------------------------------------------------------------------------------------------

def test_title_block_title_is_kept_and_new_sheets_get_the_title_block( client, tmp_path ):
    new_project( client, tmp_path / "title" )
    ok( client, "title_block_set", { "title": "Sensor", "revision": "B", "company": "ACME" } )
    block = ok( client, "title_block_get" )
    assert ( block["title"], block["revision"], block["company"] ) == ( "Sensor", "B", "ACME" )

    ok( client, "sch_sheet_add", { "name": "sub", "sheetFile": "sub.kicad_sch", "position": { "x": 50, "y": 50 } } )
    text = ok( client, "file_read", { "path": "sub.kicad_sch" } )["text"]
    assert '(rev "B")' in text and '(company "ACME")' in text


# F3 ---------------------------------------------------------------------------------------------

def test_derived_symbol_keeps_the_parents_fields( client, tmp_path ):
    ok( client, "project_create", { "path": str( tmp_path / "derive" ) } )
    ok( client, "lib_create", { "type": "symbol", "name": "Parts" } )
    ok( client, "sym_create", { "library": "Parts", "name": "HDR", "reference": "J", "footprint": "Lib:HDR" } )
    ok( client, "sym_pins_set", { "library": "Parts", "symbol": "HDR", "pins": [ { "number": "1", "name": "A" } ] } )
    ok( client, "sym_field_set", { "library": "Parts", "symbol": "HDR", "fields": { "Maker": "ACME" } } )
    ok( client, "sym_derive", { "library": "Parts", "name": "HDR_K", "parent": "HDR" } )

    check = ok( client, "sym_check", { "library": "Parts", "symbol": "HDR_K" } )
    assert check["ok"], check
    derived = { s["name"]: s for s in ok( client, "sym_list", { "library": "Parts" } )["symbols"] }["HDR_K"]
    assert derived["reference"] == "J" and derived["footprint"] == "Lib:HDR"


# F4, F5 -----------------------------------------------------------------------------------------

def test_sheet_paths_and_reused_sheet_pins( client, tmp_path ):
    new_project( client, tmp_path / "sheets" )
    first = ok( client, "sch_sheet_add", { "name": "ch1", "sheetFile": "amp.kicad_sch", "position": { "x": 50, "y": 50 },
                                           "pins": [ { "name": "IN", "side": "left", "shape": "input" },
                                                     { "name": "OUT", "side": "right", "shape": "output" } ] } )
    assert first["sheetPath"] == "/ch1/"
    assert [ s["path"] for s in ok( client, "sch_hierarchy" )["sheets"] ] == [ "/", "/ch1/" ]

    # Both the documented form and the file-name form name the sheet.
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 80, "y": 80 }, "sheet": "/ch1/" } )
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 90, "y": 80 }, "sheet": "sheets/ch1/" } )
    items = ok( client, "sch_query", { "sheet": "/ch1/", "type": "symbol" } )["items"]
    assert len( items ) == 2 and { i["sheet"] for i in items } == { "/ch1/" }

    # A sheet cannot contain itself (the sheet used last is /ch1/, whose file is amp.kicad_sch).
    assert "cannot contain itself" in fail( client, "sch_sheet_add", { "name": "loop", "sheetFile": "amp.kicad_sch",
                                                                        "position": { "x": 120, "y": 120 } } )

    # A second instance of the file gets the file's hierarchical labels as its pins.
    ok( client, "sch_sheet_add", { "name": "ch2", "sheetFile": "amp.kicad_sch", "position": { "x": 50, "y": 90 },
                                   "sheet": "/" } )
    sheets = { s["name"]: s for s in ok( client, "sch_query", { "sheet": "/", "type": "sheet" } )["items"] }
    assert sorted( p["name"] for p in sheets["ch2"]["pins"] ) == [ "IN", "OUT" ]


# F7, O4 -----------------------------------------------------------------------------------------

def test_multi_unit_symbol_reference( client, tmp_path ):
    new_project( client, tmp_path / "units" )
    two_unit_part( client )
    ok( client, "sch_symbol_place", { "libId": "Parts:DUO", "unit": 1, "position": { "x": 60, "y": 60 } } )
    ok( client, "sch_symbol_place", { "libId": "Parts:DUO", "unit": 2, "position": { "x": 60, "y": 100 },
                                      "reference": "U1" } )

    units = ok( client, "sch_query", { "reference": "U1" } )["items"]
    assert len( units ) == 2 and { u["reference"] for u in units } == { "U1" }
    assert sorted( u["unit"] for u in units ) == [ 1, 2 ]
    assert all( "unitName" in u for u in units )

    ok( client, "sch_symbol_set", { "item": "U1", "value": "DUO-2", "fields": { "MPN": "X1" } } )
    assert { u["value"] for u in ok( client, "sch_query", { "reference": "U1" } )["items"] } == { "DUO-2" }
    assert "unit" in fail( client, "sch_symbol_set", { "item": "U1", "unit": 2 } )
