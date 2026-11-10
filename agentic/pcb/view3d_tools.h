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


#ifndef AGENTIC_VIEW3D_TOOLS_H
#define AGENTIC_VIEW3D_TOOLS_H

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

class BOARD_ADAPTER;
class CAMERA;

namespace AGENTIC
{

class AGENT_PROJECT;
class AGENT_SERVICES;

/// What the agent looks at in 3D: a named view, extra rotation, zoom, and what is shown.
struct VIEW3D_STATE
{
    std::string view = "top"; ///< top, bottom, left, right, front, back
    double      rotateX = 0;  ///< degrees, applied after the view
    double      rotateY = 0;
    double      rotateZ = 0;
    double      zoom = 1.0;
    bool        perspective = false;

    bool models = true;
    bool silkscreen = true;
    bool solderMask = true;
    bool solderPaste = false;
    bool floor = false;
    bool high = false; ///< high quality (shadows, reflections) instead of basic

    std::optional<std::string> maskColor;       ///< #rrggbb
    std::optional<std::string> silkscreenColor; ///< #rrggbb

    nlohmann::json Json() const;
};


/// The 3D state of \a aProject (kept for the session, not saved).
VIEW3D_STATE& View3dState( AGENT_PROJECT& aProject );

/// Point \a aCamera as \a aState says; the board must be loaded in the renderer.
void View3dApplyCamera( CAMERA& aCamera, const VIEW3D_STATE& aState );

/// Set \a aAdapter's visible layers and colours from \a aState.
void View3dApplyOptions( BOARD_ADAPTER& aAdapter, const VIEW3D_STATE& aState );

/// view3d_camera_get|set, view3d_options, view3d_capture (stage 9), group "view3d".
void RegisterView3dTools( AGENT_SERVICES& aServices );

} // namespace AGENTIC

#endif // AGENTIC_VIEW3D_TOOLS_H
