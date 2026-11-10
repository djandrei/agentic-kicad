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
Stage 7: routing and placement aids, through KiCad's router (PNS) running headless.
"""

import json
import os
import subprocess
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import StdioClient  # noqa: E402
from conftest import KICAD_CLI  # noqa: E402
from test_stage6_pcb import fail, led_board, ok  # noqa: E402


def track( client, kiid ):
    return next( t for t in ok( client, "pcb_query", { "type": "track" } )["items"] if t["kiid"] == kiid )


@pytest.fixture
def board( isolated_env, tmp_path ):
    """The stage 6 LED board, D1 turned so its anode faces R1, and a GND track across the gap."""
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    client.initialize()
    project = tmp_path / "route"
    led_board( client, project )
    obstacle = ok( client, "pcb_track_add", { "points": [ { "x": 115, "y": 107 }, { "x": 115, "y": 113 } ],
                                               "net": "GND" } )["items"][0]
    yield client, project, obstacle
    client.close()


def test_route_around_an_obstacle( board, isolated_env, tmp_path ):
    client, project, obstacle = board

    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )
    assert routed["completed"] and routed["tracksAdded"] > 1
    assert routed["ratsnest"]["nets"]["Net-(D1-A)"]["after"] == 0
    assert track( client, obstacle )["start"] == { "x": 115.0, "y": 107.0 }

    # No errors; the GND obstacle itself dangles (a warning) and GND is unrouted.
    drc = ok( client, "pcb_drc", { "refillZones": False } )
    assert drc["errors"] == 0, drc["violations"]

    report = tmp_path / "drc.json"
    subprocess.run( [ KICAD_CLI, "pcb", "drc", "--format", "json", "-o", str( report ), str( project / "route.kicad_pcb" ) ],
                    env=isolated_env, capture_output=True, timeout=300 )
    assert not [ v for v in json.loads( report.read_text() )["violations"] if v["severity"] == "error" ]

    # Walkaround is longer than the straight 8.35 mm between the pads (the GND track is 6 mm).
    length = ok( client, "pcb_stats" )["trackLength"] - 6
    assert 8.35 < length < 20


def test_shove_moves_the_obstacle( board ):
    client, project, obstacle = board

    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2", "mode": "shove" } )
    assert routed["completed"]
    # The GND track was pushed aside: replaced by new segments or bent.
    gnd = ok( client, "pcb_query", { "type": "track", "net": "GND" } )["items"]
    assert len( gnd ) > 1 or gnd[0]["start"] != { "x": 115.0, "y": 107.0 } or gnd[0]["end"] != { "x": 115.0, "y": 113.0 }
    drc = ok( client, "pcb_drc", { "refillZones": False } )
    assert drc["errors"] == 0, drc["violations"]


def test_waypoints_and_errors( board ):
    client, project, obstacle = board

    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2", "waypoints": [ { "x": 115, "y": 116 } ] } )
    assert routed["completed"]
    assert any( t["start"]["y"] == 116 or t["end"]["y"] == 116
                for t in ok( client, "pcb_query", { "type": "track", "net": "Net-(D1-A)" } )["items"] )

    assert "same net" in fail( client, "pcb_route", { "from": "R1.1", "to": "D1.2" } )
    assert "pair" in fail( client, "pcb_diff_pair_route", { "from": "R1.2", "to": "D1.2" } )
    assert "could not" in fail( client, "pcb_import_session", { "session": "missing.ses" } )


def test_tune_length( board ):
    client, project, obstacle = board

    ok( client, "pcb_delete", { "items": [ obstacle ] } )
    routed = ok( client, "pcb_route", { "from": "R1.2", "to": "D1.2" } )
    before = ( project / "route.kicad_pcb" ).read_text()

    # A deadline that passes while tuning: cancelled, the board unchanged.
    result = client.call_tool( "pcb_tune_length", { "track": routed["items"][0], "length": 15, "timeoutSeconds": 1e-6 } )
    assert result["isError"] and "ancel" in result["content"][0]["text"]
    assert ( project / "route.kicad_pcb" ).read_text() == before

    tuned = ok( client, "pcb_tune_length", { "track": routed["items"][0], "length": 15 } )
    assert tuned["status"] == "tuned" and tuned["length"] == pytest.approx( 15, abs=0.1 )
    # The only tracks left are the tuned net's.
    assert ok( client, "pcb_stats" )["trackLength"] == pytest.approx( 15, abs=0.1 )
    drc = ok( client, "pcb_drc", { "refillZones": False } )
    assert drc["errors"] == 0, drc["violations"]


def test_autoplace( board ):
    client, project, obstacle = board

    placed = ok( client, "pcb_autoplace", { "all": True } )["placed"]
    assert { p["reference"] for p in placed } == { "R1", "D1" }

    for p in placed:
        assert 100 < p["position"]["x"] < 130 and 100 < p["position"]["y"] < 120
