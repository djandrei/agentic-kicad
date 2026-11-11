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


#ifndef AGENTIC_DOCS_TOOLS_H
#define AGENTIC_DOCS_TOOLS_H

#include <filesystem>
#include <string>

namespace AGENTIC
{

class AGENT_SERVICES;

/// docs_update, docs_search, docs_page, docs_status, docs_error_codes, and the documentation
/// resources and working-loop prompts (stage 3), group "docs".
void RegisterDocsTools( AGENT_SERVICES& aServices, const std::string& aInstructions );

/// The manuals' version on docs.kicad.org for this build: "master" for a development build
/// (minor version 99), "<major>.0" otherwise.
std::string DocsVersion();

/// $AGENTIC_KICAD_DOCS_CACHE, or agentic-docs in KiCad's cache folder (PATHS::GetUserCachePath(),
/// which honours $KICAD_CACHE_HOME).
std::filesystem::path DocsCacheRoot();

} // namespace AGENTIC

#endif // AGENTIC_DOCS_TOOLS_H
