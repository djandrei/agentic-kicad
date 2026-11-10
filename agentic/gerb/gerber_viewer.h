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


#ifndef AGENTIC_GERBER_VIEWER_H
#define AGENTIC_GERBER_VIEWER_H

#include <agentic/viewer_shell/agent_viewer.h>

namespace AGENTIC
{

class AGENT_SERVICES;
class AGENT_PROJECT;

/// The Gerber viewer (stage 11): the loaded layers, their list and file information.
AGENT_VIEWER* CreateGerberViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject );

} // namespace AGENTIC

#endif // AGENTIC_GERBER_VIEWER_H
