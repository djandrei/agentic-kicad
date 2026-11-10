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
Stage 13: the calculator (D9).  The fork's formulas are pinned to the upstream panels: for each
table row the panel is built in a hidden frame (debug_calc_panel, viewer mode), given the same
inputs, and its displayed outputs must equal calc_run's to the panel's display precision.  An
upstream formula change that the fork does not follow fails this test, and with it the gate.
"""

import itertools
import os
import sys

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import HttpClient, StdioClient  # noqa: E402
from conftest import KICAD_BIN, KICAD_CLI, start_http_server, stop  # noqa: E402
from test_stage6_pcb import fail, ok  # noqa: E402

# Panels print with %g (6 significant digits) or %f (6 decimals).
REL, ABS = 2e-5, 2e-6

PINS = {
    "track_width": [ { "current": i, "thickness": t, "temperatureRise": d }
                     for i, t, d in itertools.product( [ 0.5, 1, 3, 10 ], [ 0.035, 0.07 ], [ 10, 20 ] ) ],
    "via_size": [ { "holeDiameter": h, "padDiameter": p, "clearanceDiameter": c, "length": l, "current": i }
                  for h, p, c, l, i in [ ( 0.3, 0.6, 1.0, 1.6, 1 ), ( 0.2, 0.45, 0.8, 0.8, 0.5 ),
                                         ( 0.8, 1.4, 2.0, 2.4, 3 ) ] ],
    "fusing_current": [ dict( { "current": 10, "width": 0.25, "thickness": 0.035, "time": 0.1 }, solve=s )
                        for s in ( "current", "width", "thickness", "time" ) ]
                      + [ { "solve": "current", "ambient": 85, "width": 1, "thickness": 0.07, "time": 2 } ],
    "wavelength": [ { "frequency": f, "permittivity": e } for f, e in itertools.product( [ 1e6, 2.4e9 ], [ 1, 4.5 ] ) ],
    "cable_size": [ { "diameter": d, "temperature": t, "length": l, "current": i, "currentDensity": j }
                    for d, t, l, i, j in [ ( 0.5, 20, 1, 1, 3 ), ( 1.0, 60, 5, 2, 4 ), ( 2.5, 90, 10, 16, 6 ) ] ],
    "regulator": [
        { "solve": "vout", "r1": 10, "r2": 3.3, "vref": 1.25, "vrefMin": 1.2, "vrefMax": 1.3 },
        { "solve": "r1", "r2": 10, "vout": 5, "vref": 1.25, "tolerance": 5 },
        { "solve": "r2", "r1": 30, "vout": 3.3, "vref": 0.8 },
        { "solve": "vout", "threeTerminal": True, "r1": 0.24, "r2": 0.72, "vref": 1.25, "iadj": 50, "iadjMax": 100 },
        { "solve": "r2", "threeTerminal": True, "r1": 0.24, "vout": 12, "vref": 1.25, "iadj": 50, "iadjMax": 100 } ],
    "board_class": [ {} ],
}


def same( ours, theirs, where ):
    if isinstance( theirs, dict ):
        for key, value in theirs.items():
            same( ours[key], value, f"{where}.{key}" )
    elif isinstance( theirs, list ):
        assert len( ours ) == len( theirs ), where

        for i, ( a, b ) in enumerate( zip( ours, theirs ) ):
            same( a, b, f"{where}[{i}]" )
    elif theirs is None:
        assert ours is None, where
    else:
        assert ours == pytest.approx( theirs, rel=REL, abs=ABS ), where


@pytest.mark.skipif( not os.environ.get( "DISPLAY" ), reason="the panels need a display (run under xvfb-run)" )
def test_formulas_match_the_upstream_panels( isolated_env, tmp_path ):
    env = dict( isolated_env, AGENTIC_TEST_HOOKS="1" )
    proc, url, token = start_http_server( [ KICAD_BIN, "--mcp-port", "0" ], env )

    try:
        client = HttpClient( url, token )
        client.initialize()
        ok( client, "project_create", { "path": str( tmp_path / "calc" ) } )
        checked = 0

        for name, rows in PINS.items():
            for inputs in rows:
                theirs = ok( client, "debug_calc_panel", { "calculator": name, "inputs": inputs } )
                ours = ok( client, "calc_run", { "calculator": name, "inputs": inputs } )["outputs"]
                same( ours, theirs, f"{name}{inputs}" )
                checked += 1

        # IPC-2221: each table row (one voltage inside each range), then the row above 500 V.
        for voltage in ( 800, 2000 ):
            rows = ok( client, "debug_calc_panel", { "calculator": "electrical_spacing_ipc2221",
                                                     "inputs": { "voltage": voltage } } )["rows"]

            for row, v in enumerate( ( 10, 20, 40, 75, 125, 160, 200, 275, 400, voltage ) ):
                ours = ok( client, "calc_run", { "calculator": "electrical_spacing_ipc2221",
                                                 "inputs": { "voltage": v } } )["outputs"]["spacing"]
                same( [ ours[k] for k in ( "B1", "B2", "B3", "B4", "A5", "A6", "A7", "A8" ) ], rows[row],
                      f"ipc2221 row {row}" )
                checked += 1

        assert checked > 40

        viewers = { v["program"]: v for v in ok( client, "debug_ui_state" )["viewers"] }
        assert viewers["calc"]["rows"] == checked and viewers["calc"]["first"] == "electrical_spacing_ipc2221"
    finally:
        stop( proc )


@pytest.fixture
def cli( isolated_env ):
    c = StdioClient( [ KICAD_CLI, "mcp", "--stdio" ], env=isolated_env )
    c.initialize()
    yield c
    c.close()


def run( client, name, inputs ):
    return ok( client, "calc_run", { "calculator": name, "inputs": inputs } )["outputs"]


def test_upstream_classes( cli ):
    names = { c["name"] for c in ok( cli, "calc_list" )["calculators"] }
    assert { "track_width", "via_size", "transmission_line", "attenuator", "eseries", "regulator",
             "electrical_spacing_iec60664", "resistor_substitution", "color_code" } <= names

    # Transmission line: synthesize 50 ohm, then analyse the width found.
    params = { "epsilonR": 4.5, "h": 1.6, "t": 0.035, "frequency": 1e9, "length": 10 }
    width = run( cli, "transmission_line", { "type": "microstrip", "mode": "synthesize",
                                             "parameters": dict( params, z0=50 ) } )["results"]["width"]
    assert 2.5 < width < 3.5
    z0 = run( cli, "transmission_line", { "type": "microstrip",
                                          "parameters": dict( params, width=width ) } )["results"]["z0"]
    assert z0 == pytest.approx( 50, rel=1e-3 )
    coax = run( cli, "transmission_line", { "type": "coax", "parameters": { "epsilonR": 1, "innerDiameter": 1,
                                                                             "outerDiameter": 2.3, "frequency": 1e8 } } )
    assert coax["results"]["z0"] == pytest.approx( 60 * __import__( "math" ).log( 2.3 ), rel=1e-3 )

    # A 6 dB 50 ohm pi pad: 150.5 / 37.4 / 150.5 ohm.
    pi = run( cli, "attenuator", { "topology": "pi", "attenuation": 6, "zin": 50 } )
    assert pi["r1"] == pytest.approx( 150.48, abs=0.01 ) and pi["r2"] == pytest.approx( 37.35, abs=0.01 )

    assert len( run( cli, "eseries", { "series": "E12" } )["values"] ) == 12
    best = run( cli, "resistor_substitution", { "target": 1234, "series": "E12" } )["solutions"]
    assert min( abs( s["error"] ) for s in best ) < 0.1

    assert run( cli, "color_code", { "value": 4700 } )["bands"] == "yellow violet red gold"
    assert run( cli, "color_code", { "value": 10.2, "bands": 5, "tolerance": 1 } )["bands"] == "brown black red gold brown"

    iec = run( cli, "electrical_spacing_iec60664", { "rmsVoltage": 230, "pollutionDegree": 2 } )
    assert iec["clearance"] > 0 and iec["creepage"] >= iec["clearance"]

    # More current needs a wider track; the width found gives the current back.
    tw = run( cli, "track_width", { "current": 2 } )
    assert tw["externalWidth"] > run( cli, "track_width", { "current": 1 } )["externalWidth"]
    back = run( cli, "track_width", { "externalWidth": tw["externalWidth"] } )
    assert back["current"] == pytest.approx( 2, rel=1e-6 )

    assert "Vref" in fail( cli, "calc_run", { "calculator": "regulator", "inputs": { "vref": 0, "r1": 1, "r2": 1 } } )
    assert "No calculator" in fail( cli, "calc_run", { "calculator": "nope" } )
