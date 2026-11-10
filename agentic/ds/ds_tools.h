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


#ifndef AGENTIC_DS_TOOLS_H
#define AGENTIC_DS_TOOLS_H

#include <string>

#include <wx/image.h>

class DS_DATA_MODEL;
class PROJECT;

namespace AGENTIC
{

class AGENT_SERVICES;

/// \a aModel drawn on a page of \a aPageType (A4, A3, Letter, ...), \a aWidth pixels wide.
wxImage DrawingSheetPicture( DS_DATA_MODEL& aModel, PROJECT& aProject, const std::string& aPageType, int aWidth,
                             const std::string& aCapturePath );

/// ds_open, ds_create, ds_items, ds_item_add, ds_item_set, ds_item_delete, ds_capture
/// (stage 12), group "ds".
void RegisterDrawingSheetTools( AGENT_SERVICES& aServices );

} // namespace AGENTIC

#endif // AGENTIC_DS_TOOLS_H
