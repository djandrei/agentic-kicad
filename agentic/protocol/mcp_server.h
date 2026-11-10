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

#ifndef AGENTIC_MCP_SERVER_H
#define AGENTIC_MCP_SERVER_H

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <agentic/protocol/registry.h>

namespace AGENTIC
{

/**
 * Where the answers to one incoming message go.  For stdio this is the process's stdout; for
 * HTTP it is the response to one POST.  Send() must be thread-safe: notifications are sent
 * from the thread that runs the tool, parse errors from the transport thread.
 */
class REPLY_CHANNEL
{
public:
    virtual ~REPLY_CHANNEL() = default;

    virtual void Send( const JSON& aMessage ) = 0;
};


/**
 * Runs a unit of work asynchronously on the thread that owns the sessions (the wx main
 * thread in KiCad), one unit at a time, in order.
 */
using EXECUTOR = std::function<void( std::function<void()> )>;


struct SERVER_INFO
{
    std::string name;
    std::string version;

    /// Free text handed to the client in the initialize result: how to use the server.
    std::string instructions;
};


/**
 * Transport-agnostic MCP server.
 *
 * Submit() is called by transport threads.  Requests are executed through the executor, so
 * the registries, the handlers and the sessions behind them are only touched from one
 * thread.  Cancellation notifications are handled on the transport thread at once, so a
 * cancel reaches a tool that is running.
 */
class MCP_SERVER
{
public:
    /// Called after every tools/call with the outcome and the wall time in ms (executor thread).
    using TOOL_OBSERVER = std::function<void( const std::string& aName, const JSON& aArgs, const TOOL_RESULT& aResult,
                                              double aMillis )>;

    /// Consulted before running a tool; a returned string blocks the call with that message
    /// as a tool error (the Pause control).
    using TOOL_GATE = std::function<std::optional<std::string>( const std::string& aName )>;

    /// Called before every tools/call, on the executor thread (activity logs, viewers).
    using TOOL_START = std::function<void( const std::string& aName, const JSON& aArgs )>;

    MCP_SERVER( SERVER_INFO aInfo, TOOL_REGISTRY& aTools, RESOURCE_REGISTRY& aResources, PROMPT_REGISTRY& aPrompts,
                EXECUTOR aExecutor );

    /**
     * Handle one message as received from a transport.  Thread-safe.
     *
     * @return true when the message contains at least one request, so a response will be
     *         sent on \a aChannel (possibly later, from the executor thread).
     */
    bool Submit( const std::string& aText, std::shared_ptr<REPLY_CHANNEL> aChannel );

    /// Block until no submitted request is pending or running.  Not to be called from the
    /// executor thread.
    void WaitIdle();

    /// Server-to-client notifications that do not belong to a request (list_changed).
    void SetBroadcast( std::function<void( const JSON& )> aBroadcast );
    void Broadcast( const std::string& aMethod, JSON aParams = JSON::object() );

    void SetToolObserver( TOOL_OBSERVER aObserver ) { m_observer = std::move( aObserver ); }
    void SetToolGate( TOOL_GATE aGate ) { m_gate = std::move( aGate ); }
    void SetToolStart( TOOL_START aStart ) { m_start = std::move( aStart ); }

    /**
     * Hide the tools of these groups from tools/list and refuse calls to them.  Executor
     * thread.  Sends notifications/tools/list_changed when the set changes.
     */
    void                         SetDisabledGroups( const std::set<std::string>& aGroups );
    const std::set<std::string>& DisabledGroups() const { return m_disabledGroups; }

    bool               IsInitialized() const { return m_initialized; }
    const std::string& ProtocolVersion() const { return m_protocolVersion; }
    const std::string& ClientName() const { return m_clientName; }

    static const std::vector<std::string>& SupportedProtocolVersions();

    JSON Capabilities() const;

private:
    std::optional<JSON> handleSingle( const JSON& aMessage, const std::shared_ptr<REQUEST_STATE>& aState );
    JSON dispatch( const std::string& aMethod, const JSON& aParams, const std::shared_ptr<REQUEST_STATE>& aState );
    JSON onInitialize( const JSON& aParams );
    JSON onToolsCall( const JSON& aParams, const std::shared_ptr<REQUEST_STATE>& aState );
    JSON onResourcesRead( const JSON& aParams );
    JSON onPromptsGet( const JSON& aParams );

    void handleNotification( const JSON& aMessage );
    void registerRequest( const std::shared_ptr<REQUEST_STATE>& aState );
    void unregisterRequest( const std::shared_ptr<REQUEST_STATE>& aState );

    SERVER_INFO        m_info;
    TOOL_REGISTRY&     m_tools;
    RESOURCE_REGISTRY& m_resources;
    PROMPT_REGISTRY&   m_prompts;
    EXECUTOR           m_executor;

    TOOL_OBSERVER m_observer;
    TOOL_GATE     m_gate;
    TOOL_START    m_start;

    // Executor-thread state
    bool                  m_initialized = false;
    std::string           m_protocolVersion;
    std::string           m_clientName;
    std::set<std::string> m_disabledGroups;

    // Shared with transport threads
    std::mutex                                            m_mutex;
    std::condition_variable                               m_idle;
    int                                                   m_pending = 0;
    std::map<std::string, std::shared_ptr<REQUEST_STATE>> m_inFlight; ///< Keyed by id dump.
    std::function<void( const JSON& )>                    m_broadcast;
};

} // namespace AGENTIC

#endif // AGENTIC_MCP_SERVER_H
