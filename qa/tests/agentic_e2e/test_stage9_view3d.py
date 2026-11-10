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
Stage 9: 3D.  Captures are raytraced headless (no display); the viewer is KiCad's 3D canvas.
"""

import base64
import os
import struct
import sys
import time
import zlib

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, McpError, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage6_pcb import fail, led_board, ok  # noqa: E402


def png_pixels( data ):
    """(width, height, distinct RGB colours) of an 8-bit RGB or RGBA PNG."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width, height, kind = 8, b"", 0, 0, 0

    while pos < len( data ):
        length, tag = struct.unpack( ">I4s", data[pos:pos + 8] )
        body = data[pos + 8:pos + 8 + length]

        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack( ">IIBB", body[:10] )
            assert depth == 8 and kind in ( 2, 6 )
        elif tag == b"IDAT":
            idat += body

        pos += 12 + length

    bpp = 3 if kind == 2 else 4
    raw = zlib.decompress( idat )
    stride = width * bpp
    prev = bytearray( stride )
    colours = set()

    for y in range( height ):
        line = raw[y * ( stride + 1 ):( y + 1 ) * ( stride + 1 )]
        kind_ = line[0]
        cur = bytearray( line[1:] )

        for i in range( stride ):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0

            if kind_ == 1:
                cur[i] = ( cur[i] + a ) & 0xFF
            elif kind_ == 2:
                cur[i] = ( cur[i] + b ) & 0xFF
            elif kind_ == 3:
                cur[i] = ( cur[i] + ( a + b ) // 2 ) & 0xFF
            elif kind_ == 4:
                p = a + b - c
                pa, pb, pc = abs( p - a ), abs( p - b ), abs( p - c )
                cur[i] = ( cur[i] + ( a if pa <= pb and pa <= pc else b if pb <= pc else c ) ) & 0xFF

        if y % 4 == 0:
            for x in range( 0, stride, bpp * 4 ):
                colours.add( bytes( cur[x:x + 3] ) )

        prev = cur

    return width, height, len( colours )


def capture( client, args ):
    result = client.call_tool( "view3d_capture", args )
    assert not result["isError"], result["content"]
    image = next( c for c in result["content"] if c["type"] == "image" )
    return result["structuredContent"], base64.b64decode( image["data"] )


def test_headless_capture_and_camera( isolated_env, tmp_path ):
    env = { k: v for k, v in isolated_env.items() if k not in ( "DISPLAY", "WAYLAND_DISPLAY" ) }
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=env )

    try:
        client.initialize()
        led_board( client, tmp_path / "three" )

        info, top = capture( client, { "width": 400, "height": 300 } )
        width, height, colours = png_pixels( top )
        assert ( width, height ) == ( info["width"], info["height"] )
        assert 300 <= width <= 400 and 200 <= height <= 300
        assert colours > 20, "the picture is blank"

        info, bottom = capture( client, { "width": 400, "height": 300, "view": "bottom" } )
        assert bottom != top and png_pixels( bottom )[2] > 20

        # A capture's arguments do not move the camera.
        assert ok( client, "view3d_camera_get" )["view"] == "top"

        camera = { "view": "front", "rotation": [ -30, 0, 30 ], "zoom": 1.5, "perspective": True }
        ok( client, "view3d_camera_set", camera )
        got = ok( client, "view3d_camera_get" )
        assert { k: got[k] for k in camera } == camera

        ok( client, "view3d_options", { "solderMask": False, "maskColor": "#20a020" } )
        assert ok( client, "view3d_camera_get" )["solderMask"] is False
        assert "colour" in fail( client, "view3d_options", { "maskColor": "green" } )
        with pytest.raises( McpError, match="not one of" ):
            client.call_tool( "view3d_camera_set", { "view": "sideways" } )

        info, angled = capture( client, { "width": 320, "height": 240 } )
        assert info["camera"]["view"] == "front" and png_pixels( angled )[2] > 20
    finally:
        client.close()


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_view3d_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    def viewer():
        return { v["program"]: v for v in ok( client, "debug_ui_state" )["viewers"] }["view3d"]

    try:
        client = HttpClient( url, token )
        client.initialize()
        led_board( client, tmp_path / "view3d" )

        ok( client, "view3d_camera_set", { "view": "front" } )
        state = viewer()
        assert state["boardLoaded"] and state["view"] == "front"
        reloads = state["reloads"]

        # A board change reloads the 3D board once the changes settle (500 ms).
        ok( client, "pcb_move", { "items": [ "R1" ], "to": { "x": 108, "y": 112 } } )
        ok( client, "pcb_move", { "items": [ "R1" ], "to": { "x": 109, "y": 112 } } )
        deadline = time.time() + 10

        while time.time() < deadline and ( viewer()["reloads"] == reloads or viewer()["reloadPending"] ):
            time.sleep( 0.2 )

        assert viewer()["reloads"] == reloads + 1

        ok( client, "undo" )
        assert viewer()["boardLoaded"]
    finally:
        stop( proc )
