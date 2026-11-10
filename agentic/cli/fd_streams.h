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

#ifndef AGENTIC_FD_STREAMS_H
#define AGENTIC_FD_STREAMS_H

#include <atomic>
#include <istream>
#include <ostream>
#include <streambuf>
#include <vector>

namespace AGENTIC
{

/**
 * Output stream buffer writing straight to a file descriptor.
 */
class FD_OUT_BUF : public std::streambuf
{
public:
    explicit FD_OUT_BUF( int aFd ) :
            m_fd( aFd )
    {
    }

protected:
    int_type        overflow( int_type aChar ) override;
    std::streamsize xsputn( const char* aData, std::streamsize aCount ) override;

private:
    int m_fd;
};


/**
 * Input stream buffer reading a file descriptor with poll(), so a reader blocked on it can be
 * stopped: when \a aStop becomes true the stream reports end of file within 100 ms.
 */
class FD_IN_BUF : public std::streambuf
{
public:
    FD_IN_BUF( int aFd, const std::atomic<bool>& aStop ) :
            m_fd( aFd ),
            m_stop( aStop ),
            m_buffer( 64 * 1024 )
    {
    }

protected:
    int_type underflow() override;

private:
    int                      m_fd;
    const std::atomic<bool>& m_stop;
    std::vector<char>        m_buffer;
};

} // namespace AGENTIC

#endif // AGENTIC_FD_STREAMS_H
