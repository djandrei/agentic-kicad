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
Stage 2: projects, files, undo and the hub.  The viewer-mode test checks that a call shows in
both log_get and the hub's Activity list (read through the AGENTIC_TEST_HOOKS introspection
tool).
"""

import os
import sys

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


def test_headless_files_and_undo( isolated_env, fixture_project ):
    project = fixture_project( "basic" )
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio", "--project", str( project ) ], env=isolated_env )

    try:
        client.initialize()
        assert ok( client, "project_info" )["name"] == "basic"

        written = ok( client, "file_write", { "path": "notes/todo.md", "content": "- first\n" } )
        assert ( project / "notes" / "todo.md" ).read_text() == "- first\n"

        ok( client, "file_write", { "path": "notes/todo.md", "baseVersion": written["version"],
                                    "edits": [ { "old": "first", "new": "second" } ] } )
        assert ( project / "notes" / "todo.md" ).read_text() == "- second\n"

        # Stale version, escape and design file are all refused.
        assert "changed since" in fail( client, "file_write", { "path": "notes/todo.md", "content": "x",
                                                                "baseVersion": written["version"] } )
        assert "outside the project" in fail( client, "file_read", { "path": "../../etc/passwd" } )
        assert "design file" in fail( client, "file_write", { "path": "basic.kicad_pro", "content": "{}" } )

        ok( client, "undo" )
        assert ( project / "notes" / "todo.md" ).read_text() == "- first\n"
        ok( client, "undo" )
        assert not ( project / "notes" / "todo.md" ).exists()

        calls = [ r for r in ok( client, "log_get" )["records"] if r["severity"] == "call" ]
        assert [ r["tool"] for r in calls ][:2] == [ "project_info", "file_write" ]
    finally:
        assert client.close() == 0


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_hub_shows_calls( isolated_env, fixture_project ):
    project = fixture_project( "basic" )
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        client = HttpClient( url, token )
        client.initialize()

        ok( client, "project_open", { "path": str( project ) } )
        ok( client, "file_write", { "path": "note.txt", "content": "from the agent\n" } )
        assert ( project / "note.txt" ).read_text() == "from the agent\n"

        records = ok( client, "log_get" )["records"]
        assert any( r["severity"] == "call" and r["tool"] == "file_write" for r in records )

        state = ok( client, "debug_ui_state" )
        tools = [ row["tool"] for row in state["activity"] ]
        assert "project_open" in tools and "file_write" in tools
        assert any( "basic" in name for name in state["projects"] )
        assert "note.txt" in state["files"]

        # Pause from the hub side makes calls fail with a paused error.
        assert state["paused"] is False
    finally:
        stop( proc )
