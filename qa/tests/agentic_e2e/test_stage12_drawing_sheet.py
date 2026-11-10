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
Stage 12: the drawing sheet editor.
"""

import base64
import os
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage4_schematic import new_project  # noqa: E402
from test_stage6_pcb import fail, ok  # noqa: E402
from test_stage9_view3d import png_pixels  # noqa: E402

SHEET = "agent.kicad_wks"


def image( result ):
    assert not result["isError"], result["content"]
    return base64.b64decode( next( c for c in result["content"] if c["type"] == "image" )["data"] )


@pytest.fixture
def client( isolated_env, tmp_path ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    new_project( c, tmp_path / "sheet" )
    yield c, tmp_path / "sheet"
    c.close()


def test_default_sheet_round_trips( client, isolated_env ):
    c, project = client
    created = ok( c, "ds_create", { "file": SHEET } )
    original = ( project / SHEET ).read_text()
    items = ok( c, "ds_items", { "file": SHEET } )["items"]
    assert created["items"] == len( items ) > 10
    assert any( i["type"] == "text" and "${TITLE}" in i["text"] for i in items )
    assert any( i["type"] == "rect" for i in items )

    # A change and its reversal give the same file.
    ok( c, "ds_item_set", { "file": SHEET, "item": 0, "info": "changed" } )
    assert ( project / SHEET ).read_text() != original
    ok( c, "ds_item_set", { "file": SHEET, "item": 0, "info": items[0].get( "info", "" ) } )
    assert ( project / SHEET ).read_text() == original

    # Another process reads the same items back (the project is closed here first: it is locked).
    ok( c, "project_close", {} )
    other = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        other.initialize()
        ok( other, "project_open", { "path": str( project ) } )
        assert ok( other, "ds_items", { "file": SHEET } )["items"] == items
    finally:
        other.close()

    ok( c, "project_open", { "path": str( project ) } )
    assert "exists" in fail( c, "ds_create", { "file": SHEET } )
    assert "No drawing sheet" in fail( c, "ds_open", { "file": "missing.kicad_wks" } )


def test_items_edit_and_undo( client ):
    c, project = client
    ok( c, "ds_create", { "file": SHEET, "template": "empty" } )

    # KiCad's empty sheet holds one zero-length line; remove it.
    empty = ok( c, "ds_items", { "file": SHEET } )["items"]
    assert len( empty ) == 1 and empty[0]["type"] == "line"
    ok( c, "ds_item_delete", { "file": SHEET, "item": 0 } )

    ok( c, "ds_item_add", { "file": SHEET, "type": "rect", "name": "frame", "start": { "x": 5, "y": 5, "anchor": "lt" },
                            "end": { "x": 5, "y": 5, "anchor": "rb" }, "width": 0.3 } )
    text = ok( c, "ds_item_add", { "file": SHEET, "type": "text", "name": "logo", "text": "ACME",
                                   "start": { "x": 40, "y": 20 }, "size": 5, "bold": True } )["added"]
    assert text["index"] == 1 and text["start"]["anchor"] == "rb" and text["bold"]

    ok( c, "ds_item_set", { "file": SHEET, "item": "logo", "text": "ACME Corp", "page": "first" } )
    logo = ok( c, "ds_items", { "file": SHEET } )["items"][1]
    assert logo["text"] == "ACME Corp" and logo["page"] == "first"

    ok( c, "undo" )
    assert ok( c, "ds_items", { "file": SHEET } )["items"][1]["text"] == "ACME"

    ok( c, "ds_item_delete", { "file": SHEET, "item": "frame" } )
    assert [ i["name"] for i in ok( c, "ds_items", { "file": SHEET } )["items"] ] == [ "logo" ]
    assert "No item" in fail( c, "ds_item_delete", { "file": SHEET, "item": "nothing" } )

    picture = image( c.call_tool( "ds_capture", { "file": SHEET, "width": 600 } ) )
    assert png_pixels( picture )[0] == 600


def test_logo_appears_in_schematic_capture( client ):
    c, project = client
    ok( c, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 100, "y": 80 } } )

    before = image( c.call_tool( "sch_capture", { "width": 800 } ) )

    ok( c, "ds_create", { "file": SHEET } )
    ok( c, "ds_item_add", { "file": SHEET, "type": "text", "name": "logo", "text": "AGENTIC LOGO",
                            "start": { "x": 60, "y": 60 }, "size": 8, "bold": True } )
    ok( c, "sch_drawing_sheet_set", { "drawingSheet": SHEET } )
    assert SHEET in ( project / "sheet.kicad_pro" ).read_text()

    after = image( c.call_tool( "sch_capture", { "width": 800 } ) )
    assert after != before

    # Only the logo's corner of the page changed: the logo sits 60 mm left of and above the bottom right.
    region = { "x1": 150, "y1": 120, "x2": 240, "y2": 160 }
    logo_after = image( c.call_tool( "sch_capture", { "width": 600, "area": region } ) )
    ok( c, "sch_drawing_sheet_set", { "drawingSheet": "" } )
    logo_before = image( c.call_tool( "sch_capture", { "width": 600, "area": region } ) )
    assert logo_after != logo_before

    assert "No drawing sheet" in fail( c, "sch_drawing_sheet_set", { "drawingSheet": "nope.kicad_wks" } )


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_drawing_sheet_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        ok( c, "project_create", { "path": str( tmp_path / "dsview" ) } )
        ok( c, "ds_create", { "file": SHEET } )
        count = len( ok( c, "ds_items", { "file": SHEET } )["items"] )

        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["ds"]["file"] == SHEET and viewers["ds"]["itemRows"] == count
        assert viewers["ds"]["pictureWidth"] > 0

        ok( c, "ds_item_add", { "file": SHEET, "type": "text", "text": "X", "start": { "x": 10, "y": 10 } } )
        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["ds"]["itemRows"] == count + 1
    finally:
        stop( proc )
