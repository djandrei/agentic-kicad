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


#include <agentic/core/progress.h>
#include <agentic/protocol/registry.h>

#include <algorithm>
#include <cmath>


namespace AGENTIC
{

AGENT_PROGRESS_REPORTER::AGENT_PROGRESS_REPORTER( TOOL_CONTEXT& aContext, UI_SINK aUiSink ) :
        m_context( aContext ),
        m_uiSink( std::move( aUiSink ) )
{
}


void AGENT_PROGRESS_REPORTER::SetNumPhases( int aNumPhases )
{
    m_numPhases = std::max( 1, aNumPhases );
}


void AGENT_PROGRESS_REPORTER::AddPhases( int aNumPhases )
{
    m_numPhases += aNumPhases;
}


void AGENT_PROGRESS_REPORTER::BeginPhase( int aPhase )
{
    m_phase = aPhase;
    m_progress = 0;
    forward();
}


void AGENT_PROGRESS_REPORTER::AdvancePhase()
{
    m_phase++;
    m_progress = 0;
    forward();
}


void AGENT_PROGRESS_REPORTER::AdvancePhase( const wxString& aMessage )
{
    Report( aMessage );
    AdvancePhase();
}


void AGENT_PROGRESS_REPORTER::Report( const wxString& aMessage )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_message = aMessage.ToStdString( wxConvUTF8 );
}


void AGENT_PROGRESS_REPORTER::SetCurrentProgress( double aProgress )
{
    m_progress = static_cast<int>( std::lround( std::clamp( aProgress, 0.0, 1.0 ) * m_maxProgress ) );
}


void AGENT_PROGRESS_REPORTER::SetMaxProgress( int aMaxProgress )
{
    m_maxProgress = std::max( 1, aMaxProgress );
}


void AGENT_PROGRESS_REPORTER::AdvanceProgress()
{
    m_progress++;
}


double AGENT_PROGRESS_REPORTER::Fraction() const
{
    const double phases = std::max( 1, m_numPhases.load() );
    const double inPhase = std::clamp( double( m_progress ) / std::max( 1, m_maxProgress.load() ), 0.0, 1.0 );
    return std::clamp( ( m_phase + inPhase ) / phases, 0.0, 1.0 );
}


void AGENT_PROGRESS_REPORTER::forward()
{
    std::string message;
    double      fraction = Fraction();

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        // Throttle: one notification per percent.
        if( m_lastSent >= 0 && std::fabs( fraction - m_lastSent ) < 0.01 )
            return;

        m_lastSent = fraction;
        message = m_message;
    }

    m_context.Progress( fraction, 1.0, message );
}


bool AGENT_PROGRESS_REPORTER::KeepRefreshing( bool aWait )
{
    forward();

    if( m_uiSink )
    {
        std::string message;

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            message = m_message;
        }

        m_uiSink( Fraction(), message );
    }

    return !IsCancelled();
}


bool AGENT_PROGRESS_REPORTER::IsCancelled() const
{
    return m_context.IsCancelled();
}

} // namespace AGENTIC
