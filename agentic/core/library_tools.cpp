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


#include <agentic/core/core_tools.h>
#include <agentic/core/library_tools.h>
#include <agentic/core/services.h>

#include <libraries/library_manager.h>

#include <wx/filename.h>
#include <wx/utils.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <sstream>
#include <thread>


namespace AGENTIC
{

namespace
{

    const char* typeName( LIBRARY_TABLE_TYPE aType )
    {
        switch( aType )
        {
        case LIBRARY_TABLE_TYPE::SYMBOL: return "symbol";
        case LIBRARY_TABLE_TYPE::FOOTPRINT: return "footprint";
        case LIBRARY_TABLE_TYPE::DESIGN_BLOCK: return "design_block";
        default: return "unknown";
        }
    }


    const char* tableFile( LIBRARY_TABLE_TYPE aType )
    {
        switch( aType )
        {
        case LIBRARY_TABLE_TYPE::SYMBOL: return "sym-lib-table";
        case LIBRARY_TABLE_TYPE::FOOTPRINT: return "fp-lib-table";
        default: return "design-block-lib-table";
        }
    }


    std::vector<std::string> words( const std::string& aText )
    {
        std::vector<std::string> out;
        std::string              current;

        for( char c : aText )
        {
            if( std::isalnum( static_cast<unsigned char>( c ) ) )
            {
                current += static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
            }
            else if( !current.empty() )
            {
                out.push_back( current );
                current.clear();
            }
        }

        if( !current.empty() )
            out.push_back( current );

        return out;
    }


    JSON rowJson( AGENT_PROJECT& aProject, const LIBRARY_TABLE_ROW& aRow )
    {
        JSON row{ { "name", aRow.Nickname().ToStdString( wxConvUTF8 ) },
                  { "uri", aRow.URI().ToStdString( wxConvUTF8 ) },
                  { "type", aRow.Type().ToStdString( wxConvUTF8 ) },
                  { "scope", aRow.Scope() == LIBRARY_TABLE_SCOPE::PROJECT ? "project" : "global" },
                  { "writable",
                    aRow.Scope() == LIBRARY_TABLE_SCOPE::PROJECT && aProject.IsInsideProject( aRow.URI() ) } };

        if( !aRow.Description().IsEmpty() )
            row["description"] = aRow.Description().ToStdString( wxConvUTF8 );

        if( aRow.Disabled() )
            row["disabled"] = true;

        if( !aRow.IsOk() )
            row["error"] = aRow.ErrorDescription().ToStdString( wxConvUTF8 );

        return row;
    }


    /// Add a row to the project table and save it, inside the current Mutate().
    void addProjectRow( AGENT_PROJECT& aProject, LIBRARY_TABLE_TYPE aType, const wxString& aName, const wxString& aUri,
                        const wxString& aDescription )
    {
        LIBRARY_MANAGER& libraries = aProject.Libraries();

        if( libraries.GetRow( aType, aName, LIBRARY_TABLE_SCOPE::BOTH ) )
        {
            throw TOOL_ERROR( "A " + std::string( typeName( aType ) ) + " library named '"
                              + aName.ToStdString( wxConvUTF8 ) + "' is already in the tables; choose another name." );
        }

        std::optional<LIBRARY_TABLE*> table = libraries.Table( aType, LIBRARY_TABLE_SCOPE::PROJECT );

        if( !table )
            throw TOOL_ERROR( "The project has no library table for this type." );

        LIBRARY_TABLE_ROW& row = ( *table )->InsertRow();
        row.SetNickname( aName );
        row.SetURI( aUri );
        row.SetType( wxS( "KiCad" ) );
        row.SetDescription( aDescription );
        row.SetScope( LIBRARY_TABLE_SCOPE::PROJECT );

        const std::string file = tableFile( aType );
        aProject.BeforeFileWrite( file );

        LIBRARY_RESULT<void> saved = ( *table )->Save();

        if( !saved.has_value() )
            throw TOOL_ERROR( "Saving " + file + " failed: " + saved.error().message.ToStdString( wxConvUTF8 ) );

        aProject.AfterFileWrite( file );
        aProject.ReloadLibraryTables();
    }

} // namespace


LIBRARY_TABLE_TYPE ParseLibraryType( const std::string& aType )
{
    if( aType == "symbol" )
        return LIBRARY_TABLE_TYPE::SYMBOL;

    if( aType == "footprint" )
        return LIBRARY_TABLE_TYPE::FOOTPRINT;

    if( aType == "design_block" )
        return LIBRARY_TABLE_TYPE::DESIGN_BLOCK;

    throw TOOL_ERROR( "Library type must be symbol, footprint or design_block; got '" + aType + "'." );
}


LIBRARY_MANAGER_ADAPTER& LoadLibraries( AGENT_PROJECT& aProject, LIBRARY_TABLE_TYPE aType, TOOL_CONTEXT& aContext,
                                        JSON& aErrors )
{
    LIBRARY_MANAGER_ADAPTER* adapter = aProject.LibraryAdapter( aType );

    if( !adapter )
    {
        throw TOOL_ERROR( std::string( "No KiCad program in this server reads " ) + typeName( aType )
                          + " libraries (see status)." );
    }

    adapter->AsyncLoad();

    // Wait for the background load, forwarding progress and honouring a cancel (D10).
    for( ;; )
    {
        std::optional<float> progress = adapter->AsyncLoadProgress();

        if( !progress || *progress >= 1.0f )
            break;

        aContext.Progress( *progress, 1.0, std::string( "loading " ) + typeName( aType ) + " libraries" );

        if( aContext.IsCancelled() )
        {
            adapter->AbortAsyncLoad();
            throw TOOL_ERROR( "Cancelled while loading the libraries." );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    adapter->BlockUntilLoaded();

    for( const auto& [name, status] : adapter->GetLibraryStatuses() )
    {
        if( status.load_status == LOAD_STATUS::LOAD_ERROR && status.error )
        {
            aErrors.push_back( { { "library", name.ToStdString( wxConvUTF8 ) },
                                 { "error", status.error->message.ToStdString( wxConvUTF8 ) } } );
        }
    }

    return *adapter;
}


double SearchScore( const std::string& aQuery, const std::string& aName, const std::string& aKeywords,
                    const std::string& aDescription, int aPinCount )
{
    const std::vector<std::string> query = words( aQuery );
    const std::vector<std::string> name = words( aName );
    const std::vector<std::string> keywords = words( aKeywords );
    const std::vector<std::string> description = words( aDescription );
    const std::string              lowerName = [&]()
    {
        std::string out;

        for( char c : aName )
            out += static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );

        return out;
    }();

    auto fieldScore = []( const std::vector<std::string>& aField, const std::string& aWord ) -> double
    {
        double best = 0;

        for( const std::string& word : aField )
        {
            if( word == aWord )
                return 1.0;

            if( aWord.size() >= 2 && word.rfind( aWord, 0 ) == 0 )
                best = std::max( best, 0.5 );
        }

        return best;
    };

    double score = 0;
    int    matched = 0;

    for( size_t i = 0; i < query.size(); ++i )
    {
        const std::string& word = query[i];

        // "8 pin", "8 pins", "8-pin": a pin count, not a word to find.
        if( std::all_of( word.begin(), word.end(), ::isdigit ) && i + 1 < query.size()
            && query[i + 1].rfind( "pin", 0 ) == 0 )
        {
            if( aPinCount == std::stoi( word ) )
            {
                score += 2.0;
                matched++;
            }

            ++i;
            continue;
        }

        double wordScore = 3.0 * fieldScore( name, word ) + 2.0 * fieldScore( keywords, word )
                           + 1.0 * fieldScore( description, word );

        // "opamp" also finds "op amp", and a word inside a name ("OPA_Dual") counts.
        if( wordScore == 0 && word.size() >= 3 && lowerName.find( word ) != std::string::npos )
            wordScore = 1.5;

        if( wordScore > 0 )
            matched++;

        score += wordScore;
    }

    if( matched == 0 )
        return 0;

    // Candidates that match every word come first.
    if( matched == static_cast<int>( query.size() ) )
        score *= 1.5;

    return score;
}


void RegisterLibraryTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY&   tools = aServices.Tools();
    SESSION_MANAGER& sessions = aServices.Sessions();
    const JSON       typeArg{ { "type", "string" }, { "enum", { "symbol", "footprint", "design_block" } } };

    tools.Add(
            { "lib_tables",
              "The symbol, footprint and design block libraries the project sees: rows of the global and project "
              "library tables (name, URI, scope, whether the agent may change the library). The agent may change "
              "only project libraries stored inside the project folder.",
              "project", ProjectToolSchema( WithPageArgs( { { "type", typeArg } } ) ),
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project = sessions.Resolve( aArgs );
                  JSON           out = JSON::object();

                  for( LIBRARY_TABLE_TYPE type :
                       { LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_TYPE::DESIGN_BLOCK } )
                  {
                      if( aArgs.contains( "type" ) && ParseLibraryType( aArgs["type"] ) != type )
                          continue;

                      JSON rows = JSON::array();

                      for( LIBRARY_TABLE_ROW* row : project.Libraries().Rows( type, LIBRARY_TABLE_SCOPE::BOTH, true ) )
                      {
                          rows.push_back( rowJson( project, *row ) );
                      }

                      out[typeName( type )] = Page( rows, aArgs );
                      out["totals"][typeName( type )] = rows.size();
                  }

                  return TOOL_RESULT::Data( out );
              } } );

    tools.Add( { "lib_add",
                 "Add an existing library to the project's library table. uri may use ${KIPRJMOD} (the project "
                 "folder) or other path variables. A library outside the project folder can be used but not "
                 "changed by the agent.",
                 "project",
                 ProjectToolSchema( { { "type", typeArg },
                                      { "name", { { "type", "string" }, { "description", "Library nickname." } } },
                                      { "uri", { { "type", "string" } } },
                                      { "description", { { "type", "string" } } } },
                                    { "type", "name", "uri" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&           project = sessions.Resolve( aArgs );
                     const LIBRARY_TABLE_TYPE type = ParseLibraryType( aArgs["type"] );
                     const wxString           name = wxString::FromUTF8( aArgs["name"].get<std::string>() );
                     const wxString           uri = wxString::FromUTF8( aArgs["uri"].get<std::string>() );

                     if( name.IsEmpty() || name.find_first_of( wxS( ":\"" ) ) != wxString::npos )
                         throw TOOL_ERROR( "A library name cannot be empty or contain ':' or '\"'." );

                     const wxString expanded = LIBRARY_MANAGER::ExpandURI( uri, project.KicadProject() );

                     if( !wxFileName::Exists( expanded ) )
                         throw TOOL_ERROR( "No library at " + expanded.ToStdString( wxConvUTF8 ) + "." );

                     JSON result = project.Mutate(
                             "lib_add " + aArgs["name"].get<std::string>(), false,
                             [&]() -> JSON
                             {
                                 addProjectRow( project, type, name, uri,
                                                wxString::FromUTF8( aArgs.value( "description", "" ) ) );

                                 std::optional<LIBRARY_TABLE_ROW*> row = project.Libraries().GetRow( type, name );
                                 return JSON{ { "added", row ? rowJson( project, **row ) : JSON() } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "lib_create",
              "Create a new empty library in the project folder and add it to the project table: a .kicad_sym "
              "file for symbols, a .pretty folder for footprints. Its URI uses ${KIPRJMOD}.",
              "project",
              ProjectToolSchema(
                      { { "type", { { "type", "string" }, { "enum", { "symbol", "footprint" } } } },
                        { "name", { { "type", "string" } } },
                        { "folder",
                          { { "type", "string" }, { "description", "Project subfolder, default the project root." } } },
                        { "description", { { "type", "string" } } } },
                      { "type", "name" } ),
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT&           project = sessions.Resolve( aArgs );
                  const LIBRARY_TABLE_TYPE type = ParseLibraryType( aArgs["type"] );
                  const std::string        name = aArgs["name"].get<std::string>();

                  if( name.empty() || name.find_first_of( "/\\:\"*?<>|" ) != std::string::npos )
                      throw TOOL_ERROR( "'" + name + "' is not a valid library name." );

                  std::string folder = aArgs.value( "folder", "" );

                  while( !folder.empty() && folder.back() == '/' )
                      folder.pop_back();

                  const std::string rel = ( folder.empty() ? "" : folder + "/" ) + name
                                          + ( type == LIBRARY_TABLE_TYPE::SYMBOL ? ".kicad_sym" : ".pretty" );
                  const std::filesystem::path path = project.Files().Resolve( rel );

                  if( std::filesystem::exists( path ) )
                      throw TOOL_ERROR( "'" + rel + "' already exists; add it with lib_add." );

                  LIBRARY_MANAGER_ADAPTER* adapter = project.LibraryAdapter( type );

                  if( !adapter )
                      throw TOOL_ERROR( std::string( "No KiCad program in this server writes " ) + typeName( type )
                                        + " libraries." );

                  JSON result =
                          project.Mutate( "lib_create " + name, false,
                                          [&]() -> JSON
                                          {
                                              std::filesystem::create_directories( path.parent_path() );
                                              addProjectRow( project, type, wxString::FromUTF8( name ),
                                                             wxS( "${KIPRJMOD}/" ) + wxString::FromUTF8( rel ),
                                                             wxString::FromUTF8( aArgs.value( "description", "" ) ) );

                                              LIBRARY_MANAGER_ADAPTER* fresh = project.LibraryAdapter( type );

                                              if( type == LIBRARY_TABLE_TYPE::SYMBOL )
                                                  project.BeforeFileWrite( rel );

                                              if( !fresh->CreateLibrary( wxString::FromUTF8( name ) ) )
                                                  throw TOOL_ERROR( "KiCad could not create the library " + rel + "." );

                                              if( type == LIBRARY_TABLE_TYPE::SYMBOL )
                                                  project.AfterFileWrite( rel );

                                              return JSON{ { "created", rel }, { "name", name } };
                                          } );

                  return TOOL_RESULT::Data( result );
              } } );
}

} // namespace AGENTIC
