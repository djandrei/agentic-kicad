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
Shared fixtures of the agentic end-to-end tests: a private KiCad configuration, server
processes, and a copy of a fixture project.
"""

import os
import re
import shutil
import subprocess
import sys
import threading

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

KICAD_CLI = os.environ.get( "KICAD_CLI", "kicad-cli" )
KICAD_BIN = os.environ.get( "KICAD_BIN", "kicad" )
DATA_DIR = os.path.join( os.path.dirname( __file__ ), "..", "..", "data", "agentic" )

URL_RE = re.compile( r"listening at (http://\S+)" )
TOKEN_RE = re.compile( r"Bearer token: (\S+)" )


@pytest.fixture
def isolated_env( tmp_path ):
    """A private configuration directory, so tests do not touch the user's settings."""
    env = dict( os.environ )
    env["KICAD_CONFIG_HOME"] = str( tmp_path / "config" )
    env["XDG_CONFIG_HOME"] = str( tmp_path / "xdg" )
    return env


def start_http_server( argv, env, timeout=120 ):
    """Start a server process and read its URL and token from stderr."""
    proc = subprocess.Popen( argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=env, text=True )
    found = {}
    lines = []
    ready = threading.Event()

    def read_stderr():
        for line in proc.stderr:
            lines.append( line.rstrip( "\n" ) )

            if m := URL_RE.search( line ):
                found["url"] = m.group( 1 )

            if m := TOKEN_RE.search( line ):
                found["token"] = m.group( 1 )

            if "url" in found and "token" in found:
                ready.set()

    threading.Thread( target=read_stderr, daemon=True ).start()
    threading.Thread( target=lambda: [ None for _ in proc.stdout ], daemon=True ).start()

    if not ready.wait( timeout ):
        proc.kill()
        raise AssertionError( "server did not print its URL and token:\n" + "\n".join( lines ) )

    return proc, found["url"], found["token"]


def stop( proc, timeout=30 ):
    proc.terminate()

    try:
        return proc.wait( timeout )
    except subprocess.TimeoutExpired:
        proc.kill()
        raise




@pytest.fixture
def fixture_project( tmp_path ):
    """A writable copy of a project under qa/data/agentic; call with the fixture name."""
    def copy( name ):
        target = tmp_path / "projects" / name
        shutil.copytree( os.path.join( DATA_DIR, name ), target )
        return target

    return copy
