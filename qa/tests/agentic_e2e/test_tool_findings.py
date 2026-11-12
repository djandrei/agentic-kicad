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


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_viewers_show_what_the_agent_looks_at( isolated_env, tmp_path ):
    """Found taking README screenshots: the schematic viewer drew hidden text, and the symbol and
    footprint viewers stayed empty after a capture (only edits made a symbol current)."""
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        led_board( c, tmp_path / "look" )
        ok( c, "lib_create", { "type": "symbol", "name": "Parts" } )
        ok( c, "sym_create", { "library": "Parts", "name": "PART", "reference": "U" } )
        ok( c, "sym_pins_set", { "library": "Parts", "symbol": "PART", "pins": [ { "number": "1", "name": "A" } ] } )
        ok( c, "sym_create", { "library": "Parts", "name": "OTHER", "reference": "U" } )
        ok( c, "lib_create", { "type": "footprint", "name": "Fp" } )
        ok( c, "fp_create", { "library": "Fp", "name": "ONE", "type": "smd" } )
        ok( c, "fp_pads_add", { "library": "Fp", "footprint": "ONE", "type": "smd", "shape": "rect", "size": 1,
                                "pads": [ { "number": "1", "x": 0, "y": 0 } ] } )
        ok( c, "fp_create", { "library": "Fp", "name": "TWO", "type": "smd" } )

        ok( c, "sym_capture", { "library": "Parts", "symbol": "PART" } )
        ok( c, "fp_capture", { "library": "Fp", "footprint": "ONE" } )
        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }

        assert viewers["sch"]["hiddenTextShown"] is False
        assert viewers["sym"]["symbol"] == "PART" and viewers["sym"]["pinRows"] == 1
        assert viewers["fp"]["footprint"] == "ONE" and viewers["fp"]["padRows"] == 1
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


def overlapping_copper( client, net ):
    """Pairs of collinear segments of a net on one layer that share a stretch of copper."""
    import itertools
    import math

    tracks = ok( client, "pcb_query", { "net": net, "type": "track" } )["items"]
    segments = [ ( ( t["start"]["x"], t["start"]["y"] ), ( t["end"]["x"], t["end"]["y"] ), t["layer"] ) for t in tracks ]
    found = []

    for ( a1, a2, la ), ( b1, b2, lb ) in itertools.combinations( segments, 2 ):
        dx, dy = a2[0] - a1[0], a2[1] - a1[1]
        n = math.hypot( dx, dy )

        if la != lb or n == 0:
            continue

        if any( abs( dx * ( p[1] - a1[1] ) - dy * ( p[0] - a1[0] ) ) / n > 1e-3 for p in ( b1, b2 ) ):
            continue

        t = sorted( ( ( p[0] - a1[0] ) * dx + ( p[1] - a1[1] ) * dy ) / n for p in ( b1, b2 ) )

        if min( n, t[1] ) - max( 0, t[0] ) > 1e-3:
            found.append( ( a1, a2, b1, b2 ) )

    return found


def test_routes_into_one_pad_do_not_double_the_copper( client, fixture_project ):
    """A second route into a pad followed the first one's track: 4.5 mm of doubled copper, which a
    later shove or tuning tore into a dangling stub (seen on the psu_monitor board)."""
    ok( client, "project_open", { "path": str( fixture_project( "route_overlap" ) ) } )
    ok( client, "pcb_route", { "from": "J2.3", "to": "U1.7" } )
    second = ok( client, "pcb_route", { "from": "U1.7", "to": "R6.1" } )
    assert second["completed"] and second["overlapsMerged"] >= 1, second
    assert overlapping_copper( client, "/ALERT" ) == []
    assert ok( client, "pcb_unrouted", { "net": "/ALERT" } )["unrouted"] == 0
    drc = ok( client, "pcb_drc", { "severity": "all", "refillZones": False } )
    assert "track_dangling" not in [ v["type"] for v in drc["violations"] ]


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


def test_sim_run_reports_an_aborted_transient( client, tmp_path ):
    """A bridge rectifier charging 470 uF from an ideal source makes ngspice give up at the first
    peak ("timestep too small"); sim_run returned ok with the 9 ms it had made of 60 ms."""
    new_project( client, tmp_path / "bridge" )
    ok( client, "lib_create", { "type": "symbol", "name": "Sources" } )
    ok( client, "sym_create", { "library": "Sources", "name": "VSRC", "reference": "V", "value": "VSRC" } )
    ok( client, "sym_pins_set", { "library": "Sources", "symbol": "VSRC", "pins": [
        { "number": "1", "name": "+", "type": "passive", "side": "top" },
        { "number": "2", "name": "-", "type": "passive", "side": "bottom" } ] } )
    ok( client, "sch_symbol_place", { "libId": "Sources:VSRC", "position": { "x": 40, "y": 80 } } )

    for x, y in ( ( 70, 60 ), ( 70, 100 ), ( 90, 60 ), ( 90, 100 ) ):
        ok( client, "sch_symbol_place", { "libId": "Device:D", "position": { "x": x, "y": y }, "rotation": 90 } )

    ok( client, "sch_symbol_place", { "libId": "Device:C", "position": { "x": 120, "y": 80 }, "value": "470u" } )
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 140, "y": 80 }, "value": "1k" } )
    # D (pin 1 cathode, pin 2 anode): D1 AC1->OUT, D2 AC2->OUT, D3 GND->AC1, D4 GND->AC2.
    for at, net in ( ( "V1.1", "AC1" ), ( "V1.2", "AC2" ), ( "D1.2", "AC1" ), ( "D1.1", "OUT" ), ( "D2.2", "AC2" ),
                     ( "D2.1", "OUT" ), ( "D3.1", "AC1" ), ( "D4.1", "AC2" ), ( "C1.1", "OUT" ), ( "R1.1", "OUT" ) ):
        ok( client, "sch_label_add", { "kind": "local", "text": net, "at": at } )

    for at in ( "D3.2", "D4.2", "C1.2", "R1.2" ):
        ok( client, "sch_label_add", { "kind": "power", "text": "GND", "at": at } )

    ok( client, "sim_model_set", { "item": "V1", "device": "V", "type": "SIN", "params": "dc=0 ampl=17 f=50" } )

    for d in ( "D1", "D2", "D3", "D4" ):
        ok( client, "sim_model_set", { "item": d, "device": "D", "params": { "is": "1e-9", "n": "1.8", "rs": "0.05" },
                                       "pins": { "1": "2", "2": "1" } } )

    ok( client, "sim_analysis_set", { "type": "tran", "step": "50u", "stop": "60m" } )
    message = fail( client, "sim_run" )
    assert "stopped the analysis" in message and "imestep too small" in message, message

    # With realistic diode resistance the same run completes.
    for d in ( "D1", "D2", "D3", "D4" ):
        ok( client, "sim_model_set", { "item": d, "device": "D", "params": { "is": "1e-9", "n": "1.8", "rs": "1" },
                                       "pins": { "1": "2", "2": "1" } } )

    ok( client, "sim_run" )
    assert abs( ok( client, "sim_signal", { "vector": "out", "points": 3 } )["x"][-1] - 0.06 ) < 1e-9


def test_annotate_start_number_is_the_first_number( client, tmp_path ):
    new_project( client, tmp_path / "annotate" )
    for x in ( 50, 70, 90 ):
        ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": x, "y": 50 } } )

    def refs():
        return sorted( i["reference"] for i in ok( client, "sch_query", { "type": "symbol" } )["items"] )

    ok( client, "sch_annotate", { "reset": True, "startNumber": 1 } )
    assert refs() == [ "R1", "R2", "R3" ]
    ok( client, "sch_annotate", { "reset": True, "startNumber": 10 } )
    assert refs() == [ "R10", "R11", "R12" ]


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

    unknown = fail( client, "calc_run", { "calculator": "transmission_line", "inputs": {
        "type": "coax", "parameters": { "din": 0.9 } } } )
    assert "innerDiameter" in unknown and "outerDiameter" in unknown, unknown

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


# Round 3: engineering changes (eco_logger) and a high-speed board (hs_board) --------------------

def test_update_keeps_renumbered_parts_and_reports_left_copper( client, tmp_path ):
    """A renumbered part was taken for a new one and moved to the origin row; copper of removed
    parts stays as in KiCad, the update reports it and pcb_cleanup removes it."""
    led_board( client, tmp_path / "eco" )
    assert ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )["completed"]
    ok( client, "sch_symbol_set", { "item": "R1", "reference": "R7" } )
    update = ok( client, "pcb_update_from_schematic" )
    assert update["footprintsAdded"] == [] and update["footprintsRemoved"] == [] and update["danglingCopper"] == 0, update
    assert position( ok( client, "pcb_query", { "reference": "R7" } )["items"][0] ) == ( 110, 110 )

    ok( client, "sch_delete", { "items": [ "D1" ] } )
    update = ok( client, "pcb_update_from_schematic" )
    assert update["footprintsRemoved"] == [ "D1" ] and update["danglingCopper"] > 0, update
    assert "pcb_cleanup" in update["note"]

    tracks = ok( client, "pcb_query", { "type": "track" } )["items"]
    assert tracks
    dry = ok( client, "pcb_cleanup", { "dryRun": True } )
    assert dry["dryRun"] and dry["counts"].get( "dangling_track", 0 ) >= 1, dry
    assert len( ok( client, "pcb_query", { "type": "track" } )["items"] ) == len( tracks )

    clean = ok( client, "pcb_cleanup" )
    assert not clean["dryRun"] and clean["counts"].get( "dangling_track", 0 ) >= 1, clean
    assert ok( client, "pcb_query", { "type": "track" } )["items"] == []
    assert ok( client, "pcb_cleanup", { "refillZones": False } )["changes"] == []


def test_shove_leaves_no_copper_leading_nowhere( client, fixture_project ):
    """The shove of this route broke the +5V track next to J2.2 into two dead ends."""
    ok( client, "project_open", { "path": str( fixture_project( "shove_stub" ) ) } )
    before = ok( client, "pcb_unrouted", { "net": "+5V" } )["unrouted"]
    route = ok( client, "pcb_route", { "from": "U2.6", "to": "J2.2", "mode": "shove" } )
    assert route["completed"] and route["danglingRemoved"] > 0, route
    drc = ok( client, "pcb_drc", { "severity": "all", "refillZones": False } )
    assert [ v for v in drc["violations"] if v["type"] in ( "track_dangling", "via_dangling" ) ] == []
    assert ok( client, "pcb_unrouted", { "net": "+5V" } )["unrouted"] == before


def test_lengths_net_classes_and_meander_settings( client, tmp_path ):
    """Arcs had no mid point or length and nets no length, so a tuned net could not be measured;
    nets kept the class they had at load; the tuner's meander settings could not be changed."""
    led_board( client, tmp_path / "len" )
    net = next( p["net"] for p in ok( client, "pcb_query", { "type": "pad" } )["items"] if p["pad"] == "R1.2" )
    ok( client, "netclasses_set", { "netclasses": [ { "name": "Sig", "trackWidth": 0.3 } ],
                                    "patterns": [ { "pattern": net, "netclass": "Sig" } ] } )
    nets = { n["net"]: n for n in ok( client, "pcb_query", { "type": "net" } )["items"] }
    assert nets[net]["netclass"] == "Sig", nets[net]

    ok( client, "pcb_track_add", { "points": [ [ 103, 103 ], [ 105, 103 ] ], "layer": "F.Cu", "net": net } )
    added = ok( client, "pcb_query", { "type": "track", "area": { "x1": 102, "y1": 102, "x2": 106, "y2": 104 } } )["items"]
    assert [ round( t["width"], 3 ) for t in added ] == [ 0.3 ]
    ok( client, "undo" )

    assert ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )["completed"]
    straight = { n["net"]: n for n in ok( client, "pcb_query", { "type": "net" } )["items"] }[net]["trackLength"]
    longest = max( ok( client, "pcb_query", { "type": "track", "net": net } )["items"], key=lambda t: t["length"] )
    message = fail( client, "pcb_tune_length", { "track": longest["kiid"], "length": straight + 2,
                                                 "minAmplitude": 1, "maxAmplitude": 0.5 } )
    assert "minAmplitude" in message
    tuned = ok( client, "pcb_tune_length", { "track": longest["kiid"], "length": straight + 2, "spacing": 0.5 } )

    arcs = ok( client, "pcb_query", { "type": "arc", "net": net } )["items"]
    assert arcs and all( "mid" in a and a["length"] > 0 for a in arcs )
    pieces = sum( t["length"] for kind in ( "track", "arc" )
                  for t in ok( client, "pcb_query", { "type": kind, "net": net } )["items"] )
    length = { n["net"]: n for n in ok( client, "pcb_query", { "type": "net" } )["items"] }[net]["trackLength"]
    assert abs( pieces - length ) < 0.002 and abs( length - tuned["length"] ) < 0.01, ( pieces, length, tuned )
