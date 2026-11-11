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
from test_stage4_schematic import build_led_circuit, new_project  # noqa: E402
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


def test_route_changes_layer_at_a_waypoint( client, tmp_path ):
    """A waypoint with a layer places a via and continues there (front pad to a back pad)."""
    led_board( client, tmp_path / "via" )
    ok( client, "pcb_flip", { "items": [ "D1" ] } )
    assert "not on F.Cu" in fail( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )
    assert "already on" in fail( client, "pcb_route", { "from": "R1.2", "to": "D1.2",
                                                        "waypoints": [ { "x": 115, "y": 110, "layer": "F.Cu" } ] } )

    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2",
                                        "waypoints": [ { "x": 115, "y": 110, "layer": "B.Cu" } ] } )
    assert routed["completed"] and routed["vias"] == 1, routed
    assert ok( client, "pcb_unrouted" )["unrouted"] == 0
    assert ok( client, "pcb_drc", { "refillZones": False } )["errors"] == 0


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


# F1 ---------------------------------------------------------------------------------------------

def test_docs_cache_follows_kicad_cache_home( isolated_env, tmp_path ):
    env = dict( isolated_env, KICAD_CACHE_HOME=str( tmp_path / "cache" ) )
    env.pop( "AGENTIC_KICAD_DOCS_CACHE", None )
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=env )

    try:
        c.initialize()
        assert ok( c, "docs_status" )["cache"].startswith( str( tmp_path / "cache" ) )
    finally:
        c.close()


# O2 ---------------------------------------------------------------------------------------------

def test_symbol_body_fits_top_and_bottom_pin_names( client, tmp_path ):
    ok( client, "project_create", { "path": str( tmp_path / "body" ) } )
    ok( client, "lib_create", { "type": "symbol", "name": "Parts" } )
    ok( client, "sym_create", { "library": "Parts", "name": "PWR", "reference": "U" } )
    sym = ok( client, "sym_pins_set", { "library": "Parts", "symbol": "PWR", "pins": [
        { "number": "1", "name": "VDD_ANALOG", "type": "power_in", "side": "top" },
        { "number": "2", "name": "GND_ANALOG", "type": "power_in", "side": "bottom" } ] } )
    ys = sorted( p["position"]["y"] for p in sym["pins"] )

    # Pin ends sit a pin length (2.54) outside the body; the names (10 characters of 1.27 mm text)
    # must fit between the two body edges without meeting.
    assert ( ys[1] - ys[0] ) - 2 * 2.54 >= 2 * 10 * 1.27


# O3 ---------------------------------------------------------------------------------------------

def test_footprint_text_positions( client, tmp_path ):
    ok( client, "project_create", { "path": str( tmp_path / "fptext" ) } )
    ok( client, "lib_create", { "type": "footprint", "name": "Fp" } )
    ok( client, "fp_create", { "library": "Fp", "name": "PART", "type": "smd" } )
    ok( client, "fp_field_set", { "library": "Fp", "footprint": "PART", "referencePosition": { "x": 0, "y": -2.5 },
                                  "valuePosition": { "x": 0, "y": 2.5 }, "textSize": 0.8 } )
    got = ok( client, "fp_get", { "library": "Fp", "footprint": "PART" } )
    assert got["referencePosition"] == { "x": 0.0, "y": -2.5 } and got["valuePosition"] == { "x": 0.0, "y": 2.5 }
    assert got["textSize"] == 0.8


# O5 ---------------------------------------------------------------------------------------------

def test_drawing_sheet_of_one_project_does_not_leak_into_another( client, tmp_path ):
    """KiCad keeps one drawing sheet model per process: ERC must load its own project's sheet."""
    new_project( client, tmp_path / "with_sheet" )
    build_led_circuit( client )
    ok( client, "text_vars_set", { "vars": { "BUILD": "7" } } )
    ok( client, "ds_create", { "file": "my.kicad_wks" } )
    ok( client, "ds_item_add", { "file": "my.kicad_wks", "type": "text", "text": "build ${BUILD}",
                                 "start": { "x": 50, "y": 30 } } )
    ok( client, "sch_drawing_sheet_set", { "drawingSheet": "my.kicad_wks" } )
    assert ok( client, "sch_erc" )["violations"] == []
    ok( client, "sch_capture" )  # loads this project's sheet (${BUILD}) into the shared model

    new_project( client, tmp_path / "plain" )  # no BUILD variable here
    build_led_circuit( client )
    assert [ v["type"] for v in ok( client, "sch_erc" )["violations"] ] == []


# O8 ---------------------------------------------------------------------------------------------

def test_coupled_line_synthesis_needs_its_targets( client, tmp_path ):
    """zDiff is a result of coupled lines; synthesis without z0Odd/z0Even returned nothing, silently."""
    ok( client, "project_create", { "path": str( tmp_path / "calc" ) } )
    assert "z0Odd" in fail( client, "calc_run", { "calculator": "transmission_line", "inputs": {
        "type": "coupled_microstrip", "mode": "synthesize",
        "parameters": { "zDiff": 90, "h": 0.2, "t": 0.035, "epsilonR": 4.5, "frequency": 1e9 } } } )

    pair = ok( client, "calc_run", { "calculator": "transmission_line", "inputs": {
        "type": "coupled_microstrip", "mode": "synthesize",
        "parameters": { "z0Odd": 45, "z0Even": 65, "h": 0.2, "t": 0.035, "epsilonR": 4.5, "frequency": 4.8e8 } } } )
    assert abs( pair["outputs"]["results"]["zDiff"] - 90 ) < 0.5


# O9, O10 ----------------------------------------------------------------------------------------

def test_stackup_names_and_plain_rule_errors( client, tmp_path ):
    led_board( client, tmp_path / "stackup" )
    ok( client, "pcb_stackup_set", { "copperLayers": 4 } )
    names = [ layer["name"] for layer in ok( client, "pcb_stackup_get" )["layers"] ]
    assert all( names ) and [ n for n in names if n.endswith( ".Cu" ) ] == [ "F.Cu", "In1.Cu", "In2.Cu", "B.Cu" ]

    message = fail( client, "pcb_rules_set", { "customRules": "(version 1)\n(rule a (constraint clearance (min 1mm))\n" } )
    assert "<a" not in message and "line" in message
