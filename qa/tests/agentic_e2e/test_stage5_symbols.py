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
Stage 5: the symbol editor.  A 2-unit dual op amp is built and passes the symbol checker, its
capture is a real picture, and the library round-trips through upstream's loader.
"""

import base64
import io
import os
import struct
import subprocess
import sys
import zlib

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402


def ok( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert not result["isError"], f"{name}: {result['content']}"
    return result["structuredContent"]


def fail( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert result["isError"], f"{name} should fail: {result}"
    return result["content"][0]["text"]


def png_colours( data ):
    """Distinct RGB(A) values of an 8-bit PNG, decoded with the standard library."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks, width, height, ctype = 8, [], 0, 0, 0

    while pos < len( data ):
        length, kind = struct.unpack( ">I4s", data[pos:pos + 8] )
        body = data[pos + 8:pos + 8 + length]

        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack( ">IIBB", body[:10] )
        elif kind == b"IDAT":
            chunks.append( body )

        pos += 12 + length

    raw = zlib.decompress( b"".join( chunks ) )
    bpp = { 2: 3, 6: 4 }[ctype]
    stride = width * bpp + 1
    colours = set()
    prev = bytearray( width * bpp )

    for y in range( height ):
        filt = raw[y * stride]
        line = bytearray( raw[y * stride + 1:( y + 1 ) * stride] )

        for i in range( len( line ) ):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0

            if filt == 1:
                line[i] = ( line[i] + a ) & 255
            elif filt == 2:
                line[i] = ( line[i] + b ) & 255
            elif filt == 3:
                line[i] = ( line[i] + ( a + b ) // 2 ) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs( p - a ), abs( p - b ), abs( p - c )
                line[i] = ( line[i] + ( a if pa <= pb and pa <= pc else b if pb <= pc else c ) ) & 255

        for x in range( 0, width * bpp, bpp * 4 ):
            colours.add( bytes( line[x:x + 3] ) )

        prev = line

    return colours


DUAL_OPAMP_PINS = [
    { "number": "1", "name": "OUT", "type": "output", "side": "right", "unit": 1 },
    { "number": "2", "name": "-", "type": "input", "side": "left", "unit": 1 },
    { "number": "3", "name": "+", "type": "input", "side": "left", "unit": 1 },
    { "number": "4", "name": "V-", "type": "power_in", "side": "bottom", "unit": 1 },
    { "number": "8", "name": "V+", "type": "power_in", "side": "top", "unit": 1 },
    { "number": "7", "name": "OUT", "type": "output", "side": "right", "unit": 2 },
    { "number": "6", "name": "-", "type": "input", "side": "left", "unit": 2 },
    { "number": "5", "name": "+", "type": "input", "side": "left", "unit": 2 },
]


def test_dual_opamp_symbol( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        project = tmp_path / "symlib"
        ok( client, "project_create", { "path": str( project ) } )
        ok( client, "lib_create", { "type": "symbol", "name": "MyParts" } )

        ok( client, "sym_create", { "library": "MyParts", "name": "DUAL_OPAMP", "reference": "U",
                                    "description": "Dual operational amplifier", "keywords": "dual op amp",
                                    "footprintFilters": [ "SOIC*" ], "units": 2 } )
        sym = ok( client, "sym_pins_set", { "library": "MyParts", "symbol": "DUAL_OPAMP", "pins": DUAL_OPAMP_PINS } )
        assert sorted( p["number"] for p in sym["pins"] ) == list( "12345678" )
        assert { p["unit"] for p in sym["pins"] } == { 1, 2 }

        for pin in sym["pins"]:
            assert abs( pin["position"]["x"] / 2.54 - round( pin["position"]["x"] / 2.54 ) ) < 1e-6

        check = ok( client, "sym_check", { "library": "MyParts", "symbol": "DUAL_OPAMP" } )
        assert check["ok"], check

        # A duplicate pin number is caught.
        ok( client, "sym_create", { "library": "MyParts", "name": "BAD", "reference": "U" } )
        ok( client, "sym_pins_set", { "library": "MyParts", "symbol": "BAD",
                                      "pins": [ { "number": "1", "name": "A" }, { "number": "1", "name": "B" } ] } )
        assert not ok( client, "sym_check", { "library": "MyParts", "symbol": "BAD" } )["ok"]
        ok( client, "sym_delete", { "library": "MyParts", "symbol": "BAD" } )

        ok( client, "sym_derive", { "library": "MyParts", "name": "DUAL_OPAMP_LP", "parent": "DUAL_OPAMP",
                                    "description": "Low power version" } )
        ok( client, "sym_field_set", { "library": "MyParts", "symbol": "DUAL_OPAMP",
                                       "fields": { "Datasheet": "https://example.com/ds.pdf", "MPN": "OPA2X" } } )
        ok( client, "sym_graphic_add", { "library": "MyParts", "symbol": "DUAL_OPAMP", "kind": "polyline", "unit": 1,
                                         "points": [ { "x": -2.54, "y": -2.54 }, { "x": 2.54, "y": 0 },
                                                     { "x": -2.54, "y": 2.54 } ] } )

        capture = client.call_tool( "sym_capture", { "library": "MyParts", "symbol": "DUAL_OPAMP", "unit": 2 } )
        image = base64.b64decode( capture["content"][0]["data"] )
        assert len( png_colours( image ) ) > 3, "the capture is blank"

        svgs = ok( client, "sym_export_svg", { "library": "MyParts", "symbol": "DUAL_OPAMP" } )["files"]
        assert len( svgs ) == 2 and all( ( project / f ).exists() for f in svgs )

        # Placeable from the schematic, through the project's adapter.
        found = ok( client, "symbol_search", { "query": "dual op amp", "library": "MyParts" } )["results"]
        assert { r["id"] for r in found } == { "MyParts:DUAL_OPAMP", "MyParts:DUAL_OPAMP_LP" }

        # Upstream's loader reads it back, and finds nothing to upgrade.
        library = project / "MyParts.kicad_sym"
        upgrade = subprocess.run( [ KICAD_CLI, "sym", "upgrade", str( library ) ], env=isolated_env,
                                  capture_output=True, text=True, timeout=120 )
        assert "not updated" in upgrade.stdout + upgrade.stderr

        exported = tmp_path / "svg"
        subprocess.run( [ KICAD_CLI, "sym", "export", "svg", "-o", str( exported ), str( library ) ],
                        env=isolated_env, capture_output=True, timeout=120 )
        names = sorted( p.name for p in exported.iterdir() )
        assert "DUAL_OPAMP_unit1.svg" in names and "DUAL_OPAMP_unit2.svg" in names

        assert "outside the project" in fail( client, "sym_list", { "library": "../x.kicad_sym" } )
    finally:
        assert client.close() == 0


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_symbol_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        client = HttpClient( url, token )
        client.initialize()
        ok( client, "project_create", { "path": str( tmp_path / "symview" ) } )
        ok( client, "lib_create", { "type": "symbol", "name": "MyParts" } )
        ok( client, "sym_create", { "library": "MyParts", "name": "DUAL_OPAMP", "units": 2 } )
        ok( client, "sym_pins_set", { "library": "MyParts", "symbol": "DUAL_OPAMP", "pins": DUAL_OPAMP_PINS } )

        viewers = { v["program"]: v for v in ok( client, "debug_ui_state" )["viewers"] }
        assert viewers["sym"]["symbol"] == "DUAL_OPAMP"
        assert viewers["sym"]["units"] == 2 and viewers["sym"]["pinRows"] == 8
    finally:
        stop( proc )
