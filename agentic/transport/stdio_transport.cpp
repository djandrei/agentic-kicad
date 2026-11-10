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

#include <agentic/transport/stdio_transport.h>
#include <agentic/protocol/json_rpc.h>

#include <istream>
#include <ostream>
#include <string>


namespace AGENTIC
{

namespace
{

    class STDIO_CHANNEL : public REPLY_CHANNEL
    {
    public:
        explicit STDIO_CHANNEL( STDIO_TRANSPORT& aTransport ) :
                m_transport( aTransport )
        {
        }

        void Send( const JSON& aMessage ) override { m_transport.Send( aMessage ); }

    private:
        STDIO_TRANSPORT& m_transport;
    };

} // namespace


STDIO_TRANSPORT::STDIO_TRANSPORT( MCP_SERVER& aServer, std::istream& aIn, std::ostream& aOut ) :
        m_server( aServer ),
        m_in( aIn ),
        m_out( aOut )
{
}


void STDIO_TRANSPORT::Send( const JSON& aMessage )
{
    std::lock_guard<std::mutex> lock( m_writeMutex );
    m_out << JSON_RPC::Serialize( aMessage ) << '\n';
    m_out.flush();
}


void STDIO_TRANSPORT::Run()
{
    auto channel = std::make_shared<STDIO_CHANNEL>( *this );

    m_server.SetBroadcast(
            [this]( const JSON& aMessage )
            {
                Send( aMessage );
            } );

    std::string line;

    while( std::getline( m_in, line ) )
    {
        if( line.find_first_not_of( " \t\r" ) == std::string::npos )
            continue;

        m_server.Submit( line, channel );
    }

    m_server.WaitIdle();
    m_server.SetBroadcast( nullptr );
}

} // namespace AGENTIC
