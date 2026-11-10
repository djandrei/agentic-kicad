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


#ifndef AGENTIC_PCM_TOOLS_H
#define AGENTIC_PCM_TOOLS_H

#include <filesystem>

namespace AGENTIC
{

class AGENT_SERVICES;

/**
 * pcm_catalog and pcm_install (stage 3), group "project".  Only content packages (types
 * "library" and "colortheme") are installed: a plugin would run code inside KiCad, so plugin
 * packages are refused.  Packages are unpacked the way KiCad's Plugin and Content Manager
 * does (<3rd party>/<folder>/<package id>/...) and recorded in its installed_packages.json.
 */
void RegisterPcmTools( AGENT_SERVICES& aServices );

/// The folder packages install into: ${KICADn_3RD_PARTY}, or KiCad's default.
std::filesystem::path PcmInstallRoot();

} // namespace AGENTIC

#endif // AGENTIC_PCM_TOOLS_H
