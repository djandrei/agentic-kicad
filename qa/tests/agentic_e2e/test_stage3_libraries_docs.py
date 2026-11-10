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
Stage 3: libraries, Plugin and Content Manager packages, and documentation.
"""

import hashlib
import json
import os
import sys
import zipfile

import pytest

sys.path.insert( 0, os.path.dirname( __file__ ) )

from client import StdioClient  # noqa: E402
from conftest import DATA_DIR, KICAD_CLI  # noqa: E402


def ok( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert not result["isError"], f"{name}: {result['content']}"
    return result["structuredContent"]


def fail( client, name, args=None ):
    result = client.call_tool( name, args or {} )
    assert result["isError"], f"{name} should fail: {result}"
    return result["content"][0]["text"]


@pytest.fixture
def libs_client( isolated_env, fixture_project, tmp_path ):
    project = fixture_project( "libs" )
    env = dict( isolated_env )
    env["KICAD10_3RD_PARTY"] = str( tmp_path / "3rdparty" )
    env["AGENTIC_KICAD_DOCS_CACHE"] = str( tmp_path / "docs-cache" )
    client = StdioClient( [ KICAD_CLI, "mcp", "--stdio", "--project", str( project ) ], env=env )
    client.initialize()
    yield client, project, tmp_path
    client.close()


def test_symbol_search_ranks_dual_op_amp_first( libs_client ):
    client, _, _ = libs_client
    results = ok( client, "symbol_search", { "query": "op amp dual 8 pin" } )["results"]
    assert results[0]["id"] == "Agentic_Amplifiers:OPA_Dual_SOIC8"
    assert all( r["id"] != "Agentic_Amplifiers:R_Generic" for r in results )

    info = ok( client, "symbol_info", { "id": "Agentic_Amplifiers:OPA_Dual_SOIC8" } )
    assert info["pins"] == 8
    assert { p["number"] for p in info["pinList"] } == { str( n ) for n in range( 1, 9 ) }
    assert info["footprintFilters"] == [ "SOIC*3.9x4.9mm*P1.27mm*" ]

    assert "No symbol" in fail( client, "symbol_info", { "id": "Agentic_Amplifiers:Nope" } )


def test_footprints_and_tables( libs_client ):
    client, project, _ = libs_client
    results = ok( client, "footprint_search", { "query": "soic 8 pin" } )["results"]
    assert results[0]["id"] == "Agentic_Packages:SOIC-8"

    info = ok( client, "footprint_info", { "id": "Agentic_Packages:SOT-23-5" } )
    assert len( info["padList"] ) == 5
    assert info["courtyard"]["F.CrtYd"]["x2"] == pytest.approx( 2.095 )

    tables = ok( client, "lib_tables" )
    assert tables["symbol"][0]["writable"] is True

    ok( client, "lib_create", { "type": "symbol", "name": "Mine", "folder": "lib" } )
    assert ( project / "lib" / "Mine.kicad_sym" ).exists()
    names = [ row["name"] for row in ok( client, "lib_tables", { "type": "symbol" } )["symbol"] ]
    assert "Mine" in names

    ok( client, "undo" )
    names = [ row["name"] for row in ok( client, "lib_tables", { "type": "symbol" } )["symbol"] ]
    assert "Mine" not in names

    assert "already in the tables" in fail( client, "lib_add", {
            "type": "symbol", "name": "Agentic_Amplifiers", "uri": "${KIPRJMOD}/Agentic_Amplifiers.kicad_sym" } )


def make_repository( root ):
    """A Plugin and Content Manager repository on disk: a library package and a plugin."""
    root.mkdir( parents=True )
    library_zip = root / "library.zip"

    with zipfile.ZipFile( library_zip, "w" ) as z:
        z.write( os.path.join( DATA_DIR, "libs", "Agentic_Amplifiers.kicad_sym" ), "symbols/Fixture_Amps.kicad_sym" )
        z.write( os.path.join( DATA_DIR, "libs", "Agentic_Packages.pretty", "SOIC-8.kicad_mod" ),
                 "footprints/Fixture_Pkg.pretty/SOIC-8.kicad_mod" )
        z.writestr( "metadata.json", "{}" )

    plugin_zip = root / "plugin.zip"

    with zipfile.ZipFile( plugin_zip, "w" ) as z:
        z.writestr( "plugins/__init__.py", "print('would run')\n" )

    def version( path ):
        data = path.read_bytes()
        return { "version": "1.0.0", "status": "stable", "kicad_version": "8.0",
                 "download_url": path.as_uri(), "download_sha256": hashlib.sha256( data ).hexdigest(),
                 "download_size": len( data ), "install_size": len( data ) }

    packages = { "packages": [
        { "name": "Fixture amplifiers", "description": "Op amps for the tests", "identifier": "org.example.fixtureamps",
          "type": "library", "author": { "name": "qa" }, "license": "CC0-1.0", "resources": {},
          "versions": [ version( library_zip ) ] },
        { "name": "Fixture plugin", "description": "A plugin that must be refused", "identifier": "org.example.plugin",
          "type": "plugin", "author": { "name": "qa" }, "license": "MIT", "resources": {},
          "versions": [ version( plugin_zip ) ] } ] }

    packages_file = root / "packages.json"
    packages_file.write_text( json.dumps( packages ) )
    repository = { "name": "Fixture repository",
                   "packages": { "url": packages_file.as_uri(),
                                 "sha256": hashlib.sha256( packages_file.read_bytes() ).hexdigest(),
                                 "update_timestamp": 0 } }
    ( root / "repository.json" ).write_text( json.dumps( repository ) )
    return ( root / "repository.json" ).as_uri()


def test_pcm_installs_libraries_and_refuses_plugins( libs_client ):
    client, project, tmp = libs_client
    url = make_repository( tmp / "repo" )

    catalog = { p["identifier"]: p for p in ok( client, "pcm_catalog", { "repository": url } )["packages"] }
    assert catalog["org.example.fixtureamps"]["installable"] is True
    assert catalog["org.example.plugin"]["installable"] is False

    assert "plugin" in fail( client, "pcm_install", { "identifier": "org.example.plugin", "repository": url } )
    assert not ( tmp / "3rdparty" / "plugins" ).exists()

    result = ok( client, "pcm_install", { "identifier": "org.example.fixtureamps", "repository": url } )
    assert result["version"] == "1.0.0"
    assert ( tmp / "3rdparty" / "symbols" / "org_example_fixtureamps" / "Fixture_Amps.kicad_sym" ).exists()
    assert ( tmp / "3rdparty" / "footprints" / "org_example_fixtureamps" / "Fixture_Pkg.pretty" / "SOIC-8.kicad_mod" ).exists()
    assert not ( tmp / "3rdparty" / "metadata.json" ).exists()
    added = { lib["name"] for lib in result["librariesAdded"] }
    assert added == { "PCM_Fixture_Amps", "PCM_Fixture_Pkg" }

    # The installed library is searchable from the project.
    results = ok( client, "symbol_search", { "query": "dual op amp", "library": "PCM_Fixture_Amps" } )["results"]
    assert results[0]["id"] == "PCM_Fixture_Amps:OPA_Dual_SOIC8"

    # KiCad's own record of installed packages.
    installed = json.loads( ( tmp / "config" / "10.99" / "installed_packages.json" ).read_text() )
    assert installed["packages"][0]["package"]["identifier"] == "org.example.fixtureamps"

    again = { p["identifier"]: p for p in ok( client, "pcm_catalog", { "repository": url } )["packages"] }
    assert again["org.example.fixtureamps"]["installed"] == "1.0.0"


def test_docs_index_from_fixture_corpus( libs_client ):
    client, _, tmp = libs_client
    assert "docs_update" in fail( client, "docs_search", { "query": "zones" } )

    source = "file://" + os.path.abspath( os.path.join( DATA_DIR, "docs" ) )
    update = ok( client, "docs_update", { "source": source, "manuals": [ "pcbnew", "eeschema", "missing" ] } )
    assert { m["manual"] for m in update["fetched"] } == { "pcbnew", "eeschema" }
    assert update["failed"][0]["manual"] == "missing"

    hits = ok( client, "docs_search", { "query": "zone priority" } )["results"]
    assert hits[0]["id"] == "pcbnew#_zone_priority"

    hits = ok( client, "docs_search", { "query": "unconnected pins", "manual": "eeschema" } )["results"]
    assert hits[0]["id"] == "eeschema#_electrical_rules_check"

    page = ok( client, "docs_page", { "id": "eeschema#_labels" } )
    assert "global labels connect across sheets" in page["text"]
    assert "—" in page["text"]

    status = ok( client, "docs_status" )
    assert status["manuals"] == { "pcbnew": 5, "eeschema": 3 }
    assert ( tmp / "docs-cache" / "master" / "index.json" ).exists()

    content = client.request( "resources/read", { "uri": "kicad://docs/page/pcbnew#_zones" } )["contents"][0]
    assert "Copper zones" in content["text"]


def test_error_codes_and_rules_syntax( libs_client ):
    client, _, _ = libs_client
    codes = ok( client, "docs_error_codes" )
    erc = { c["key"] for c in codes["erc"] }
    drc = { c["key"] for c in codes["drc"] }

    assert { "pin_not_connected", "power_pin_not_driven", "label_dangling" } <= erc
    assert { "clearance", "unconnected_items", "courtyards_overlap", "track_dangling", "shorting_items",
             "lib_footprint_issues" } <= drc
    assert len( { c["code"] for c in codes["drc"] } ) == len( codes["drc"] )

    rules = client.call_tool( "docs_rules_syntax" )["content"][0]["text"]
    assert "(constraint <constraint_type> ...)" in rules

    prompts = { p["name"] for p in client.request( "prompts/list" )["prompts"] }
    assert { "schematic_loop", "board_loop", "library_loop" } <= prompts
