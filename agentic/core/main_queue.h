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

#ifndef AGENTIC_MAIN_QUEUE_H
#define AGENTIC_MAIN_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include <agentic/protocol/mcp_server.h>

namespace AGENTIC
{

/**
 * Work queue drained by the main thread of a process without a running wx event loop
 * (kicad-cli).  Its executor hands work from transport threads to the main thread.
 */
class MAIN_QUEUE
{
public:
    EXECUTOR Executor();

    void Post( std::function<void()> aWork );

    /**
     * Run queued work on the calling thread.  Waits up to \a aWait for the first unit when the
     * queue is empty.
     *
     * @return the number of units run.
     */
    int RunPending( std::chrono::milliseconds aWait );

    /// Wake a RunPending() that is waiting.
    void Wake();

private:
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    std::deque<std::function<void()>> m_queue;
    bool                              m_wake = false;
};

} // namespace AGENTIC

#endif // AGENTIC_MAIN_QUEUE_H
