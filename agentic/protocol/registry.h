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

#ifndef AGENTIC_REGISTRY_H
#define AGENTIC_REGISTRY_H

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/**
 * Registries of the three MCP primitives: tools, resources and prompts.  No wx dependency.
 */
namespace AGENTIC
{

using JSON = nlohmann::json;


/**
 * Thrown by tool handlers for a failure the agent should see.  The server turns it into a
 * tool result with isError = true, not a JSON-RPC error.  \a aData is returned as
 * structuredContent so the agent can act on it (for example the current version of a file
 * after a conflict).
 */
class TOOL_ERROR : public std::runtime_error
{
public:
    explicit TOOL_ERROR( const std::string& aMessage, JSON aData = nullptr ) :
            std::runtime_error( aMessage ),
            m_data( std::move( aData ) )
    {
    }

    const JSON& Data() const { return m_data; }

private:
    JSON m_data;
};


JSON TextContent( const std::string& aText );
JSON ImageContent( const std::string& aBase64Data, const std::string& aMimeType );


struct TOOL_RESULT
{
    JSON content = JSON::array();
    JSON structuredContent = nullptr;
    bool isError = false;

    static TOOL_RESULT Text( const std::string& aText );

    /// Structured result: the text content carries the pretty-printed JSON for clients that
    /// read only text, structuredContent carries it verbatim.
    static TOOL_RESULT Data( const JSON& aStructured );

    static TOOL_RESULT Error( const std::string& aMessage, JSON aData = nullptr );

    JSON ToJson() const;
};


/**
 * State of one request shared between the transport threads (cancellation) and the thread
 * that runs the tool (progress, deadline).
 */
class REQUEST_STATE
{
public:
    using SENDER = std::function<void( const JSON& )>;

    REQUEST_STATE( JSON aId, JSON aProgressToken, SENDER aSender ) :
            m_id( std::move( aId ) ),
            m_progressToken( std::move( aProgressToken ) ),
            m_sender( std::move( aSender ) )
    {
    }

    const JSON& Id() const { return m_id; }

    void Cancel() { m_cancelled = true; }
    bool WasCancelledByClient() const { return m_cancelled; }

    /// Arm a deadline counted from now.  Non-positive values clear it.
    void SetTimeout( double aSeconds );
    bool TimedOut() const;

    /// Send notifications/progress if the client asked for progress (a progressToken).
    void Progress( double aProgress, std::optional<double> aTotal, const std::string& aMessage ) const;

    bool WantsProgress() const { return !m_progressToken.is_null(); }

private:
    JSON              m_id;
    JSON              m_progressToken;
    SENDER            m_sender;
    std::atomic<bool> m_cancelled{ false };

    std::optional<std::chrono::steady_clock::time_point> m_deadline;
};


/**
 * What a tool handler sees of the request it serves.
 */
class TOOL_CONTEXT
{
public:
    explicit TOOL_CONTEXT( std::shared_ptr<REQUEST_STATE> aState ) :
            m_state( std::move( aState ) )
    {
    }

    /// True when the client cancelled the request or the timeout set by the tool passed.
    bool IsCancelled() const { return m_state && ( m_state->WasCancelledByClient() || m_state->TimedOut() ); }

    bool TimedOut() const { return m_state && m_state->TimedOut(); }

    void SetTimeout( double aSeconds )
    {
        if( m_state )
            m_state->SetTimeout( aSeconds );
    }

    void Progress( double aProgress, std::optional<double> aTotal = std::nullopt,
                   const std::string& aMessage = "" ) const
    {
        if( m_state )
            m_state->Progress( aProgress, aTotal, aMessage );
    }

private:
    std::shared_ptr<REQUEST_STATE> m_state;
};


struct TOOL_DEF
{
    std::string name;
    std::string description;

    /// Tool group (a provider name such as "pcb", or "project"); groups can be switched off.
    std::string group;
    JSON        inputSchema = JSON{ { "type", "object" }, { "properties", JSON::object() } };

    std::function<TOOL_RESULT( const JSON& aArgs, TOOL_CONTEXT& aContext )> handler;
};


class TOOL_REGISTRY
{
public:
    /// Add a tool; a tool with the same name is replaced.
    void Add( TOOL_DEF aDef );

    const TOOL_DEF* Find( const std::string& aName ) const;

    /// The tools/list array, leaving out tools of \a aDisabledGroups.
    JSON ListJson( const std::set<std::string>& aDisabledGroups = {} ) const;

    std::set<std::string> Groups() const;

    const std::vector<TOOL_DEF>& Tools() const { return m_tools; }

    size_t Size() const { return m_tools.size(); }

private:
    std::vector<TOOL_DEF> m_tools;
};


struct RESOURCE_CONTENT
{
    std::string uri;
    std::string mimeType;
    std::string text;       ///< For text resources.
    std::string blobBase64; ///< For binary resources (text left empty).

    JSON ToJson() const;
};


struct RESOURCE_DEF
{
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType;

    std::function<RESOURCE_CONTENT()> read;
};


struct RESOURCE_TEMPLATE_DEF
{
    std::string uriTemplate;
    std::string name;
    std::string description;
    std::string mimeType;
};


class RESOURCE_REGISTRY
{
public:
    /// Resolvers serve URIs that are not registered one by one (templates).
    using RESOLVER = std::function<std::optional<RESOURCE_CONTENT>( const std::string& aUri )>;

    void Add( RESOURCE_DEF aDef );
    void AddTemplate( RESOURCE_TEMPLATE_DEF aDef );
    void AddResolver( RESOLVER aResolver );

    JSON ListJson() const;
    JSON ListTemplatesJson() const;

    std::optional<RESOURCE_CONTENT> Read( const std::string& aUri ) const;

private:
    std::vector<RESOURCE_DEF>          m_resources;
    std::vector<RESOURCE_TEMPLATE_DEF> m_templates;
    std::vector<RESOLVER>              m_resolvers;
};


struct PROMPT_DEF
{
    std::string name;
    std::string description;

    /// [{name, description, required}]
    JSON arguments = JSON::array();

    /// Returns {"description": ..., "messages": [...]}.
    std::function<JSON( const JSON& aArgs )> get;
};


class PROMPT_REGISTRY
{
public:
    void              Add( PROMPT_DEF aDef );
    const PROMPT_DEF* Find( const std::string& aName ) const;
    JSON              ListJson() const;

private:
    std::vector<PROMPT_DEF> m_prompts;
};

} // namespace AGENTIC

#endif // AGENTIC_REGISTRY_H
