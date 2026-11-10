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

#ifndef AGENTIC_HTTP_TRANSPORT_H
#define AGENTIC_HTTP_TRANSPORT_H

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <agentic/protocol/mcp_server.h>

namespace AGENTIC
{

struct HTTP_OPTIONS
{
    std::string    bindAddress = "127.0.0.1"; ///< Must be a loopback address.
    unsigned short port = 0;                  ///< 0 picks a free port; see HTTP_TRANSPORT::Port().
    std::string    token;                     ///< Empty disables authentication (tests only).
    std::string    path = "/mcp";
};


/// A random bearer token: \a aBytes bytes from std::random_device, as hex.
std::string GenerateToken( size_t aBytes = 16 );


/**
 * MCP Streamable HTTP transport.
 *
 * POST only: there is no standing server-to-client stream, so broadcasts (list_changed) are
 * not delivered over HTTP; clients re-list tools on their own schedule.  A request that
 * carries a progressToken and accepts text/event-stream is answered as an event stream
 * with its progress notifications followed by the response; anything else is answered
 * with one JSON body.
 *
 * Binds to loopback only, rejects foreign Origin headers (DNS rebinding) and requires
 * "Authorization: Bearer <token>".  Each connection is served on its own thread, so a
 * cancellation can be posted while another request is running.
 */
class HTTP_TRANSPORT
{
public:
    struct RESPONSE
    {
        unsigned                           status = 200;
        std::string                        contentType;
        std::string                        body;
        std::map<std::string, std::string> headers;

        /// Set for an event stream: each call returns the next event, std::nullopt at the end.
        std::function<std::optional<std::string>()> nextEvent;
    };

    HTTP_TRANSPORT( MCP_SERVER& aServer, HTTP_OPTIONS aOptions );
    ~HTTP_TRANSPORT();

    HTTP_TRANSPORT( const HTTP_TRANSPORT& ) = delete;
    HTTP_TRANSPORT& operator=( const HTTP_TRANSPORT& ) = delete;

    /// Bind and start serving.  Throws std::runtime_error when the address cannot be used.
    void Start();

    /// Stop accepting, abort waiting requests and join every thread.
    void Stop();

    bool           Running() const;
    unsigned short Port() const;
    std::string    Url() const; ///< http://<bind>:<port><path>
    std::string    SessionId() const;

    /**
     * Handle one HTTP request without a socket.  Header names must be lowercase.  Public so
     * the protocol-level behaviour can be tested.
     */
    RESPONSE Handle( const std::string& aMethod, const std::string& aTarget,
                     const std::map<std::string, std::string>& aHeaders, const std::string& aBody );

private:
    class IMPL;
    std::unique_ptr<IMPL> m_impl;
};

} // namespace AGENTIC

#endif // AGENTIC_HTTP_TRANSPORT_H
