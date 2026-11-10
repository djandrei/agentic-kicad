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
Stage 15: hardening.  Shutdown paths leave every file at its old or its new version; tool groups
switch off; lists are paged; captures are capped.
"""

import http.client
import json
import os
import signal
import sys
import threading
import time
import urllib.parse

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, McpError, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage6_pcb import led_board, ok  # noqa: E402

TEXTS = 400


def slow_batch( rid ):
    """One call that takes a while: many board texts in one batch (one save at the end)."""
    calls = [ { "tool": "pcb_text_add", "arguments": { "text": f"T{i}", "position": { "x": 101 + i % 25, "y": 101 + i // 25 } } }
              for i in range( TEXTS ) ]
    return { "jsonrpc": "2.0", "id": rid, "method": "tools/call",
             "params": { "name": "batch", "arguments": { "calls": calls } } }


def texts_on_board( env, project ):
    """The board as a new process reads it: its text count (old version 0, new version TEXTS)."""
    reader = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=env )

    try:
        reader.initialize()
        ok( reader, "project_open", { "path": str( project ), "force": True } )
        return ok( reader, "pcb_query", { "type": "text", "limit": 1 } )["total"]
    finally:
        reader.close()


def test_stdio_eof_during_a_call( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    client.initialize()
    led_board( client, tmp_path / "eof" )

    client.send_raw( json.dumps( slow_batch( 900 ) ) )
    assert client.close( timeout=120 ) == 0
    assert texts_on_board( isolated_env, tmp_path / "eof" ) in ( 0, TEXTS )


def test_sigterm_cli_during_a_call( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    client.initialize()
    led_board( client, tmp_path / "term" )

    client.send_raw( json.dumps( slow_batch( 901 ) ) )
    time.sleep( 0.3 )
    client.proc.send_signal( signal.SIGTERM )
    assert client.proc.wait( 120 ) == 0
    assert texts_on_board( isolated_env, tmp_path / "term" ) in ( 0, TEXTS )


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_sigterm_viewer_during_a_call( isolated_env, tmp_path ):
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], isolated_env )
    client = HttpClient( url, token )
    client.initialize()
    led_board( client, tmp_path / "vterm" )

    def post():
        try:
            client.post( slow_batch( 902 ), timeout=120 )
        except OSError:
            pass  # the server goes away; that is the point

    worker = threading.Thread( target=post, daemon=True )
    worker.start()
    time.sleep( 0.5 )

    # The hub closes as from its close box; the process exits normally.
    proc.send_signal( signal.SIGTERM )
    assert proc.wait( 120 ) == 0
    assert texts_on_board( isolated_env, tmp_path / "vterm" ) in ( 0, TEXTS )


def test_http_client_disconnects_during_a_call( isolated_env, tmp_path ):
    proc, url, token = start_http_server( [ KICAD_CLI, "mcp", "--port", "0" ], isolated_env )

    try:
        client = HttpClient( url, token )
        client.initialize()
        project = tmp_path / "drop"
        led_board( client, project )

        # Send the call and hang up without reading the answer.
        parts = urllib.parse.urlparse( url )
        conn = http.client.HTTPConnection( parts.hostname, parts.port, timeout=30 )
        conn.request( "POST", parts.path, json.dumps( slow_batch( 903 ) ),
                      { "Content-Type": "application/json", "Accept": "application/json",
                        "Authorization": f"Bearer {token}", "Mcp-Session-Id": client.session_id } )
        conn.close()

        # The server is still there, and the call finished or left the board as it was.
        deadline = time.time() + 60
        total = None

        while time.time() < deadline:
            total = ok( client, "pcb_query", { "type": "text", "limit": 1 } )["total"]

            if total == TEXTS:
                break

            time.sleep( 0.5 )

        assert total in ( 0, TEXTS ) and proc.poll() is None
    finally:
        assert stop( proc ) == 0


def test_groups_paging_and_capture_caps( isolated_env, tmp_path ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.initialize()
        led_board( client, tmp_path / "caps" )
        everything = { t["name"] for t in client.list_tools() }
        assert any( n.startswith( "sim_" ) for n in everything )

        ok( client, "project_set", { "disabledGroups": [ "sim", "calc" ] } )
        shorter = { t["name"] for t in client.list_tools() }
        assert not any( n.startswith( ( "sim_", "calc_" ) ) for n in shorter ) and "pcb_query" in shorter
        assert any( n.get( "method" ) == "notifications/tools/list_changed" for n in client.notifications )
        ok( client, "project_set", { "disabledGroups": [] } )
        assert { t["name"] for t in client.list_tools() } == everything

        nets = ok( client, "sch_nets", { "limit": 1 } )
        assert len( nets["nets"] ) == 1 and nets["total"] >= 3
        assert len( ok( client, "sch_nets", { "offset": 1, "limit": 500 } )["nets"] ) == nets["total"] - 1
        tables = ok( client, "lib_tables", { "limit": 1 } )
        assert len( tables["footprint"] ) <= 1 and "footprint" in tables["totals"]

        capture = client.call_tool( "pcb_capture", {} )
        assert capture["structuredContent"]["width"] <= 1024

        with pytest.raises( McpError, match="maximum" ):
            client.call_tool( "pcb_capture", { "width": 5000 } )
    finally:
        client.close()
