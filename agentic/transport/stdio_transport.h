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

#ifndef AGENTIC_STDIO_TRANSPORT_H
#define AGENTIC_STDIO_TRANSPORT_H

#include <iosfwd>
#include <mutex>

#include <agentic/protocol/mcp_server.h>

namespace AGENTIC
{

/**
 * MCP stdio transport: newline-delimited JSON-RPC messages on an input and an output stream.
 *
 * Run() reads on the calling thread, which must not be the executor thread: requests are
 * handed to the server, which runs them on the executor, while reading goes on so that a
 * cancellation can arrive.  The output stream must carry nothing else while this runs.
 */
class STDIO_TRANSPORT
{
public:
    STDIO_TRANSPORT( MCP_SERVER& aServer, std::istream& aIn, std::ostream& aOut );

    /// Read messages until end of input, then wait until every request has been answered.
    void Run();

    /// Write one message.  Thread-safe.
    void Send( const JSON& aMessage );

private:
    MCP_SERVER&   m_server;
    std::istream& m_in;
    std::ostream& m_out;
    std::mutex    m_writeMutex;
};

} // namespace AGENTIC

#endif // AGENTIC_STDIO_TRANSPORT_H
