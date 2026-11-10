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


#ifndef AGENTIC_GERBER_TOOLS_H
#define AGENTIC_GERBER_TOOLS_H

#include <memory>
#include <string>
#include <vector>

#include <wx/image.h>

class GERBER_FILE_IMAGE_LIST;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;

/// The Gerber and drill files loaded in a project (they are outputs, not design files).
struct GERBER_STATE
{
    GERBER_STATE();
    ~GERBER_STATE();

    std::unique_ptr<GERBER_FILE_IMAGE_LIST> images;
    std::vector<std::string>                files; ///< project-relative, by image index
    int                                     loads = 0;
};


GERBER_STATE& GerberState( AGENT_PROJECT& aProject );

/// The loaded layers \a aIndices (all when empty) drawn over each other, \a aWidth wide.
wxImage GerberPicture( AGENT_PROJECT& aProject, const std::vector<int>& aIndices, int aWidth );

/// gerber_load, gerber_layers, gerber_info, gerber_capture, gerber_diff, gerber_to_pcb
/// (stage 11), group "gerber".
void RegisterGerberTools( AGENT_SERVICES& aServices );

} // namespace AGENTIC

#endif // AGENTIC_GERBER_TOOLS_H
