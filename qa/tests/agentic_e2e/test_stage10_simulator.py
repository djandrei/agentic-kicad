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
Stage 10: the simulator.  An RC low-pass drawn with the stage 4 tools, a source made with the
stage 5 tools and given a SPICE model, simulated by ngspice in-process.
"""

import math
import os
import sys
import time

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, McpError, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage4_schematic import new_project  # noqa: E402
from test_stage6_pcb import fail, ok  # noqa: E402

R, C = 1000.0, 100e-9
CORNER = 1 / ( 2 * math.pi * R * C )  # 1591.55 Hz


def rc_lowpass( client, path ):
    """V1 (dc 0, ac 1) -> R1 1k -> out -> C1 100n -> GND."""
    new_project( client, path )
    ok( client, "lib_create", { "type": "symbol", "name": "Sources" } )
    ok( client, "sym_create", { "library": "Sources", "name": "VSRC", "reference": "V", "value": "VSRC" } )
    ok( client, "sym_pins_set", { "library": "Sources", "symbol": "VSRC", "pins": [
        { "number": "1", "name": "+", "type": "passive", "side": "top" },
        { "number": "2", "name": "-", "type": "passive", "side": "bottom" } ] } )

    ok( client, "sch_symbol_place", { "libId": "Sources:VSRC", "position": { "x": 60, "y": 80 } } )
    ok( client, "sch_symbol_place", { "libId": "Device:R", "position": { "x": 80, "y": 60 }, "rotation": 90,
                                      "value": "1k" } )
    ok( client, "sch_symbol_place", { "libId": "Device:C", "position": { "x": 100, "y": 80 }, "value": "100n" } )

    ok( client, "sch_connect", { "from": "V1.1", "to": "R1.1" } )
    ok( client, "sch_connect", { "from": "R1.2", "to": "C1.1" } )
    ok( client, "sch_label_add", { "text": "out", "kind": "local", "at": "C1.1" } )
    ok( client, "sch_label_add", { "text": "GND", "kind": "power", "at": "C1.2" } )
    ok( client, "sch_label_add", { "text": "GND", "kind": "power", "at": "V1.2" } )

    ok( client, "sim_model_set", { "item": "V1", "device": "V", "type": "DC", "params": { "dc": 0, "ac": 1 } } )


@pytest.fixture
def client( isolated_env, tmp_path ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    rc_lowpass( c, tmp_path / "rc" )
    yield c
    c.close()


def test_rc_corner_frequency( client ):
    fields = ok( client, "sim_model_get", { "item": "V1" } )["fields"]
    assert fields["Device"] == "V" and fields["Type"] == "DC"
    assert set( fields["Params"].split() ) == { "dc=0", "ac=1" }
    assert ok( client, "sim_model_get", { "item": "R1" } )["model"] == "inferred"

    ok( client, "sim_analysis_set", { "type": "ac", "sweep": "dec", "points": 200, "start": 1, "stop": "1Meg" } )
    run = ok( client, "sim_run" )
    assert run["type"] == "AC" and run["xAxis"] == "frequency"

    corner = ok( client, "sim_measure", { "vector": "out", "kind": "crossing", "level": -3.0103 } )["value"]
    assert abs( corner - CORNER ) / CORNER < 0.01, corner

    low = ok( client, "sim_measure", { "vector": "out", "kind": "at", "x": 10 } )["value"]
    assert abs( low ) < 0.01  # 0 dB in the pass band

    signal = ok( client, "sim_signal", { "vector": "out", "points": 50 } )
    assert len( signal["x"] ) == 50 and signal["quantity"] == "gain_db"
    phase = ok( client, "sim_signal", { "vector": "out", "quantity": "phase", "points": 50 } )
    assert -91 < phase["y"][-1] < -80

    capture = client.call_tool( "sim_plot_capture", { "width": 500, "height": 300 } )
    assert capture["content"][0]["type"] == "image" and capture["structuredContent"]["logX"]

    netlist = ok( client, "sim_netlist" )["netlist"]
    assert ".ac dec 200 1 1Meg" in netlist


def test_operating_point_and_transient( client ):
    ok( client, "sim_model_set", { "item": "V1", "device": "V", "type": "DC", "params": "dc=5" } )
    op = ok( client, "sim_run", { "analysis": ".op" } )
    # ngspice names node voltages by net: /out.
    assert op["operatingPoint"]["/out"] == pytest.approx( 5, abs=1e-6 )

    ok( client, "sim_model_set", { "item": "V1", "device": "V", "type": "PULSE",
                                   "params": "y1=0 y2=1 td=0 tr=1n tf=1n tw=10m per=20m" } )
    ok( client, "sim_run", { "analysis": ".tran 1u 2m" } )
    rise = ok( client, "sim_measure", { "vector": "out", "kind": "rise_time" } )["value"]
    # 10-90% of an RC step is ln(9) * RC = 0.2197 ms; the run stops at 2 ms (98.6%).
    assert rise == pytest.approx( math.log( 9 ) * R * C, rel=0.05 )


def test_control_code_is_refused( client ):
    for command in ( ".control", "shell rm -rf /", ".tran 1u 1m ; shell ls", ".endc" ):
        text = fail( client, "sim_analysis_set", { "command": command } )
        assert "not accepted" in text or "plain SPICE" in text

    # A directive on the sheet goes into the netlist: refused at run time.
    ok( client, "sch_text_add", { "text": ".control\nshell touch /tmp/agentic-should-not-exist\n.endc",
                                  "position": { "x": 60, "y": 120 } } )
    assert ".control" in fail( client, "sim_run", { "analysis": ".op" } )
    assert not os.path.exists( "/tmp/agentic-should-not-exist" )


def test_cancel_long_transient( client ):
    ok( client, "sim_model_set", { "item": "V1", "device": "V", "type": "SIN", "params": "dc=0 ampl=1 f=1k" } )
    started = time.time()
    result = client.call_tool( "sim_run", { "analysis": ".tran 1n 10", "timeoutSeconds": 1 } )
    assert result["isError"] and "ancel" in result["content"][0]["text"]
    assert time.time() - started < 15

    # ngspice is usable again after the stop.
    ok( client, "sim_run", { "analysis": ".op" } )


def test_status_reports_ngspice( client ):
    status = ok( client, "status" )["simulator"]
    assert status["engine"] == "ngspice" and status["available"]
    assert status["version"]


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="needs a display (run under xvfb-run)" )
def test_simulator_viewer( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        c = HttpClient( url, token )
        c.initialize()
        rc_lowpass( c, tmp_path / "simview" )
        ok( c, "sim_run", { "analysis": ".ac dec 20 1 1Meg" } )
        ok( c, "sim_signal", { "vector": "out" } )
        ok( c, "sim_measure", { "vector": "out", "kind": "crossing", "level": -3.0103 } )

        viewers = { v["program"]: v for v in ok( c, "debug_ui_state" )["viewers"] }
        assert viewers["sim"]["traces"] == 1 and viewers["sim"]["logX"]
        assert viewers["sim"]["measurementRows"] == 1
        assert viewers["sim"]["analysis"].startswith( ".ac" )
    finally:
        stop( proc )
