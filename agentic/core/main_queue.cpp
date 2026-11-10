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

#include <agentic/core/main_queue.h>


namespace AGENTIC
{

EXECUTOR MAIN_QUEUE::Executor()
{
    return [this]( std::function<void()> aWork )
    {
        Post( std::move( aWork ) );
    };
}


void MAIN_QUEUE::Post( std::function<void()> aWork )
{
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_queue.push_back( std::move( aWork ) );
    }

    m_cv.notify_all();
}


void MAIN_QUEUE::Wake()
{
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_wake = true;
    }

    m_cv.notify_all();
}


int MAIN_QUEUE::RunPending( std::chrono::milliseconds aWait )
{
    std::deque<std::function<void()>> work;

    {
        std::unique_lock<std::mutex> lock( m_mutex );
        m_cv.wait_for( lock, aWait,
                       [this]()
                       {
                           return m_wake || !m_queue.empty();
                       } );
        m_wake = false;
        work.swap( m_queue );
    }

    for( std::function<void()>& unit : work )
        unit();

    return static_cast<int>( work.size() );
}

} // namespace AGENTIC
