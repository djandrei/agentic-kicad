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
Stage 15: timings on the largest demo board, against the thresholds in doc/agentic/timings.json.
A step more than twice as slow as recorded fails (and with it the stage gate).

    AGENTIC_RECORD_TIMINGS=1 pytest qa/tests/agentic_perf    # record new thresholds
"""

import json
import os
import shutil
import sys
import time

sys.path.insert( 0, os.path.join( os.path.dirname( __file__ ), "..", "agentic_e2e" ) )

from client import StdioClient  # noqa: E402
from conftest import KICAD_CLI, isolated_env  # noqa: E402,F401

ROOT = os.path.abspath( os.path.join( os.path.dirname( __file__ ), "..", "..", ".." ) )
DEMO = os.path.join( ROOT, "demos", "jetson-agx-thor-baseboard" )
THRESHOLDS = os.path.join( ROOT, "doc", "agentic", "timings.json" )
FACTOR = 2.0


def ok( client, name, args=None ):
    result = client.call_tool( name, args or {}, timeout=1200 )
    assert not result["isError"], f"{name}: {result['content']}"
    return result["structuredContent"]


def timed( client, name, args=None ):
    started = time.monotonic()
    out = ok( client, name, args )
    return time.monotonic() - started, out


def test_timings_on_the_largest_demo_board( isolated_env, tmp_path ):
    project = tmp_path / "board"
    shutil.copytree( DEMO, project )
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        ok( client, "project_open", { "path": str( project ) } )
        measured = {}

        measured["load"], stats = timed( client, "pcb_stats" )
        assert stats["footprints"]["front"] + stats["footprints"]["back"] > 1000

        first = ok( client, "pcb_query", { "type": "footprint", "limit": 1 } )["items"][0]
        to = { "x": first["position"]["x"] + 1, "y": first["position"]["y"] }
        measured["commit"], _ = timed( client, "pcb_move", { "items": [ first["kiid"] ], "to": to } )
        measured["undo"], _ = timed( client, "undo" )
        measured["drc"], _ = timed( client, "pcb_drc", { "refillZones": False } )
        measured["schematic_load"], _ = timed( client, "sch_query", { "type": "symbol", "limit": 1 } )
    finally:
        client.close( timeout=300 )

    print( json.dumps( { k: round( v, 2 ) for k, v in measured.items() } ) )

    if os.environ.get( "AGENTIC_RECORD_TIMINGS" ):
        with open( THRESHOLDS, "w" ) as f:
            json.dump( { "board": "demos/jetson-agx-thor-baseboard", "factor": FACTOR,
                         "seconds": { k: round( v, 2 ) for k, v in measured.items() } }, f, indent=2 )
            f.write( "\n" )
        return

    with open( THRESHOLDS ) as f:
        recorded = json.load( f )["seconds"]

    slow = { k: ( round( v, 2 ), recorded[k] ) for k, v in measured.items() if v > FACTOR * recorded[k] }
    assert not slow, f"more than {FACTOR}x the recorded time (measured, recorded): {slow}"
