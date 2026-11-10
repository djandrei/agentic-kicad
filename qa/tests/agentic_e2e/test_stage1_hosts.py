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
Stage 1 acceptance: both hosts serve version and status, over stdio (kicad-cli) and HTTP
(kicad-cli and the kicad viewer mode).
"""

import os
import subprocess
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, McpError, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402


def check_core_tools( client, expected_mode ):
    init = client.initialize()
    assert init["serverInfo"]["name"] == "agentic-kicad"
    assert "project_open" in ( init.get( "instructions" ) or "" )

    names = { tool["name"] for tool in client.list_tools() }
    assert { "version", "status" } <= names

    version = client.call_tool( "version" )
    assert not version["isError"]
    assert version["structuredContent"]["server"] == "agentic-kicad"
    assert version["structuredContent"]["kicadVersion"]

    status = client.call_tool( "status" )
    assert not status["isError"]
    assert status["structuredContent"]["mode"] == expected_mode
    assert status["structuredContent"]["paused"] is False
    assert any( provider.get( "group" ) == "pcb" and provider["available"]
                for provider in status["structuredContent"]["providers"] )


def test_cli_stdio( isolated_env ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        check_core_tools( client, "headless" )

        with pytest.raises( McpError ) as err:
            client.request( "no/such/method" )

        assert err.value.error["code"] == -32601
    finally:
        assert client.close() == 0


def test_cli_stdio_ignores_garbage( isolated_env ):
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )

    try:
        client.send_raw( "this is not json" )
        check_core_tools( client, "headless" )
    finally:
        client.close()


def test_cli_http( isolated_env ):
    proc, url, token = start_http_server( [ KICAD_CLI, "mcp", "--port", "0" ], isolated_env )

    try:
        assert url.startswith( "http://127.0.0.1:" )

        # Wrong token, then a foreign origin: both refused.
        assert HttpClient( url, "wrong" ).post( { "jsonrpc": "2.0", "id": 1, "method": "ping" } )[0] == 401
        foreign = HttpClient( url, token, origin="http://evil.example" )
        assert foreign.post( { "jsonrpc": "2.0", "id": 1, "method": "ping" } )[0] == 403

        client = HttpClient( url, token )
        check_core_tools( client, "headless" )
        assert client.session_id
    finally:
        stop( proc )


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_kicad_viewer_mode_http( isolated_env ):
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], isolated_env )

    try:
        client = HttpClient( url, token )
        check_core_tools( client, "viewer" )

        # Pause and resume are a viewer control; the agent sees status even when paused.
        status = client.call_tool( "status" )
        assert status["structuredContent"]["paused"] is False
    finally:
        stop( proc )


def test_cli_rejects_both_modes( isolated_env ):
    result = subprocess.run( [ KICAD_CLI, "mcp", "--stdio", "--port", "0" ], env=isolated_env,
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60 )
    assert result.returncode != 0
    assert "exactly one" in result.stderr
