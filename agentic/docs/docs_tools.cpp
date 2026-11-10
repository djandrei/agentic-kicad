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


#include <agentic/core/fetch.h>
#include <agentic/core/services.h>
#include <agentic/docs/docs_index.h>
#include <agentic/docs/docs_tools.h>

#include <build_version.h>

#include <wx/datetime.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <tuple>


namespace AGENTIC
{

namespace
{

    const char* DEFAULT_SOURCE = "https://docs.kicad.org";

    /// The manuals on docs.kicad.org, in the order the agent will want them.
    const char* MANUALS[] = {
        "getting_started_in_kicad", "eeschema",    "pcbnew", "kicad", "cli", "gerbview", "pl_editor",
        "pcb_calculator",           "introduction"
    };

    const size_t MAX_MANUAL_BYTES = 64 * 1024 * 1024;


    struct DOCS_STATE
    {
        DOCS_INDEX     index;
        nlohmann::json meta;
        bool           loaded = false;
    };


    DOCS_STATE& state()
    {
        static DOCS_STATE s;
        return s;
    }


    std::filesystem::path indexFile( const std::string& aVersion )
    {
        return DocsCacheRoot() / aVersion / "index.json";
    }


    DOCS_STATE& loaded()
    {
        DOCS_STATE& s = state();

        if( !s.loaded )
        {
            s.meta = s.index.Load( indexFile( DocsVersion() ) );
            s.loaded = true;
        }

        return s;
    }


    void requireIndex( const DOCS_STATE& aState )
    {
        if( aState.index.PageCount() == 0 )
            throw TOOL_ERROR( "The KiCad manuals are not downloaded yet. Call docs_update once (it needs network "
                              "access to docs.kicad.org); after that they are searched offline." );
    }


    JSON prompt( const std::string& aDescription, const std::string& aText )
    {
        return JSON{ { "description", aDescription },
                     { "messages", JSON::array( { { { "role", "user" },
                                                    { "content", { { "type", "text" }, { "text", aText } } } } } ) } };
    }

} // namespace


std::string DocsVersion()
{
    const int major = std::get<0>( GetMajorMinorPatchTuple() );
    const int minor = std::get<1>( GetMajorMinorPatchTuple() );

    if( minor == 99 )
        return "master";

    return std::to_string( major ) + ".0";
}


std::filesystem::path DocsCacheRoot()
{
    if( const char* overridden = std::getenv( "AGENTIC_KICAD_DOCS_CACHE" ); overridden && *overridden )
        return std::filesystem::path( overridden );

    std::filesystem::path base;

    if( const char* xdg = std::getenv( "XDG_CACHE_HOME" ); xdg && *xdg )
        base = xdg;
    else if( const char* home = std::getenv( "HOME" ); home && *home )
        base = std::filesystem::path( home ) / ".cache";
    else
        base = std::filesystem::temp_directory_path();

    return base / "agentic-kicad" / "docs";
}


void RegisterDocsTools( AGENT_SERVICES& aServices, const std::string& aInstructions )
{
    TOOL_REGISTRY& tools = aServices.Tools();
    const JSON     noArgs{ { "type", "object" }, { "properties", JSON::object() }, { "additionalProperties", false } };

    tools.Add( { "docs_update",
                 "Download the KiCad manuals for this KiCad version from docs.kicad.org into the local cache and "
                 "index them for docs_search. Needed once; later searches are offline.",
                 "docs",
                 JSON{ { "type", "object" },
                       { "properties",
                         { { "manuals",
                             { { "type", "array" },
                               { "items", { { "type", "string" } } },
                               { "description", "Only these manuals (e.g. [\"pcbnew\"]); default all." } } },
                           { "source",
                             { { "type", "string" }, { "description", "Base URL; default https://docs.kicad.org." } } },
                           { "version",
                             { { "type", "string" },
                               { "description", "Manual version, e.g. 9.0 or master; default this build's." } } } } },
                       { "additionalProperties", false } },
                 []( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     DOCS_STATE&       s = loaded();
                     const std::string source = aArgs.value( "source", DEFAULT_SOURCE );
                     const std::string version = aArgs.value( "version", DocsVersion() );

                     std::vector<std::string> manuals;

                     if( aArgs.contains( "manuals" ) )
                         manuals = aArgs["manuals"].get<std::vector<std::string>>();
                     else
                         manuals.assign( std::begin( MANUALS ), std::end( MANUALS ) );

                     if( version != DocsVersion() )
                         s.index.Load( indexFile( version ) );

                     JSON fetched = JSON::array();
                     JSON failed = JSON::array();

                     for( size_t i = 0; i < manuals.size(); ++i )
                     {
                         const std::string& manual = manuals[i];
                         const std::string  url = source + "/" + version + "/en/" + manual + "/" + manual + ".html";

                         try
                         {
                             std::string html = Fetch( url, MAX_MANUAL_BYTES,
                                                       [&]( double aFraction )
                                                       {
                                                           aContext.Progress( ( i + aFraction ) / manuals.size(), 1.0,
                                                                              "downloading " + manual );
                                                           return !aContext.IsCancelled();
                                                       } );

                             std::vector<DOC_PAGE> pages = SplitHtmlManual( manual, html );

                             if( pages.empty() )
                                 throw TOOL_ERROR( "no sections found in " + url );

                             fetched.push_back( { { "manual", manual }, { "pages", pages.size() } } );
                             s.index.SetManual( manual, std::move( pages ) );
                         }
                         catch( const std::exception& e )
                         {
                             if( aContext.IsCancelled() )
                                 throw TOOL_ERROR( "Cancelled; manuals fetched so far were not saved." );

                             failed.push_back( { { "manual", manual }, { "error", e.what() } } );
                         }
                     }

                     if( fetched.empty() )
                         throw TOOL_ERROR( "No manual could be downloaded: " + failed.dump() );

                     s.meta = JSON{ { "version", version },
                                    { "source", source },
                                    { "updated", wxDateTime::Now().FormatISOCombined().ToStdString() } };
                     s.index.Save( indexFile( version ), s.meta );

                     JSON out{ { "version", version },
                               { "fetched", fetched },
                               { "pages", s.index.PageCount() },
                               { "cache", indexFile( version ).parent_path().string() } };

                     if( !failed.empty() )
                         out["failed"] = failed;

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "docs_search",
                 "Search the downloaded KiCad manuals (see docs_update). Returns page ids, titles and snippets, best "
                 "first; read a page with docs_page.",
                 "docs",
                 JSON{ { "type", "object" },
                       { "properties",
                         { { "query", { { "type", "string" } } },
                           { "manual", { { "type", "string" }, { "description", "Only this manual, e.g. pcbnew." } } },
                           { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 } } } } },
                       { "required", { "query" } },
                       { "additionalProperties", false } },
                 []( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     DOCS_STATE& s = loaded();
                     requireIndex( s );

                     JSON results = JSON::array();

                     for( const DOCS_INDEX::HIT& hit :
                          s.index.Search( aArgs["query"].get<std::string>(), aArgs.value( "limit", size_t( 8 ) ),
                                          aArgs.value( "manual", "" ) ) )
                     {
                         results.push_back( { { "id", hit.page->id },
                                              { "title", hit.page->title },
                                              { "manual", hit.page->manual },
                                              { "score", std::round( hit.score * 100 ) / 100 },
                                              { "snippet", hit.snippet } } );
                     }

                     return TOOL_RESULT::Data( JSON{ { "results", results } } );
                 } } );

    tools.Add( { "docs_page", "The text of one manual page (a section) by id, from docs_search.", "docs",
                 JSON{ { "type", "object" },
                       { "properties",
                         { { "id", { { "type", "string" } } },
                           { "offset", { { "type", "integer" }, { "minimum", 0 } } },
                           { "limit", { { "type", "integer" }, { "minimum", 100 } } } } },
                       { "required", { "id" } },
                       { "additionalProperties", false } },
                 []( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     DOCS_STATE& s = loaded();
                     requireIndex( s );

                     const DOC_PAGE* page = s.index.Find( aArgs["id"].get<std::string>() );

                     if( !page )
                         throw TOOL_ERROR( "No manual page '" + aArgs["id"].get<std::string>()
                                           + "'; use docs_search." );

                     const size_t offset = std::min( aArgs.value( "offset", size_t( 0 ) ), page->text.size() );
                     const size_t limit = aArgs.value( "limit", size_t( 20000 ) );
                     JSON         out{ { "id", page->id },
                                       { "title", page->title },
                                       { "manual", page->manual },
                                       { "text", page->text.substr( offset, limit ) } };

                     if( offset + limit < page->text.size() )
                         out["next"] = offset + limit;

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "docs_status", "Whether the manuals are downloaded: version, pages per manual, cache folder.", "docs",
                 noArgs, []( const JSON&, TOOL_CONTEXT& )
                 {
                     DOCS_STATE& s = loaded();
                     JSON        manuals = JSON::object();

                     for( const auto& [manual, pages] : s.index.Manuals() )
                         manuals[manual] = pages;

                     return TOOL_RESULT::Data( JSON{ { "version", DocsVersion() },
                                                     { "pages", s.index.PageCount() },
                                                     { "manuals", manuals },
                                                     { "meta", s.meta.is_null() ? JSON::object() : s.meta },
                                                     { "cache", indexFile( DocsVersion() ).parent_path().string() } } );
                 } } );

    tools.Add( { "docs_error_codes",
                 "The ERC and DRC violation types of this KiCad: code, settings key (as in results and severity "
                 "settings), title and group. Built from KiCad's own registries.",
                 "docs",
                 JSON{ { "type", "object" },
                       { "properties", { { "checker", { { "type", "string" }, { "enum", { "erc", "drc" } } } } } },
                       { "additionalProperties", false } },
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON out = JSON::object();

                     for( const auto& [checker, list] : aServices.ErrorCodes() )
                     {
                         if( !aArgs.contains( "checker" ) || aArgs["checker"] == checker )
                             out[checker] = list();
                     }

                     if( out.empty() )
                         throw TOOL_ERROR( "No KiCad program in this server provides those error codes (see status)." );

                     return TOOL_RESULT::Data( out );
                 } } );

    // Resources ------------------------------------------------------------------------------

    RESOURCE_REGISTRY& resources = aServices.Resources();

    resources.Add( { "kicad://server/instructions", "How to use this server",
                     "The working loops and conventions, as sent in the initialize result.", "text/plain",
                     [aInstructions]()
                     {
                         return RESOURCE_CONTENT{ "kicad://server/instructions", "text/plain", aInstructions, "" };
                     } } );

    resources.Add( { "kicad://docs/error-codes", "ERC and DRC violation types",
                     "Codes, settings keys and titles of every ERC and DRC check.", "application/json", [&aServices]()
                     {
                         JSON out = JSON::object();

                         for( const auto& [checker, list] : aServices.ErrorCodes() )
                             out[checker] = list();

                         return RESOURCE_CONTENT{ "kicad://docs/error-codes", "application/json", out.dump( 2 ), "" };
                     } } );

    resources.AddTemplate( { "kicad://docs/page/{id}", "KiCad manual page",
                             "A section of a downloaded KiCad manual (ids from docs_search).", "text/plain" } );

    resources.AddResolver(
            []( const std::string& aUri ) -> std::optional<RESOURCE_CONTENT>
            {
                const std::string prefix = "kicad://docs/page/";

                if( aUri.rfind( prefix, 0 ) != 0 )
                    return std::nullopt;

                DOCS_STATE&     s = loaded();
                const DOC_PAGE* page = s.index.Find( aUri.substr( prefix.size() ) );

                if( !page )
                    return std::nullopt;

                return RESOURCE_CONTENT{ aUri, "text/plain", page->title + "\n\n" + page->text, "" };
            } );

    resources.Add( { "kicad://project/log", "Current project log", "The current project's message and call log.",
                     "application/json", [&aServices]()
                     {
                         JSON records = JSON::array();

                         if( AGENT_PROJECT* project = aServices.Sessions().Current() )
                         {
                             for( const LOG_RECORD& record : project->Log().Since( 0, 1000 ) )
                                 records.push_back( record.ToJson() );
                         }

                         return RESOURCE_CONTENT{ "kicad://project/log", "application/json", records.dump( 2 ), "" };
                     } } );

    resources.Add( { "kicad://project/documents", "Current project's open documents",
                     "Project folder, open documents with their versions, and undo state.", "application/json",
                     [&aServices]()
                     {
                         JSON info = JSON::object();

                         if( AGENT_PROJECT* project = aServices.Sessions().Current() )
                             info = project->Info();

                         return RESOURCE_CONTENT{ "kicad://project/documents", "application/json", info.dump( 2 ), "" };
                     } } );

    // Prompts: the working loops (DESIGN section 5) ------------------------------------------

    PROMPT_REGISTRY& prompts = aServices.Prompts();

    prompts.Add( { "schematic_loop", "Draw or change a schematic: place, connect, annotate, ERC.", JSON::array(),
                   []( const JSON& )
                   {
                       return prompt( "Schematic working loop",
                                      "Work on the schematic in this loop:\n"
                                      "1. Find parts with symbol_search and check them with symbol_info.\n"
                                      "2. Place symbols, then connect pins with wires, labels and power symbols. "
                                      "Positions are in mm and snap to the 1.27 mm grid.\n"
                                      "3. Annotate, then run ERC and fix every error before moving on.\n"
                                      "4. Capture the sheet to look at it. Each call is saved at once; 'undo' "
                                      "takes back the last call." );
                   } } );

    prompts.Add( { "board_loop", "Lay out a board: update from schematic, place, route, fill, DRC, export.",
                   JSON::array(), []( const JSON& )
                   {
                       return prompt( "Board working loop",
                                      "Work on the board in this loop:\n"
                                      "1. Set the board outline, stackup and net classes (netclasses_set).\n"
                                      "2. Update the board from the schematic, then place footprints, checking "
                                      "courtyards.\n"
                                      "3. Route, refill zones, and run DRC; fix violations and unconnected items.\n"
                                      "4. Capture the board, then export fabrication outputs." );
                   } } );

    prompts.Add( { "library_loop", "Make a part: create a symbol or footprint, check it, assign it.", JSON::array(),
                   []( const JSON& )
                   {
                       return prompt( "Library working loop",
                                      "Make a missing part in this loop:\n"
                                      "1. Create a project library with lib_create if there is none.\n"
                                      "2. Draw the symbol or footprint from the datasheet; check pin numbers, pad "
                                      "sizes and the courtyard.\n"
                                      "3. Run the library checks, then assign the footprint to the symbol and use "
                                      "it in the design." );
                   } } );
}

} // namespace AGENTIC
