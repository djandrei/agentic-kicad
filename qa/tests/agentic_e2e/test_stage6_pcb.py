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
Stage 6: the PCB tools and viewer.  The acceptance takes the stage 4 LED circuit to a board:
update from the schematic, outline, placement, routing, a GND zone, DRC clean, Gerbers that
KiCad's Gerber reader accepts.
"""

import json
import os
import subprocess
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
import test_stage4_schematic as stage4  # noqa: E402

LIBS = stage4.LIBS
FOOTPRINT = "Resistor_SMD:R_0603_1608Metric"


def ok( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert not result["isError"], f"{name}: {result['content']}"
    return result["structuredContent"]


def fail( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert result["isError"], f"{name} should fail: {result}"
    return result["content"][0]["text"]


def led_board( client, path ):
    """The stage 4 circuit with footprints, on a 30 x 20 mm board, parts placed."""
    stage4.new_project( client, path )
    stage4.build_led_circuit( client )
    ok( client, "lib_add", { "type": "footprint", "name": "Resistor_SMD", "uri": LIBS + "/Resistor_SMD.pretty" } )
    ok( client, "sch_symbol_set", { "item": "R1", "footprint": FOOTPRINT } )
    ok( client, "sch_symbol_set", { "item": "D1", "footprint": FOOTPRINT } )

    update = ok( client, "pcb_update_from_schematic" )
    assert sorted( update["footprintsAdded"] ) == [ "D1", "R1" ]

    ok( client, "pcb_outline_set", { "rectangle": { "x": 100, "y": 100, "width": 30, "height": 20 },
                                     "cornerRadius": 1 } )
    ok( client, "pcb_move", { "items": [ "R1" ], "to": { "x": 110, "y": 110 } } )
    ok( client, "pcb_move", { "items": [ "D1" ], "to": { "x": 120, "y": 110 } } )
    rotated = ok( client, "pcb_rotate", { "items": [ "D1" ], "to": 180 } )
    assert rotated["changed"][0]["after"]["rotation"] in ( 180, -180 )


def test_board_acceptance( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "led"
        led_board( client, project )

        unrouted = ok( client, "pcb_unrouted" )
        assert unrouted["unrouted"] == 1
        assert { unrouted["nets"][0]["connections"][0]["from"]["pad"],
                 unrouted["nets"][0]["connections"][0]["to"]["pad"] } == { "R1.2", "D1.2" }

        track = ok( client, "pcb_track_add", { "points": [ "R1.2", "D1.2" ] } )
        assert track["ratsnest"]["unroutedAfter"] == 0

        # GND: a via off the LED's cathode into a GND pour on the bottom.
        ok( client, "pcb_track_add", { "points": [ "D1.1", { "x": 124, "y": 110 } ] } )
        ok( client, "pcb_via_add", { "position": { "x": 124, "y": 110 }, "net": "GND" } )
        ok( client, "pcb_zone_add", { "net": "GND", "layers": [ "B.Cu" ], "outline": True } )
        fill = ok( client, "pcb_zones_fill" )
        assert all( z["filled"] for z in fill["zones"] )

        drc = ok( client, "pcb_drc" )
        assert drc["errors"] == 0, drc["violations"]
        assert drc["unconnectedCount"] == 0

        stats = ok( client, "pcb_stats" )
        assert stats["footprints"]["front"] == 2 and stats["vias"] == 1 and stats["unrouted"] == 0
        assert stats["width"] == pytest.approx( 30, abs=0.2 )

        files = ok( client, "pcb_export", { "format": "gerbers" } )["files"]
        gerbers = [ f for f in files if f.endswith( ".gbr" ) ]
        assert any( "F_Cu" in f for f in gerbers ) and any( "Edge_Cuts" in f for f in gerbers )
        assert any( f.endswith( ".gbrjob" ) for f in files )

        for gerber in gerbers:
            info = subprocess.run( [ KICAD_CLI, "gerber", "info", "--strict", str( project / gerber ) ],
                                   env=isolated_env, capture_output=True, text=True, timeout=120 )
            assert info.returncode == 0, info.stdout + info.stderr

        drill = ok( client, "pcb_export", { "format": "drill" } )["files"]
        assert any( f.endswith( ".drl" ) for f in drill )
        position = ok( client, "pcb_export", { "format": "position" } )["files"][0]
        assert "R1" in ( project / position ).read_text()

        capture = client.call_tool( "pcb_capture", { "width": 600 } )
        assert capture["content"][0]["type"] == "image"
        assert capture["structuredContent"]["area"]["x1"] < 100

        # The saved board is upstream's: kicad-cli's DRC agrees.
        report = tmp_path / "drc.json"
        subprocess.run( [ KICAD_CLI, "pcb", "drc", "--format", "json", "-o", str( report ), str( project / "led.kicad_pcb" ) ],
                        env=isolated_env, capture_output=True, timeout=300 )
        cli = json.loads( report.read_text() )
        assert not [ v for v in cli["violations"] if v["severity"] == "error" ]
        assert not cli["unconnected_items"]
    finally:
        assert client.close() == 0


def test_drc_matches_kicad_cli( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "drc"
        led_board( client, project )

        # A track straight through the LED: a short and a mask bridge, an unrouted GND pad.
        ok( client, "pcb_rotate", { "items": [ "D1" ], "to": 0 } )
        ok( client, "pcb_track_add", { "points": [ "R1.2", "D1.2" ] } )
        ours = ok( client, "pcb_drc", { "refillZones": False } )

        report = tmp_path / "drc.json"
        subprocess.run( [ KICAD_CLI, "pcb", "drc", "--format", "json", "-o", str( report ), str( project / "drc.kicad_pcb" ) ],
                        env=isolated_env, capture_output=True, timeout=300 )
        cli = json.loads( report.read_text() )

        def key( v ):
            return ( v["type"], v["severity"], tuple( sorted( i["uuid"] for i in v["items"] ) ) )

        assert sorted( map( key, ours["violations"] ) ) == sorted( map( key, cli["violations"] ) )
        assert len( ours["unconnected"] ) == len( cli["unconnected_items"] )
        assert any( v["type"] == "shorting_items" for v in ours["violations"] )
    finally:
        client.close()


def test_batch_is_one_step( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "batch"
        led_board( client, project )
        before = ( project / "batch.kicad_pcb" ).read_text()

        result = ok( client, "batch", { "calls": [
            { "tool": "pcb_text_add", "arguments": { "text": "REV A", "position": { "x": 102, "y": 102 } } },
            { "tool": "pcb_text_add", "arguments": { "text": "LED", "position": { "x": 102, "y": 118 } } },
            { "tool": "pcb_graphic_add", "arguments": { "kind": "line", "layer": "F.SilkS",
                                                        "start": { "x": 105, "y": 105 }, "end": { "x": 125, "y": 105 } } } ] } )
        assert len( result["results"] ) == 3 and len( result["saved"] ) == 1
        assert ok( client, "pcb_query", { "type": "text" } )["total"] == 2

        ok( client, "undo" )
        assert ( project / "batch.kicad_pcb" ).read_text() == before

        # A failing call undoes the whole batch.
        text = fail( client, "batch", { "calls": [
            { "tool": "pcb_text_add", "arguments": { "text": "kept?", "position": { "x": 102, "y": 102 } } },
            { "tool": "pcb_via_add", "arguments": { "position": { "x": 110, "y": 110 }, "net": "NO_SUCH_NET" } } ] } )
        assert "batch was undone" in text
        assert ok( client, "pcb_query", { "type": "text" } )["total"] == 0
        assert "cannot be batched" in fail( client, "batch", { "calls": [ { "tool": "undo" } ] } )
    finally:
        client.close()


def test_setup_tools( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "setup"
        led_board( client, project )

        ok( client, "pcb_stackup_set", { "copperLayers": 4, "thickness": 1.2 } )
        stackup = ok( client, "pcb_stackup_get" )
        assert stackup["copperLayers"] == 4 and stackup["thickness"] == pytest.approx( 1.2 )

        ok( client, "pcb_rules_set", { "minClearance": "0.15mm", "customRules":
            "(version 1)\n(rule \"edge\" (constraint edge_clearance (min 0.5mm)))\n" } )
        rules = ok( client, "pcb_rules_get" )
        assert rules["minClearance"] == pytest.approx( 0.15 ) and "edge_clearance" in rules["customRules"]
        assert ( project / "setup.kicad_dru" ).exists()
        broken = fail( client, "pcb_rules_set", { "customRules": "(rule broken" } )
        assert "errors" in broken or "parse" in broken

        ok( client, "pcb_footprint_place", { "footprint": FOOTPRINT, "position": { "x": 115, "y": 115 },
                                             "reference": "R99", "side": "back" } )
        placed = ok( client, "pcb_query", { "reference": "R99" } )["items"][0]
        assert placed["side"] == "back"
        ok( client, "pcb_flip", { "items": [ "R99" ] } )
        assert ok( client, "pcb_query", { "reference": "R99" } )["items"][0]["side"] == "front"
        ok( client, "pcb_delete", { "items": [ "R99" ] } )
        assert ok( client, "pcb_query", { "reference": "R99" } )["total"] == 0

        ok( client, "pcb_title_block_set", { "title": "LED board", "revision": "1" } )
        assert ok( client, "pcb_title_block_get" )["title"] == "LED board"
        assert len( ok( client, "pcb_query", { "type": "net" } )["items"] ) == 3
    finally:
        client.close()


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_pcb_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        client = HttpClient( url, token )
        client.initialize()
        led_board( client, tmp_path / "view" )

        viewers = { v["program"]: v for v in ok( client, "debug_ui_state" )["viewers"] }
        assert viewers["pcb"]["footprintsInView"] == 2
        assert viewers["pcb"]["layerRows"] > 0 and viewers["pcb"]["netRows"] == 3

        ok( client, "pcb_track_add", { "points": [ "R1.2", "D1.2" ] } )
        ok( client, "undo" )
        viewers = { v["program"]: v for v in ok( client, "debug_ui_state" )["viewers"] }
        assert viewers["pcb"]["footprintsInView"] == 2
        assert "1 unrouted" in viewers["pcb"]["unrouted"]
    finally:
        stop( proc )
