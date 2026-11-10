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

#include <agentic/cli/fd_streams.h>

#include <cerrno>

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif


namespace AGENTIC
{

FD_OUT_BUF::int_type FD_OUT_BUF::overflow( int_type aChar )
{
    if( traits_type::eq_int_type( aChar, traits_type::eof() ) )
        return traits_type::not_eof( aChar );

    char c = traits_type::to_char_type( aChar );
    return xsputn( &c, 1 ) == 1 ? aChar : traits_type::eof();
}


std::streamsize FD_OUT_BUF::xsputn( const char* aData, std::streamsize aCount )
{
    std::streamsize written = 0;

    while( written < aCount )
    {
        ssize_t n = ::write( m_fd, aData + written, static_cast<size_t>( aCount - written ) );

        if( n < 0 )
        {
            if( errno == EINTR )
                continue;

            break;
        }

        written += n;
    }

    return written;
}


FD_IN_BUF::int_type FD_IN_BUF::underflow()
{
    if( gptr() < egptr() )
        return traits_type::to_int_type( *gptr() );

    for( ;; )
    {
        if( m_stop )
            return traits_type::eof();

        pollfd pfd{ m_fd, POLLIN, 0 };
        int    ready = ::poll( &pfd, 1, 100 );

        if( ready < 0 && errno != EINTR )
            return traits_type::eof();

        if( ready <= 0 )
            continue;

        ssize_t n = ::read( m_fd, m_buffer.data(), m_buffer.size() );

        if( n < 0 && errno == EINTR )
            continue;

        if( n <= 0 )
            return traits_type::eof();

        setg( m_buffer.data(), m_buffer.data(), m_buffer.data() + n );
        return traits_type::to_int_type( *gptr() );
    }
}

} // namespace AGENTIC
