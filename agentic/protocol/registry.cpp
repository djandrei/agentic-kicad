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

#include <agentic/protocol/registry.h>
#include <agentic/protocol/json_rpc.h>


namespace AGENTIC
{

JSON TextContent( const std::string& aText )
{
    return JSON{ { "type", "text" }, { "text", aText } };
}


JSON ImageContent( const std::string& aBase64Data, const std::string& aMimeType )
{
    return JSON{ { "type", "image" }, { "data", aBase64Data }, { "mimeType", aMimeType } };
}


TOOL_RESULT TOOL_RESULT::Text( const std::string& aText )
{
    TOOL_RESULT r;
    r.content.push_back( TextContent( aText ) );
    return r;
}


TOOL_RESULT TOOL_RESULT::Data( const JSON& aStructured )
{
    TOOL_RESULT r;
    r.content.push_back( TextContent( aStructured.dump( 2, ' ', false, JSON::error_handler_t::replace ) ) );
    r.structuredContent = aStructured;
    return r;
}


TOOL_RESULT TOOL_RESULT::Error( const std::string& aMessage, JSON aData )
{
    TOOL_RESULT r;
    r.content.push_back( TextContent( aMessage ) );
    r.isError = true;

    if( !aData.is_null() )
        r.structuredContent = std::move( aData );

    return r;
}


JSON TOOL_RESULT::ToJson() const
{
    JSON j{ { "content", content }, { "isError", isError } };

    if( !structuredContent.is_null() )
        j["structuredContent"] = structuredContent;

    return j;
}


void REQUEST_STATE::SetTimeout( double aSeconds )
{
    if( aSeconds <= 0 )
    {
        m_deadline.reset();
        return;
    }

    auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>( aSeconds ) );
    m_deadline = std::chrono::steady_clock::now() + duration;
}


bool REQUEST_STATE::TimedOut() const
{
    return m_deadline && std::chrono::steady_clock::now() >= *m_deadline;
}


void REQUEST_STATE::Progress( double aProgress, std::optional<double> aTotal, const std::string& aMessage ) const
{
    if( m_progressToken.is_null() || !m_sender )
        return;

    JSON params{ { "progressToken", m_progressToken }, { "progress", aProgress } };

    if( aTotal )
        params["total"] = *aTotal;

    if( !aMessage.empty() )
        params["message"] = aMessage;

    m_sender( JSON_RPC::MakeNotification( "notifications/progress", params ) );
}


void TOOL_REGISTRY::Add( TOOL_DEF aDef )
{
    for( TOOL_DEF& existing : m_tools )
    {
        if( existing.name == aDef.name )
        {
            existing = std::move( aDef );
            return;
        }
    }

    m_tools.push_back( std::move( aDef ) );
}


const TOOL_DEF* TOOL_REGISTRY::Find( const std::string& aName ) const
{
    for( const TOOL_DEF& tool : m_tools )
    {
        if( tool.name == aName )
            return &tool;
    }

    return nullptr;
}


JSON TOOL_REGISTRY::ListJson( const std::set<std::string>& aDisabledGroups ) const
{
    JSON list = JSON::array();

    for( const TOOL_DEF& tool : m_tools )
    {
        if( aDisabledGroups.count( tool.group ) )
            continue;

        list.push_back( JSON{
                { "name", tool.name }, { "description", tool.description }, { "inputSchema", tool.inputSchema } } );
    }

    return list;
}


std::set<std::string> TOOL_REGISTRY::Groups() const
{
    std::set<std::string> groups;

    for( const TOOL_DEF& tool : m_tools )
        groups.insert( tool.group );

    return groups;
}


JSON RESOURCE_CONTENT::ToJson() const
{
    JSON j{ { "uri", uri }, { "mimeType", mimeType } };

    if( !blobBase64.empty() )
        j["blob"] = blobBase64;
    else
        j["text"] = text;

    return j;
}


void RESOURCE_REGISTRY::Add( RESOURCE_DEF aDef )
{
    m_resources.push_back( std::move( aDef ) );
}


void RESOURCE_REGISTRY::AddTemplate( RESOURCE_TEMPLATE_DEF aDef )
{
    m_templates.push_back( std::move( aDef ) );
}


void RESOURCE_REGISTRY::AddResolver( RESOLVER aResolver )
{
    m_resolvers.push_back( std::move( aResolver ) );
}


JSON RESOURCE_REGISTRY::ListJson() const
{
    JSON list = JSON::array();

    for( const RESOURCE_DEF& res : m_resources )
    {
        list.push_back( JSON{ { "uri", res.uri },
                              { "name", res.name },
                              { "description", res.description },
                              { "mimeType", res.mimeType } } );
    }

    return list;
}


JSON RESOURCE_REGISTRY::ListTemplatesJson() const
{
    JSON list = JSON::array();

    for( const RESOURCE_TEMPLATE_DEF& tpl : m_templates )
    {
        list.push_back( JSON{ { "uriTemplate", tpl.uriTemplate },
                              { "name", tpl.name },
                              { "description", tpl.description },
                              { "mimeType", tpl.mimeType } } );
    }

    return list;
}


std::optional<RESOURCE_CONTENT> RESOURCE_REGISTRY::Read( const std::string& aUri ) const
{
    for( const RESOURCE_DEF& res : m_resources )
    {
        if( res.uri == aUri )
            return res.read();
    }

    for( const RESOLVER& resolver : m_resolvers )
    {
        if( std::optional<RESOURCE_CONTENT> content = resolver( aUri ) )
            return content;
    }

    return std::nullopt;
}


void PROMPT_REGISTRY::Add( PROMPT_DEF aDef )
{
    m_prompts.push_back( std::move( aDef ) );
}


const PROMPT_DEF* PROMPT_REGISTRY::Find( const std::string& aName ) const
{
    for( const PROMPT_DEF& prompt : m_prompts )
    {
        if( prompt.name == aName )
            return &prompt;
    }

    return nullptr;
}


JSON PROMPT_REGISTRY::ListJson() const
{
    JSON list = JSON::array();

    for( const PROMPT_DEF& prompt : m_prompts )
    {
        list.push_back( JSON{
                { "name", prompt.name }, { "description", prompt.description }, { "arguments", prompt.arguments } } );
    }

    return list;
}

} // namespace AGENTIC
