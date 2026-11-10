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


#include <agentic/core/file_history.h>
#include <agentic/core/file_store.h>
#include <agentic/protocol/registry.h>

#include <algorithm>
#include <set>


namespace AGENTIC
{

namespace fs = std::filesystem;


nlohmann::json FILE_HISTORY::STEP::ToJson() const
{
    nlohmann::json files = nlohmann::json::array();

    for( const CHANGE& change : changes )
        files.push_back( change.path );

    return nlohmann::json{ { "label", label }, { "files", files } };
}


FILE_HISTORY::FILE_HISTORY( fs::path aProjectRoot ) :
        m_root( std::move( aProjectRoot ) ),
        m_dir( m_root / ".agentic" / "history" )
{
    loadIndex();
}


fs::path FILE_HISTORY::absolute( const std::string& aRelative ) const
{
    return m_root / fs::path( aRelative );
}


std::string FILE_HISTORY::currentVersion( const std::string& aRelative ) const
{
    const fs::path path = absolute( aRelative );

    if( !fs::is_regular_file( path ) )
        return std::string();

    return FILE_STORE::VersionOf( FILE_STORE::ReadFile( path ) );
}


std::string FILE_HISTORY::snapshot( const std::string& aRelative )
{
    const fs::path path = absolute( aRelative );

    if( !fs::is_regular_file( path ) )
        return std::string();

    const std::string content = FILE_STORE::ReadFile( path );
    const std::string version = FILE_STORE::VersionOf( content );
    const fs::path    blob = m_dir / version;

    if( !fs::exists( blob ) )
        FILE_STORE::WriteAtomic( blob, content );

    return version;
}


void FILE_HISTORY::restore( const std::string& aRelative, const std::string& aVersion )
{
    const fs::path path = absolute( aRelative );

    if( aVersion.empty() )
    {
        std::error_code ec;
        fs::remove( path, ec );
        return;
    }

    const fs::path blob = m_dir / aVersion;

    if( !fs::exists( blob ) )
        throw TOOL_ERROR( "The saved version " + aVersion + " of '" + aRelative + "' is missing from the history." );

    FILE_STORE::WriteAtomic( path, FILE_STORE::ReadFile( blob ) );
}


void FILE_HISTORY::Begin( const std::string& aLabel )
{
    if( m_depth++ == 0 )
        m_current = STEP{ aLabel, {} };
}


void FILE_HISTORY::Before( const std::string& aRelative )
{
    if( !m_current )
        return;

    for( const CHANGE& change : m_current->changes )
    {
        if( change.path == aRelative )
            return; // the first "before" of the call is the one undo restores
    }

    m_current->changes.push_back( CHANGE{ aRelative, snapshot( aRelative ), std::string() } );
}


void FILE_HISTORY::After( const std::string& aRelative )
{
    if( !m_current )
        return;

    for( CHANGE& change : m_current->changes )
    {
        if( change.path == aRelative )
        {
            change.after = snapshot( aRelative );
            return;
        }
    }
}


void FILE_HISTORY::End()
{
    if( m_depth == 0 || --m_depth > 0 )
        return;

    STEP step = std::move( *m_current );
    m_current.reset();

    std::erase_if( step.changes,
                   []( const CHANGE& aChange )
                   {
                       return aChange.before == aChange.after;
                   } );

    if( step.changes.empty() )
        return;

    m_undo.push_back( std::move( step ) );
    m_redo.clear();

    while( m_undo.size() > MAX_STEPS )
        m_undo.pop_front();

    collectGarbage();
    saveIndex();
}


void FILE_HISTORY::Abandon()
{
    if( m_depth == 0 || --m_depth > 0 )
        return;

    m_current.reset();
}


void FILE_HISTORY::apply( const STEP& aStep, bool aToAfter, bool aForce )
{
    if( !aForce )
    {
        for( const CHANGE& change : aStep.changes )
        {
            const std::string expected = aToAfter ? change.before : change.after;
            const std::string current = currentVersion( change.path );

            if( current != expected )
            {
                throw TOOL_ERROR(
                        "'" + change.path + "' was changed outside the agent since '" + aStep.label
                                + "'. Nothing was restored; pass force:true to overwrite it anyway.",
                        nlohmann::json{ { "conflict", true }, { "path", change.path }, { "version", current } } );
            }
        }
    }

    for( const CHANGE& change : aStep.changes )
        restore( change.path, aToAfter ? change.after : change.before );
}


FILE_HISTORY::STEP FILE_HISTORY::Undo( bool aForce )
{
    if( m_undo.empty() )
        throw TOOL_ERROR( "Nothing to undo in this project." );

    STEP step = m_undo.back();
    apply( step, false, aForce );
    m_undo.pop_back();
    m_redo.push_back( step );
    saveIndex();
    return step;
}


FILE_HISTORY::STEP FILE_HISTORY::Redo( bool aForce )
{
    if( m_redo.empty() )
        throw TOOL_ERROR( "Nothing to redo in this project." );

    STEP step = m_redo.back();
    apply( step, true, aForce );
    m_redo.pop_back();
    m_undo.push_back( step );
    saveIndex();
    return step;
}


void FILE_HISTORY::collectGarbage()
{
    std::set<std::string> used;

    for( const auto* stack : { &m_undo, &m_redo } )
    {
        for( const STEP& step : *stack )
        {
            for( const CHANGE& change : step.changes )
            {
                used.insert( change.before );
                used.insert( change.after );
            }
        }
    }

    std::error_code ec;

    for( const fs::directory_entry& entry : fs::directory_iterator( m_dir, ec ) )
    {
        const std::string name = entry.path().filename().string();

        if( entry.is_regular_file() && name != "index.json" && !used.contains( name ) )
            fs::remove( entry.path(), ec );
    }
}


void FILE_HISTORY::saveIndex() const
{
    auto stackJson = []( const std::deque<STEP>& aStack )
    {
        nlohmann::json out = nlohmann::json::array();

        for( const STEP& step : aStack )
        {
            nlohmann::json changes = nlohmann::json::array();

            for( const CHANGE& change : step.changes )
                changes.push_back(
                        { { "path", change.path }, { "before", change.before }, { "after", change.after } } );

            out.push_back( { { "label", step.label }, { "changes", changes } } );
        }

        return out;
    };

    nlohmann::json index{ { "version", 1 }, { "undo", stackJson( m_undo ) }, { "redo", stackJson( m_redo ) } };
    FILE_STORE::WriteAtomic( m_dir / "index.json", index.dump( 1 ) );
}


void FILE_HISTORY::loadIndex()
{
    const fs::path indexFile = m_dir / "index.json";

    if( !fs::exists( indexFile ) )
        return;

    try
    {
        nlohmann::json index = nlohmann::json::parse( FILE_STORE::ReadFile( indexFile ) );

        auto readStack = []( const nlohmann::json& aJson, std::deque<STEP>& aStack )
        {
            for( const nlohmann::json& stepJson : aJson )
            {
                STEP step{ stepJson.at( "label" ).get<std::string>(), {} };

                for( const nlohmann::json& change : stepJson.at( "changes" ) )
                {
                    step.changes.push_back( CHANGE{ change.at( "path" ).get<std::string>(),
                                                    change.at( "before" ).get<std::string>(),
                                                    change.at( "after" ).get<std::string>() } );
                }

                aStack.push_back( std::move( step ) );
            }
        };

        readStack( index.at( "undo" ), m_undo );
        readStack( index.at( "redo" ), m_redo );
    }
    catch( const std::exception& )
    {
        // A damaged index loses the undo history, never the project.
        m_undo.clear();
        m_redo.clear();
    }
}

} // namespace AGENTIC
