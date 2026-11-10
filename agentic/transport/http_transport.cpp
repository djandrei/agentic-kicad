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

#include <agentic/transport/http_transport.h>
#include <agentic/protocol/json_rpc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <list>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;


namespace AGENTIC
{

std::string GenerateToken( size_t aBytes )
{
    std::random_device rd;
    std::ostringstream out;
    out << std::hex << std::setfill( '0' );

    for( size_t i = 0; i < aBytes; ++i )
        out << std::setw( 2 ) << ( rd() & 0xffU );

    return out.str();
}


namespace
{

    std::string toLower( std::string aText )
    {
        std::transform( aText.begin(), aText.end(), aText.begin(),
                        []( unsigned char c )
                        {
                            return static_cast<char>( std::tolower( c ) );
                        } );
        return aText;
    }


    /// Host part of an Origin header value ("http://localhost:1234" -> "localhost").
    std::string originHost( const std::string& aOrigin )
    {
        std::string rest = aOrigin;
        size_t      scheme = rest.find( "://" );

        if( scheme != std::string::npos )
            rest = rest.substr( scheme + 3 );

        if( !rest.empty() && rest[0] == '[' )
        {
            size_t close = rest.find( ']' );
            return close == std::string::npos ? rest : rest.substr( 0, close + 1 );
        }

        size_t end = rest.find_first_of( ":/" );
        return end == std::string::npos ? rest : rest.substr( 0, end );
    }


    bool originAllowed( const std::string& aOrigin )
    {
        if( aOrigin.empty() || aOrigin == "null" )
            return true;

        const std::string host = toLower( originHost( aOrigin ) );
        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }


    HTTP_TRANSPORT::RESPONSE jsonResponse( unsigned aStatus, const std::string& aBody )
    {
        HTTP_TRANSPORT::RESPONSE r;
        r.status = aStatus;
        r.contentType = "application/json";
        r.body = aBody;
        return r;
    }


    HTTP_TRANSPORT::RESPONSE errorResponse( unsigned aStatus, const std::string& aMessage )
    {
        return jsonResponse( aStatus, JSON{ { "error", aMessage } }.dump() );
    }


    /// True when a request in the message carries params._meta.progressToken.
    bool wantsProgress( const JSON& aMessage )
    {
        auto check = []( const JSON& aItem )
        {
            if( !JSON_RPC::IsRequest( aItem ) || !aItem.contains( "params" ) || !aItem["params"].is_object() )
                return false;

            const JSON& params = aItem["params"];
            return params.contains( "_meta" ) && params["_meta"].is_object()
                   && params["_meta"].contains( "progressToken" );
        };

        if( aMessage.is_array() )
            return std::any_of( aMessage.begin(), aMessage.end(), check );

        return check( aMessage );
    }


    bool isInitialize( const JSON& aMessage )
    {
        return aMessage.is_object() && aMessage.value( "method", "" ) == "initialize";
    }


    /**
 * Shut a connected socket down at the OS level.  Used from another thread than the one that
 * reads the socket, to wake it; the asio object itself is not touched.
 */
    void shutdownNative( tcp::socket& aSocket )
    {
#ifdef _WIN32
        ::shutdown( aSocket.native_handle(), SD_BOTH );
#else
        ::shutdown( aSocket.native_handle(), SHUT_RDWR );
#endif
    }


    /**
 * The answers to one POST: notifications as they come, then the final response.
 */
    class HTTP_CHANNEL : public REPLY_CHANNEL
    {
    public:
        void Send( const JSON& aMessage ) override
        {
            {
                std::lock_guard<std::mutex> lock( m_mutex );

                if( m_aborted )
                    return;

                m_queue.push_back( aMessage );
            }

            m_cv.notify_all();
        }

        /// Next message, or std::nullopt when the channel was aborted.
        std::optional<JSON> Next()
        {
            std::unique_lock<std::mutex> lock( m_mutex );
            m_cv.wait( lock,
                       [this]()
                       {
                           return m_aborted || !m_queue.empty();
                       } );

            if( m_queue.empty() )
                return std::nullopt;

            JSON message = std::move( m_queue.front() );
            m_queue.pop_front();
            return message;
        }

        void Abort()
        {
            {
                std::lock_guard<std::mutex> lock( m_mutex );
                m_aborted = true;
            }

            m_cv.notify_all();
        }

        /// A message is final when it is a response (no "method"), or a batch of responses.
        static bool IsFinal( const JSON& aMessage ) { return aMessage.is_array() || !aMessage.contains( "method" ); }

    private:
        std::mutex              m_mutex;
        std::condition_variable m_cv;
        std::deque<JSON>        m_queue;
        bool                    m_aborted = false;
    };

} // namespace


class HTTP_TRANSPORT::IMPL
{
public:
    struct CONNECTION
    {
        std::thread                  thread;
        std::shared_ptr<tcp::socket> socket;
        std::atomic<bool>            done{ false };
    };

    IMPL( MCP_SERVER& aServer, HTTP_OPTIONS aOptions ) :
            m_server( aServer ),
            m_options( std::move( aOptions ) ),
            m_sessionId( GenerateToken( 16 ) )
    {
    }

    MCP_SERVER&                  m_server;
    HTTP_OPTIONS                 m_options;
    std::string                  m_sessionId;
    net::io_context              m_ioc;
    std::optional<tcp::acceptor> m_acceptor;
    std::thread                  m_acceptThread;
    std::atomic<bool>            m_running{ false };

    std::mutex                              m_mutex;
    std::list<std::unique_ptr<CONNECTION>>  m_connections;
    std::set<std::shared_ptr<HTTP_CHANNEL>> m_channels;

    void trackChannel( const std::shared_ptr<HTTP_CHANNEL>& aChannel )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_channels.insert( aChannel );
    }

    void untrackChannel( const std::shared_ptr<HTTP_CHANNEL>& aChannel )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_channels.erase( aChannel );
    }

    void reapConnections()
    {
        std::lock_guard<std::mutex> lock( m_mutex );

        for( auto it = m_connections.begin(); it != m_connections.end(); )
        {
            if( ( *it )->done )
            {
                if( ( *it )->thread.joinable() )
                    ( *it )->thread.join();

                it = m_connections.erase( it );
            }
            else
            {
                ++it;
            }
        }
    }

    void acceptLoop( HTTP_TRANSPORT* aOwner )
    {
        while( m_running )
        {
            auto              socket = std::make_shared<tcp::socket>( m_ioc );
            beast::error_code ec;
            m_acceptor->accept( *socket, ec );

            if( !m_running )
                break;

            if( ec )
                continue;

            reapConnections();

            auto        conn = std::make_unique<CONNECTION>();
            CONNECTION* raw = conn.get();
            conn->socket = socket;

            std::lock_guard<std::mutex> lock( m_mutex );
            m_connections.push_back( std::move( conn ) );
            raw->thread = std::thread(
                    [this, aOwner, raw]()
                    {
                        serve( aOwner, *raw->socket );
                        raw->done = true;
                    } );
        }
    }

    void serve( HTTP_TRANSPORT* aOwner, tcp::socket& aSocket )
    {
        // An idle client must not hold its thread for ever
#ifndef _WIN32
        const timeval timeout{ 30, 0 };
        setsockopt( aSocket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) );
        setsockopt( aSocket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof( timeout ) );
#endif

        try
        {
            serveRequests( aOwner, aSocket );
        }
        catch( const std::exception& )
        {
            // Malformed request or broken connection: drop it
        }

        // Only shut down here; the socket is closed when its CONNECTION is destroyed after the
        // thread is joined, so Stop() never touches a closed (and possibly reused) descriptor.
        shutdownNative( aSocket );
    }

    void serveRequests( HTTP_TRANSPORT* aOwner, tcp::socket& aSocket )
    {
        beast::error_code ec;

        while( m_running )
        {
            beast::flat_buffer                      buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit( 64U * 1024U * 1024U );
            http::read( aSocket, buffer, parser, ec );

            if( ec )
                break;

            const auto req = parser.release();

            std::map<std::string, std::string> headers;

            for( const auto& field : req )
                headers[toLower( std::string( field.name_string() ) )] = std::string( field.value() );

            RESPONSE result = aOwner->Handle( std::string( req.method_string() ), std::string( req.target() ), headers,
                                              req.body() );

            if( result.nextEvent )
            {
                http::response<http::empty_body> res{ http::status::ok, req.version() };
                res.set( http::field::server, "agentic-kicad" );
                res.set( http::field::content_type, "text/event-stream" );
                res.set( http::field::cache_control, "no-cache" );

                for( const auto& [name, value] : result.headers )
                    res.set( name, value );

                res.chunked( true );
                res.keep_alive( false );

                http::response_serializer<http::empty_body> sr{ res };
                http::write_header( aSocket, sr, ec );

                while( !ec )
                {
                    std::optional<std::string> event = result.nextEvent();

                    if( !event )
                        break;

                    net::write( aSocket, http::make_chunk( net::buffer( *event ) ), ec );
                }

                if( !ec )
                    net::write( aSocket, http::make_chunk_last(), ec );

                break;
            }

            http::response<http::string_body> res{ static_cast<http::status>( result.status ), req.version() };
            res.set( http::field::server, "agentic-kicad" );
            res.set( http::field::content_type, result.contentType );

            for( const auto& [name, value] : result.headers )
                res.set( name, value );

            res.keep_alive( req.keep_alive() );
            res.body() = result.body;
            res.prepare_payload();
            http::write( aSocket, res, ec );

            if( ec || !req.keep_alive() )
                break;
        }
    }
};


HTTP_TRANSPORT::HTTP_TRANSPORT( MCP_SERVER& aServer, HTTP_OPTIONS aOptions ) :
        m_impl( std::make_unique<IMPL>( aServer, std::move( aOptions ) ) )
{
}


HTTP_TRANSPORT::~HTTP_TRANSPORT()
{
    Stop();
}


void HTTP_TRANSPORT::Start()
{
    if( m_impl->m_running )
        return;

    beast::error_code ec;
    const auto        address = net::ip::make_address( m_impl->m_options.bindAddress, ec );

    if( ec )
        throw std::runtime_error( "MCP HTTP: invalid bind address " + m_impl->m_options.bindAddress );

    if( !address.is_loopback() )
        throw std::runtime_error( "MCP HTTP: refusing to bind a non-loopback address "
                                  + m_impl->m_options.bindAddress );

    const tcp::endpoint endpoint( address, m_impl->m_options.port );

    m_impl->m_acceptor.emplace( m_impl->m_ioc );
    m_impl->m_acceptor->open( endpoint.protocol(), ec );

    if( !ec )
        m_impl->m_acceptor->set_option( net::socket_base::reuse_address( true ), ec );

    if( !ec )
        m_impl->m_acceptor->bind( endpoint, ec );

    if( !ec )
        m_impl->m_acceptor->listen( net::socket_base::max_listen_connections, ec );

    if( ec )
    {
        m_impl->m_acceptor.reset();
        throw std::runtime_error( "MCP HTTP: cannot listen on " + m_impl->m_options.bindAddress + ":"
                                  + std::to_string( m_impl->m_options.port ) + ": " + ec.message() );
    }

    m_impl->m_options.port = m_impl->m_acceptor->local_endpoint().port();
    m_impl->m_running = true;
    m_impl->m_acceptThread = std::thread(
            [this]()
            {
                m_impl->acceptLoop( this );
            } );
}


void HTTP_TRANSPORT::Stop()
{
    if( !m_impl->m_running )
        return;

    m_impl->m_running = false;

    // Wake the blocking accept() with a connection of our own
    {
        beast::error_code ec;
        net::io_context   ioc;
        tcp::socket       wake( ioc );
        wake.connect( tcp::endpoint( net::ip::make_address( m_impl->m_options.bindAddress ), m_impl->m_options.port ),
                      ec );
    }

    if( m_impl->m_acceptThread.joinable() )
        m_impl->m_acceptThread.join();

    beast::error_code ec;
    m_impl->m_acceptor->close( ec );

    {
        std::lock_guard<std::mutex> lock( m_impl->m_mutex );

        for( const std::shared_ptr<HTTP_CHANNEL>& channel : m_impl->m_channels )
            channel->Abort();

        for( const std::unique_ptr<IMPL::CONNECTION>& conn : m_impl->m_connections )
            shutdownNative( *conn->socket );
    }

    std::list<std::unique_ptr<IMPL::CONNECTION>> connections;

    {
        std::lock_guard<std::mutex> lock( m_impl->m_mutex );
        connections.swap( m_impl->m_connections );
    }

    for( const std::unique_ptr<IMPL::CONNECTION>& conn : connections )
    {
        if( conn->thread.joinable() )
            conn->thread.join();
    }

    m_impl->m_acceptor.reset();
}


bool HTTP_TRANSPORT::Running() const
{
    return m_impl->m_running;
}


unsigned short HTTP_TRANSPORT::Port() const
{
    return m_impl->m_options.port;
}


std::string HTTP_TRANSPORT::Url() const
{
    return "http://" + m_impl->m_options.bindAddress + ":" + std::to_string( Port() ) + m_impl->m_options.path;
}


std::string HTTP_TRANSPORT::SessionId() const
{
    return m_impl->m_sessionId;
}


HTTP_TRANSPORT::RESPONSE HTTP_TRANSPORT::Handle( const std::string& aMethod, const std::string& aTarget,
                                                 const std::map<std::string, std::string>& aHeaders,
                                                 const std::string&                        aBody )
{
    const std::string path = aTarget.substr( 0, aTarget.find( '?' ) );

    if( path != m_impl->m_options.path )
        return errorResponse( 404, "Not found" );

    auto origin = aHeaders.find( "origin" );

    if( origin != aHeaders.end() && !originAllowed( origin->second ) )
        return errorResponse( 403, "Origin not allowed" );

    if( !m_impl->m_options.token.empty() )
    {
        auto auth = aHeaders.find( "authorization" );

        if( auth == aHeaders.end() || auth->second != "Bearer " + m_impl->m_options.token )
        {
            RESPONSE r = errorResponse( 401, "Unauthorized: expected Authorization: Bearer <token>" );
            r.headers["WWW-Authenticate"] = "Bearer";
            return r;
        }
    }

    if( aMethod != "POST" )
    {
        RESPONSE r = errorResponse( 405, "Method not allowed: this server accepts JSON-RPC over POST only" );
        r.headers["Allow"] = "POST";
        return r;
    }

    auto session = aHeaders.find( "mcp-session-id" );

    if( session != aHeaders.end() && session->second != m_impl->m_sessionId )
        return errorResponse( 404, "Session not found" );

    const JSON parsed = JSON::parse( aBody, nullptr, false );
    const bool initialize = !parsed.is_discarded() && isInitialize( parsed );

    auto       accept = aHeaders.find( "accept" );
    const bool stream = !parsed.is_discarded() && wantsProgress( parsed ) && accept != aHeaders.end()
                        && accept->second.find( "text/event-stream" ) != std::string::npos;

    auto channel = std::make_shared<HTTP_CHANNEL>();
    m_impl->trackChannel( channel );

    if( !m_impl->m_server.Submit( aBody, channel ) )
    {
        m_impl->untrackChannel( channel );
        RESPONSE r;
        r.status = 202;
        r.contentType = "application/json";
        return r;
    }

    if( stream )
    {
        RESPONSE r;
        r.status = 200;
        r.contentType = "text/event-stream";
        r.headers["Mcp-Session-Id"] = m_impl->m_sessionId;

        auto  finished = std::make_shared<bool>( false );
        IMPL* impl = m_impl.get();

        r.nextEvent = [channel, finished, impl]() -> std::optional<std::string>
        {
            if( *finished )
                return std::nullopt;

            std::optional<JSON> message = channel->Next();

            if( !message || HTTP_CHANNEL::IsFinal( *message ) )
            {
                *finished = true;
                impl->untrackChannel( channel );
            }

            if( !message )
                return std::nullopt;

            return "event: message\ndata: " + JSON_RPC::Serialize( *message ) + "\n\n";
        };

        return r;
    }

    // Plain JSON: wait for the final response, dropping progress notifications
    for( ;; )
    {
        std::optional<JSON> message = channel->Next();

        if( !message )
        {
            m_impl->untrackChannel( channel );
            return errorResponse( 503, "Server stopping" );
        }

        if( HTTP_CHANNEL::IsFinal( *message ) )
        {
            m_impl->untrackChannel( channel );
            RESPONSE r = jsonResponse( 200, JSON_RPC::Serialize( *message ) );

            if( initialize )
                r.headers["Mcp-Session-Id"] = m_impl->m_sessionId;

            return r;
        }
    }
}

} // namespace AGENTIC
