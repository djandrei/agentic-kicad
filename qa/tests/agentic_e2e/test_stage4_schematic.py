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
Stage 4: the schematic tools and viewer.  The acceptance builds an LED and resistor circuit
with power symbols in an empty project, ERC clean, in a file upstream KiCad loads unchanged.
"""

import json
import os
import subprocess
import sys
import time

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402

LIBS = os.path.abspath( os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "libraries" ) )
GRID = 1.27


def ok( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert not result["isError"], f"{name}: {result['content']}"
    return result["structuredContent"]


def fail( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert result["isError"], f"{name} should fail: {result}"
    return result["content"][0]["text"]


def on_grid( value ):
    return abs( value / GRID - round( value / GRID ) ) < 1e-6


def new_project( client, path ):
    ok( client, "project_create", { "path": str( path ) } )
    ok( client, "lib_add", { "type": "symbol", "name": "Device", "uri": LIBS + "/Device.kicad_sym" } )
    ok( client, "lib_add", { "type": "symbol", "name": "power", "uri": LIBS + "/power.kicad_sym" } )


def build_led_circuit( client ):
    """R1 from +5V to the LED's anode, the LED's cathode to GND, flagged power: ERC clean."""
    r = ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 100.1, "y": 80 }, "value": "330" } )
    assert r["reference"] == "R1"
    pos = r["created"][0]["position"]
    assert on_grid( pos["x"] ) and on_grid( pos["y"] )

    d = ok( client, "sch_symbol_place", { "libId": "Device:LED", "position": { "x": 100, "y": 100 }, "rotation": 90 } )
    assert d["reference"] == "D1"

    ok( client, "sch_label_add", { "text": "+5V", "kind": "power", "at": "R1.1" } )
    ok( client, "sch_label_add", { "text": "GND", "kind": "power", "at": "D1.1" } )

    # Flag the two power nets as driven, on short wires off the power pins.
    for pin, y in ( ( "R1.1", 65 ), ( "D1.1", 120 ) ):
        flag = ok( client, "sch_symbol_place", { "libId": "power:PWR_FLAG", "position": { "x": 85, "y": y } } )
        ok( client, "sch_connect", { "from": f"{flag['reference']}.1", "to": pin } )

    connect = ok( client, "sch_connect", { "from": "R1.2", "to": "D1.2" } )
    created = connect["connectivity"].get( "created", [] )
    assert any( set( net["pins"] ) == { "R1.2", "D1.2" } for net in created )
    assert connect["connectivity"]["unconnectedPins"] == []
    return connect


def test_led_circuit_acceptance( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "led"
        new_project( client, project )
        build_led_circuit( client )

        nets = { n["net"]: set( n["pins"] ) for n in ok( client, "sch_nets" )["nets"] }
        assert nets["+5V"] == { "R1.1" }
        assert nets["GND"] == { "D1.1" }

        erc = ok( client, "sch_erc" )
        assert erc["errors"] == 0 and erc["warnings"] == 0, erc

        # The saved file: upstream KiCad finds nothing to change, and its ERC agrees.
        schematic = project / "led.kicad_sch"
        upgrade = subprocess.run( [ KICAD_CLI, "sch", "upgrade", str( schematic ) ], env=isolated_env,
                                  capture_output=True, text=True, timeout=120 )
        assert "not updated" in upgrade.stdout + upgrade.stderr

        report = tmp_path / "erc.json"
        subprocess.run( [ KICAD_CLI, "sch", "erc", "--format", "json", "-o", str( report ), str( schematic ) ],
                        env=isolated_env, capture_output=True, timeout=120 )
        cli = json.loads( report.read_text() )
        assert sum( len( s["violations"] ) for s in cli["sheets"] ) == 0

        netlist = tmp_path / "led.net"
        subprocess.run( [ KICAD_CLI, "sch", "export", "netlist", "-o", str( netlist ), str( schematic ) ],
                        env=isolated_env, capture_output=True, timeout=120 )
        text = netlist.read_text()
        assert '(ref "R1")' in text and '(ref "D1")' in text and '(name "+5V")' in text
    finally:
        assert client.close() == 0


def test_erc_matches_kicad_cli_and_undo( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "erc"
        new_project( client, project )
        ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 50, "y": 50 } } )
        ok( client, "sch_symbol_place", { "libId": "Device:C", "position": { "x": 80, "y": 50 } } )
        ok( client, "sch_label_add", { "text": "SIG", "kind": "global", "at": "R1.2" } )

        ours = ok( client, "sch_erc", { "severity": "all" } )["violations"]

        report = tmp_path / "erc.json"
        subprocess.run( [ KICAD_CLI, "sch", "erc", "--format", "json", "--severity-all", "-o", str( report ),
                          str( project / "erc.kicad_sch" ) ], env=isolated_env, capture_output=True, timeout=120 )
        cli = [ v for s in json.loads( report.read_text() )["sheets"] for v in s["violations"] ]

        def key( v ):
            return ( v["type"], v["severity"], tuple( sorted( i["uuid"] for i in v["items"] ) ) )

        assert sorted( map( key, ours ) ) == sorted( map( key, cli ) )
        assert any( v["type"] == "pin_not_connected" for v in ours )

        # Undo and redo through the file history.
        before = ( project / "erc.kicad_sch" ).read_text()
        ok( client, "sch_label_add", { "text": "NOTE", "kind": "local", "at": { "x": 120, "y": 120 } } )
        ok( client, "undo" )
        assert ( project / "erc.kicad_sch" ).read_text() == before
        ok( client, "redo" )
        assert '"NOTE"' in ( project / "erc.kicad_sch" ).read_text()

        # An edit made outside the agent is a conflict; nothing is written.
        outside = ( project / "erc.kicad_sch" ).read_text().replace( '"NOTE"', '"PERSON"' )
        ( project / "erc.kicad_sch" ).write_text( outside )
        assert "changed outside the agent" in fail( client, "sch_text_add", { "text": "x", "position": { "x": 10, "y": 10 } } )
        assert ( project / "erc.kicad_sch" ).read_text() == outside
        labels = ok( client, "sch_query", { "type": "label" } )["items"]
        assert [ l["text"] for l in labels ] == [ "PERSON" ]
    finally:
        client.close()


def test_editing_tools( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "edit"
        new_project( client, project )
        ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 50, "y": 50 } } )

        moved = ok( client, "sch_move", { "items": [ "R1" ], "delta": { "x": 2.54, "y": 0 } } )
        assert moved["changed"][0]["after"]["position"]["x"] == pytest.approx( moved["changed"][0]["before"]["position"]["x"] + 2.54 )

        rotated = ok( client, "sch_rotate", { "items": [ "R1" ] } )
        assert rotated["changed"][0]["after"]["rotation"] == 90

        mirrored = ok( client, "sch_mirror", { "items": [ "R1" ], "axis": "x" } )
        assert mirrored["changed"][0]["after"]["mirrorX"] or mirrored["changed"][0]["after"]["rotation"] != 90

        sym = ok( client, "sch_symbol_set", { "item": "R1", "value": "10k", "footprint": "Resistor_SMD:R_0603",
                                              "fields": { "MPN": "RC0603" }, "dnp": True } )
        after = sym["changed"][0]["after"]
        assert after["value"] == "10k" and after["footprint"] == "Resistor_SMD:R_0603" and after["dnp"]

        ok( client, "sch_text_add", { "text": "hello", "position": { "x": 20, "y": 20 } } )
        ok( client, "sch_no_connect_add", { "at": [ "R1.1" ] } )
        # A junction joins two crossing wires; KiCad's cleanup removes junctions that join nothing.
        ok( client, "sch_wire_add", { "points": [ { "x": 40.64, "y": 60.96 }, { "x": 81.28, "y": 60.96 } ] } )
        ok( client, "sch_wire_add", { "points": [ { "x": 60.96, "y": 40.64 }, { "x": 60.96, "y": 81.28 } ] } )
        junction = ok( client, "sch_junction_add", { "position": { "x": 60.96, "y": 60.96 } } )
        assert junction["created"][0]["type"] == "junction"
        ok( client, "sch_bus_add", { "points": [ { "x": 30, "y": 100 }, { "x": 60, "y": 100 } ] } )

        sheet = ok( client, "sch_sheet_add", { "name": "power", "sheetFile": "power.kicad_sch",
                                               "position": { "x": 150, "y": 50 },
                                               "pins": [ { "name": "VIN", "side": "left", "shape": "input" } ] } )
        assert sheet["sheetPath"].endswith( "power/" )
        assert ( project / "power.kicad_sch" ).exists()

        hierarchy = ok( client, "sch_hierarchy" )["sheets"]
        assert [ s["file"] for s in hierarchy ] == [ "edit.kicad_sch", "power.kicad_sch" ]
        sub = ok( client, "sch_query", { "sheet": sheet["sheetPath"], "type": "hierarchical_label" } )["items"]
        assert [ l["text"] for l in sub ] == [ "VIN" ]

        counts = { t: ok( client, "sch_query", { "type": t } )["total"]
                   for t in ( "symbol", "text", "no_connect", "junction", "bus", "sheet" ) }
        assert counts == { "symbol": 1, "text": 1, "no_connect": 1, "junction": 1, "bus": 1, "sheet": 1 }

        ok( client, "sch_delete", { "items": [ "R1" ] } )
        assert ok( client, "sch_query", { "type": "symbol" } )["total"] == 0

        ok( client, "title_block_set", { "title": "Edit test", "revision": "B" } )
        assert ok( client, "title_block_get" )["revision"] == "B"

        files = ok( client, "sch_export", { "format": "pdf" } )["files"]
        assert files and ( project / files[0] ).exists()
        bom = ok( client, "sch_export", { "format": "bom" } )["files"][0]
        assert ( project / bom ).read_text().startswith( '"Reference"' )

        capture = client.call_tool( "sch_capture", { "width": 400 } )
        assert capture["content"][0]["type"] == "image"
        assert capture["structuredContent"]["width"] == 400
    finally:
        client.close()


def test_annotate( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        new_project( client, tmp_path / "ann" )

        for x in ( 80, 40, 60 ):
            ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": x, "y": 50 }, "reference": "R?" } )

        renamed = ok( client, "sch_annotate", { "order": "x" } )["renamed"]
        refs = { ok( client, "sch_query", { "reference": f"R{n}" } )["items"][0]["position"]["x"]: f"R{n}"
                 for n in ( 1, 2, 3 ) }
        assert [ refs[x] for x in sorted( refs ) ] == [ "R1", "R2", "R3" ]
        assert len( renamed ) == 3
        assert "already annotated" in fail( client, "sch_annotate" )
    finally:
        client.close()


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_viewer_opens_and_reopens_with_full_state( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        client = HttpClient( url, token )
        client.initialize()
        new_project( client, tmp_path / "view" )

        assert ok( client, "debug_ui_state" )["viewers"] == []
        ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 100, "y": 80 } } )
        viewer = ok( client, "debug_ui_state" )["viewers"][0]
        assert viewer["program"] == "sch" and viewer["shown"] and viewer["itemsInView"] == 1

        ok( client, "debug_viewer", { "program": "sch", "action": "close" } )
        ok( client, "sch_symbol_place", { "libId": "Device:LED", "position": { "x": 120, "y": 80 } } )
        state = ok( client, "debug_ui_state" )
        assert state["viewers"][0]["shown"] is False
        assert state["dots"]["sch"] == "green"

        ok( client, "debug_viewer", { "program": "sch", "action": "show" } )
        viewer = ok( client, "debug_ui_state" )["viewers"][0]
        assert viewer["shown"] and viewer["itemsInView"] == 2 and len( viewer["changes"] ) == 2

        ok( client, "undo" )
        viewer = ok( client, "debug_ui_state" )["viewers"][0]
        assert viewer["itemsInView"] == 1
    finally:
        stop( proc )
