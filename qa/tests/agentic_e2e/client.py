#!/usr/bin/env python3
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
Minimal MCP client for the agentic end-to-end tests.  Standard library only.

    StdioClient( [ "kicad-cli", "mcp", "--stdio" ] )
    HttpClient( "http://127.0.0.1:PORT/mcp", token )

Both offer initialize(), request(), notify(), call_tool() and close().  Run as a script to
print the tools of a server:

    client.py stdio kicad-cli mcp --stdio
    client.py http URL TOKEN
"""

import json
import subprocess
import sys
import threading
import urllib.error
import urllib.request

PROTOCOL_VERSION = "2025-06-18"


class McpError( Exception ):
    def __init__( self, error ):
        super().__init__( f"{error.get( 'code' )}: {error.get( 'message' )}" )
        self.error = error


class _Base:
    def __init__( self ):
        self._next_id = 1
        self.server_info = None
        self.instructions = None

    def _new_id( self ):
        rid = self._next_id
        self._next_id += 1
        return rid

    def initialize( self, client_name="agentic-e2e" ):
        result = self.request( "initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": { "name": client_name, "version": "1" } } )
        self.server_info = result.get( "serverInfo" )
        self.instructions = result.get( "instructions" )
        self.notify( "notifications/initialized" )
        return result

    def list_tools( self ):
        return self.request( "tools/list" )["tools"]

    def call_tool( self, name, arguments=None, timeout=120 ):
        """Return the tool result; raise McpError for protocol errors only."""
        return self.request( "tools/call", { "name": name, "arguments": arguments or {} },
                             timeout=timeout )


class StdioClient( _Base ):
    """Talks to a server started as a child process, one JSON message per line."""

    def __init__( self, argv, env=None, cwd=None ):
        super().__init__()
        self.proc = subprocess.Popen( argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, env=env, cwd=cwd, text=True,
                                      bufsize=1 )
        self.notifications = []
        self.stderr_lines = []
        self._responses = {}
        self._cond = threading.Condition()
        self._closed = False
        threading.Thread( target=self._read_stdout, daemon=True ).start()
        threading.Thread( target=self._read_stderr, daemon=True ).start()

    def _read_stdout( self ):
        for line in self.proc.stdout:
            line = line.strip()

            if not line:
                continue

            message = json.loads( line )
            messages = message if isinstance( message, list ) else [ message ]

            with self._cond:
                for m in messages:
                    if "id" in m and ( "result" in m or "error" in m ):
                        self._responses[json.dumps( m["id"] )] = m
                    else:
                        self.notifications.append( m )

                self._cond.notify_all()

        with self._cond:
            self._closed = True
            self._cond.notify_all()

    def _read_stderr( self ):
        for line in self.proc.stderr:
            self.stderr_lines.append( line.rstrip( "\n" ) )

    def send_raw( self, text ):
        self.proc.stdin.write( text + "\n" )
        self.proc.stdin.flush()

    def wait_response( self, rid, timeout=120 ):
        key = json.dumps( rid )

        with self._cond:
            if not self._cond.wait_for( lambda: key in self._responses or self._closed, timeout ):
                raise TimeoutError( f"no response to request {rid}" )

            if key not in self._responses:
                raise EOFError( "server closed stdout:\n" + "\n".join( self.stderr_lines[-20:] ) )

            return self._responses.pop( key )

    def request( self, method, params=None, timeout=120, meta=None ):
        rid = self._new_id()
        message = { "jsonrpc": "2.0", "id": rid, "method": method }

        if params is not None or meta is not None:
            message["params"] = dict( params or {} )

            if meta:
                message["params"]["_meta"] = meta

        self.send_raw( json.dumps( message ) )
        response = self.wait_response( rid, timeout )

        if "error" in response:
            raise McpError( response["error"] )

        return response["result"]

    def notify( self, method, params=None ):
        message = { "jsonrpc": "2.0", "method": method }

        if params is not None:
            message["params"] = params

        self.send_raw( json.dumps( message ) )

    def close( self, timeout=30 ):
        if self.proc.stdin and not self.proc.stdin.closed:
            self.proc.stdin.close()

        try:
            return self.proc.wait( timeout )
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise


class HttpClient( _Base ):
    """Streamable HTTP: one POST per message; plain JSON or an SSE stream in reply."""

    def __init__( self, url, token, origin=None ):
        super().__init__()
        self.url = url
        self.token = token
        self.origin = origin
        self.session_id = None
        self.notifications = []

    def post( self, message, timeout=120, accept_sse=False ):
        """Return (status, headers, list of JSON messages in the reply)."""
        headers = { "Content-Type": "application/json",
                    "Accept": "application/json, text/event-stream" if accept_sse
                              else "application/json" }

        if self.token is not None:
            headers["Authorization"] = f"Bearer {self.token}"

        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id

        if self.origin:
            headers["Origin"] = self.origin

        req = urllib.request.Request( self.url, data=json.dumps( message ).encode(),
                                      headers=headers, method="POST" )

        try:
            with urllib.request.urlopen( req, timeout=timeout ) as resp:
                body = resp.read().decode( "utf-8", "replace" )
                status, resp_headers = resp.status, dict( resp.headers )
                content_type = resp.headers.get( "Content-Type", "" )
        except urllib.error.HTTPError as e:
            return e.code, dict( e.headers ), []

        if not body:
            return status, resp_headers, []

        if content_type.startswith( "text/event-stream" ):
            messages = [ json.loads( line[len( "data:" ):].strip() )
                         for line in body.splitlines() if line.startswith( "data:" ) ]
        else:
            parsed = json.loads( body )
            messages = parsed if isinstance( parsed, list ) else [ parsed ]

        return status, resp_headers, messages

    def request( self, method, params=None, timeout=120, meta=None ):
        rid = self._new_id()
        message = { "jsonrpc": "2.0", "id": rid, "method": method }

        if params is not None or meta is not None:
            message["params"] = dict( params or {} )

            if meta:
                message["params"]["_meta"] = meta

        status, headers, messages = self.post( message, timeout,
                                               accept_sse=bool( meta and "progressToken" in meta ) )

        if status != 200:
            raise McpError( { "code": status, "message": f"HTTP {status}" } )

        for key, value in headers.items():
            if key.lower() == "mcp-session-id":
                self.session_id = value

        response = None

        for m in messages:
            if m.get( "id" ) == rid and ( "result" in m or "error" in m ):
                response = m
            else:
                self.notifications.append( m )

        if response is None:
            raise McpError( { "code": -1, "message": "no response in reply" } )

        if "error" in response:
            raise McpError( response["error"] )

        return response["result"]

    def notify( self, method, params=None ):
        message = { "jsonrpc": "2.0", "method": method }

        if params is not None:
            message["params"] = params

        status, _, _ = self.post( message )

        if status != 202:
            raise McpError( { "code": status, "message": f"notification answered with HTTP {status}" } )

    def close( self ):
        return 0


def main( argv ):
    if len( argv ) >= 3 and argv[1] == "stdio":
        client = StdioClient( argv[2:] )
    elif len( argv ) == 4 and argv[1] == "http":
        client = HttpClient( argv[2], argv[3] )
    else:
        print( __doc__, file=sys.stderr )
        return 2

    try:
        info = client.initialize()
        print( json.dumps( info.get( "serverInfo" ) ) )

        for tool in client.list_tools():
            print( tool["name"] )

        return 0
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit( main( sys.argv ) )
