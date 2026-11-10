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
#include <agentic/core/fetch.h>
#include <agentic/core/file_store.h>
#include <agentic/core/library_tools.h>
#include <agentic/core/pcm_tools.h>
#include <agentic/core/services.h>

#include <build_version.h>
#include <env_vars.h>
#include <libraries/library_manager.h>
#include <paths.h>
#include <pgm_base.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>

#include <picosha2.h>

#include <wx/mstream.h>
#include <wx/utils.h>
#include <wx/zipstrm.h>

#include <algorithm>
#include <ctime>
#include <set>
#include <sstream>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    const char* OFFICIAL_REPOSITORY = "https://repository.kicad.org/repository.json";

    const std::set<std::string> INSTALLABLE_TYPES = { "library", "colortheme" };

    /// Top-level folders of a package archive that KiCad's PCM installs (pcm.h).
    const std::set<std::string> PACKAGE_FOLDERS = { "plugins", "footprints", "3dmodels", "symbols",      "resources",
                                                    "colors",  "templates",  "scripts",  "design_blocks" };

    const size_t MAX_METADATA_BYTES = 16 * 1024 * 1024;
    const size_t MAX_PACKAGE_BYTES = 1024ull * 1024 * 1024;


    std::string sha256( const std::string& aData )
    {
        return picosha2::hash256_hex_string( aData );
    }


    struct REPOSITORY
    {
        std::string url;
        std::string name;
        JSON        packages = JSON::array();
    };


    /// Version a.b.c compared numerically.
    std::vector<int> versionParts( const std::string& aVersion )
    {
        std::vector<int>  parts;
        std::stringstream in( aVersion );
        std::string       part;

        while( std::getline( in, part, '.' ) )
            parts.push_back( std::atoi( part.c_str() ) );

        return parts;
    }


    bool kicadCompatible( const JSON& aVersion )
    {
        const int              major = std::get<0>( GetMajorMinorPatchTuple() );
        const int              minor = std::get<1>( GetMajorMinorPatchTuple() );
        const std::vector<int> running = { major, minor };

        if( aVersion.contains( "kicad_version" ) )
        {
            if( versionParts( aVersion["kicad_version"].get<std::string>() ) > running )
                return false;
        }

        if( aVersion.contains( "kicad_version_max" ) )
        {
            std::vector<int> max = versionParts( aVersion["kicad_version_max"].get<std::string>() );

            if( !max.empty() && max[0] < major )
                return false;
        }

        return true;
    }


    /// The newest stable version this KiCad can use, or the one named.  The result points
    /// into \a aPackage.
    const JSON* chooseVersion( const JSON& aPackage, const std::string& aWanted )
    {
        const JSON* best = nullptr;

        if( !aPackage.contains( "versions" ) || !aPackage["versions"].is_array() )
            return nullptr;

        for( const JSON& version : aPackage["versions"] )
        {
            if( !aWanted.empty() )
            {
                if( version.value( "version", "" ) == aWanted )
                    return &version;

                continue;
            }

            if( version.value( "status", "stable" ) != "stable" || !kicadCompatible( version ) )
                continue;

            if( !best || versionParts( version.value( "version", "" ) ) > versionParts( best->value( "version", "" ) ) )
                best = &version;
        }

        return best;
    }


    std::string resolveUrl( const std::string& aBase, const std::string& aUrl )
    {
        if( aUrl.find( "://" ) != std::string::npos )
            return aUrl;

        const size_t slash = aBase.find_last_of( '/' );
        return slash == std::string::npos ? aUrl : aBase.substr( 0, slash + 1 ) + aUrl;
    }


    REPOSITORY fetchRepository( const std::string& aUrl, TOOL_CONTEXT& aContext )
    {
        REPOSITORY repository;
        repository.url = aUrl;

        JSON meta;

        try
        {
            meta = JSON::parse( Fetch( aUrl, MAX_METADATA_BYTES ) );
        }
        catch( const JSON::exception& e )
        {
            throw TOOL_ERROR( "The repository at " + aUrl + " is not valid JSON: " + e.what() );
        }

        repository.name = meta.value( "name", aUrl );

        if( !meta.contains( "packages" ) || !meta["packages"].contains( "url" ) )
            throw TOOL_ERROR( "The repository at " + aUrl + " lists no packages." );

        const std::string packagesUrl = resolveUrl( aUrl, meta["packages"]["url"].get<std::string>() );
        const std::string packagesText = Fetch( packagesUrl, MAX_METADATA_BYTES,
                                                [&]( double aFraction )
                                                {
                                                    aContext.Progress( aFraction, 1.0, "fetching package list" );
                                                    return !aContext.IsCancelled();
                                                } );

        if( meta["packages"].contains( "sha256" )
            && sha256( packagesText ) != meta["packages"]["sha256"].get<std::string>() )
            throw TOOL_ERROR( "The package list of " + repository.name + " does not match its checksum." );

        try
        {
            repository.packages = JSON::parse( packagesText ).value( "packages", JSON::array() );
        }
        catch( const JSON::exception& e )
        {
            throw TOOL_ERROR( "The package list of " + repository.name + " is not valid JSON: " + e.what() );
        }

        return repository;
    }


    std::vector<std::pair<std::string, std::string>> configuredRepositories()
    {
        std::vector<std::pair<std::string, std::string>> out;

        if( KICAD_SETTINGS* settings = Pgm().GetSettingsManager().GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
        {
            for( const auto& [name, url] : settings->m_PcmRepositories )
                out.emplace_back( name.ToStdString( wxConvUTF8 ), url.ToStdString( wxConvUTF8 ) );
        }

        if( out.empty() )
            out.emplace_back( "KiCad official repository", OFFICIAL_REPOSITORY );

        return out;
    }


    fs::path installedFile()
    {
        return fs::path( PATHS::GetUserSettingsPath().ToStdString( wxConvUTF8 ) ) / "installed_packages.json";
    }


    JSON installedPackages()
    {
        if( !fs::exists( installedFile() ) )
            return JSON{ { "packages", JSON::array() } };

        try
        {
            JSON data = JSON::parse( FILE_STORE::ReadFile( installedFile() ) );

            if( data.contains( "packages" ) && data["packages"].is_array() )
                return data;
        }
        catch( const std::exception& )
        {
        }

        return JSON{ { "packages", JSON::array() } };
    }


    std::string installedVersion( const JSON& aInstalled, const std::string& aIdentifier )
    {
        for( const JSON& entry : aInstalled["packages"] )
        {
            if( entry.contains( "package" ) && entry["package"].value( "identifier", "" ) == aIdentifier )
                return entry.value( "current_version", "" );
        }

        return std::string();
    }


    /// Unpack the archive as KiCad's PCM does; returns the files written (relative to aRoot).
    std::vector<std::string> extract( const std::string& aZip, const fs::path& aRoot, const std::string& aIdentifier )
    {
        std::string folderId = aIdentifier;
        std::replace( folderId.begin(), folderId.end(), '.', '_' );

        wxMemoryInputStream memory( aZip.data(), aZip.size() );
        wxZipInputStream    zip( memory );

        if( !zip.IsOk() )
            throw TOOL_ERROR( "The package is not a valid zip archive." );

        std::vector<std::string> written;

        for( std::unique_ptr<wxZipEntry> entry( zip.GetNextEntry() ); entry; entry.reset( zip.GetNextEntry() ) )
        {
            const std::string name = entry->GetName( wxPATH_UNIX ).ToStdString( wxConvUTF8 );
            fs::path          path( name );

            if( entry->IsDir() || path.is_absolute() )
                continue;

            std::vector<std::string> parts;

            for( const fs::path& part : path )
            {
                if( part == ".." )
                    throw TOOL_ERROR( "The package contains a path leaving its folder: " + name );

                if( part != "." && !part.empty() )
                    parts.push_back( part.string() );
            }

            if( parts.size() < 2 || !PACKAGE_FOLDERS.count( parts[0] ) )
                continue; // files at the archive root (metadata.json, icon) are not installed

            fs::path target = aRoot / parts[0] / folderId;

            for( size_t i = 1; i < parts.size(); ++i )
                target /= parts[i];

            std::string        content;
            std::ostringstream buffer;
            char               chunk[65536];

            while( zip.Read( chunk, sizeof( chunk ) ).LastRead() > 0 )
                content.append( chunk, zip.LastRead() );

            FILE_STORE::WriteAtomic( target, content );
            written.push_back( target.lexically_relative( aRoot ).generic_string() );
        }

        return written;
    }


    JSON packageSummary( const JSON& aPackage, const JSON& aInstalled )
    {
        const std::string identifier = aPackage.value( "identifier", "" );
        const std::string type = aPackage.value( "type", "" );
        const JSON*       version = chooseVersion( aPackage, "" );
        JSON              out{ { "identifier", identifier },
                               { "name", aPackage.value( "name", "" ) },
                               { "type", type },
                               { "description", aPackage.value( "description", "" ) },
                               { "installable", INSTALLABLE_TYPES.count( type ) > 0 } };

        if( version )
            out["version"] = version->value( "version", "" );

        if( const std::string installed = installedVersion( aInstalled, identifier ); !installed.empty() )
            out["installed"] = installed;

        if( !INSTALLABLE_TYPES.count( type ) )
            out["reason"] = "only library and colortheme packages can be installed by the agent";

        return out;
    }

} // namespace


fs::path PcmInstallRoot()
{
    // The process environment overrides the configured path, as at KiCad's startup.
    wxString fromEnv;

    if( wxGetEnv( ENV_VAR::GetVersionedEnvVarName( wxS( "3RD_PARTY" ) ), &fromEnv ) && !fromEnv.IsEmpty() )
        return fs::path( fromEnv.ToStdString( wxConvUTF8 ) );

    const ENV_VAR_MAP&      vars = Pgm().GetLocalEnvVariables();
    std::optional<wxString> path = ENV_VAR::GetVersionedEnvVarValue( vars, wxT( "3RD_PARTY" ) );

    if( !path || path->IsEmpty() )
        path = PATHS::GetDefault3rdPartyPath();

    return fs::path( path->ToStdString( wxConvUTF8 ) );
}


void RegisterPcmTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY&   tools = aServices.Tools();
    SESSION_MANAGER& sessions = aServices.Sessions();

    tools.Add( { "pcm_catalog",
                 "Packages offered by KiCad's Plugin and Content Manager repositories (the ones KiCad is set up "
                 "with, or the one given): identifier, type, description, newest compatible version, installed "
                 "version. Only library and colortheme packages can be installed by the agent.",
                 "project",
                 JSON{ { "type", "object" },
                       { "properties",
                         { { "repository", { { "type", "string" }, { "description", "repository.json URL." } } },
                           { "query", { { "type", "string" } } },
                           { "type", { { "type", "string" } } },
                           { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } } } },
                       { "additionalProperties", false } },
                 []( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     std::vector<std::pair<std::string, std::string>> repositories;

                     if( aArgs.contains( "repository" ) )
                         repositories.emplace_back( "", aArgs["repository"].get<std::string>() );
                     else
                         repositories = configuredRepositories();

                     const JSON        installed = installedPackages();
                     const std::string query = aArgs.value( "query", "" );
                     const size_t      limit = aArgs.value( "limit", size_t( 50 ) );
                     JSON              out = JSON::array();
                     JSON              errors = JSON::array();

                     for( const auto& [name, url] : repositories )
                     {
                         try
                         {
                             REPOSITORY                           repository = fetchRepository( url, aContext );
                             std::vector<std::pair<double, JSON>> hits;

                             for( const JSON& package : repository.packages )
                             {
                                 if( aArgs.contains( "type" )
                                     && package.value( "type", "" ) != aArgs["type"].get<std::string>() )
                                     continue;

                                 double score = query.empty() ? 1.0
                                                              : SearchScore( query, package.value( "name", "" ),
                                                                             package.value( "identifier", "" ),
                                                                             package.value( "description", "" ), -1 );

                                 if( score <= 0 )
                                     continue;

                                 JSON summary = packageSummary( package, installed );
                                 summary["repository"] = repository.name;
                                 hits.emplace_back( score, summary );
                             }

                             std::stable_sort( hits.begin(), hits.end(),
                                               []( const auto& a, const auto& b )
                                               {
                                                   return a.first > b.first;
                                               } );

                             for( size_t i = 0; i < hits.size() && out.size() < limit; ++i )
                                 out.push_back( hits[i].second );
                         }
                         catch( const std::exception& e )
                         {
                             errors.push_back( { { "repository", url }, { "error", e.what() } } );
                         }
                     }

                     JSON result{ { "packages", out } };

                     if( !errors.empty() )
                         result["errors"] = errors;

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "pcm_install",
              "Install a library or color theme package from a Plugin and Content Manager repository, as KiCad's "
              "own manager does (KiCad sees it as installed). Plugin packages are refused. With addToProject "
              "(default true) its symbol and footprint libraries are added to the current project's tables "
              "with the PCM_ prefix.",
              "project",
              ProjectToolSchema(
                      { { "identifier", { { "type", "string" } } },
                        { "version", { { "type", "string" } } },
                        { "repository", { { "type", "string" }, { "description", "repository.json URL." } } },
                        { "addToProject", { { "type", "boolean" } } } },
                      { "identifier" } ),
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  const std::string                                identifier = aArgs["identifier"].get<std::string>();
                  std::vector<std::pair<std::string, std::string>> repositories;

                  if( aArgs.contains( "repository" ) )
                      repositories.emplace_back( "", aArgs["repository"].get<std::string>() );
                  else
                      repositories = configuredRepositories();

                  REPOSITORY  found;
                  const JSON* package = nullptr;

                  for( const auto& [name, url] : repositories )
                  {
                      found = fetchRepository( url, aContext );

                      for( const JSON& candidate : found.packages )
                      {
                          if( candidate.value( "identifier", "" ) == identifier )
                              package = &candidate;
                      }

                      if( package )
                          break;
                  }

                  if( !package )
                      throw TOOL_ERROR( "No package '" + identifier + "' in the repositories; see pcm_catalog." );

                  const std::string type = package->value( "type", "" );

                  if( !INSTALLABLE_TYPES.count( type ) )
                  {
                      throw TOOL_ERROR(
                              "'" + identifier + "' is a " + type
                                      + " package. The agent installs only "
                                        "library and colortheme packages: a plugin would run code inside KiCad. "
                                        "Ask the person to install it from KiCad's Plugin and Content Manager.",
                              JSON{ { "refused", true }, { "type", type } } );
                  }

                  const JSON* version = chooseVersion( *package, aArgs.value( "version", "" ) );

                  if( !version || !version->contains( "download_url" ) )
                      throw TOOL_ERROR( "No installable version of '" + identifier + "' for this KiCad." );

                  const std::string url = resolveUrl( found.url, ( *version )["download_url"].get<std::string>() );
                  const std::string archive =
                          Fetch( url, MAX_PACKAGE_BYTES,
                                 [&]( double aFraction )
                                 {
                                     aContext.Progress( aFraction, 1.0, "downloading " + identifier );
                                     return !aContext.IsCancelled();
                                 } );

                  if( version->contains( "download_sha256" )
                      && sha256( archive ) != ( *version )["download_sha256"].get<std::string>() )
                  {
                      throw TOOL_ERROR( "The download of '" + identifier
                                        + "' does not match its checksum; nothing "
                                          "was installed." );
                  }

                  const fs::path           root = PcmInstallRoot();
                  std::vector<std::string> files = extract( archive, root, identifier );

                  // Record it as KiCad's PCM does, so the manager shows it installed.
                  JSON installed = installedPackages();
                  JSON kept = JSON::array();

                  for( const JSON& entry : installed["packages"] )
                  {
                      if( !entry.contains( "package" ) || entry["package"].value( "identifier", "" ) != identifier )
                          kept.push_back( entry );
                  }

                  installed["packages"] = kept;
                  installed["packages"].push_back(
                          { { "package", *package },
                            { "current_version", ( *version )["version"] },
                            { "repository_id", found.url },
                            { "repository_name", found.name },
                            { "install_timestamp", static_cast<uint64_t>( std::time( nullptr ) ) },
                            { "pinned", false } } );
                  FILE_STORE::WriteAtomic( installedFile(), installed.dump( 4 ) + "\n" );

                  JSON result{ { "installed", identifier },
                               { "version", ( *version )["version"] },
                               { "folder", root.string() },
                               { "files", files.size() } };

                  if( !aArgs.value( "addToProject", true ) || !sessions.Current() )
                      return TOOL_RESULT::Data( result );

                  // Add its libraries to the project's tables.
                  AGENT_PROJECT& project = sessions.Resolve( aArgs );
                  std::string    folderId = identifier;
                  std::replace( folderId.begin(), folderId.end(), '.', '_' );

                  const wxString variable =
                          wxS( "${" ) + ENV_VAR::GetVersionedEnvVarName( wxS( "3RD_PARTY" ) ) + wxS( "}" );
                  wxString prefix = wxS( "PCM_" );

                  if( KICAD_SETTINGS* settings = Pgm().GetSettingsManager().GetAppSettings<KICAD_SETTINGS>( "kicad" ) )
                      prefix = settings->m_PcmLibPrefix;

                  std::vector<std::tuple<LIBRARY_TABLE_TYPE, wxString, wxString>> libraries;
                  std::set<std::string>                                           seen;

                  for( const std::string& file : files )
                  {
                      fs::path    path( file );
                      std::string top = path.begin()->string();

                      if( top == "symbols" && path.extension() == ".kicad_sym" )
                      {
                          libraries.emplace_back( LIBRARY_TABLE_TYPE::SYMBOL,
                                                  prefix + wxString::FromUTF8( path.stem().string() ),
                                                  variable + wxS( "/" ) + wxString::FromUTF8( file ) );
                      }
                      else if( top == "footprints" && path.parent_path().extension() == ".pretty"
                               && seen.insert( path.parent_path().generic_string() ).second )
                      {
                          libraries.emplace_back( LIBRARY_TABLE_TYPE::FOOTPRINT,
                                                  prefix + wxString::FromUTF8( path.parent_path().stem().string() ),
                                                  variable + wxS( "/" )
                                                          + wxString::FromUTF8( path.parent_path().generic_string() ) );
                      }
                  }

                  JSON added = JSON::array();

                  if( !libraries.empty() )
                  {
                      project.Mutate(
                              "pcm_install " + identifier, false,
                              [&]() -> JSON
                              {
                                  for( const auto& [type, name, uri] : libraries )
                                  {
                                      if( project.Libraries().GetRow( type, name ) )
                                          continue;

                                      std::optional<LIBRARY_TABLE*> table =
                                              project.Libraries().Table( type, LIBRARY_TABLE_SCOPE::PROJECT );

                                      if( !table )
                                          continue;

                                      LIBRARY_TABLE_ROW& row = ( *table )->InsertRow();
                                      row.SetNickname( name );
                                      row.SetURI( uri );
                                      row.SetType( wxS( "KiCad" ) );
                                      row.SetScope( LIBRARY_TABLE_SCOPE::PROJECT );
                                      added.push_back(
                                              { { "type", type == LIBRARY_TABLE_TYPE::SYMBOL ? "symbol" : "footprint" },
                                                { "name", name.ToStdString( wxConvUTF8 ) },
                                                { "uri", uri.ToStdString( wxConvUTF8 ) } } );
                                  }

                                  for( LIBRARY_TABLE_TYPE type :
                                       { LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_TYPE::FOOTPRINT } )
                                  {
                                      const std::string file =
                                              type == LIBRARY_TABLE_TYPE::SYMBOL ? "sym-lib-table" : "fp-lib-table";
                                      std::optional<LIBRARY_TABLE*> table =
                                              project.Libraries().Table( type, LIBRARY_TABLE_SCOPE::PROJECT );

                                      if( !table || ( *table )->Rows().empty() )
                                          continue;

                                      project.BeforeFileWrite( file );
                                      ( *table )->Save();
                                      project.AfterFileWrite( file );
                                  }

                                  project.ReloadLibraryTables();
                                  return JSON::object();
                              } );
                  }

                  result["librariesAdded"] = added;
                  return TOOL_RESULT::Data( result );
              } } );
}

} // namespace AGENTIC
