/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/**
 * Stage 3: library search ranking and the documentation index.
 */

#include <boost/test/unit_test.hpp>

#include <agentic/core/library_tools.h>
#include <agentic/docs/docs_index.h>

#include <filesystem>

using namespace AGENTIC;


BOOST_AUTO_TEST_SUITE( AgenticLibrarySearch )


BOOST_AUTO_TEST_CASE( DualOpAmpRanksFirst )
{
    struct CANDIDATE
    {
        const char* name;
        const char* keywords;
        const char* description;
        int         pins;
    };

    const CANDIDATE candidates[] = {
        { "OPA_Single_SOT23", "single op amp opamp amplifier", "Single operational amplifier, SOT-23-5", 5 },
        { "OPA_Dual_SOIC8", "dual op amp opamp amplifier", "Dual operational amplifier, low noise, SOIC-8", 8 },
        { "OPA_Quad_SOIC14", "quad op amp opamp amplifier", "Quad operational amplifier, SOIC-14", 14 },
        { "CMP_Dual_SOIC8", "dual comparator", "Dual voltage comparator, SOIC-8", 8 },
        { "R_Generic", "R res resistor", "Resistor", 2 },
    };

    const char* best = nullptr;
    double      bestScore = 0;

    for( const CANDIDATE& c : candidates )
    {
        double score = SearchScore( "op amp dual 8 pin", c.name, c.keywords, c.description, c.pins );

        if( score > bestScore )
        {
            bestScore = score;
            best = c.name;
        }
    }

    BOOST_CHECK_EQUAL( std::string( best ), "OPA_Dual_SOIC8" );
    BOOST_CHECK_EQUAL( SearchScore( "op amp", "R_Generic", "R res resistor", "Resistor", 2 ), 0.0 );
    // A word inside a longer name still counts.
    BOOST_CHECK_GT( SearchScore( "amp", "OPAMP2134", "", "", 8 ), 0.0 );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( AgenticDocsIndex )


BOOST_AUTO_TEST_CASE( SplitAndSearch )
{
    const std::string html = R"(<html><head><style>h2 { color: red }</style><script>var x = "zones";</script></head>
<body><h1>PCB Editor</h1>
<h2 id="_zones">Zones</h2><p>Copper zones are filled areas &amp; follow clearance.</p>
<h3 id="_zone_priority">Zone priority</h3><p>Higher priority fills first.</p>
<h2 id="_drc">Design rule checking</h2><p>DRC reports clearance violations.</p>
<h2>Zones</h2><p>A second section with the same title.</p></body></html>)";

    std::vector<DOC_PAGE> pages = SplitHtmlManual( "pcbnew", html );
    BOOST_REQUIRE_EQUAL( pages.size(), 5 );
    BOOST_CHECK_EQUAL( pages[1].id, "pcbnew#_zones" );
    BOOST_CHECK_EQUAL( pages[1].text, "Copper zones are filled areas & follow clearance." );
    BOOST_CHECK_EQUAL( pages[4].id, "pcbnew#zones" );

    for( const DOC_PAGE& page : pages )
        BOOST_CHECK( page.text.find( "var x" ) == std::string::npos );

    DOCS_INDEX index;
    index.SetManual( "pcbnew", pages );

    std::vector<DOCS_INDEX::HIT> hits = index.Search( "zone priority", 3 );
    BOOST_REQUIRE( !hits.empty() );
    BOOST_CHECK_EQUAL( hits[0].page->id, "pcbnew#_zone_priority" );

    hits = index.Search( "drc clearance", 3 );
    BOOST_REQUIRE( !hits.empty() );
    BOOST_CHECK_EQUAL( hits[0].page->id, "pcbnew#_drc" );
    BOOST_CHECK( index.Search( "nonexistentword", 3 ).empty() );

    // Saved and loaded, the index answers the same.
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "kicad_qa_agentic_docs" / "index.json";
    BOOST_REQUIRE( index.Save( file, { { "version", "test" } } ) );

    DOCS_INDEX     loaded;
    nlohmann::json meta = loaded.Load( file );
    BOOST_CHECK_EQUAL( meta["version"], "test" );
    BOOST_CHECK_EQUAL( loaded.PageCount(), 5 );
    BOOST_CHECK_EQUAL( loaded.Search( "zone priority", 1 )[0].page->id, "pcbnew#_zone_priority" );

    // Replacing a manual replaces only its pages.
    index.SetManual( "eeschema", SplitHtmlManual( "eeschema", "<h2 id='_erc'>ERC</h2><p>Electrical rules.</p>" ) );
    index.SetManual( "pcbnew", {} );
    BOOST_CHECK_EQUAL( index.PageCount(), 1 );
}


BOOST_AUTO_TEST_SUITE_END()
