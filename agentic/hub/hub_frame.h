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

#ifndef AGENTIC_HUB_FRAME_H
#define AGENTIC_HUB_FRAME_H

#include <memory>

#include <wx/frame.h>

class KIWAY;
class wxTextCtrl;

namespace AGENTIC
{

class AGENT_HOST;
class HTTP_TRANSPORT;

/**
 * The hub window of viewer mode.  It owns the agent host and the HTTP transport, and runs
 * tool calls on the GUI thread through CallAfter on itself, so no call outlives the frame.
 */
class HUB_FRAME : public wxFrame
{
public:
    explicit HUB_FRAME( KIWAY* aKiway );
    ~HUB_FRAME() override;

    /// Start the HTTP server; prints the connection details on stderr.  False on failure.
    bool StartServer( int aPort );

    AGENT_HOST& Host() { return *m_host; }

private:
    void onClose( wxCloseEvent& aEvent );

    KIWAY*                          m_kiway;
    std::unique_ptr<AGENT_HOST>     m_host;
    std::unique_ptr<HTTP_TRANSPORT> m_transport;
    wxTextCtrl*                     m_info;
};

} // namespace AGENTIC

#endif // AGENTIC_HUB_FRAME_H
