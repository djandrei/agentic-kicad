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


#ifndef AGENTIC_PROGRESS_H
#define AGENTIC_PROGRESS_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <progress_reporter.h>

namespace AGENTIC
{

class TOOL_CONTEXT;

/**
 * The PROGRESS_REPORTER handed to long engine calls (D10): DRC, ERC, zone fill, routing,
 * raytracing, simulation.  It forwards progress to the client (notifications/progress, when
 * it asked) and to the hub and viewers, and reports a cancel sent by the client or a passed
 * timeout through IsCancelled().
 *
 * Engines may call it from worker threads; forwarding to the client is thread-safe, the UI
 * forward is called on the main thread from KeepRefreshing().
 */
class AGENT_PROGRESS_REPORTER : public PROGRESS_REPORTER
{
public:
    using UI_SINK = std::function<void( double aFraction, const std::string& aMessage )>;

    AGENT_PROGRESS_REPORTER( TOOL_CONTEXT& aContext, UI_SINK aUiSink = nullptr );

    void SetNumPhases( int aNumPhases ) override;
    void AddPhases( int aNumPhases ) override;
    void BeginPhase( int aPhase ) override;
    void AdvancePhase() override;
    void AdvancePhase( const wxString& aMessage ) override;
    void Report( const wxString& aMessage ) override;
    void SetCurrentProgress( double aProgress ) override;
    void SetMaxProgress( int aMaxProgress ) override;
    void AdvanceProgress() override;
    bool KeepRefreshing( bool aWait = false ) override;
    void SetTitle( const wxString& aTitle ) override {}
    bool IsCancelled() const override;

    /// Overall fraction done, 0 to 1.
    double Fraction() const;

private:
    void forward();

    TOOL_CONTEXT&      m_context;
    UI_SINK            m_uiSink;
    std::atomic_int    m_phase{ 0 };
    std::atomic_int    m_numPhases{ 1 };
    std::atomic_int    m_progress{ 0 };
    std::atomic_int    m_maxProgress{ 1000 };
    mutable std::mutex m_mutex;
    std::string        m_message;
    double             m_lastSent = -1;
};

} // namespace AGENTIC

#endif // AGENTIC_PROGRESS_H
