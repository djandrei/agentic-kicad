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

#ifndef AGENTIC_QA_TEST_HELPERS_H
#define AGENTIC_QA_TEST_HELPERS_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <agentic/protocol/mcp_server.h>

namespace AGENTIC_QA
{

/**
 * An EXECUTOR that runs work in order on one worker thread, like the wx main thread does in
 * KiCad.
 */
class QUEUE_EXECUTOR
{
public:
    QUEUE_EXECUTOR() :
            m_thread(
                    [this]()
                    {
                        run();
                    } )
    {
    }

    ~QUEUE_EXECUTOR()
    {
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_stop = true;
        }

        m_cv.notify_all();
        m_thread.join();
    }

    AGENTIC::EXECUTOR Executor()
    {
        return [this]( std::function<void()> aWork )
        {
            {
                std::lock_guard<std::mutex> lock( m_mutex );
                m_queue.push_back( std::move( aWork ) );
            }

            m_cv.notify_all();
        };
    }

private:
    void run()
    {
        for( ;; )
        {
            std::function<void()> work;

            {
                std::unique_lock<std::mutex> lock( m_mutex );
                m_cv.wait( lock,
                           [this]()
                           {
                               return m_stop || !m_queue.empty();
                           } );

                if( m_queue.empty() )
                    return;

                work = std::move( m_queue.front() );
                m_queue.pop_front();
            }

            work();
        }
    }

    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    std::deque<std::function<void()>> m_queue;
    bool                              m_stop = false;
    std::thread                       m_thread;
};


/// Runs work at once on the calling thread.
inline AGENTIC::EXECUTOR DirectExecutor()
{
    return []( std::function<void()> aWork )
    {
        aWork();
    };
}


/// Collects what the server sends; Wait() blocks until a given number of messages arrived.
class CAPTURE_CHANNEL : public AGENTIC::REPLY_CHANNEL
{
public:
    void Send( const AGENTIC::JSON& aMessage ) override
    {
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_messages.push_back( aMessage );
        }

        m_cv.notify_all();
    }

    std::vector<AGENTIC::JSON> Wait( size_t aCount )
    {
        std::unique_lock<std::mutex> lock( m_mutex );
        m_cv.wait_for( lock, std::chrono::seconds( 10 ),
                       [&]()
                       {
                           return m_messages.size() >= aCount;
                       } );
        return m_messages;
    }

private:
    std::mutex                 m_mutex;
    std::condition_variable    m_cv;
    std::vector<AGENTIC::JSON> m_messages;
};


/// A request message as a string.
inline std::string Request( int aId, const std::string& aMethod, AGENTIC::JSON aParams = AGENTIC::JSON::object() )
{
    return AGENTIC::JSON{ { "jsonrpc", "2.0" }, { "id", aId }, { "method", aMethod }, { "params", aParams } }.dump();
}


inline std::string InitializeRequest( int aId = 1 )
{
    return Request( aId, "initialize",
                    AGENTIC::JSON{ { "protocolVersion", "2025-06-18" },
                                   { "capabilities", AGENTIC::JSON::object() },
                                   { "clientInfo", { { "name", "qa" }, { "version", "1" } } } } );
}

} // namespace AGENTIC_QA

#endif // AGENTIC_QA_TEST_HELPERS_H
