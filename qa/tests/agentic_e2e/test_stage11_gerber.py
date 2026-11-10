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
Stage 11: the Gerber viewer, on the stage 6 LED board's own Gerbers and drill files.
"""

import base64
import json
import os
import subprocess
import sys
import zipfile

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage6_pcb import fail, led_board, ok  # noqa: E402
from test_stage9_view3d import png_pixels  # noqa: E402


def board_outputs( client, project ):
    """The LED board, routed, with its Gerbers (and job file) and drill files under output/."""
    led_board( client, project )
    ok( client, "pcb_track_add", { "points": [ "R1.2", "D1.2" ] } )
    ok( client, "pcb_via_add", { "position": { "x": 115, "y": 115 }, "net": "GND" } )
    gerbers = ok( client, "pcb_export", { "format": "gerbers" } )["files"]
    drills = ok( client, "pcb_export", { "format": "drill" } )["files"]
    return gerbers, [ d for d in drills if d.endswith( ".drl" ) ]


@pytest.fixture
def loaded( isolated_env, tmp_path ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    project = tmp_path / "gbr"
    gerbers, drills = board_outputs( c, project )
    job = next( g for g in gerbers if g.endswith( ".gbrjob" ) )
    result = ok( c, "gerber_load", { "job": job } )
    ok( c, "gerber_load", { "files": drills, "append": True } )
    yield c, project, result
    c.close()


def cli_info( env, path ):
    run = subprocess.run( [ KICAD_CLI, "gerber", "info", "--format", "json", str( path ) ], env=env,
                          capture_output=True, text=True, timeout=120 )
    assert run.returncode == 0, run.stdout + run.stderr
    return json.loads( run.stdout[run.stdout.index( "{" ):] )


def test_info_matches_kicad_cli( loaded, isolated_env ):
    client, project, result = loaded
    assert not result.get( "errors" ) and len( result["loaded"] ) == 9

    layers = ok( client, "gerber_layers" )["layers"]
    assert len( layers ) == 11  # 9 Gerbers + PTH and NPTH drill
    assert sum( 1 for l in layers if l["type"] == "Excellon Drill" ) == 2
    front = next( l for l in layers if l["file"].endswith( "F_Cu.gbr" ) )
    assert front["x2"]["fileFunction"] == "Copper" and front["x2"]["side"] == "Top"

    for name in ( "F_Cu.gbr", "Edge_Cuts.gbr", "F_Mask.gbr", "PTH.drl" ):
        ours = ok( client, "gerber_info", { "layer": name } )
        theirs = cli_info( isolated_env, project / ours["file"] )
        assert ours["item_count"] == theirs["item_count"]
        assert ours["aperture_count"] == theirs["aperture_count"]
        assert ours["type"] == theirs["type"]

        for key in ( "origin_x", "origin_y", "width", "height" ):
            assert ours["bounding_box"][key] == pytest.approx( theirs["bounding_box"][key], abs=1e-6 )

    apertures = ok( client, "gerber_info", { "layer": "F_Cu.gbr" } )["apertures"]
    assert any( a["shape"] == "Roundrect" or "Macro" in a["shape"] or a["shape"] for a in apertures )


def test_diff_and_capture( loaded ):
    client, project, result = loaded

    same = ok( client, "gerber_diff", { "a": "F_Cu.gbr", "b": "F_Cu.gbr" } )
    assert same["additions"]["area"] == pytest.approx( 0, abs=1e-6 )
    assert same["removals"]["area"] == pytest.approx( 0, abs=1e-6 )

    other = ok( client, "gerber_diff", { "a": "F_Cu.gbr", "b": "B_Cu.gbr", "align": False } )
    assert other["removals"]["area"] > 0

    capture = client.call_tool( "gerber_capture", { "width": 400 } )
    assert capture["content"][0]["type"] == "image" and capture["structuredContent"]["width"] == 400
    assert png_pixels( base64.b64decode( capture["content"][0]["data"] ) )[2] > 3, "the picture is blank"

    assert "No loaded layer" in fail( client, "gerber_info", { "layer": "nothing.gbr" } )


def test_zip_and_board_import( loaded, tmp_path ):
    client, project, result = loaded

    with zipfile.ZipFile( project / "fab.zip", "w" ) as z:
        for f in ( project / "output" ).iterdir():
            if f.suffix in ( ".gbr", ".drl" ):
                z.write( f, "fab/" + f.name )

        z.writestr( "../escape.gbr", "G04 not a real file*" )

    # Entries are extracted by base name only: "../escape.gbr" stays inside the folder.
    zipped = ok( client, "gerber_load", { "zip": "fab.zip" } )
    assert len( zipped["loaded"] ) == 12
    assert not ( tmp_path / "escape.gbr" ).exists() and not ( project / ".agentic" / "escape.gbr" ).exists()
    assert ( project / ".agentic" / "gerbers" / "fab" / "escape.gbr" ).exists()

    ok( client, "gerber_load", { "job": "output/gbr-job.gbrjob" } )
    ok( client, "gerber_load", { "files": [ "output/gbr-PTH.drl" ], "append": True } )
    imported = ok( client, "gerber_to_pcb", { "output": "imported.kicad_pcb" } )
    assert imported["layers"]["output/gbr-F_Cu.gbr"] == "F.Cu"
    assert imported["layers"]["output/gbr-PTH.drl"] == "holes"
    assert ( project / "imported.kicad_pcb" ).exists()

    stats = ok( client, "pcb_stats", { "file": "imported.kicad_pcb" } )
    assert stats["tracks"] >= 1 and stats["vias"] >= 1

    assert "exists" in fail( client, "gerber_to_pcb", { "output": "imported.kicad_pcb" } )
    ok( client, "undo" )
    assert not ( project / "imported.kicad_pcb" ).exists()


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_gerber_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        gerbers, drills = board_outputs( c, tmp_path / "gview" )
        ok( c, "gerber_load", { "files": [ g for g in gerbers if g.endswith( ".gbr" ) ] } )

        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["gerber"]["layerRows"] == 9 and viewers["gerber"]["pictureWidth"] > 0

        ok( c, "gerber_load", { "files": drills } )
        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["gerber"]["layerRows"] == 2
    finally:
        stop( proc )
