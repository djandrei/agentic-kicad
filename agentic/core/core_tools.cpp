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
#include <agentic/core/handles.h>
#include <agentic/core/services.h>

#include <base_units.h>
#include <netclass.h>
#include <project.h>
#include <project/net_settings.h>
#include <project/project_file.h>

#include <cmath>
#include <optional>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    const JSON PROJECT_ARG = JSON{ { "type", "string" },
                                   { "description", "Open project to act on (name or folder); default: the "
                                                    "current project, the one last used." } };

    const size_t READ_LIMIT = 256 * 1024;


    bool isText( const std::string& aContent )
    {
        size_t checked = std::min<size_t>( aContent.size(), 8192 );

        for( size_t i = 0; i < checked; ++i )
        {
            if( aContent[i] == '\0' )
                return false;
        }

        return true;
    }


    std::string base64( const std::string& aData )
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string        out;
        size_t             i = 0;

        for( ; i + 2 < aData.size(); i += 3 )
        {
            unsigned v = ( (unsigned char) aData[i] << 16 ) | ( (unsigned char) aData[i + 1] << 8 )
                         | (unsigned char) aData[i + 2];
            out += table[( v >> 18 ) & 63];
            out += table[( v >> 12 ) & 63];
            out += table[( v >> 6 ) & 63];
            out += table[v & 63];
        }

        if( i + 1 == aData.size() )
        {
            unsigned v = (unsigned char) aData[i] << 16;
            out += table[( v >> 18 ) & 63];
            out += table[( v >> 12 ) & 63];
            out += "==";
        }
        else if( i + 2 == aData.size() )
        {
            unsigned v = ( (unsigned char) aData[i] << 16 ) | ( (unsigned char) aData[i + 1] << 8 );
            out += table[( v >> 18 ) & 63];
            out += table[( v >> 12 ) & 63];
            out += table[( v >> 6 ) & 63];
            out += '=';
        }

        return out;
    }


    // Net class fields: name in JSON, getter, setter, scale (PCB values in pcbIU, schematic
    // wire and bus widths in schIU).
    struct NETCLASS_FIELD
    {
        const char* name;
        std::optional<int> ( NETCLASS::*get )() const;
        void ( NETCLASS::*set )( std::optional<int> );
        double iuPerMm;
    };

    const NETCLASS_FIELD NETCLASS_FIELDS[] = {
        { "clearance", &NETCLASS::GetClearanceOpt, &NETCLASS::SetClearance, pcbIUScale.IU_PER_MM },
        { "trackWidth", &NETCLASS::GetTrackWidthOpt, &NETCLASS::SetTrackWidth, pcbIUScale.IU_PER_MM },
        { "viaDiameter", &NETCLASS::GetViaDiameterOpt, &NETCLASS::SetViaDiameter, pcbIUScale.IU_PER_MM },
        { "viaDrill", &NETCLASS::GetViaDrillOpt, &NETCLASS::SetViaDrill, pcbIUScale.IU_PER_MM },
        { "microViaDiameter", &NETCLASS::GetuViaDiameterOpt, &NETCLASS::SetuViaDiameter, pcbIUScale.IU_PER_MM },
        { "microViaDrill", &NETCLASS::GetuViaDrillOpt, &NETCLASS::SetuViaDrill, pcbIUScale.IU_PER_MM },
        { "diffPairWidth", &NETCLASS::GetDiffPairWidthOpt, &NETCLASS::SetDiffPairWidth, pcbIUScale.IU_PER_MM },
        { "diffPairGap", &NETCLASS::GetDiffPairGapOpt, &NETCLASS::SetDiffPairGap, pcbIUScale.IU_PER_MM },
        { "diffPairViaGap", &NETCLASS::GetDiffPairViaGapOpt, &NETCLASS::SetDiffPairViaGap, pcbIUScale.IU_PER_MM },
        { "wireWidth", &NETCLASS::GetWireWidthOpt, &NETCLASS::SetWireWidth, schIUScale.IU_PER_MM },
        { "busWidth", &NETCLASS::GetBusWidthOpt, &NETCLASS::SetBusWidth, schIUScale.IU_PER_MM },
    };


    JSON netclassJson( const NETCLASS& aClass )
    {
        JSON out{ { "name", aClass.GetName().ToStdString( wxConvUTF8 ) } };

        if( !aClass.GetDescription().IsEmpty() )
            out["description"] = aClass.GetDescription().ToStdString( wxConvUTF8 );

        for( const NETCLASS_FIELD& field : NETCLASS_FIELDS )
        {
            if( std::optional<int> value = ( aClass.*field.get )() )
                out[field.name] = std::round( *value / field.iuPerMm * 1e6 ) / 1e6;
        }

        return out;
    }


    void applyNetclass( NETCLASS& aClass, const JSON& aSpec )
    {
        if( aSpec.contains( "description" ) )
            aClass.SetDescription( wxString::FromUTF8( aSpec["description"].get<std::string>() ) );

        for( const NETCLASS_FIELD& field : NETCLASS_FIELDS )
        {
            if( !aSpec.contains( field.name ) )
                continue;

            if( aSpec[field.name].is_null() )
            {
                ( aClass.*field.set )( std::nullopt );
                continue;
            }

            const double mm = ParseLengthMm( aSpec[field.name], field.name );

            if( mm < 0 || mm > 100 )
                throw TOOL_ERROR( std::string( "'" ) + field.name + "' must be between 0 and 100 mm." );

            ( aClass.*field.set )( static_cast<int>( std::lround( mm * field.iuPerMm ) ) );
        }
    }


    JSON netSettingsJson( NET_SETTINGS& aSettings )
    {
        JSON classes = JSON::array();
        classes.push_back( netclassJson( *aSettings.GetDefaultNetclass() ) );

        for( const auto& [name, netclass] : aSettings.GetNetclasses() )
            classes.push_back( netclassJson( *netclass ) );

        JSON patterns = JSON::array();

        for( const auto& [matcher, netclass] : aSettings.GetNetclassPatternAssignments() )
        {
            patterns.push_back( { { "pattern", matcher->GetPattern().ToStdString( wxConvUTF8 ) },
                                  { "netclass", netclass.ToStdString( wxConvUTF8 ) } } );
        }

        return JSON{ { "netclasses", classes }, { "patterns", patterns } };
    }

} // namespace


JSON ProjectToolSchema( JSON aProperties, JSON aRequired )
{
    aProperties["project"] = PROJECT_ARG;

    JSON schema{ { "type", "object" }, { "properties", aProperties }, { "additionalProperties", false } };

    if( !aRequired.empty() )
        schema["required"] = aRequired;

    return schema;
}


void RegisterProjectTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY&   tools = aServices.Tools();
    SESSION_MANAGER& sessions = aServices.Sessions();
    const JSON       forceArg{ { "type", "boolean" },
                               { "description", "Take over a lock or overwrite a change made outside the agent." } };

    // Projects -------------------------------------------------------------------------------

    tools.Add(
            { "project_open",
              "Open a KiCad project: a folder holding a .kicad_pro, or the .kicad_pro itself (absolute path). It "
              "becomes the current project. Opening an open project makes it current.",
              "project",
              JSON{ { "type", "object" },
                    { "properties",
                      { { "path",
                          { { "type", "string" }, { "description", "Absolute path of the folder or .kicad_pro." } } },
                        { "force", forceArg } } },
                    { "required", { "path" } },
                    { "additionalProperties", false } },
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project =
                          sessions.Open( aArgs["path"].get<std::string>(), aArgs.value( "force", false ) );
                  return TOOL_RESULT::Data( project.Info() );
              } } );

    tools.Add(
            { "project_create",
              "Create a new KiCad project: makes the folder (absolute path) if needed and writes <name>.kicad_pro. "
              "Fails if the folder already holds a project. The new project becomes current.",
              "project",
              JSON{ { "type", "object" },
                    { "properties",
                      { { "path", { { "type", "string" }, { "description", "Absolute path of the project folder." } } },
                        { "name",
                          { { "type", "string" }, { "description", "Project name; default: the folder name." } } } } },
                    { "required", { "path" } },
                    { "additionalProperties", false } },
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project =
                          sessions.Create( aArgs["path"].get<std::string>(), aArgs.value( "name", "" ) );
                  return TOOL_RESULT::Data( project.Info() );
              } } );

    tools.Add( { "project_list", "The open projects, and which one is current.", "project",
                 JSON{ { "type", "object" }, { "properties", JSON::object() }, { "additionalProperties", false } },
                 [&sessions]( const JSON&, TOOL_CONTEXT& )
                 {
                     JSON list = JSON::array();

                     for( AGENT_PROJECT* project : sessions.Projects() )
                     {
                         list.push_back( { { "name", project->Name() },
                                           { "root", project->Root().string() },
                                           { "current", project == sessions.Current() } } );
                     }

                     return TOOL_RESULT::Data( JSON{ { "projects", list } } );
                 } } );

    tools.Add( { "project_info", "A project's folder, files, open documents with their versions, and undo state.",
                 "project", ProjectToolSchema( JSON::object() ), [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( sessions.Resolve( aArgs ).Info() );
                 } } );

    tools.Add( { "project_set",
                 "Make a project current, and change the agent settings kept in its .agentic/agentic.json: "
                 "disabledGroups hides whole tool groups (e.g. [\"sim\"]) for this project.",
                 "project",
                 ProjectToolSchema( { { "disabledGroups",
                                        { { "type", "array" },
                                          { "items", { { "type", "string" } } },
                                          { "description", "Tool groups to hide; [] shows every group." } } } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );

                     if( aArgs.contains( "disabledGroups" ) )
                     {
                         JSON settings = project.Settings();
                         settings["disabledGroups"] = aArgs["disabledGroups"];
                         project.SetSettings( settings );
                     }

                     return TOOL_RESULT::Data( project.Info() );
                 } } );

    tools.Add( { "project_close",
                 "Close a project: its documents are already saved (every call saves), so this only releases the "
                 "locks and frees memory.",
                 "project", ProjectToolSchema( JSON::object() ), [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string name = project.Name();
                     sessions.Close( project );

                     JSON out{ { "closed", name } };

                     if( sessions.Current() )
                         out["current"] = sessions.Current()->Name();

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "log_get",
                 "Messages KiCad reported in this project's calls (errors, warnings, info), oldest first. Every call "
                 "already returns its own messages; use this to page the history.",
                 "project",
                 ProjectToolSchema( { { "since",
                                        { { "type", "integer" },
                                          { "minimum", 0 },
                                          { "description", "Return records after this sequence number." } } },
                                      { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 1000 } } } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     JSON           records = JSON::array();

                     for( const LOG_RECORD& record : project.Log().Since( aArgs.value( "since", uint64_t( 0 ) ),
                                                                          aArgs.value( "limit", size_t( 200 ) ) ) )
                     {
                         records.push_back( record.ToJson() );
                     }

                     return TOOL_RESULT::Data( JSON{ { "records", records }, { "lastSeq", project.Log().LastSeq() } } );
                 } } );

    tools.Add( { "undo",
                 "Undo the last change made by the agent in this project: every file the call wrote goes back to its "
                 "previous saved version, and the documents are reloaded.",
                 "project", ProjectToolSchema( { { "force", forceArg } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( sessions.Resolve( aArgs ).Undo( aArgs.value( "force", false ) ) );
                 } } );

    tools.Add( { "redo", "Redo the last undone change. Any new change clears what can be redone.", "project",
                 ProjectToolSchema( { { "force", forceArg } } ), [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( sessions.Resolve( aArgs ).Redo( aArgs.value( "force", false ) ) );
                 } } );

    // Files ----------------------------------------------------------------------------------

    tools.Add( { "file_list",
                 "Files and folders of the project (paths relative to the project folder). Design files are marked; "
                 "they can be read but only changed through their program's tools.",
                 "project",
                 ProjectToolSchema(
                         { { "dir", { { "type", "string" }, { "description", "Folder, default the project root." } } },
                           { "recursive", { { "type", "boolean" } } } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     JSON           entries = JSON::array();

                     for( const FILE_STORE::ENTRY& entry :
                          project.Files().List( aArgs.value( "dir", "" ), aArgs.value( "recursive", false ) ) )
                     {
                         JSON item{ { "path", entry.path }, { "type", entry.directory ? "dir" : "file" } };

                         if( !entry.directory )
                         {
                             item["size"] = entry.size;
                             item["design"] = entry.design;
                         }

                         entries.push_back( item );
                     }

                     return TOOL_RESULT::Data( JSON{ { "entries", entries } } );
                 } } );

    tools.Add( { "file_read",
                 "Read a project file. Text comes back as text, binary as base64. Returns the content version to pass "
                 "as baseVersion to file_write. Large files are returned in parts (offset, limit in bytes).",
                 "project",
                 ProjectToolSchema( { { "path", { { "type", "string" } } },
                                      { "offset", { { "type", "integer" }, { "minimum", 0 } } },
                                      { "limit", { { "type", "integer" }, { "minimum", 1 } } } },
                                    { "path" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string path = aArgs["path"].get<std::string>();
                     const std::string content = project.Files().Read( path );
                     const size_t      offset = std::min( aArgs.value( "offset", size_t( 0 ) ), content.size() );
                     const size_t      limit = aArgs.value( "limit", READ_LIMIT );
                     const std::string part = content.substr( offset, limit );

                     JSON out{ { "path", project.Files().Relative( project.Files().Resolve( path ) ) },
                               { "version", FILE_STORE::VersionOf( content ) },
                               { "size", content.size() },
                               { "design", FILE_STORE::IsDesignFile( path ) } };

                     if( offset + part.size() < content.size() )
                         out["next"] = offset + part.size();

                     if( isText( content ) )
                         out["text"] = part;
                     else
                         out["base64"] = base64( part );

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "file_write",
                 "Write a project file that is not a KiCad design file (notes, scripts, BOM templates, README). Give "
                 "the whole content, or exact edits [{old, new}] applied in order to the current content. With "
                 "baseVersion the write fails if the file changed since that version (use \"\" for a new file). "
                 "Undo restores the previous content.",
                 "project",
                 ProjectToolSchema(
                         { { "path", { { "type", "string" } } },
                           { "content", { { "type", "string" } } },
                           { "edits",
                             { { "type", "array" },
                               { "items",
                                 { { "type", "object" },
                                   { "properties",
                                     { { "old", { { "type", "string" } } }, { "new", { { "type", "string" } } } } },
                                   { "required", { "old", "new" } } } } } },
                           { "baseVersion", { { "type", "string" } } } },
                         { "path" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string path = aArgs["path"].get<std::string>();

                     if( aArgs.contains( "content" ) == aArgs.contains( "edits" ) )
                         throw TOOL_ERROR( "Give either content or edits." );

                     std::optional<std::string> base;

                     if( aArgs.contains( "baseVersion" ) )
                         base = aArgs["baseVersion"].get<std::string>();

                     JSON result = project.Mutate(
                             "file_write " + path, false,
                             [&]() -> JSON
                             {
                                 std::string content;

                                 if( aArgs.contains( "content" ) )
                                 {
                                     content = aArgs["content"].get<std::string>();
                                 }
                                 else
                                 {
                                     content = project.Files().Read( path );

                                     for( const JSON& edit : aArgs["edits"] )
                                     {
                                         const std::string oldText = edit["old"].get<std::string>();
                                         const size_t      at = content.find( oldText );

                                         if( oldText.empty() || at == std::string::npos )
                                             throw TOOL_ERROR( "Edit text not found in " + path + ": " + oldText );

                                         if( content.find( oldText, at + 1 ) != std::string::npos )
                                             throw TOOL_ERROR( "Edit text occurs more than once in " + path
                                                               + "; include more context: " + oldText );

                                         content.replace( at, oldText.size(), edit["new"].get<std::string>() );
                                     }
                                 }

                                 const std::string rel = project.Files().Relative( project.Files().Resolve( path ) );
                                 project.BeforeFileWrite( rel );
                                 const std::string version = project.Files().Write( path, content, base );
                                 project.AfterFileWrite( rel );
                                 return JSON{ { "path", rel }, { "version", version }, { "size", content.size() } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "file_rename", "Rename or move a project file that is not a design file.", "project",
                 ProjectToolSchema( { { "from", { { "type", "string" } } },
                                      { "to", { { "type", "string" } } },
                                      { "overwrite", { { "type", "boolean" } } } },
                                    { "from", "to" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string from = aArgs["from"].get<std::string>();
                     const std::string to = aArgs["to"].get<std::string>();

                     JSON result =
                             project.Mutate( "file_rename " + from, false,
                                             [&]() -> JSON
                                             {
                                                 const std::string relFrom =
                                                         project.Files().Relative( project.Files().Resolve( from ) );
                                                 const std::string relTo =
                                                         project.Files().Relative( project.Files().Resolve( to ) );
                                                 project.BeforeFileWrite( relFrom );
                                                 project.BeforeFileWrite( relTo );
                                                 project.Files().Rename( from, to, aArgs.value( "overwrite", false ) );
                                                 project.AfterFileWrite( relFrom );
                                                 project.AfterFileWrite( relTo );
                                                 return JSON{ { "from", relFrom }, { "to", relTo } };
                                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "file_delete",
                 "Delete a project file (or an empty folder) that is not a design file. Undo restores it.", "project",
                 ProjectToolSchema( { { "path", { { "type", "string" } } } }, { "path" } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = sessions.Resolve( aArgs );
                     const std::string path = aArgs["path"].get<std::string>();

                     JSON result = project.Mutate(
                             "file_delete " + path, false,
                             [&]() -> JSON
                             {
                                 const std::string rel = project.Files().Relative( project.Files().Resolve( path ) );
                                 const bool        isFile = fs::is_regular_file( project.Files().Resolve( path ) );

                                 if( isFile )
                                     project.BeforeFileWrite( rel );

                                 project.Files().Remove( path );

                                 if( isFile )
                                     project.AfterFileWrite( rel );

                                 return JSON{ { "deleted", rel } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    // Project settings -----------------------------------------------------------------------

    tools.Add( { "text_vars_get",
                 "The project's text variables (used as ${NAME} in schematics, boards and drawing sheets).", "project",
                 ProjectToolSchema( JSON::object() ), [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     JSON           vars = JSON::object();

                     for( const auto& [name, value] : project.KicadProject().GetTextVars() )
                         vars[name.ToStdString( wxConvUTF8 )] = value.ToStdString( wxConvUTF8 );

                     return TOOL_RESULT::Data( JSON{ { "vars", vars } } );
                 } } );

    tools.Add(
            { "text_vars_set",
              "Set or remove project text variables: {NAME: value} sets, {NAME: null} removes. Other variables "
              "are kept.",
              "project",
              ProjectToolSchema(
                      { { "vars",
                          { { "type", "object" }, { "additionalProperties", { { "type", { "string", "null" } } } } } },
                        { "force", forceArg } },
                      { "vars" } ),
              [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project = sessions.Resolve( aArgs );

                  JSON result = project.Mutate(
                          "text_vars_set", aArgs.value( "force", false ),
                          [&]() -> JSON
                          {
                              std::map<wxString, wxString>& vars = project.KicadProject().GetTextVars();

                              for( const auto& [name, value] : aArgs["vars"].items() )
                              {
                                  if( name.empty() || name.find_first_of( "${} " ) != std::string::npos )
                                      throw TOOL_ERROR( "'" + name + "' is not a valid text variable name." );

                                  if( value.is_null() )
                                      vars.erase( wxString::FromUTF8( name ) );
                                  else
                                      vars[wxString::FromUTF8( name )] = wxString::FromUTF8( value.get<std::string>() );
                              }

                              project.TouchProjectFile();

                              JSON out = JSON::object();

                              for( const auto& [name, value] : vars )
                                  out[name.ToStdString( wxConvUTF8 )] = value.ToStdString( wxConvUTF8 );

                              return JSON{ { "vars", out } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "netclasses_get",
                 "The project's net classes (sizes in mm; a missing field inherits from Default) and the net name "
                 "patterns that assign them.",
                 "project", ProjectToolSchema( JSON::object() ), [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );
                     return TOOL_RESULT::Data(
                             netSettingsJson( *project.KicadProject().GetProjectFile().NetSettings() ) );
                 } } );

    JSON netclassSpec{ { "type", "object" },
                       { "properties",
                         { { "name", { { "type", "string" } } },
                           { "description", { { "type", "string" } } },
                           { "remove", { { "type", "boolean" }, { "description", "Delete this net class." } } } } },
                       { "required", { "name" } } };

    for( const NETCLASS_FIELD& field : NETCLASS_FIELDS )
        netclassSpec["properties"][field.name] = { { "type", { "number", "string", "null" } } };

    tools.Add( { "netclasses_set",
                 "Create, change or delete net classes, and replace the net name patterns. Each entry names a class; "
                 "given fields are set (lengths in mm or with units, null to inherit), others kept. \"Default\" can "
                 "be changed but not removed. patterns, when given, replaces all [{pattern, netclass}] rules.",
                 "project",
                 ProjectToolSchema( { { "netclasses", { { "type", "array" }, { "items", netclassSpec } } },
                                      { "patterns",
                                        { { "type", "array" },
                                          { "items",
                                            { { "type", "object" },
                                              { "properties",
                                                { { "pattern", { { "type", "string" } } },
                                                  { "netclass", { { "type", "string" } } } } },
                                              { "required", { "pattern", "netclass" } } } } } },
                                      { "force", forceArg } } ),
                 [&sessions]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = sessions.Resolve( aArgs );

                     JSON result = project.Mutate(
                             "netclasses_set", aArgs.value( "force", false ),
                             [&]() -> JSON
                             {
                                 NET_SETTINGS& settings = *project.KicadProject().GetProjectFile().NetSettings();

                                 for( const JSON& spec : aArgs.value( "netclasses", JSON::array() ) )
                                 {
                                     const wxString name = wxString::FromUTF8( spec["name"].get<std::string>() );

                                     if( name == NETCLASS::Default )
                                     {
                                         if( spec.value( "remove", false ) )
                                             throw TOOL_ERROR( "The Default net class cannot be removed." );

                                         applyNetclass( *settings.GetDefaultNetclass(), spec );
                                         continue;
                                     }

                                     std::map<wxString, std::shared_ptr<NETCLASS>> classes = settings.GetNetclasses();

                                     if( spec.value( "remove", false ) )
                                     {
                                         if( !classes.erase( name ) )
                                             throw TOOL_ERROR( "No net class '" + spec["name"].get<std::string>()
                                                               + "'." );

                                         settings.SetNetclasses( classes );
                                         continue;
                                     }

                                     std::shared_ptr<NETCLASS> netclass;

                                     if( classes.count( name ) )
                                         netclass = classes[name];
                                     else
                                         netclass = std::make_shared<NETCLASS>( name, false );

                                     applyNetclass( *netclass, spec );
                                     settings.SetNetclass( name, netclass );
                                 }

                                 if( aArgs.contains( "patterns" ) )
                                 {
                                     settings.ClearNetclassPatternAssignments();

                                     for( const JSON& rule : aArgs["patterns"] )
                                     {
                                         const wxString netclass =
                                                 wxString::FromUTF8( rule["netclass"].get<std::string>() );

                                         if( netclass != NETCLASS::Default && !settings.HasNetclass( netclass ) )
                                             throw TOOL_ERROR( "Pattern names unknown net class '"
                                                               + rule["netclass"].get<std::string>() + "'." );

                                         settings.SetNetclassPatternAssignment(
                                                 wxString::FromUTF8( rule["pattern"].get<std::string>() ), netclass );
                                     }
                                 }

                                 settings.ClearAllCaches();
                                 project.TouchProjectFile();
                                 return netSettingsJson( settings );
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
